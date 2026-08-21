/* See LICENSE file for copyright and license details. */
/* graph status — what this machine is serving and what it has connected */

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __linux__
#include <stdlib.h>
#else
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/ucred.h>
#endif

#include "graph.h"
#include "smb.h"

/* A machine can be both ends at once — serving its own repositories and
 * holding others connected from elsewhere — so both are reported. Nothing
 * here writes anything, and nothing needs root: the configuration is
 * readable, and the kernel will say what is mounted. */

#ifdef __linux__
/* /proc/self/mounts escapes the characters that would otherwise break its own
 * field separation. Undo that so a path with a space reads correctly. */
static void
unescape(char *s)
{
	char *w = s;

	while (*s) {
		if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' &&
		    s[2] >= '0' && s[2] <= '7' && s[3] >= '0' && s[3] <= '7') {
			*w++ = (char)(((s[1] - '0') << 6) |
			    ((s[2] - '0') << 3) | (s[3] - '0'));
			s += 4;
			continue;
		}
		*w++ = *s++;
	}
	*w = '\0';
}

static int
each_mount(void (*fn)(void *, const char *, const char *), void *ctx)
{
	FILE *f;
	char line[4096];
	int n = 0;

	if (!(f = fopen("/proc/self/mounts", "r")))
		return -1;
	while (fgets(line, sizeof(line), f)) {
		char *src, *dst, *type, *save;

		if (!(src = strtok_r(line, " ", &save)))
			continue;
		if (!(dst = strtok_r(NULL, " ", &save)))
			continue;
		if (!(type = strtok_r(NULL, " ", &save)))
			continue;
		/* The kernel names the SMB filesystem by dialect generation. */
		if (strcmp(type, "cifs") && strcmp(type, "smb3"))
			continue;
		unescape(src);
		unescape(dst);
		fn(ctx, dst, src);
		n++;
	}
	fclose(f);
	return n;
}
#else
static int
each_mount(void (*fn)(void *, const char *, const char *), void *ctx)
{
	struct statfs *mnt;
	int i, count, n = 0;

	if ((count = getmntinfo(&mnt, MNT_NOWAIT)) <= 0)
		return -1;
	for (i = 0; i < count; i++) {
		if (strcmp(mnt[i].f_fstypename, "smbfs"))
			continue;
		fn(ctx, mnt[i].f_mntonname, mnt[i].f_mntfromname);
		n++;
	}
	return n;
}
#endif

static void
print_mount(void *ctx, const char *dst, const char *src)
{
	(void)ctx;
	printf("  %-24s %s\n", dst, src);
}

int
cmd_status(int argc, char *argv[])
{
	struct smb_conf conf;
	size_t i;
	int mounts;

	(void)argv;
	if (argc != 0) {
		fputs("usage: graph status\n", stderr);
		return 1;
	}

	/* Serving is configured through Samba, which graph only drives on
	 * Linux; elsewhere there is nothing of graph's to report. */
	if (smb_on_linux()) {
		printf("serving\n");
		if (!smb_present()) {
			printf("  Samba is not installed\n");
		} else if (smb_read(&conf) < 1) {
			printf("  nothing configured\n");
		} else {
			printf("  %-24s %s\n", "service",
			    smb_running() ? "running" : "stopped");
			if (conf.naddrs == 0)
				printf("  %-24s %s\n", "address",
				    "none configured");
			for (i = 0; i < conf.naddrs; i++)
				printf("  %-24s %s\n",
				    i ? "" : "address", conf.addrs[i]);

			if (conf.nshares == 0)
				printf("  no repositories served\n");
			for (i = 0; i < conf.nshares; i++) {
				const struct smb_share *s = &conf.shares[i];

				printf("  %-24s %s", s->name, s->path);
				/* A served path can stop being a repository
				 * underneath graph; the filesystem is the
				 * truth, so it is checked rather than
				 * assumed. */
				if (!is_graph_repo(s->path))
					printf("  (no longer a repository)");
				if (s->user[0])
					printf("  [%s]", s->user);
				printf("\n");
			}
		}
		printf("\n");
	}

	printf("connected\n");
	mounts = each_mount(print_mount, NULL);
	if (mounts == 0)
		printf("  no repositories connected\n");
	else if (mounts < 0)
		printf("  cannot tell what is mounted\n");

	return 0;
}
