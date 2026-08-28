/* See LICENSE file for copyright and license details. */
/* graph enable mcp — wire an agent memory server into a repository */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "graph.h"

/* Everything this writes lives under .graph/, except the pointer file: agents
 * discover project servers from a .mcp.json at the repository root and will
 * not look anywhere else for it. It is six lines and starts with a dot, so
 * the scan skips it and it never shows in the display. */
#define MCP_DIR     GRAPH_DIR "/mcp"
#define MCP_SERVE   MCP_DIR "/serve.sh"
#define MCP_STORE   MCP_DIR "/self.jsonl"
#define MCP_CONFIG  ".mcp.json"

/* The launcher. MEMORY_FILE_PATH has to be absolute: the memory server
 * resolves a relative one against its own package directory under the npx
 * cache, not against the repository, so a relative path silently writes the
 * brain somewhere else. Deriving it from the script's own location keeps it
 * right however the agent was started and wherever the repository is mounted.
 *
 * npx caches the package in ~/.npm/_npx, so nothing is installed into the
 * repository — no node_modules, nothing to commit, nothing to back up. */
static const char serve_sh[] =
    "#!/usr/bin/env sh\n"
    "# Runs the memory MCP server over this repository's self.jsonl.\n"
    "#\n"
    "# The server resolves a relative MEMORY_FILE_PATH against its own package\n"
    "# directory rather than the repository, so the path is derived from this\n"
    "# script instead of from whatever directory the agent started in.\n"
    "set -eu\n"
    "dir=$(cd \"$(dirname \"$0\")\" && pwd)\n"
    "MEMORY_FILE_PATH=\"$dir/self.jsonl\"\n"
    "export MEMORY_FILE_PATH\n"
    "exec npx -y @modelcontextprotocol/server-memory\n";

/* Named `self` rather than `memory` so the tools read as mcp__self__* at the
 * call site, and so a second memory server can be added later without a
 * collision. The path is relative because the agent runs at the repository
 * root, which is also where it found this file. */
static const char mcp_json[] =
    "{\n"
    "  \"mcpServers\": {\n"
    "    \"self\": {\n"
    "      \"command\": \"sh\",\n"
    "      \"args\": [\"" MCP_SERVE "\"]\n"
    "    }\n"
    "  }\n"
    "}\n";

static int
exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

static void
write_file(const char *path, const char *data, size_t len, mode_t mode)
{
	FILE *f;

	if (!(f = fopen(path, "w")))
		die("cannot write %s: %s", path, strerror(errno));
	if (fwrite(data, 1, len, f) != len || fclose(f) != 0)
		die("cannot write %s: %s", path, strerror(errno));
	if (chmod(path, mode) < 0)
		die("cannot set permissions on %s: %s", path, strerror(errno));
}

/* True when the file holds exactly what enable would have written. Disable
 * uses it to tell its own .mcp.json from one the user has since edited or
 * added other servers to — without a JSON parser, byte equality is the only
 * honest test, and being wrong here means deleting someone's config. */
static int
is_ours(const char *path, const char *want, size_t len)
{
	char buf[4096];
	size_t n;
	FILE *f;
	int same;

	if (len >= sizeof(buf))
		return 0;
	if (!(f = fopen(path, "r")))
		return 0;
	n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	same = n == len && memcmp(buf, want, len) == 0;
	return same;
}

/* The repository to act on: the argument if given, otherwise the current
 * directory, matching how the other commands take an optional path. */
static const char *
repo_arg(int argc, char *argv[])
{
	const char *root;

	root = argc > 0 ? argv[0] : ".";
	if (!is_graph_repo(root))
		die("%s is not a Graph repository", root);
	return root;
}

int
cmd_enable(int argc, char *argv[])
{
	char dir[PATH_MAX], serve[PATH_MAX], store[PATH_MAX], config[PATH_MAX];
	const char *root;

	if (argc < 1 || strcmp(argv[0], "mcp") != 0) {
		fputs("usage: graph enable mcp [path]\n", stderr);
		return 1;
	}
	root = repo_arg(argc - 1, argv + 1);

	if (join_path(dir, sizeof(dir), root, MCP_DIR) < 0 ||
	    join_path(serve, sizeof(serve), root, MCP_SERVE) < 0 ||
	    join_path(store, sizeof(store), root, MCP_STORE) < 0 ||
	    join_path(config, sizeof(config), root, MCP_CONFIG) < 0)
		die("path too long: %s", root);

	if (mkdir_p(dir, 0755) < 0)
		die("cannot create %s: %s", dir, strerror(errno));

	/* Rewritten every time, so a repository picks up a corrected launcher
	 * by running enable again. */
	write_file(serve, serve_sh, sizeof(serve_sh) - 1, 0755);

	/* Never rewritten. This is the one file here that cannot be
	 * regenerated from anything. */
	if (!exists(store))
		write_file(store, "", 0, 0644);

	/* A .mcp.json that is already there may name other servers, and
	 * merging JSON needs a parser this program does not have. Say what to
	 * add and let the user add it. */
	if (exists(config) && !is_ours(config, mcp_json, sizeof(mcp_json) - 1)) {
		warn("%s already exists and was not written by graph", config);
		fputs("\nAdd this server to it:\n\n"
		      "    \"self\": {\n"
		      "      \"command\": \"sh\",\n"
		      "      \"args\": [\"" MCP_SERVE "\"]\n"
		      "    }\n\n", stderr);
		return 1;
	}
	write_file(config, mcp_json, sizeof(mcp_json) - 1, 0644);

	/* Cheap to check now, and the alternative is the agent failing to
	 * start the server much later with nothing pointing back to here. */
	if (system("command -v npx >/dev/null 2>&1") != 0)
		warn("npx was not found: the server needs Node installed to run");

	printf("memory enabled for %s\n", root);
	printf("  %s\n", MCP_STORE);
	puts("\nRestart your agent in this directory to pick up the server.");
	return 0;
}

int
cmd_disable(int argc, char *argv[])
{
	char serve[PATH_MAX], store[PATH_MAX], config[PATH_MAX];
	const char *root;

	if (argc < 1 || strcmp(argv[0], "mcp") != 0) {
		fputs("usage: graph disable mcp [path]\n", stderr);
		return 1;
	}
	root = repo_arg(argc - 1, argv + 1);

	if (join_path(serve, sizeof(serve), root, MCP_SERVE) < 0 ||
	    join_path(store, sizeof(store), root, MCP_STORE) < 0 ||
	    join_path(config, sizeof(config), root, MCP_CONFIG) < 0)
		die("path too long: %s", root);

	if (unlink(serve) < 0 && errno != ENOENT)
		die("cannot remove %s: %s", serve, strerror(errno));

	/* Only if it is still the file enable wrote. One the user has added
	 * their own servers to is theirs to edit. */
	if (exists(config)) {
		if (is_ours(config, mcp_json, sizeof(mcp_json) - 1)) {
			if (unlink(config) < 0)
				die("cannot remove %s: %s", config,
				    strerror(errno));
		} else {
			warn("left %s alone: it has been edited", config);
		}
	}

	/* The store stays. Disabling the server is not a request to forget,
	 * and nothing else in the repository can reconstruct it. */
	printf("memory disabled for %s\n", root);
	if (exists(store))
		printf("what it remembered is kept at %s\n", MCP_STORE);
	return 0;
}
