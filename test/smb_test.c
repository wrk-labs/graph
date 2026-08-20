/* See LICENSE file for copyright and license details. */
/* Exercises the Samba configuration module. Rewrites /etc/samba/smb.conf and
 * must only ever run inside the test container. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "smb.h"

static int failed;

static void
ok(const char *name)
{
	printf("  ok   %s\n", name);
}

static void
fail(const char *name, const char *detail)
{
	printf("  FAIL %s\n", name);
	if (detail)
		printf("       %s\n", detail);
	failed = 1;
}

static void
check(const char *name, int cond)
{
	cond ? ok(name) : fail(name, NULL);
}

static void
check_has(const char *name, const char *hay, const char *needle)
{
	check(name, strstr(hay, needle) != NULL);
}

static void
check_lacks(const char *name, const char *hay, const char *needle)
{
	check(name, strstr(hay, needle) == NULL);
}

static void
put_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");

	if (!f) {
		perror(path);
		exit(1);
	}
	fputs(text, f);
	fclose(f);
}

static char *
get_file(const char *path)
{
	static char buf[65536];
	FILE *f = fopen(path, "rb");
	size_t n;

	if (!f)
		return NULL;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	return buf;
}

/* A configuration Graph did not write, standing in for whatever was on the
 * machine before it first ran. */
static const char *foreign =
    "[global]\n\tworkgroup = OTHER\n[public]\n\tpath = /srv/public\n";

int
main(void)
{
	struct smb_conf conf;
	char text[65536];
	char other[65536];
	enum smb_write how;
	char *got;

	memset(&conf, 0, sizeof(conf));
	snprintf(conf.addrs[0], SMB_ADDR_MAX, "127.0.0.1");
	conf.naddrs = 1;
	snprintf(conf.shares[0].name, SMB_NAME_MAX, "opsys");
	snprintf(conf.shares[0].path, PATH_MAX, "/tmp/graph-test/new");
	conf.nshares = 1;

	printf("\ngenerated configuration\n");

	check("generates", smb_generate(&conf, text, sizeof(text)) == 0);
	check_has("carries the configured address", text,
	    "interfaces = 127.0.0.1");
	check_has("binds only the configured interfaces", text,
	    "bind interfaces only = yes");
	check_has("defines the share", text, "[opsys]");
	check_has("points the share at the repository", text,
	    "path = /tmp/graph-test/new");

	/* §9.6 */
	check_lacks("no home shares", text, "[homes]");
	check_lacks("no printer shares", text, "[printers]");
	check_has("no user-published shares", text, "usershare max shares = 0");
	check_has("no guest access", text, "map to guest = never");
	check_has("no SMB1", text, "server min protocol = SMB2");
	check_has("confines the share to the repository", text,
	    "wide links = no");
	check_has("a symlink is not a path out", text, "follow symlinks = no");

	/* macOS metadata belongs in the attribute, not in an AppleDouble file
	 * beside every entry in the repository. */
	check_has("supports extended attributes", text, "ea support = yes");
	check_has("carries the macOS metadata", text,
	    "vfs objects = fruit streams_xattr");
	check_has("keeps it out of the tree", text, "fruit:metadata = stream");

	/* A user is optional (§8); when absent nothing restricts the share to
	 * one, and when present it must. */
	check_lacks("no valid users without a user", text, "valid users");
	snprintf(conf.shares[0].user, SMB_USER_MAX, "will");
	check("regenerates", smb_generate(&conf, text, sizeof(text)) == 0);
	check_has("restricts the share to the given user", text,
	    "valid users = will");

	/* An address is never defaulted, so a share must never end up offered
	 * on every interface. */
	check_has("still binds only configured interfaces", text,
	    "bind interfaces only = yes");

	printf("\nevery interface\n");

	/* Naming addresses must keep the restriction in place, or a repository
	 * could be offered somewhere it was never asked to be. */
	check_has("named addresses stay restricted", text,
	    "bind interfaces only = yes");

	{
		struct smb_conf every;
		char etext[SMB_TEXT_MAX];
		char expect[SMB_TEXT_MAX];
		struct smb_conf back;

		memset(&every, 0, sizeof(every));
		snprintf(every.addrs[0], SMB_ADDR_MAX, "0.0.0.0");
		every.naddrs = 1;
		snprintf(every.shares[0].name, SMB_NAME_MAX, "opsys");
		snprintf(every.shares[0].path, PATH_MAX, "/tmp/graph-test/new");
		every.nshares = 1;

		check("recognises the request", smb_every_interface(&every));
		check("a named address is not that request",
		    !smb_every_interface(&conf));
		check("generates", smb_generate(&every, etext,
		    sizeof(etext)) == 0);
		snprintf(expect, sizeof(expect), "%s", etext);
		/* Samba cannot bind 0.0.0.0; lifting the restriction is what
		 * actually serves every interface. */
		check_has("lifts the restriction", etext,
		    "bind interfaces only = no");
		check("Samba accepts it", smb_validate(etext) == 1);

		/* The request must survive a round trip like any other. */
		check("installs", smb_install(etext, &how) == 0);
		check("reads back", smb_read(&back) == 1);
		check("recovers the request",
		    back.naddrs == 1 && !strcmp(back.addrs[0], "0.0.0.0"));
		check("still reads as every interface",
		    smb_every_interface(&back));
		/* Regenerating from what was read back must reproduce the
		 * every-interface file, not merely something valid. */
		check("regenerates identically",
		    smb_generate(&back, etext, sizeof(etext)) == 0 &&
		    !strcmp(etext, expect));
		check("differs from a named-address configuration",
		    strcmp(expect, text) != 0);
	}

	printf("\nvalidation\n");

	check("accepts a generated configuration", smb_validate(text) == 1);
	check("rejects a malformed configuration",
	    smb_validate("[global]\n\tinterfaces = 127.0.0.1\n[bad\n") == 0);

	printf("\ninstall rules\n");

	/* First run finds a configuration Graph did not write. */
	unlink(SMB_CONF_PRESERVED);
	put_file(SMB_CONF, foreign);

	check("installs over a foreign configuration",
	    smb_install(text, &how) == 0);
	check("reports what it displaced", how == SMB_WRITE_DISPLACED);
	got = get_file(SMB_CONF_PRESERVED);
	check("preserves the original", got && !strcmp(got, foreign));
	got = get_file(SMB_CONF);
	check("installs the generated configuration",
	    got && !strcmp(got, text));

	/* An identical write is skipped: a restart drops live connections. */
	check("installs again", smb_install(text, &how) == 0);
	check("skips an identical write", how == SMB_WRITE_UNCHANGED);

	/* A later run must not overwrite the preserved original. */
	conf.naddrs = 2;
	snprintf(conf.addrs[1], SMB_ADDR_MAX, "192.168.0.102");
	check("generates a changed configuration",
	    smb_generate(&conf, other, sizeof(other)) == 0);
	check("installs the change", smb_install(other, &how) == 0);
	check("reports an update, not a displacement",
	    how == SMB_WRITE_UPDATED);
	got = get_file(SMB_CONF_PRESERVED);
	check("never overwrites the preserved original",
	    got && !strcmp(got, foreign));

	printf("\nread-back\n");

	/* A first run finds no configuration and has nothing to carry. */
	unlink(SMB_CONF);
	{
		struct smb_conf back;
		check("reports nothing to carry forward on a first run",
		    smb_read(&back) == 0);
		check("carries no shares", back.nshares == 0);
		check("carries no addresses", back.naddrs == 0);
	}

	/* A configuration graph did not write must not be carried forward: its
	 * shares are not Graph repositories (§9.6). */
	put_file(SMB_CONF, foreign);
	{
		struct smb_conf back;
		check("ignores a configuration graph did not write",
		    smb_read(&back) == 0);
		check("carries no foreign shares", back.nshares == 0);
	}

	/* What graph wrote must survive a round trip, or a later serve would
	 * silently drop an earlier one. */
	check("installs a configuration to read back",
	    smb_install(other, &how) == 0);
	{
		struct smb_conf back;

		check("reads back a graph configuration", smb_read(&back) == 1);
		check("recovers both addresses", back.naddrs == 2);
		check("recovers the first address",
		    back.naddrs > 0 && !strcmp(back.addrs[0], "127.0.0.1"));
		check("recovers the second address",
		    back.naddrs > 1 && !strcmp(back.addrs[1], "192.168.0.102"));
		check("recovers the share", back.nshares == 1);
		check("recovers the share name",
		    back.nshares > 0 && !strcmp(back.shares[0].name, "opsys"));
		check("recovers the share path",
		    back.nshares > 0 &&
		    !strcmp(back.shares[0].path, "/tmp/graph-test/new"));
		check("recovers the share user",
		    back.nshares > 0 && !strcmp(back.shares[0].user, "will"));

		/* Regenerating from what was read back must reproduce the file
		 * byte for byte; anything else loses information. */
		check("regenerates identically",
		    smb_generate(&back, text, sizeof(text)) == 0 &&
		    !strcmp(text, other));
	}

	/* Serving a second repository must keep the first (§8). */
	{
		struct smb_conf back;
		char multi[65536];

		check("reads before adding", smb_read(&back) == 1);
		snprintf(back.shares[back.nshares].name, SMB_NAME_MAX, "work");
		snprintf(back.shares[back.nshares].path, PATH_MAX, "/tmp/graph-test/empty");
		back.nshares++;
		check("generates with both shares",
		    smb_generate(&back, multi, sizeof(multi)) == 0);
		check("installs both", smb_install(multi, &how) == 0);

		check("reads both back", smb_read(&back) == 1);
		check("keeps two shares", back.nshares == 2);
		check("keeps the first share",
		    back.nshares > 0 && !strcmp(back.shares[0].name, "opsys"));
		check("adds the second share",
		    back.nshares > 1 && !strcmp(back.shares[1].name, "work"));
		check("keeps the addresses", back.naddrs == 2);
		check("keeps the first share user",
		    back.nshares > 0 && !strcmp(back.shares[0].user, "will"));
		check("second share has no user",
		    back.nshares > 1 && back.shares[1].user[0] == 0);

		/* Restore the single-share configuration the next check expects. */
		check("reinstalls the single-share configuration",
		    smb_install(other, &how) == 0);
	}

	/* The safety property: an invalid configuration must never reach the
	 * service, and must leave what is installed alone. */
	check("refuses an invalid configuration",
	    smb_install("[global]\n\tinterfaces = 127.0.0.1\n[bad\n", &how) < 0);
	got = get_file(SMB_CONF);
	check("leaves the installed configuration untouched",
	    got && !strcmp(got, other));

	return failed;
}
