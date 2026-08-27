/* See LICENSE file for copyright and license details. */
/* graph display — serve a local, read-only view of a repository
 *
 * Experimental. The repository stays the source of truth: this walks the tree
 * on every request, holds no index, and never writes. */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "graph.h"
#include "ui.h"

#define MAX_NODES 4096
#define MAX_EDGES 8192
#define MAX_BODY  (1 << 20)

enum { T_DIR, T_NOTE, T_FILE, T_MISSING };

struct node {
	char path[512];		/* repository-relative; "" is the root */
	char name[256];
	int type;
	long size;		/* bytes; 0 for directories and missing */
	long mtime;		/* epoch seconds; 0 when unknown */
};

struct edge {
	int s, t;		/* t < 0 never happens: unresolved targets get
				 * a T_MISSING node so they are visible */
	char raw[256];
};

static struct node nodes[MAX_NODES];
static struct edge edges[MAX_EDGES];
static int nnodes, nedges;
static char repo_root[PATH_MAX];
static char token[33];		/* per-run secret; requests must carry it */
static int srv_port;

static const char *
type_name(int t)
{
	switch (t) {
	case T_DIR:     return "dir";
	case T_NOTE:    return "note";
	case T_MISSING: return "missing";
	default:        return "file";
	}
}

static int
find_node(const char *path)
{
	int i;

	for (i = 0; i < nnodes; i++)
		if (!strcmp(nodes[i].path, path))
			return i;
	return -1;
}

static int
add_node(const char *path, const char *name, int type)
{
	int i;

	if ((i = find_node(path)) >= 0)
		return i;
	if (nnodes >= MAX_NODES)
		return -1;
	i = nnodes++;
	snprintf(nodes[i].path, sizeof(nodes[i].path), "%s", path);
	snprintf(nodes[i].name, sizeof(nodes[i].name), "%s", name);
	nodes[i].type = type;
	nodes[i].size = 0;
	nodes[i].mtime = 0;
	return i;
}

static int
has_suffix(const char *s, const char *suf)
{
	size_t ls = strlen(s), lf = strlen(suf);

	return ls >= lf && !strcmp(s + ls - lf, suf);
}

/* Walk the tree, adding a node per directory and file. Dotfiles are skipped:
 * .graph is Graph's own business, and the rest is editor and OS litter. */
static void
scan(const char *abs, const char *rel)
{
	DIR *d;
	struct dirent *e;
	struct stat st;
	char child_abs[PATH_MAX], child_rel[512];
	int n;

	if (!(d = opendir(abs)))
		return;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		if (join_path(child_abs, sizeof(child_abs), abs, e->d_name) < 0)
			continue;
		/* a name that does not fit would be truncated into a name for
		 * something else, so it is left out entirely */
		if (*rel)
			n = snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, e->d_name);
		else
			n = snprintf(child_rel, sizeof(child_rel), "%s", e->d_name);
		if (n < 0 || (size_t)n >= sizeof(child_rel))
			continue;
		if (lstat(child_abs, &st) < 0)
			continue;
		if (S_ISLNK(st.st_mode)) {
			/* a link that leaves the tree would make whatever it
			 * points at readable and writable through the page */
			char real[PATH_MAX];
			size_t rl = strlen(repo_root);

			if (!realpath(child_abs, real) ||
			    strncmp(real, repo_root, rl) ||
			    (real[rl] && real[rl] != '/'))
				continue;
			if (stat(child_abs, &st) < 0)
				continue;
		}
		if (S_ISDIR(st.st_mode)) {
			add_node(child_rel, e->d_name, T_DIR);
			scan(child_abs, child_rel);
		} else if (S_ISREG(st.st_mode)) {
			int ni = add_node(child_rel, e->d_name,
			    has_suffix(e->d_name, ".md") ? T_NOTE : T_FILE);
			if (ni >= 0) {
				nodes[ni].size = (long)st.st_size;
				nodes[ni].mtime = (long)st.st_mtime;
			}
		}
	}
	closedir(d);
}

/* Resolution order, matching the plan: exact path, then `.md` appended, then
 * directory. No global name lookup, so no index is needed. */
static int
resolve(const char *target, const char *from_dir)
{
	/* Wide enough for the longest join a caller can hand over — a full
	 * node path, a separator and a full link target — so the candidate is
	 * always formed whole. Anything that long matches no node anyway; the
	 * point is that it fails by lookup rather than by truncation. */
	char buf[sizeof(nodes[0].path) + sizeof(edges[0].raw) + 1];
	int i;

	if (!strncmp(target, "./", 2)) {
		if (*from_dir)
			snprintf(buf, sizeof(buf), "%s/%s", from_dir, target + 2);
		else
			snprintf(buf, sizeof(buf), "%s", target + 2);
	} else {
		snprintf(buf, sizeof(buf), "%s", target);
	}
	if ((i = find_node(buf)) >= 0)
		return i;

	{
		char md[sizeof(buf) + 3];

		snprintf(md, sizeof(md), "%s.md", buf);
		if ((i = find_node(md)) >= 0)
			return i;
	}
	return -1;
}

static char *
slurp(const char *abs, size_t *len)
{
	FILE *f;
	char *buf;
	long n;

	if (!(f = fopen(abs, "rb")))
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	if (n < 0 || n > MAX_BODY) {
		fclose(f);
		return NULL;
	}
	if (!(buf = malloc((size_t)n + 1))) {
		fclose(f);
		return NULL;
	}
	*len = fread(buf, 1, (size_t)n, f);
	buf[*len] = '\0';
	fclose(f);
	return buf;
}

/* Pull [[targets]] out of a note. An alias after `|` is display sugar; the
 * part before it is the path. */
static void
extract_links(int from)
{
	char abs[PATH_MAX], dir[512], target[256];
	char *src, *p, *q, *bar;
	size_t len, n;
	int to, mi, fenced = 0, ticks = 0;

	if (join_path(abs, sizeof(abs), repo_root, nodes[from].path) < 0)
		return;
	if (!(src = slurp(abs, &len)))
		return;

	snprintf(dir, sizeof(dir), "%s", nodes[from].path);
	if ((p = strrchr(dir, '/')))
		*p = '\0';
	else
		dir[0] = '\0';

	/* Walk the text tracking code context, so a `[[link]]` shown as an
	 * example — as the notes about this very syntax do — is not mistaken
	 * for a reference. */
	for (p = src; *p; p++) {
		if (p == src || p[-1] == '\n') {
			if (!strncmp(p, "```", 3)) {
				fenced = !fenced;
				p += 2;
				continue;
			}
			ticks = 0;
		}
		if (*p == '`')
			ticks++;
		if (fenced || (ticks & 1))
			continue;
		if (p[0] != '[' || p[1] != '[')
			continue;
		if (!(q = strstr(p + 2, "]]")))
			break;
		n = (size_t)(q - (p + 2));
		if (n == 0 || n >= sizeof(target))
			continue;
		memcpy(target, p + 2, n);
		target[n] = '\0';
		if ((bar = strchr(target, '|')))
			*bar = '\0';
		while (n && (target[n - 1] == ' ' || target[n - 1] == '\t'))
			target[--n] = '\0';
		if (!*target)
			continue;

		if (nedges >= MAX_EDGES)
			break;
		to = resolve(target, dir);
		if (to < 0) {
			/* Surface the gap rather than dropping the link. An
			 * unresolved link is not an error, but it is worth
			 * seeing. */
			mi = add_node(target, target, T_MISSING);
			if (mi < 0)
				continue;
			to = mi;
		}
		edges[nedges].s = from;
		edges[nedges].t = to;
		snprintf(edges[nedges].raw, sizeof(edges[nedges].raw), "%s", target);
		nedges++;
		p = q + 1;
	}
	free(src);
}

static void
build(void)
{
	int i, n;

	nnodes = nedges = 0;
	add_node("", "", T_DIR);
	scan(repo_root, "");
	n = nnodes;			/* missing nodes get appended below */
	for (i = 0; i < n; i++)
		if (nodes[i].type == T_NOTE)
			extract_links(i);
}

/* ---- http ---- */

static void
json_str(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			fprintf(f, "\\%c", *s);
		else if ((unsigned char)*s < 0x20)
			fprintf(f, "\\u%04x", *s);
		else
			fputc(*s, f);
	}
	fputc('"', f);
}

/* extra: further header lines, each ending in \r\n, or "". */
static void
send_head_x(FILE *f, const char *status, const char *type, size_t len,
    const char *extra)
{
	fprintf(f,
	    "HTTP/1.1 %s\r\n"
	    "Content-Type: %s\r\n"
	    "Content-Length: %zu\r\n"
	    "Cache-Control: no-store\r\n"
	    "X-Content-Type-Options: nosniff\r\n"
	    "%s"
	    "Connection: close\r\n\r\n",
	    status, type, len, extra);
}

static void
send_head(FILE *f, const char *status, const char *type, size_t len)
{
	send_head_x(f, status, type, len, "");
}

/* The page itself. Scripts and styles are inline, so those stay open; the
 * rest is pinned to this origin, nothing may frame it, and no referrer
 * leaks a repository path to a site a note links to. */
#define PAGE_HEADERS \
	"Content-Security-Policy: default-src 'self'; " \
	    "script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; " \
	    "img-src 'self' data: http: https:; frame-src 'self'; " \
	    "connect-src 'self'; object-src 'none'; base-uri 'none'; " \
	    "form-action 'none'; frame-ancestors 'none'\r\n" \
	"Referrer-Policy: no-referrer\r\n" \
	"X-Frame-Options: DENY\r\n"

static void
send_text(FILE *f, const char *status, const char *type, const char *body, size_t len)
{
	send_head(f, status, type, len);
	fwrite(body, 1, len, f);
}

static void
send_graph(FILE *f)
{
	char *buf;
	size_t len;
	FILE *m;
	int i;

	if (!(m = open_memstream(&buf, &len)))
		return;
	fputs("{\"root\":", m);
	json_str(m, repo_root);
	fputs(",\"nodes\":[", m);
	for (i = 0; i < nnodes; i++) {
		if (i)
			fputc(',', m);
		fputs("{\"path\":", m);
		json_str(m, nodes[i].path);
		fputs(",\"name\":", m);
		json_str(m, nodes[i].name);
		fprintf(m, ",\"type\":\"%s\",\"size\":%ld,\"mtime\":%ld}",
		    type_name(nodes[i].type), nodes[i].size, nodes[i].mtime);
	}
	fputs("],\"edges\":[", m);
	for (i = 0; i < nedges; i++) {
		if (i)
			fputc(',', m);
		fprintf(m, "{\"s\":%d,\"t\":%d,\"raw\":", edges[i].s,
		    nodes[edges[i].t].type == T_MISSING ? -1 : edges[i].t);
		json_str(m, edges[i].raw);
		fputc('}', m);
	}
	fputs("]}", m);
	fclose(m);

	send_text(f, "200 OK", "application/json", buf, len);
	free(buf);
}

/* Case-insensitive substring search across note contents. Grep, essentially:
 * no index, no ranking, no stemming — the tree is small and the filesystem is
 * fast enough that pretending otherwise would only add ways to be wrong. */
static void
send_search(FILE *f, const char *q)
{
	char abs[PATH_MAX], *buf, *m;
	char *line, *next, lower[1024], needle[256];
	size_t len, i, n;
	char *json;
	size_t jlen;
	FILE *o;
	int node, lineno, hits = 0;

	for (i = 0; q[i] && i < sizeof(needle) - 1; i++)
		needle[i] = (char)tolower((unsigned char)q[i]);
	needle[i] = '\0';

	if (!(o = open_memstream(&json, &jlen)))
		return;
	fputs("{\"hits\":[", o);

	for (node = 0; node < nnodes && hits < 200; node++) {
		if (nodes[node].type != T_NOTE)
			continue;
		if (join_path(abs, sizeof(abs), repo_root, nodes[node].path) < 0)
			continue;
		if (!(buf = slurp(abs, &len)))
			continue;
		lineno = 0;
		for (line = buf; line && *line; line = next) {
			if ((next = strchr(line, '\n')))
				*next++ = '\0';
			lineno++;
			n = strlen(line);
			if (n >= sizeof(lower))
				n = sizeof(lower) - 1;
			for (i = 0; i < n; i++)
				lower[i] = (char)tolower((unsigned char)line[i]);
			lower[n] = '\0';
			if (!(m = strstr(lower, needle)))
				continue;
			if (hits++)
				fputc(',', o);
			fputs("{\"path\":", o);
			json_str(o, nodes[node].path);
			fprintf(o, ",\"line\":%d,\"text\":", lineno);
			json_str(o, line);
			fputc('}', o);
			if (hits >= 200)
				break;
		}
		free(buf);
	}
	fprintf(o, "],\"n\":%d}", hits);
	fclose(o);
	send_text(f, "200 OK", "application/json", json, jlen);
	free(json);
}

static int
hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* In place; the result is never longer than the input. Only two real hex
 * digits make an escape — sscanf's %x would also take a sign or a space. */
static void
url_decode(char *s)
{
	char *o = s;
	int hi, lo;

	for (; *s; s++) {
		if (*s == '%' && (hi = hexval(s[1])) >= 0 && (lo = hexval(s[2])) >= 0) {
			*o++ = (char)(hi << 4 | lo);
			s += 2;
		} else if (*s == '+') {
			*o++ = ' ';
		} else {
			*o++ = *s;
		}
	}
	*o = '\0';
}

/* Only paths that turned up in the scan are readable, which rules out
 * traversal without any string surgery. */
static void
send_file(FILE *f, const char *rel)
{
	char abs[PATH_MAX];
	char *body;
	size_t len;
	int i;

	i = find_node(rel);
	if (i < 0 || nodes[i].type == T_DIR || nodes[i].type == T_MISSING) {
		send_text(f, "404 Not Found", "text/plain", "not found", 9);
		return;
	}
	if (join_path(abs, sizeof(abs), repo_root, rel) < 0 ||
	    !(body = slurp(abs, &len))) {
		send_text(f, "404 Not Found", "text/plain", "not found", 9);
		return;
	}
	send_text(f, "200 OK", "text/plain; charset=utf-8", body, len);
	free(body);
}

/* Write a note back to disk.
 *
 * Three guards, none of them optional:
 *   - the path must already exist in the scan, which rules out traversal and
 *     creation by the same argument that protects reads;
 *   - the caller must present the mtime it loaded, so a note changed on disk
 *     since — over SMB, by an editor, by anything — is refused rather than
 *     silently overwritten;
 *   - the new content lands in a temporary file and is renamed into place, so
 *     a failure part way through cannot leave a truncated note.
 */
static void
write_file(FILE *f, const char *rel, long expect_mtime, const char *body, size_t len)
{
	char abs[PATH_MAX], tmp[PATH_MAX];
	struct stat st;
	FILE *o;
	int i;

	i = find_node(rel);
	if (i < 0 || (nodes[i].type != T_NOTE && nodes[i].type != T_FILE)) {
		send_text(f, "404 Not Found", "text/plain", "not found", 9);
		return;
	}
	if (join_path(abs, sizeof(abs), repo_root, rel) < 0) {
		send_text(f, "400 Bad Request", "text/plain", "bad path", 8);
		return;
	}
	if (stat(abs, &st) < 0) {
		send_text(f, "404 Not Found", "text/plain", "not found", 9);
		return;
	}
	if (expect_mtime && (long)st.st_mtime != expect_mtime) {
		send_text(f, "409 Conflict", "text/plain",
		    "changed on disk since loaded", 28);
		return;
	}

	if ((size_t)snprintf(tmp, sizeof(tmp), "%s.graph-tmp", abs) >= sizeof(tmp)) {
		send_text(f, "400 Bad Request", "text/plain", "bad path", 8);
		return;
	}
	if (!(o = fopen(tmp, "wb"))) {
		send_text(f, "500 Internal Server Error", "text/plain", "cannot write", 12);
		return;
	}
	if (fwrite(body, 1, len, o) != len || fclose(o) != 0) {
		unlink(tmp);
		send_text(f, "500 Internal Server Error", "text/plain", "cannot write", 12);
		return;
	}
	if (rename(tmp, abs) < 0) {
		unlink(tmp);
		send_text(f, "500 Internal Server Error", "text/plain", "cannot replace", 14);
		return;
	}
	if (stat(abs, &st) == 0) {
		char out[64];
		int n = snprintf(out, sizeof(out), "{\"mtime\":%ld,\"size\":%ld}",
		    (long)st.st_mtime, (long)st.st_size);
		send_text(f, "200 OK", "application/json", out, (size_t)n);
	} else {
		send_text(f, "200 OK", "application/json", "{}", 2);
	}
}

static const char *
mime_of(const char *path)
{
	const char *d = strrchr(path, '.');

	if (!d)
		return "application/octet-stream";
	if (!strcasecmp(d, ".png"))  return "image/png";
	if (!strcasecmp(d, ".jpg") || !strcasecmp(d, ".jpeg")) return "image/jpeg";
	if (!strcasecmp(d, ".gif"))  return "image/gif";
	if (!strcasecmp(d, ".webp")) return "image/webp";
	if (!strcasecmp(d, ".svg"))  return "image/svg+xml";
	if (!strcasecmp(d, ".pdf"))  return "application/pdf";
	if (!strcasecmp(d, ".csv"))  return "text/csv; charset=utf-8";
	if (!strcasecmp(d, ".txt") || !strcasecmp(d, ".md"))
		return "text/plain; charset=utf-8";
	return "application/octet-stream";
}

/* Serve a file with its real content type so the browser can display it in
 * place. Same guard as every other read: only paths the scan already found. */
static void
send_raw(FILE *f, const char *rel)
{
	char abs[PATH_MAX];
	char *body;
	size_t len;
	int i;

	i = find_node(rel);
	if (i < 0 || nodes[i].type == T_DIR || nodes[i].type == T_MISSING) {
		send_text(f, "404 Not Found", "text/plain", "not found", 9);
		return;
	}
	if (join_path(abs, sizeof(abs), repo_root, rel) < 0 ||
	    !(body = slurp(abs, &len))) {
		send_text(f, "404 Not Found", "text/plain", "not found", 9);
		return;
	}
	/* An SVG opened by itself is a document that may run script; the
	 * sandbox drops it into an opaque origin, where it can see nothing
	 * of this one. As an <img> it never ran script anyway. */
	send_head_x(f, "200 OK", mime_of(rel), len,
	    strstr(mime_of(rel), "svg") ?
	    "Content-Security-Policy: sandbox\r\n" : "");
	fwrite(body, 1, len, f);
	free(body);
}

/* Hand a file to whatever the desktop opens it with, or a directory to the
 * file manager. Same rules as a write: only a path the scan knows, and
 * never something the opener would run rather than show. Safety is an
 * allow-list — a file opens only when its extension is a known viewer
 * type, so a file whose type we don't recognise (or that carries no
 * extension) is an answer of no, not a guess. The opener runs detached
 * with its output discarded, so it can neither block nor talk back into
 * this connection. */
static int
viewer_ext(const char *name)
{
	static const char *const ok[] = {
		/* images */
		".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".tif",
		".tiff", ".heic", ".heif", ".avif", ".ico",
		/* documents */
		".pdf", ".txt", ".text", ".md", ".markdown", ".rtf", ".csv",
		".tsv", ".log", ".json", ".xml", ".yaml", ".yml",
		/* office / iWork */
		".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx", ".odt",
		".ods", ".odp", ".pages", ".numbers", ".key",
		/* audio */
		".mp3", ".wav", ".flac", ".aac", ".m4a", ".ogg", ".oga",
		".opus", ".aiff",
		/* video */
		".mp4", ".mov", ".m4v", ".webm", ".mkv", ".avi", ".mpg",
		".mpeg",
		/* ebook */
		".epub", NULL
	};
	const char *d = strrchr(name, '.');
	int i;

	if (!d)
		return 0;
	for (i = 0; ok[i]; i++)
		if (!strcasecmp(d, ok[i]))
			return 1;
	return 0;
}

static void
open_path(FILE *f, const char *rel)
{
	char abs[PATH_MAX];
	struct stat st;
	pid_t pid;
	int i;

	i = find_node(rel);
	if (i < 0 || nodes[i].type == T_MISSING) {
		send_text(f, "404 Not Found", "text/plain", "not found", 9);
		return;
	}
	if (join_path(abs, sizeof(abs), repo_root, rel) < 0 || stat(abs, &st) < 0) {
		send_text(f, "404 Not Found", "text/plain", "not found", 9);
		return;
	}
	if (nodes[i].type != T_DIR &&
	    ((st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) || !viewer_ext(rel))) {
		send_text(f, "403 Forbidden", "text/plain",
		    "will not open this file type", 28);
		return;
	}
	if (!have_display()) {
		send_text(f, "501 Not Implemented", "text/plain", "no desktop here", 15);
		return;
	}
	if ((pid = fork()) < 0) {
		send_text(f, "500 Internal Server Error", "text/plain", "cannot open", 11);
		return;
	}
	if (pid == 0) {
		int nul = open("/dev/null", O_RDWR);

		if (fork() > 0)
			_exit(0);
		setsid();
		if (nul >= 0) {
			dup2(nul, 0); dup2(nul, 1); dup2(nul, 2);
			if (nul > 2)
				close(nul);
		}
#ifdef __APPLE__
		execlp("open", "open", abs, (char *)NULL);
#else
		execlp("xdg-open", "xdg-open", abs, (char *)NULL);
#endif
		_exit(127);
	}
	waitpid(pid, NULL, 0);
	send_text(f, "200 OK", "application/json", "{\"ok\":true}", 11);
}

/* ---- access ----
 *
 * The server listens on loopback only, but loopback is shared with every
 * other user and program on the machine, and a web page can reach it too.
 * Two checks stand in for the login there is no room for:
 *
 *   - the Host header must name this machine, so a page whose hostname was
 *     pointed at 127.0.0.1 (DNS rebinding) is turned away — as far as the
 *     browser knew, that page was talking to itself;
 *   - every request must carry the token minted at startup, first in the
 *     URL this program prints and opens, then in a cookie it sets from that
 *     visit. Anyone who did not see the URL does not get in.
 *
 * Writes need the custom header on top, which the browser only sends from
 * this origin. */

static void
gen_token(void)
{
	static const char hex[] = "0123456789abcdef";
	unsigned char b[16];
	FILE *r;
	int i;

	if (!(r = fopen("/dev/urandom", "rb")) || fread(b, 1, sizeof(b), r) != sizeof(b))
		die("cannot read /dev/urandom");
	fclose(r);
	for (i = 0; i < 16; i++) {
		token[2 * i] = hex[b[i] >> 4];
		token[2 * i + 1] = hex[b[i] & 15];
	}
	token[32] = '\0';
}

/* Same length and content, in time that does not depend on where they
 * differ, so the token cannot be guessed one character at a time. */
static int
token_ok(const char *s, size_t n)
{
	unsigned d = 0;
	size_t i;

	if (n != 32)
		return 0;
	for (i = 0; i < 32; i++)
		d |= (unsigned char)s[i] ^ (unsigned char)token[i];
	return d == 0;
}

/* "127.0.0.1", "localhost" or "[::1]", with this port or none at all. */
static int
host_ok(const char *h)
{
	const char *colon;
	size_t n;

	while (*h == ' ' || *h == '\t')
		h++;
	n = strcspn(h, " \t\r\n");
	colon = h[0] == '[' ? strchr(h, ']') : NULL;
	colon = colon ? (colon[1] == ':' ? colon + 1 : NULL) : memchr(h, ':', n);
	if (colon) {
		if (atoi(colon + 1) != srv_port)
			return 0;
		n = (size_t)(colon - h);
	}
	return (n == 9 && !strncmp(h, "127.0.0.1", 9)) ||
	    (n == 9 && !strncasecmp(h, "localhost", 9)) ||
	    (n == 5 && !strncmp(h, "[::1]", 5));
}

/* Look for graph_<port>=<token> among the cookies. The name carries the
 * port because cookies ignore ports: several repositories open at once
 * are several servers on one host, each with a token of its own. */
static int
cookie_ok(const char *c)
{
	char name[32];
	size_t nl;
	const char *p;

	nl = (size_t)snprintf(name, sizeof(name), "graph_%d=", srv_port);
	for (p = c; (p = strstr(p, name)); p += nl) {
		if (p != c && p[-1] != ' ' && p[-1] != ';')
			continue;
		if (token_ok(p + nl, strcspn(p + nl, "; \r\n")))
			return 1;
	}
	return 0;
}

static void
handle(int fd)
{
	FILE *f;
	char line[2048], *path, *end, *query, *body = NULL;
	long clen = 0, want_mtime = 0;
	int is_put = 0, guarded = 0, method = 0, host = 0, cookie = 0;
	enum { M_GET = 1, M_PUT, M_POST };
	size_t got = 0;
	struct timeval tv = { 15, 0 };

	/* one connection at a time is served, so one that stalls — a client
	 * that never finishes its request, or never reads the answer — must
	 * not be allowed to hold everyone else */
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	if (!(f = fdopen(fd, "r+"))) {
		close(fd);
		return;
	}
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return;
	}
	if (!strchr(line, '\n')) {
		send_text(f, "414 URI Too Long", "text/plain", "too long", 8);
		fclose(f);
		return;
	}
	if (!strncmp(line, "PUT ", 4)) {
		is_put = 1;
		method = M_PUT;
		path = line + 4;
	} else if (!strncmp(line, "GET ", 4)) {
		method = M_GET;
		path = line + 4;
	} else if (!strncmp(line, "POST ", 5)) {
		method = M_POST;
		path = line + 5;
	} else {
		send_text(f, "405 Method Not Allowed", "text/plain", "unsupported", 11);
		fclose(f);
		return;
	}
	if (!(end = strchr(path, ' '))) {
		fclose(f);
		return;
	}
	*end = '\0';
	if ((query = strchr(path, '?')))
		*query++ = '\0';

	{
		char hdr[2048];

		while (fgets(hdr, sizeof(hdr), f)) {
			if (!strcmp(hdr, "\r\n") || !strcmp(hdr, "\n"))
				break;
			if (!strncasecmp(hdr, "Content-Length:", 15))
				clen = atol(hdr + 15);
			else if (!strncasecmp(hdr, "Host:", 5))
				host = host_ok(hdr + 5);
			else if (!strncasecmp(hdr, "Cookie:", 7))
				cookie = cookie_ok(hdr + 7);
			/* A custom header cannot be sent cross-origin without a
			 * preflight this server never answers, so requiring one
			 * keeps another page on the machine from writing here. */
			else if (!strncasecmp(hdr, "X-Graph-Write:", 14))
				guarded = 1;
		}
	}
	if (!host) {
		send_text(f, "421 Misdirected Request", "text/plain", "wrong host", 10);
		fclose(f);
		return;
	}

	/* the front door: the printed URL sets the cookie and moves on */
	if (method == M_GET && !strcmp(path, "/") && query &&
	    !strncmp(query, "token=", 6)) {
		if (token_ok(query + 6, strcspn(query + 6, "&"))) {
			char extra[160];

			snprintf(extra, sizeof(extra),
			    "Location: /\r\n"
			    "Set-Cookie: graph_%d=%s; Path=/; HttpOnly; SameSite=Strict\r\n",
			    srv_port, token);
			send_head_x(f, "303 See Other", "text/plain", 0, extra);
		} else {
			send_text(f, "403 Forbidden", "text/plain", "bad token", 9);
		}
		fclose(f);
		return;
	}
	if (!cookie) {
		static const char msg[] =
		    "graph: not signed in — open the address that graph display printed\n";
		send_text(f, "401 Unauthorized", "text/plain; charset=utf-8", msg,
		    sizeof(msg) - 1);
		fclose(f);
		return;
	}

	if (method != M_GET) {
		if (!guarded) {
			send_text(f, "403 Forbidden", "text/plain", "missing write header", 20);
			fclose(f);
			return;
		}
		if (clen < 0 || clen > MAX_BODY) {
			send_text(f, "413 Payload Too Large", "text/plain", "too large", 9);
			fclose(f);
			return;
		}
		if (is_put) {
			if (!(body = malloc((size_t)clen + 1))) {
				fclose(f);
				return;
			}
			got = fread(body, 1, (size_t)clen, f);
			body[got] = '\0';
			if (got != (size_t)clen) {
				/* a body cut short must not become the note */
				send_text(f, "400 Bad Request", "text/plain", "short body", 10);
				free(body);
				fclose(f);
				return;
			}
		}
	}

	/* Every request rescans: the filesystem is the source of truth, and at
	 * this scale a walk is cheaper than any cache would be to keep honest. */
	build();

	if (method == M_POST) {
		if (!strcmp(path, "/api/open") && query && !strncmp(query, "path=", 5)) {
			url_decode(query + 5);
			open_path(f, query + 5);
		} else {
			send_text(f, "404 Not Found", "text/plain", "not found", 9);
		}
		fclose(f);
		return;
	}

	if (is_put) {
		if (!strcmp(path, "/api/file") && query && !strncmp(query, "path=", 5)) {
			char *amp = strchr(query, '&');
			if (amp) {
				*amp = '\0';
				if (!strncmp(amp + 1, "mtime=", 6))
					want_mtime = atol(amp + 7);
			}
			url_decode(query + 5);
			write_file(f, query + 5, want_mtime, body, got);
		} else {
			send_text(f, "404 Not Found", "text/plain", "not found", 9);
		}
		free(body);
		fclose(f);
		return;
	}

	if (!strcmp(path, "/")) {
		/* Embedded as unsigned bytes; it is text all the same. */
		const char *html = (const char *)ui_html;
		size_t n = strlen(html);

		send_head_x(f, "200 OK", "text/html; charset=utf-8",
		    n, PAGE_HEADERS);
		fwrite(html, 1, n, f);
	} else if (!strcmp(path, "/api/graph")) {
		send_graph(f);
	} else if (!strcmp(path, "/api/search")) {
		if (query && !strncmp(query, "q=", 2)) {
			url_decode(query + 2);
			send_search(f, query + 2);
		} else {
			send_text(f, "400 Bad Request", "text/plain", "no query", 8);
		}
	} else if (!strcmp(path, "/api/raw")) {
		if (query && !strncmp(query, "path=", 5)) {
			url_decode(query + 5);
			send_raw(f, query + 5);
		} else {
			send_text(f, "400 Bad Request", "text/plain", "no path", 7);
		}
	} else if (!strcmp(path, "/api/file")) {
		if (query && !strncmp(query, "path=", 5)) {
			url_decode(query + 5);
			send_file(f, query + 5);
		} else {
			send_text(f, "400 Bad Request", "text/plain", "no path", 7);
		}
	} else {
		send_text(f, "404 Not Found", "text/plain", "not found", 9);
	}
	fclose(f);
}

/* Bind the loopback port. With tries > 1 a busy port is not an error: walk
 * upwards until one is free, and report which in *port. An explicit --port
 * passes tries = 1, since a port someone asked for by number should not be
 * quietly swapped for another. */
static int
listen_local(int *port, int tries)
{
	struct sockaddr_in a;
	int fd, yes = 1, p = *port, last = p + tries - 1;

	if (last > 65535)
		last = 65535;
	for (;;) {
		if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
			die("socket: %s", strerror(errno));
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

		memset(&a, 0, sizeof(a));
		a.sin_family = AF_INET;
		a.sin_port = htons((unsigned short)p);
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

		if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0)
			break;
		if (errno != EADDRINUSE || p >= last) {
			if (tries > 1)
				die("cannot bind 127.0.0.1:%d-%d: %s", *port, last,
				    strerror(errno));
			die("cannot bind 127.0.0.1:%d: %s", p, strerror(errno));
		}
		close(fd);
		p++;
	}
	if (listen(fd, 16) < 0)
		die("listen: %s", strerror(errno));
	*port = p;
	return fd;
}

/* Run a command to completion and return its exit status, 127 if it could
 * not be started. Used from the detached opener, so blocking is fine. */
static int
run(char *const argv[])
{
	pid_t pid;
	int st;

	if ((pid = fork()) < 0)
		return 127;
	if (pid == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}
	if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st))
		return 127;
	return WEXITSTATUS(st);
}

/* ---- desktop ----
 *
 * The server is the application; what shows it is a detail. In order of
 * preference: the native shell built alongside this binary (Graph.app on
 * macOS, graph-shell on Linux), a Chromium-family browser opened chromeless
 * with --app, the ordinary opener. $BROWSER set means "a browser, this one",
 * skipping the shell as well.
 *
 * With a shell the roles invert: `graph display <path>` only hands the
 * repository to the shell and returns, and the shell runs one server per
 * open repository — its tabs — with --no-open, reading the URL this prints. */

/* True when there is a desktop to draw on at all. */
int
have_display(void)
{
#ifdef __APPLE__
	return 1;
#else
	return getenv("DISPLAY") || getenv("WAYLAND_DISPLAY");
#endif
}

/* Locate the native shell: beside this binary in a build tree or bundle, or
 * under ../libexec/graph next to an installed bin/graph. NULL if absent. */
static const char *
find_shell(char *buf, size_t n)
{
	static const char *const rel[] = {
#ifdef __APPLE__
		"../libexec/graph/Graph.app/Contents/MacOS/graph-shell",
		"Graph.app/Contents/MacOS/graph-shell",
#else
		"graph-shell", "../libexec/graph/graph-shell",
#endif
		NULL
	};
	char exe[PATH_MAX], dir[PATH_MAX], *slash;
	size_t i, n2;
#ifdef __APPLE__
	uint32_t sz = sizeof(exe);

	if (_NSGetExecutablePath(exe, &sz) != 0)
		return NULL;
#else
	ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);

	if (len < 0)
		return NULL;
	exe[len] = '\0';
#endif
	if (!realpath(exe, dir) || !(slash = strrchr(dir, '/')))
		return NULL;
	*slash = '\0';
#ifdef __APPLE__
	/* Inside the bundle, the shell is our sibling; the Linux name is
	 * reused there so it can never collide with graph itself on a
	 * case-insensitive disk. */
	n2 = strlen(dir);
	if (n2 > 15 && !strcmp(dir + n2 - 15, "/Contents/MacOS") &&
	    join_path(buf, n, dir, "graph-shell") == 0 && access(buf, X_OK) == 0)
		return buf;
#else
	(void)n2;
#endif
	for (i = 0; rel[i]; i++) {
		if (join_path(buf, n, dir, rel[i]) < 0)
			continue;
		if (access(buf, X_OK) == 0)
			return buf;
	}
	return NULL;
}

/* Give the shell a repository to show, or with dir NULL just bring it up
 * (it restores its last session or asks). Returns 0 if the shell could not
 * be started, in which case the caller falls back to serving here. */
static int
hand_to_shell(const char *shell, const char *dir)
{
#ifdef __APPLE__
	/* The shell lives in a bundle: go through open(1) so a running
	 * instance receives the folder as a new tab instead of a second
	 * application starting. The bundle is three levels up. */
	char bundle[PATH_MAX], *p;
	int i;

	snprintf(bundle, sizeof(bundle), "%s", shell);
	for (i = 0; i < 3; i++) {
		if (!(p = strrchr(bundle, '/')))
			return 0;
		*p = '\0';
	}
	{
		char *const argv[] = { "open", "-a", bundle, (char *)dir, NULL };
		return run(argv) == 0;
	}
#else
	/* Detached, so the terminal is free and its closing does not take the
	 * window along. GtkApplication makes a second launch forward to the
	 * first and exit 0, so a quick exit is only a failure when the status
	 * says so: 127 for a shell that could not be run at all, 1 for one that
	 * ran and found DISPLAY pointing at nothing. The brief look afterwards
	 * catches both, and the browser takes over. */
	struct timespec ts = { 0, 200 * 1000 * 1000 };
	pid_t pid;
	int st;

	if ((pid = fork()) < 0)
		return 0;
	if (pid == 0) {
		int nul = open("/dev/null", O_RDWR);

		setsid();
		if (nul >= 0) {
			dup2(nul, 0); dup2(nul, 1); dup2(nul, 2);
			if (nul > 2)
				close(nul);
		}
		execl(shell, shell, dir, (char *)NULL);
		_exit(127);
	}
	nanosleep(&ts, NULL);
	if (waitpid(pid, &st, WNOHANG) == pid && WIFEXITED(st) &&
	    WEXITSTATUS(st) != 0)
		return 0;
	return 1;
#endif
}

/* Every URL handed around below is a loopback address carrying the run
 * token, so one size covers them all. */
#define URL_MAX 96

/* Hand the URL to a browser from a detached child. Chromium-family browsers
 * open a URL chromeless with --app, which lives exactly as long as this
 * server does and leaves nothing installed behind. Without one of those the
 * URL goes to the ordinary opener.
 *
 * Best effort: no browser or a failing one must not take the server down, so
 * every error is swallowed and the URL is printed regardless. */
static void
open_browser(const char *url)
{
	const char *cmd;
	char app[sizeof "--app=" + URL_MAX];
	pid_t pid;
	size_t i;
#ifdef __APPLE__
	static const char *const ids[] = {
		"com.google.Chrome", "org.chromium.Chromium",
		"com.brave.Browser", "com.microsoft.edgemac", NULL
	};
	const char *fallback = "open";
#else
	static const char *const ids[] = {
		"google-chrome", "google-chrome-stable", "chromium",
		"chromium-browser", "brave-browser", "microsoft-edge", NULL
	};
	const char *fallback = "xdg-open";
#endif

	if ((cmd = getenv("BROWSER")) && !*cmd)
		cmd = NULL;
	if ((pid = fork()) < 0)
		return;
	if (pid > 0) {
		waitpid(pid, NULL, 0);	/* the grandchild is what runs */
		return;
	}
	if (fork() > 0)
		_exit(0);
	{
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) {
			dup2(nul, 0); dup2(nul, 1); dup2(nul, 2);
			if (nul > 2)
				close(nul);
		}
	}
	if (cmd) {
		execlp(cmd, cmd, url, (char *)NULL);
		_exit(127);
	}
	snprintf(app, sizeof(app), "--app=%s", url);
	for (i = 0; ids[i]; i++) {
#ifdef __APPLE__
		/* -n: a fresh process even if the browser is running; it hands
		 * the window to the running one and exits. -b: by bundle id, so
		 * a missing browser is a clean failure rather than a dialog. */
		char *const argv[] = { "open", "-nb", (char *)ids[i], "--args",
		    app, NULL };
		if (run(argv) == 0)
			_exit(0);
#else
		/* A browser that is not running stays in the foreground, so
		 * this returns only once its window closes. Whatever it exits
		 * with then, it did open; only "not found" moves on. */
		char *const argv[] = { (char *)ids[i], app, NULL };
		if (run(argv) != 127)
			_exit(0);
#endif
	}
	execlp(fallback, fallback, url, (char *)NULL);
	_exit(127);
}

/* Without a path, the repository is the one the working directory sits in,
 * found the way git finds its own; walking up stops at the filesystem root. */
static int
repo_above(char *buf, size_t n)
{
	char *slash;

	if (!getcwd(buf, n))
		return 0;
	for (;;) {
		if (is_graph_repo(buf))
			return 1;
		if (!(slash = strrchr(buf, '/')))
			return 0;
		if (slash == buf) {
			if (buf[1] == '\0')
				return 0;
			buf[1] = '\0';	/* "/" itself, checked once more */
		} else {
			*slash = '\0';
		}
	}
}

int
cmd_display(int argc, char *argv[])
{
	const char *path = NULL, *shell;
	char url[URL_MAX], shellbuf[PATH_MAX];
	int port = 7373, i, srv, fd, notes, launch = 1, tries = 20, bad = 0;
	int follow = 0;
	pid_t parent = getppid();

	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--port") && i + 1 < argc) {
			port = atoi(argv[++i]);
			tries = 1;
		}
		else if (!strcmp(argv[i], "--no-open"))
			launch = 0;
		else if (!strcmp(argv[i], "--exit-with-parent"))
			follow = 1;	/* for the shell: die with it, quietly */
		else if (argv[i][0] == '-' || path)
			bad = 1;
		else
			path = argv[i];
	}
	if (bad || port <= 0 || port > 65535) {
		fputs("usage: graph display [path] [--port <port>] [--no-open]\n", stderr);
		return 1;
	}

	/* A browser wants a browser, and a port asked for by number wants a
	 * server here; otherwise the shell, when there is one and a desktop
	 * to put it on. */
	shell = NULL;
	if (launch && tries > 1 && !getenv("BROWSER") && have_display())
		shell = find_shell(shellbuf, sizeof(shellbuf));

	if (path) {
		if (!realpath(path, repo_root))
			die("cannot resolve %s: %s", path, strerror(errno));
	} else if (repo_above(repo_root, sizeof(repo_root))) {
		;
	} else if (shell) {
		/* nothing to point at: the shell knows what was open last */
		if (hand_to_shell(shell, NULL))
			return 0;
		die("not inside a Graph repository; give a path");
	} else {
		die("not inside a Graph repository; give a path");
	}
	if (!is_graph_repo(repo_root))
		die("%s is not a Graph repository", repo_root);

	if (shell && hand_to_shell(shell, repo_root)) {
		printf("%s\n", repo_root);
		return 0;
	}

	signal(SIGPIPE, SIG_IGN);	/* a browser that walks away is not fatal */

	build();
	for (i = 0, notes = 0; i < nnodes; i++)
		if (nodes[i].type == T_NOTE)
			notes++;
	srv = listen_local(&port, tries);
	srv_port = port;
	gen_token();
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/?token=%s", port, token);
	printf("%s\n", repo_root);
	printf("%d notes, %d links — %s\n", notes, nedges, url);
	fflush(stdout);
	if (launch)
		open_browser(url);	/* only once we know the port is ours */

	for (;;) {
		struct pollfd pfd = { srv, POLLIN, 0 };
		int r = poll(&pfd, 1, follow ? 1000 : -1);

		if (follow && getppid() != parent)
			break;	/* the shell is gone; so is the reason to serve */
		if (r < 0 && errno != EINTR)
			break;
		if (r <= 0)
			continue;
		if ((fd = accept(srv, NULL, NULL)) < 0) {
			if (errno == EINTR || errno == ECONNABORTED)
				continue;
			break;
		}
		handle(fd);
	}
	close(srv);
	return 0;
}
