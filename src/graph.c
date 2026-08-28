/* See LICENSE file for copyright and license details. */
/* graph — personal knowledge hub */

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
	    "  config --address <addr>  configure the SMB server\n"
	    "  serve <path> --name <name> [--user <user>]\n"
	    "                           expose a repository through SMB\n"
	    "  unserve <name>           stop serving a repository\n"
	    "  connect <address> <name> --to=<destination>\n"
	    "                           mount a remote repository\n"
	    "  disconnect <destination> release a connected repository\n"
	    "  status                   what is served and what is connected\n"
	    "  enable mcp [path]        give agents a memory of the repository\n"
	    "  disable mcp [path]       remove it, keeping what it remembered\n"
	    "\n"
	    "  -v, --version  print version\n"
	    "  -h, --help     print this message\n",
	    out);
}

int
main(int argc, char *argv[])
{
	const char *cmd;

	/* Bare `graph` opens the app, the way the desktop entry does. Only with
	 * no arguments at all: a path in that position would have to be told
	 * apart from a command name, and a repository directory called `init`
	 * is not something to gamble on. Without a desktop to open onto — over
	 * ssh, in a script — the help is the more useful answer. */
	if (argc < 2) {
		if (have_display())
			return cmd_display(0, argv + 1);
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
	if (!strcmp(cmd, "config"))
		return cmd_config(argc - 2, argv + 2);
	if (!strcmp(cmd, "serve"))
		return cmd_serve(argc - 2, argv + 2);
	if (!strcmp(cmd, "unserve"))
		return cmd_unserve(argc - 2, argv + 2);
	if (!strcmp(cmd, "connect"))
		return cmd_connect(argc - 2, argv + 2);
	if (!strcmp(cmd, "disconnect"))
		return cmd_disconnect(argc - 2, argv + 2);
	if (!strcmp(cmd, "status"))
		return cmd_status(argc - 2, argv + 2);
	if (!strcmp(cmd, "enable"))
		return cmd_enable(argc - 2, argv + 2);
	if (!strcmp(cmd, "disable"))
		return cmd_disable(argc - 2, argv + 2);

	warn("unknown command: %s", cmd);
	usage(stderr);
	return 1;
}
