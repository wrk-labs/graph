/* See LICENSE file for copyright and license details. */
/* graph init — create a Graph repository */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "graph.h"

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
};

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

	write_marker(root);

	printf("created Graph repository at %s\n", root);
	return 0;
}
