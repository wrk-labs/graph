/* See LICENSE file for copyright and license details. */
/* graph — filesystem repository over SMB */

#include <stdio.h>
#include <string.h>

#include "graph.h"

static void
usage(FILE *out)
{
	fputs(
	    "usage: graph <command> [args]\n"
	    "\n"
	    "commands:\n"
	    "  init <path>              create a Graph repository\n"
	    "  display [path] [--port <port>] [--no-open]\n"
	    "                           open a local view of a repository\n"
	    "\n"
	    "  -v, --version  print version\n"
	    "  -h, --help     print this message\n",
	    out);
}

int
main(int argc, char *argv[])
{
	const char *cmd;

	if (argc < 2) {
		usage(stderr);
		return 1;
	}
	cmd = argv[1];

	if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help")) {
		usage(stdout);
		return 0;
	}
	if (!strcmp(cmd, "-v") || !strcmp(cmd, "--version")) {
		puts("graph " VERSION);
		return 0;
	}
	if (!strcmp(cmd, "init"))
		return cmd_init(argc - 2, argv + 2);
	if (!strcmp(cmd, "display"))
		return cmd_display(argc - 2, argv + 2);

	warn("unknown command: %s", cmd);
	usage(stderr);
	return 1;
}
