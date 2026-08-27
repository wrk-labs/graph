/* See LICENSE file for copyright and license details. */
/* graph init — create a Graph repository */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "graph.h"
#include "templates.h"

/* The default structure. A recommended starting point rather than a schema:
 * nothing downstream depends on these names, and users are free to rename,
 * nest, or add to them afterwards. */
static const char *layout[] = {
	GRAPH_DIR,
	"inbox",
	"people",
	"organizations",
	"finance",
	"research",
	"knowledge",
	"archive",
	"journal",
	"journal/agents",
};

/* AGENTS.md and the README of each default directory. They describe the
 * layout and its conventions to whoever — or whatever — reads the repository
 * next, and are ordinary files from then on: the user may edit or delete them,
 * and nothing in Graph depends on their contents. */
static void
write_templates(const char *root)
{
	char path[PATH_MAX];
	FILE *f;
	size_t i;

	for (i = 0; i < sizeof(templates) / sizeof(*templates); i++) {
		if (join_path(path, sizeof(path), root, templates[i].path) < 0)
			die("path too long: %s/%s", root, templates[i].path);
		if (!(f = fopen(path, "w")))
			die("cannot write %s: %s", path, strerror(errno));
		if (fwrite(templates[i].data, 1, templates[i].len, f) !=
		    templates[i].len || fclose(f) != 0)
			die("cannot write %s: %s", path, strerror(errno));
	}
}

/* A .graphignore to start from. Directories listed in it still show in the
 * display tree and their contents stay reachable — listed level by level as
 * they are opened — but nothing inside one is indexed, scanned for [[links]]
 * or searched, so a dependency tree costs one node however large it is.
 * Like the templates, the file is the user's from then on: init writes it
 * once and nothing refills it. */
static void
write_ignore(const char *root)
{
	static const char defaults[] =
	    "# What Graph shows but does not read. An ignored directory stays\n"
	    "# visible and navigable, listed as it is opened, but nothing inside\n"
	    "# it is scanned for [[links]] or searched. An ignored file stays in\n"
	    "# the tree, readable and editable, but is not scanned or searched.\n"
	    "#\n"
	    "# One pattern per line. A bare name matches that name anywhere in\n"
	    "# the tree; a pattern with / matches one path from the repository\n"
	    "# root, a leading / anchoring a root-level file; *.ext matches by\n"
	    "# name suffix.\n"
	    "node_modules\n"
	    "vendor\n"
	    "target\n"
	    "dist\n"
	    "build\n"
	    "__pycache__\n"
	    "venv\n"
	    "\n"
	    "# The starter documents init wrote: layout guidance, not knowledge.\n"
	    "# Remove a line once you have made the file your own and want it in\n"
	    "# the graph.\n"
	    "/AGENTS.md\n"
	    "/README.md\n"
	    "inbox/README.md\n"
	    "people/README.md\n"
	    "organizations/README.md\n"
	    "finance/README.md\n"
	    "research/README.md\n"
	    "knowledge/README.md\n"
	    "archive/README.md\n"
	    "journal/README.md\n"
	    "journal/agents/README.md\n";
	char path[PATH_MAX];
	FILE *f;

	if (join_path(path, sizeof(path), root, ".graphignore") < 0)
		die("path too long: %s/.graphignore", root);
	if (!(f = fopen(path, "w")))
		die("cannot write %s: %s", path, strerror(errno));
	if (fwrite(defaults, 1, sizeof(defaults) - 1, f) != sizeof(defaults) - 1 ||
	    fclose(f) != 0)
		die("cannot write %s: %s", path, strerror(errno));
}

static void
write_marker(const char *root)
{
	char dir[PATH_MAX];
	char marker[PATH_MAX];
	FILE *f;

	if (join_path(dir, sizeof(dir), root, GRAPH_DIR) < 0 ||
	    join_path(marker, sizeof(marker), dir, GRAPH_MARKER) < 0)
		die("path too long: %s", root);

	if (!(f = fopen(marker, "w")))
		die("cannot write %s: %s", marker, strerror(errno));
	fprintf(f, "version=%d\n", GRAPH_FORMAT);
	if (fclose(f) != 0)
		die("cannot write %s: %s", marker, strerror(errno));
}

int
cmd_init(int argc, char *argv[])
{
	char path[PATH_MAX];
	const char *root;
	struct stat st;
	size_t i;
	int empty;

	if (argc != 1) {
		fputs("usage: graph init <path>\n", stderr);
		return 1;
	}
	root = argv[0];

	/* Only a nonexistent path or an existing empty directory. Refusing
	 * anything else keeps init from adopting or overwriting a tree that
	 * already holds someone's data. */
	if (stat(root, &st) == 0) {
		if (!S_ISDIR(st.st_mode))
			die("%s exists and is not a directory", root);
		if (is_graph_repo(root))
			die("%s is already a Graph repository", root);
		if ((empty = dir_is_empty(root)) < 0)
			die("cannot read %s: %s", root, strerror(errno));
		if (!empty)
			die("%s is not empty", root);
	} else if (errno != ENOENT) {
		die("cannot stat %s: %s", root, strerror(errno));
	} else if (mkdir_p(root, 0755) < 0) {
		die("cannot create %s: %s", root, strerror(errno));
	}

	for (i = 0; i < sizeof(layout) / sizeof(*layout); i++) {
		if (join_path(path, sizeof(path), root, layout[i]) < 0)
			die("path too long: %s/%s", root, layout[i]);
		if (mkdir(path, 0755) < 0)
			die("cannot create %s: %s", path, strerror(errno));
	}

	write_templates(root);
	write_ignore(root);
	write_marker(root);

	printf("created Graph repository at %s\n", root);
	return 0;
}
