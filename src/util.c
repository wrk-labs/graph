/* See LICENSE file for copyright and license details. */

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "graph.h"

void
die(const char *fmt, ...)
{
	va_list ap;

	fputs("graph: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

void
warn(const char *fmt, ...)
{
	va_list ap;

	fputs("graph: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* Join base and name into dst. Returns 0 on success, -1 if the result would
 * not fit. */
int
join_path(char *dst, size_t size, const char *base, const char *name)
{
	int n;

	n = snprintf(dst, size, "%s/%s", base, name);
	if (n < 0 || (size_t)n >= size)
		return -1;
	return 0;
}

/* Returns 1 if path is an empty directory, 0 if it holds anything, -1 on
 * error. Any entry counts, including dotfiles: a directory carrying a stray
 * .DS_Store is not empty, and treating it as empty would risk writing into a
 * tree that already means something to someone. */
int
dir_is_empty(const char *path)
{
	DIR *d;
	struct dirent *e;
	int empty = 1;

	if (!(d = opendir(path)))
		return -1;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		empty = 0;
		break;
	}
	closedir(d);
	return empty;
}

/* Returns 1 if path carries the Graph marker. */
int
is_graph_repo(const char *path)
{
	char marker[PATH_MAX];
	char dir[PATH_MAX];
	struct stat st;

	if (join_path(dir, sizeof(dir), path, GRAPH_DIR) < 0)
		return 0;
	if (join_path(marker, sizeof(marker), dir, GRAPH_MARKER) < 0)
		return 0;
	return stat(marker, &st) == 0 && S_ISREG(st.st_mode);
}

/* Create path along with any missing parents, like `mkdir -p`. An existing
 * directory is not an error. */
int
mkdir_p(const char *path, mode_t mode)
{
	char buf[PATH_MAX];
	struct stat st;
	char *p;
	size_t len;

	len = strlen(path);
	if (len == 0 || len >= sizeof(buf)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(buf, path, len + 1);

	/* trailing slashes would produce an empty final component */
	while (len > 1 && buf[len - 1] == '/')
		buf[--len] = '\0';

	for (p = buf + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(buf, mode) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(buf, mode) < 0) {
		if (errno != EEXIST)
			return -1;
		if (stat(buf, &st) < 0)
			return -1;
		if (!S_ISDIR(st.st_mode)) {
			errno = ENOTDIR;
			return -1;
		}
	}
	return 0;
}
