/* graph-shell — the desktop face of graph on Linux.
 *
 * usage: graph-shell [dir ...]
 *
 * One tab per repository, each backed by its own `graph display --no-open`
 * server that this process starts and stops with the tab. The server is the
 * application; a tab is a pane of glass onto one of them.
 *
 * A GtkApplication with a fixed id, so a second launch hands its folders to
 * the running instance as new tabs and exits. Folders also come from the "+"
 * in the tab bar (a folder chooser) and the recents menu next to it. Started
 * with nothing, it reopens what was open last, or asks.
 *
 * State is two plain files under $XDG_CONFIG_HOME/graph: `session` (the open
 * tabs, in order) and `recent` (most recent first). */

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <signal.h>
#include <glib-unix.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "term.h"

#define MAX_RECENT 15

struct tab {
	char *dir;
	char *url;		/* as printed by the server, token and all */
	char *host;
	guint port;
	GPid pid;
	GtkWidget *page;	/* the web view; also the notebook child */
};

/* A shell tab: a web view running xterm.js over a pty. Not a struct tab —
 * it has no server, and it is not part of the session. */
struct term {
	char *dir;
	char *pending;		/* a command to run once the shell is up */
	char *branch;		/* last branch shown in the header */
	GtkWidget *page;
	int master;
	GPid pid;
	guint reader;		/* master fd source */
	guint watch;		/* child watch */
	GFileMonitor *headmon;	/* fires when .git/HEAD changes */
};

static GtkApplication *app;
static GtkWidget *win, *book;
static GList *tabs, *terms;
static gboolean quitting;

/* ---- state files ---- */

static char *
state_path(const char *name)
{
	char *dir = g_build_filename(g_get_user_config_dir(), "graph", NULL);
	char *p;

	g_mkdir_with_parents(dir, 0755);
	p = g_build_filename(dir, name, NULL);
	g_free(dir);
	return p;
}

/* NULL-terminated vector of non-empty lines; free with g_strfreev. */
static char **
read_lines(const char *name)
{
	char *p = state_path(name), *text = NULL, **raw, **out;
	int i, n = 0;

	g_file_get_contents(p, &text, NULL, NULL);
	g_free(p);
	raw = g_strsplit(text ? text : "", "\n", -1);
	g_free(text);
	out = g_new0(char *, g_strv_length(raw) + 1);
	for (i = 0; raw[i]; i++)
		if (*raw[i])
			out[n++] = g_strdup(raw[i]);
	g_strfreev(raw);
	return out;
}

static void
write_lines(const char *name, char **lines)
{
	char *p = state_path(name), *body = g_strjoinv("\n", lines);
	char *text = g_strconcat(body, lines[0] ? "\n" : "", NULL);

	g_file_set_contents(p, text, -1, NULL);
	g_free(text);
	g_free(body);
	g_free(p);
}

static gboolean
is_repo(const char *dir)
{
	char *m = g_build_filename(dir, ".graph", "repository", NULL);
	gboolean ok = g_file_test(m, G_FILE_TEST_IS_REGULAR);

	g_free(m);
	return ok;
}

/* The graph binary: beside this executable, else on $PATH. */
static char *
graph_binary(void)
{
	char exe[4096], *slash, *sib;
	ssize_t n;

	if ((n = readlink("/proc/self/exe", exe, sizeof(exe) - 1)) > 0) {
		exe[n] = '\0';
		if ((slash = strrchr(exe, '/'))) {
			*slash = '\0';
			sib = g_build_filename(exe, "graph", NULL);
			if (g_file_test(sib, G_FILE_TEST_IS_EXECUTABLE))
				return sib;
			g_free(sib);
		}
	}
	return g_find_program_in_path("graph");
}

/* ---- session and recents ---- */

static void
save_session(void)
{
	char **dirs;
	int i, n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(book));

	if (quitting)
		return;	/* the tabs closing now are the session to keep */
	dirs = g_new0(char *, n + 1);
	for (i = 0, n = 0; i < gtk_notebook_get_n_pages(GTK_NOTEBOOK(book)); i++) {
		GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(book), i);
		struct tab *t = g_object_get_data(G_OBJECT(page), "tab");
		if (t)	/* shell tabs are not kept */
			dirs[n++] = t->dir;
	}
	write_lines("session", dirs);
	g_free(dirs);
}

static void
remember(const char *dir)
{
	char **r = read_lines("recent"), **out;
	int i, n = 1;

	out = g_new0(char *, MAX_RECENT + 2);
	out[0] = g_strdup(dir);
	for (i = 0; r[i] && n < MAX_RECENT; i++)
		if (strcmp(r[i], dir))
			out[n++] = g_strdup(r[i]);
	write_lines("recent", out);
	g_strfreev(out);
	g_strfreev(r);
}

/* ---- tabs ---- */

static void open_dir(const char *dir);

static struct tab *
tab_for(const char *dir)
{
	GList *l;

	for (l = tabs; l; l = l->next)
		if (!strcmp(((struct tab *)l->data)->dir, dir))
			return l->data;
	return NULL;
}

static struct tab *
current_tab(void)
{
	int i = gtk_notebook_get_current_page(GTK_NOTEBOOK(book));
	GtkWidget *page;

	if (i < 0)
		return NULL;
	page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(book), i);
	return g_object_get_data(G_OBJECT(page), "tab");
}

static void
fail(const char *dir, const char *why)
{
	GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(win),
	    GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
	    "Cannot open %s", strrchr(dir, '/') ? strrchr(dir, '/') + 1 : dir);

	gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", why);
	gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
}

/* Start the server and wait for the line that names its URL. */
static gboolean
start_server(struct tab *t, char **err)
{
	char *graph = graph_binary();
	char *argv[6], *line = NULL, *http, *end;
	int out = -1, errfd = -1;
	GError *e = NULL;
	GString *buf = g_string_new(NULL);
	char chunk[512];
	ssize_t n;

	if (!graph) {
		*err = g_strdup("the graph command was not found");
		return FALSE;
	}
	argv[0] = graph;
	argv[1] = "display";
	argv[2] = "--no-open";
	argv[3] = "--exit-with-parent";
	argv[4] = t->dir;
	argv[5] = NULL;
	if (!g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
	    NULL, NULL, &t->pid, NULL, &out, &errfd, &e)) {
		*err = g_strdup(e ? e->message : "could not start graph");
		g_clear_error(&e);
		g_free(graph);
		return FALSE;
	}
	g_free(graph);
	while ((n = read(out, chunk, sizeof(chunk))) > 0) {
		g_string_append_len(buf, chunk, n);
		if ((http = strstr(buf->str, "http://")) && strchr(http, '\n')) {
			GUri *u;

			end = http + strcspn(http, " \t\r\n");
			line = g_strndup(http, end - http);
			if ((u = g_uri_parse(line, G_URI_FLAGS_NONE, NULL))) {
				t->url = g_strdup(line);
				t->host = g_strdup(g_uri_get_host(u));
				t->port = (guint)g_uri_get_port(u);
				g_uri_unref(u);
			}
			break;
		}
	}
	close(out);
	if (!line) {
		/* it exited: say why */
		g_string_truncate(buf, 0);
		while ((n = read(errfd, chunk, sizeof(chunk))) > 0)
			g_string_append_len(buf, chunk, n);
		close(errfd);
		g_spawn_close_pid(t->pid);
		t->pid = 0;
		*err = g_strdup(buf->len ? g_strstrip(buf->str) :
		    "graph exited without serving");
		g_string_free(buf, TRUE);
		return FALSE;
	}
	close(errfd);
	g_string_free(buf, TRUE);
	return TRUE;
}

static void
stop_server(struct tab *t)
{
	if (t->pid > 0) {
		kill(t->pid, SIGTERM);
		g_spawn_close_pid(t->pid);
		t->pid = 0;
	}
}

static void
close_tab(struct tab *t)
{
	int i = gtk_notebook_page_num(GTK_NOTEBOOK(book), t->page);

	stop_server(t);
	tabs = g_list_remove(tabs, t);
	if (i >= 0)
		gtk_notebook_remove_page(GTK_NOTEBOOK(book), i);
	g_free(t->dir);
	g_free(t->url);
	g_free(t->host);
	g_free(t);
	save_session();
	if (!tabs)
		gtk_widget_destroy(win);
}

static void
on_close_clicked(GtkButton *b, gpointer data)
{
	(void)b;
	close_tab(data);
}

static gboolean
is_home(struct tab *t, const char *uri)
{
	GUri *u = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
	gboolean same;

	if (!u)
		return FALSE;
	same = g_uri_get_host(u) && t->host && !strcmp(g_uri_get_host(u), t->host) &&
	    (guint)g_uri_get_port(u) == t->port;
	g_uri_unref(u);
	return same;
}

/* What a link out may be: a web page or a mail address. file:, custom
 * schemes and the rest would hand a note the power to start programs. */
static gboolean
is_link_out(const char *uri)
{
	return g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://") ||
	    g_str_has_prefix(uri, "mailto:");
}

/* Only the server's own origin is shown here; anything else is a link out. */
static gboolean
on_decide(WebKitWebView *web, WebKitPolicyDecision *d,
    WebKitPolicyDecisionType type, gpointer data)
{
	struct tab *t = data;
	WebKitNavigationAction *a;
	const char *uri;

	if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
	    type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
		return FALSE;
	a = webkit_navigation_policy_decision_get_navigation_action(
	    WEBKIT_NAVIGATION_POLICY_DECISION(d));
	uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(a));
	if (is_home(t, uri) || g_str_has_prefix(uri, "about:")) {
		if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
			webkit_web_view_load_uri(web, uri);
			webkit_policy_decision_ignore(d);
			return TRUE;
		}
		return FALSE;
	}
	if (is_link_out(uri))
		g_app_info_launch_default_for_uri(uri, NULL, NULL);
	webkit_policy_decision_ignore(d);
	return TRUE;
}

static void
on_switch(GtkNotebook *nb, GtkWidget *page, guint num, gpointer data)
{
	struct tab *t = g_object_get_data(G_OBJECT(page), "tab");
	struct term *s = g_object_get_data(G_OBJECT(page), "term");
	char *title;

	(void)nb; (void)num; (void)data;
	if (t)
		gtk_window_set_title(GTK_WINDOW(win), strrchr(t->dir, '/') + 1);
	else if (s) {
		title = g_strconcat("❯ ", strrchr(s->dir, '/') + 1, NULL);
		gtk_window_set_title(GTK_WINDOW(win), title);
		g_free(title);
	} else
		gtk_window_set_title(GTK_WINDOW(win), "graph");
}

/* The tab's label: a name, a tooltip, a close button wired to closer(data). */
static GtkWidget *
tab_label(const char *text, const char *tip, GCallback closer, gpointer data)
{
	GtkWidget *label = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *name = gtk_label_new(text);
	GtkWidget *x = gtk_button_new_from_icon_name("window-close-symbolic",
	    GTK_ICON_SIZE_MENU);

	gtk_button_set_relief(GTK_BUTTON(x), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(label, tip);
	gtk_box_pack_start(GTK_BOX(label), name, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(label), x, FALSE, FALSE, 0);
	gtk_widget_show_all(label);
	g_signal_connect(x, "clicked", closer, data);
	return label;
}

/* ---- the shell tab ---- */

/* The page and the shell speak through the web view's message handler in
 * plain strings: "rs:<cols>x<rows>" (the first one starts the shell) and
 * "in:<text>" from the page; T.recv(<base64>) and T.fail() to it. The
 * server is not involved: a note's token opens notes, never a shell. */

static void
run_js(GtkWidget *view, const char *js)
{
#if WEBKIT_CHECK_VERSION(2, 40, 0)
	webkit_web_view_evaluate_javascript(WEBKIT_WEB_VIEW(view), js, -1,
	    NULL, NULL, NULL, NULL, NULL);
#else
	webkit_web_view_run_javascript(WEBKIT_WEB_VIEW(view), js, NULL, NULL, NULL);
#endif
}

/* Same, but for a script whose value we want back. */
static void
ask_js(GtkWidget *view, const char *js, GAsyncReadyCallback cb, gpointer data)
{
#if WEBKIT_CHECK_VERSION(2, 40, 0)
	webkit_web_view_evaluate_javascript(WEBKIT_WEB_VIEW(view), js, -1,
	    NULL, NULL, NULL, cb, data);
#else
	webkit_web_view_run_javascript(WEBKIT_WEB_VIEW(view), js, NULL, cb, data);
#endif
}

/* The string ask_js asked for, or NULL if the script failed. */
static char *
ask_js_finish(GObject *view, GAsyncResult *res)
{
#if WEBKIT_CHECK_VERSION(2, 40, 0)
	JSCValue *v = webkit_web_view_evaluate_javascript_finish(
	    WEBKIT_WEB_VIEW(view), res, NULL);
	char *s;

	if (!v)
		return NULL;
	s = jsc_value_to_string(v);
	g_object_unref(v);
	return s;
#else
	WebKitJavascriptResult *r = webkit_web_view_run_javascript_finish(
	    WEBKIT_WEB_VIEW(view), res, NULL);
	char *s;

	if (!r)
		return NULL;
	s = jsc_value_to_string(webkit_javascript_result_get_js_value(r));
	webkit_javascript_result_unref(r);
	return s;
#endif
}

/* The repository's branch, for the header. NULL if not a checkout. */
static char *
git_branch(const char *dir)
{
	char *argv[] = { "git", "-C", (char *)dir, "symbolic-ref",
	    "--short", "HEAD", NULL };
	char *out = NULL;
	int status = -1;

	if (!g_spawn_sync(NULL, argv, NULL,
	    G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
	    &out, NULL, &status, NULL))
		return NULL;
	if (status != 0 || !out) {
		g_free(out);
		return NULL;
	}
	return g_strstrip(out);	/* frees nothing; trims in place */
}

/* Fill the header: what repository this is, its branch, and whether the shell
 * is confined. Linux does not confine yet, so it says so plainly. */
static void
term_push(struct term *t)
{
	const char *home = getenv("HOME");
	const char *base = strrchr(t->dir, '/');
	char *path, *name_j, *path_j, *branch_j, *js;

	if (home && *home && g_str_has_prefix(t->dir, home))
		path = g_strconcat("~", t->dir + strlen(home), NULL);
	else
		path = g_strdup(t->dir);

	name_j = g_strescape(base ? base + 1 : t->dir, NULL);
	path_j = g_strescape(path, NULL);
	branch_j = t->branch && *t->branch ? g_strescape(t->branch, NULL) : NULL;

	js = g_strdup_printf(
	    "T.info({name:\"%s\",path:\"%s\",sandboxed:false,"
	    "writable:[\"repo\",\"/tmp\",\"~/dotfiles\"],"
	    "readonly:[\"your files\",\"~/.ssh\",\"shell config\"]%s%s%s})",
	    name_j, path_j,
	    branch_j ? ",branch:\"" : "", branch_j ? branch_j : "",
	    branch_j ? "\"" : "");
	run_js(t->page, js);

	g_free(js);
	g_free(name_j);
	g_free(path_j);
	g_free(branch_j);
	g_free(path);
}

/* Re-read the branch; refresh the header only when it changed. */
static void
term_refresh(struct term *t)
{
	char *b = git_branch(t->dir);

	if (g_strcmp0(b, t->branch) == 0) {
		g_free(b);
		return;
	}
	g_free(t->branch);
	t->branch = b;		/* owns it now (may be NULL) */
	term_push(t);
}

static void
on_head_changed(GFileMonitor *m, GFile *f, GFile *other,
    GFileMonitorEvent ev, gpointer data)
{
	(void)m; (void)f; (void)other; (void)ev;
	term_refresh(data);
}

static void
on_term_load(WebKitWebView *w, WebKitLoadEvent ev, gpointer data)
{
	struct term *t = data;
	GFile *head;

	(void)w;
	if (ev != WEBKIT_LOAD_FINISHED)
		return;

	/* First fill: always render, whether or not this is a checkout. */
	t->branch = git_branch(t->dir);
	term_push(t);

	/* Then watch .git/HEAD so switching branches updates the header at
	 * once. Event-driven; GFileMonitor follows the atomic replace git
	 * does, so there is nothing to re-arm and nothing runs on a timer. */
	if (!t->headmon) {
		head = g_file_new_build_filename(t->dir, ".git", "HEAD", NULL);
		t->headmon = g_file_monitor_file(head, G_FILE_MONITOR_WATCH_MOVES,
		    NULL, NULL);
		g_object_unref(head);
		if (t->headmon)
			g_signal_connect(t->headmon, "changed",
			    G_CALLBACK(on_head_changed), t);
	}
}

static void
term_send(struct term *t, const char *p)
{
	size_t left = strlen(p);
	ssize_t n;

	while (left > 0 && t->master >= 0) {
		if ((n = write(t->master, p, left)) <= 0)
			break;
		p += n;
		left -= (size_t)n;
	}
}

/* Whatever the shell wrote, handed to the page as it arrives. */
static gboolean
on_pty(int fd, GIOCondition cond, gpointer data)
{
	struct term *t = data;
	char buf[65536], *b64, *js;
	ssize_t n;

	(void)cond;
	if ((n = read(fd, buf, sizeof(buf))) <= 0) {
		/* the last process on the tty is gone; the child watch
		 * closes the tab */
		t->reader = 0;
		return G_SOURCE_REMOVE;
	}
	b64 = g_base64_encode((const guchar *)buf, (gsize)n);
	js = g_strconcat("T.recv(\"", b64, "\")", NULL);
	run_js(t->page, js);
	g_free(js);
	g_free(b64);
	return G_SOURCE_CONTINUE;
}

static void close_term(struct term *t);

/* The shell's exit closes the tab, so `exit` means what it says. */
static void
on_term_exit(GPid pid, int status, gpointer data)
{
	struct term *t = data;

	(void)status;
	g_spawn_close_pid(pid);
	t->pid = 0;
	t->watch = 0;
	close_term(t);
}

static gboolean
term_spawn(struct term *t, int cols, int rows)
{
	struct winsize ws;
	const char *shell = getenv("SHELL");
	pid_t pid;
	int fd;

	memset(&ws, 0, sizeof(ws));
	ws.ws_col = (unsigned short)cols;
	ws.ws_row = (unsigned short)rows;
	if (!shell || !*shell)
		shell = "/bin/sh";
	if ((pid = forkpty(&fd, NULL, NULL, &ws)) < 0)
		return FALSE;
	if (pid == 0) {
		/* A login shell, for the PATH the user's profile sets. */
		setenv("TERM", "xterm-256color", 1);
		if (chdir(t->dir) < 0)
			_exit(127);
		execl(shell, shell, "-l", (char *)NULL);
		_exit(127);
	}
	t->pid = pid;
	t->master = fd;
	t->reader = g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_pty, t);
	t->watch = g_child_watch_add(pid, on_term_exit, t);
	if (t->pending) {
		char *line = g_strconcat(t->pending, "\n", NULL);
		term_send(t, line);
		g_free(line);
		g_free(t->pending);
		t->pending = NULL;
	}
	return TRUE;
}

static void
on_term_message(WebKitUserContentManager *m, WebKitJavascriptResult *r,
    gpointer data)
{
	struct term *t = data;
	char *s = jsc_value_to_string(webkit_javascript_result_get_js_value(r));
	struct winsize ws;
	int cols, rows;

	(void)m;
	if (g_str_has_prefix(s, "in:")) {
		term_send(t, s + 3);
	} else if (g_str_has_prefix(s, "rs:") &&
	    sscanf(s + 3, "%dx%d", &cols, &rows) == 2 && cols > 0 && rows > 0) {
		if (t->master < 0) {
			if (!term_spawn(t, cols, rows))
				run_js(t->page, "T.fail()");
		} else {
			memset(&ws, 0, sizeof(ws));
			ws.ws_col = (unsigned short)cols;
			ws.ws_row = (unsigned short)rows;
			ioctl(t->master, TIOCSWINSZ, &ws);
		}
	}
	g_free(s);
}

/* Hang up on the shell and reap it. A shell that will not go on SIGHUP is
 * given a moment, then killed, so closing a tab never hangs the window. */
static void
stop_term(struct term *t)
{
	GPid pid = t->pid;
	int i;

	if (t->reader) {
		g_source_remove(t->reader);
		t->reader = 0;
	}
	if (t->master >= 0) {
		close(t->master);
		t->master = -1;
	}
	if (t->watch) {
		g_source_remove(t->watch);
		t->watch = 0;
	}
	if (pid <= 0)
		return;
	t->pid = 0;
	kill(pid, SIGHUP);
	for (i = 0; i < 20; i++) {
		if (waitpid(pid, NULL, WNOHANG) != 0)
			return;
		g_usleep(10000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
}

static void
close_term(struct term *t)
{
	int i = gtk_notebook_page_num(GTK_NOTEBOOK(book), t->page);

	stop_term(t);
	if (t->headmon)
		g_object_unref(t->headmon);
	terms = g_list_remove(terms, t);
	if (i >= 0)
		gtk_notebook_remove_page(GTK_NOTEBOOK(book), i);
	g_free(t->dir);
	g_free(t->pending);
	g_free(t->branch);
	g_free(t);
}

static void
on_term_close_clicked(GtkButton *b, gpointer data)
{
	(void)b;
	close_term(data);
}

/* A shell tab for a repository. ":sh" always opens a new one; ":!cmd" with
 * a shell already open for that repository runs there instead, as a
 * second prompt would only get in the way of the first. */
static void
open_shell(const char *dir, const char *cmd)
{
	WebKitUserContentManager *ucm;
	struct term *t;
	GList *l;
	char *name, *line;
	int i;

	if (cmd && *cmd) {
		for (l = g_list_last(terms); l; l = l->prev) {
			t = l->data;
			if (strcmp(t->dir, dir) || t->master < 0)
				continue;
			line = g_strconcat(cmd, "\n", NULL);
			term_send(t, line);
			g_free(line);
			gtk_notebook_set_current_page(GTK_NOTEBOOK(book),
			    gtk_notebook_page_num(GTK_NOTEBOOK(book), t->page));
			return;
		}
	}

	t = g_new0(struct term, 1);
	t->dir = g_strdup(dir);
	t->pending = cmd && *cmd ? g_strdup(cmd) : NULL;
	t->master = -1;

	ucm = webkit_user_content_manager_new();
	webkit_user_content_manager_register_script_message_handler(ucm, "term");
	g_signal_connect(ucm, "script-message-received::term",
	    G_CALLBACK(on_term_message), t);
	t->page = webkit_web_view_new_with_user_content_manager(ucm);
	g_object_unref(ucm);	/* the view holds it now */
	g_object_set_data(G_OBJECT(t->page), "term", t);
	g_signal_connect(t->page, "load-changed", G_CALLBACK(on_term_load), t);
	webkit_web_view_load_html(WEBKIT_WEB_VIEW(t->page),
	    (const char *)term_html, "file:///");
	gtk_widget_show(t->page);

	name = g_strconcat("❯ ", strrchr(dir, '/') + 1, NULL);
	i = gtk_notebook_append_page(GTK_NOTEBOOK(book), t->page,
	    tab_label(name, dir, G_CALLBACK(on_term_close_clicked), t));
	g_free(name);
	gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(book), t->page, TRUE);
	gtk_notebook_set_current_page(GTK_NOTEBOOK(book), i);
	terms = g_list_append(terms, t);
}

/* ":sh" and ":!cmd" in the page arrive here as "sh:<cmd>". */
static void
on_graph_message(WebKitUserContentManager *m, WebKitJavascriptResult *r,
    gpointer data)
{
	struct tab *t = data;
	char *s = jsc_value_to_string(webkit_javascript_result_get_js_value(r));

	(void)m;
	if (g_str_has_prefix(s, "sh:"))
		open_shell(t->dir, s + 3);
	g_free(s);
}

static void
add_tab(struct tab *t)
{
	WebKitUserContentManager *ucm;
	int i;

	/* The page reaches the application through one message handler,
	 * which only a page from the server's own origin can be loaded to
	 * use (see on_decide). */
	ucm = webkit_user_content_manager_new();
	webkit_user_content_manager_register_script_message_handler(ucm, "graph");
	g_signal_connect(ucm, "script-message-received::graph",
	    G_CALLBACK(on_graph_message), t);
	t->page = webkit_web_view_new_with_user_content_manager(ucm);
	g_object_unref(ucm);	/* the view holds it now */
	g_object_set_data(G_OBJECT(t->page), "tab", t);
	g_signal_connect(t->page, "decide-policy", G_CALLBACK(on_decide), t);
	webkit_web_view_load_uri(WEBKIT_WEB_VIEW(t->page), t->url);
	gtk_widget_show(t->page);

	i = gtk_notebook_append_page(GTK_NOTEBOOK(book), t->page,
	    tab_label(strrchr(t->dir, '/') + 1, t->dir,
	    G_CALLBACK(on_close_clicked), t));
	gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(book), t->page, TRUE);
	gtk_notebook_set_current_page(GTK_NOTEBOOK(book), i);
	tabs = g_list_append(tabs, t);
}

static void
open_dir(const char *dir)
{
	struct tab *t;
	char *err = NULL, *abs;

	abs = g_canonicalize_filename(dir, NULL);
	if ((t = tab_for(abs))) {
		gtk_notebook_set_current_page(GTK_NOTEBOOK(book),
		    gtk_notebook_page_num(GTK_NOTEBOOK(book), t->page));
		gtk_window_present(GTK_WINDOW(win));
		g_free(abs);
		return;
	}
	if (!is_repo(abs)) {
		fail(abs, "not a Graph repository");
		g_free(abs);
		return;
	}
	t = g_new0(struct tab, 1);
	t->dir = abs;
	if (!start_server(t, &err)) {
		fail(abs, err);
		g_free(err);
		g_free(t->dir);
		g_free(t);
		return;
	}
	add_tab(t);
	gtk_window_present(GTK_WINDOW(win));
	remember(abs);
	save_session();
}

/* ---- chooser and recents ---- */

/* The folder chooser. It insists on a repository. NULL on cancel. */
static char *
pick(void)
{
	GtkFileChooserNative *d = gtk_file_chooser_native_new(
	    "Choose a Graph repository", GTK_WINDOW(win),
	    GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Open", "_Cancel");
	char *dir;

	for (;;) {
		if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(d)) != GTK_RESPONSE_ACCEPT) {
			g_object_unref(d);
			return NULL;
		}
		dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
		if (is_repo(dir)) {
			g_object_unref(d);
			return dir;
		}
		g_free(dir);
	}
}

static void
pick_and_open(void)
{
	char *dir = pick();

	if (dir) {
		open_dir(dir);
		g_free(dir);
	}
}

static void
on_recent(GtkMenuItem *item, gpointer data)
{
	(void)data;
	open_dir(g_object_get_data(G_OBJECT(item), "dir"));
}

static void
on_clear_recent(GtkMenuItem *item, gpointer data)
{
	char *none[] = { NULL };

	(void)item; (void)data;
	write_lines("recent", none);
}

/* The recents menu is rebuilt each time it drops down. */
static void
on_recent_clicked(GtkButton *b, gpointer data)
{
	GtkWidget *menu = gtk_menu_new(), *it;
	char **r = read_lines("recent");
	int i;

	(void)data;
	for (i = 0; r[i]; i++) {
		it = gtk_menu_item_new_with_label(strrchr(r[i], '/') + 1);
		gtk_widget_set_tooltip_text(it, r[i]);
		g_object_set_data_full(G_OBJECT(it), "dir", g_strdup(r[i]), g_free);
		g_signal_connect(it, "activate", G_CALLBACK(on_recent), NULL);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
	}
	if (i)
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
	it = gtk_menu_item_new_with_label("Clear Menu");
	g_signal_connect(it, "activate", G_CALLBACK(on_clear_recent), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
	g_strfreev(r);
	gtk_widget_show_all(menu);
	gtk_menu_popup_at_widget(GTK_MENU(menu), GTK_WIDGET(b),
	    GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
}

static void
on_new_clicked(GtkButton *b, gpointer data)
{
	(void)b; (void)data;
	pick_and_open();
}

/* ---- window and application ---- */

static void
on_destroy(GtkWidget *w, gpointer data)
{
	GList *l;

	(void)w; (void)data;
	quitting = TRUE;
	for (l = tabs; l; l = l->next)
		stop_server(l->data);
	for (l = terms; l; l = l->next)
		stop_term(l->data);
	win = NULL;
}

static void
act_new(GSimpleAction *a, GVariant *v, gpointer data)
{
	(void)a; (void)v; (void)data;
	pick_and_open();
}

static void
act_close(GSimpleAction *a, GVariant *v, gpointer data)
{
	struct tab *t = current_tab();
	int i = gtk_notebook_get_current_page(GTK_NOTEBOOK(book));
	struct term *s = i < 0 ? NULL : g_object_get_data(
	    G_OBJECT(gtk_notebook_get_nth_page(GTK_NOTEBOOK(book), i)), "term");

	(void)a; (void)v; (void)data;
	if (t)
		close_tab(t);
	else if (s)
		close_term(s);
}

/* Where the page's selection sits, as a repository-relative folder: a
 * folder as itself, a note as the folder holding it, "" for the root. */
#define SEL_JS \
	"(function(){var x=(typeof cur!=='undefined'&&cur>=0&&N[cur])?N[cur]:null;" \
	"if(!x)return'';var p=x.path||'';" \
	"if(x.type!=='dir'){var i=p.lastIndexOf('/');p=i<0?'':p.substring(0,i);}" \
	"return p;})()"

/* The selection, come back from the page. The tab may have closed while we
 * were asking, so it has to be looked for before it is read. */
static void
on_sel(GObject *view, GAsyncResult *res, gpointer data)
{
	struct tab *t = data;
	char *sub, *dir = NULL;

	if (!g_list_find(tabs, t))
		return;
	if ((sub = ask_js_finish(view, res)) && *sub) {
		dir = g_build_filename(t->dir, sub, NULL);
		if (!g_file_test(dir, G_FILE_TEST_IS_DIR)) {
			g_free(dir);
			dir = NULL;	/* stale or renamed: the root will do */
		}
	}
	open_shell(dir ? dir : t->dir, NULL);
	g_free(dir);
	g_free(sub);
}

/* A shell where you already are: the selected folder of a display tab, the
 * same folder again for a shell tab, as a terminal opening a tab does. */
static void
act_shell(GSimpleAction *a, GVariant *v, gpointer data)
{
	int i = gtk_notebook_get_current_page(GTK_NOTEBOOK(book));
	GtkWidget *page;
	struct tab *t;
	struct term *s;

	(void)a; (void)v; (void)data;
	if (i < 0)
		return;
	page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(book), i);
	if ((s = g_object_get_data(G_OBJECT(page), "term")))
		open_shell(s->dir, NULL);
	else if ((t = g_object_get_data(G_OBJECT(page), "tab")))
		ask_js(t->page, SEL_JS, on_sel, t);
}

static void
act_next(GSimpleAction *a, GVariant *v, gpointer data)
{
	(void)a; (void)v;
	if (GPOINTER_TO_INT(data) > 0)
		gtk_notebook_next_page(GTK_NOTEBOOK(book));
	else
		gtk_notebook_prev_page(GTK_NOTEBOOK(book));
}

/* Ctrl+1…9 show the tab in that position. Past the last tab the notebook
 * does nothing, which is the right answer. */
static void
act_tab(GSimpleAction *a, GVariant *v, gpointer data)
{
	(void)a; (void)data;
	if (book)
		gtk_notebook_set_current_page(GTK_NOTEBOOK(book),
		    g_variant_get_int32(v) - 1);
}

static void
ensure_window(void)
{
	GtkWidget *actions, *plus, *recent;

	if (win)
		return;
	/* the desktop file and icon theme entry are both named "graph", which
	 * is how Wayland and X11 respectively find the window's icon */
	win = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(win), "graph");
	gtk_window_set_icon_name(GTK_WINDOW(win), "graph");
	gtk_window_set_default_size(GTK_WINDOW(win), 1180, 780);
	g_signal_connect(win, "destroy", G_CALLBACK(on_destroy), NULL);

	book = gtk_notebook_new();
	gtk_notebook_set_scrollable(GTK_NOTEBOOK(book), TRUE);
	gtk_notebook_set_show_border(GTK_NOTEBOOK(book), FALSE);
	g_signal_connect(book, "switch-page", G_CALLBACK(on_switch), NULL);

	actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	plus = gtk_button_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_MENU);
	recent = gtk_button_new_from_icon_name("pan-down-symbolic", GTK_ICON_SIZE_MENU);
	gtk_button_set_relief(GTK_BUTTON(plus), GTK_RELIEF_NONE);
	gtk_button_set_relief(GTK_BUTTON(recent), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(plus, "Open a repository (Ctrl+O)");
	gtk_widget_set_tooltip_text(recent, "Recent");
	g_signal_connect(plus, "clicked", G_CALLBACK(on_new_clicked), NULL);
	g_signal_connect(recent, "clicked", G_CALLBACK(on_recent_clicked), NULL);
	gtk_box_pack_start(GTK_BOX(actions), plus, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(actions), recent, FALSE, FALSE, 0);
	gtk_widget_show_all(actions);
	gtk_notebook_set_action_widget(GTK_NOTEBOOK(book), actions, GTK_PACK_END);

	gtk_container_add(GTK_CONTAINER(win), book);
	gtk_widget_show_all(win);
}

/* Nothing open: last session if there was one, otherwise ask. */
static void
open_last_or_ask(void)
{
	char **s = read_lines("session");
	int i;

	for (i = 0; s[i]; i++)
		if (is_repo(s[i]))
			open_dir(s[i]);
	g_strfreev(s);
	if (!tabs)
		pick_and_open();
	if (!tabs && win)
		gtk_widget_destroy(win);
}

static void
on_activate(GApplication *a, gpointer data)
{
	(void)a; (void)data;
	ensure_window();
	if (!tabs)
		open_last_or_ask();
	else
		gtk_window_present(GTK_WINDOW(win));
}

static void
on_open(GApplication *a, GFile **files, int n, const char *hint, gpointer data)
{
	int i;

	(void)a; (void)hint; (void)data;
	ensure_window();
	for (i = 0; i < n; i++) {
		char *p = g_file_get_path(files[i]);
		if (p)
			open_dir(p);
		g_free(p);
	}
	if (!tabs)
		gtk_widget_destroy(win);
}

static void
on_startup(GApplication *a, gpointer data)
{
	static const GActionEntry acts[] = {
		{ "new", act_new, NULL, NULL, NULL, { 0 } },
		{ "close", act_close, NULL, NULL, NULL, { 0 } },
		{ "shell", act_shell, NULL, NULL, NULL, { 0 } },
	};
	static const char *const k_new[] = { "<Primary>o", NULL };
	/* Shift, so that the shell keeps Ctrl+T for readline and fzf. */
	static const char *const k_shell[] = { "<Primary><Shift>t", NULL };
	static const char *const k_close[] = { "<Primary>w", NULL };
	static const char *const k_next[] = { "<Primary>Page_Down", NULL };
	static const char *const k_prev[] = { "<Primary>Page_Up", NULL };
	GSimpleAction *next = g_simple_action_new("next", NULL);
	GSimpleAction *prev = g_simple_action_new("prev", NULL);
	GSimpleAction *tab = g_simple_action_new("tab", G_VARIANT_TYPE_INT32);
	char detailed[16], accel[16];
	const char *k_tab[] = { accel, NULL };
	int i;

	(void)data;
	g_action_map_add_action_entries(G_ACTION_MAP(a), acts, G_N_ELEMENTS(acts), NULL);
	g_signal_connect(next, "activate", G_CALLBACK(act_next), GINT_TO_POINTER(1));
	g_signal_connect(prev, "activate", G_CALLBACK(act_next), GINT_TO_POINTER(-1));
	g_action_map_add_action(G_ACTION_MAP(a), G_ACTION(next));
	g_action_map_add_action(G_ACTION_MAP(a), G_ACTION(prev));
	g_signal_connect(tab, "activate", G_CALLBACK(act_tab), NULL);
	g_action_map_add_action(G_ACTION_MAP(a), G_ACTION(tab));
	gtk_application_set_accels_for_action(GTK_APPLICATION(a), "app.new", k_new);
	gtk_application_set_accels_for_action(GTK_APPLICATION(a), "app.close", k_close);
	gtk_application_set_accels_for_action(GTK_APPLICATION(a), "app.shell", k_shell);
	gtk_application_set_accels_for_action(GTK_APPLICATION(a), "app.next", k_next);
	gtk_application_set_accels_for_action(GTK_APPLICATION(a), "app.prev", k_prev);
	for (i = 1; i <= 9; i++) {
		snprintf(detailed, sizeof(detailed), "app.tab(%d)", i);
		snprintf(accel, sizeof(accel), "<Primary>%d", i);
		gtk_application_set_accels_for_action(GTK_APPLICATION(a),
		    detailed, k_tab);
	}
}

int
main(int argc, char *argv[])
{
	int st;

	g_set_prgname("graph");
	app = gtk_application_new("org.wrklabs.graph", G_APPLICATION_HANDLES_OPEN);
	g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
	g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
	g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);
	st = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return st;
}
