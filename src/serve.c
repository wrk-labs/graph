/* See LICENSE file for copyright and license details. */
/* graph serve — expose a Graph repository through SMB */

#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "graph.h"
#include "smb.h"

static void
usage(void)
{
	fputs("usage: graph serve <path> --name <name> [--user <user>]\n",
	    stderr);
}

/* Reads a password without echoing it. It is never passed as an argument, so
 * it does not reach the process list or the shell's history (§10). */
static int
read_password(const char *prompt, char *dst, size_t size)
{
	struct termios old, quiet;
	int have_tty;
	size_t len;

	fputs(prompt, stdout);
	fflush(stdout);

	have_tty = tcgetattr(STDIN_FILENO, &old) == 0;
	if (have_tty) {
		quiet = old;
		quiet.c_lflag &= ~(tcflag_t)ECHO;
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
	}
	if (!fgets(dst, (int)size, stdin)) {
		if (have_tty)
			tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
		return -1;
	}
	if (have_tty) {
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
		fputc('\n', stdout);
	}
	len = strlen(dst);
	if (len && dst[len - 1] == '\n')
		dst[len - 1] = '\0';
	return 0;
}

static int
confirm(const char *prompt)
{
	char line[16];

	printf("%s [Y/n] ", prompt);
	fflush(stdout);
	if (!fgets(line, sizeof(line), stdin))
		return 0;
	return line[0] == '\n' || line[0] == 'y' || line[0] == 'Y';
}

/* Ensures user exists in Samba, creating it only with the operator's consent.
 * Samba remains the authority: graph inspects the current state rather than
 * tracking users of its own (§10). */
static int
ensure_user(const char *user)
{
	char pass[256], again[256];
	int rc;

	if (!getpwnam(user)) {
		warn("%s has no account on this machine.", user);
		warn("Samba users are built on system accounts.");
		warn("Create one first, then run graph serve again.");
		return -1;
	}

	if (smb_user_exists(user)) {
		/* An existing user is reused as it is: graph must not modify a
		 * password it was not asked to change (§10). */
		printf("SMB user \"%s\" found\n", user);
		return 0;
	}

	printf("SMB user \"%s\" does not exist.\n", user);
	if (!confirm("Create it?")) {
		warn("not creating %s; nothing was changed", user);
		return -1;
	}

	if (read_password("Password: ", pass, sizeof(pass)) < 0 ||
	    read_password("Repeat: ", again, sizeof(again)) < 0) {
		warn("could not read the password");
		return -1;
	}
	if (strcmp(pass, again) != 0) {
		warn("the passwords do not match; nothing was changed");
		memset(pass, 0, sizeof(pass));
		memset(again, 0, sizeof(again));
		return -1;
	}
	if (!pass[0]) {
		warn("an empty password is not accepted");
		memset(pass, 0, sizeof(pass));
		memset(again, 0, sizeof(again));
		return -1;
	}

	rc = smb_user_create(user, pass);
	memset(pass, 0, sizeof(pass));
	memset(again, 0, sizeof(again));
	if (rc < 0) {
		warn("could not create the SMB user %s", user);
		return -1;
	}
	printf("SMB user \"%s\" created\n", user);
	return 0;
}

int
cmd_serve(int argc, char *argv[])
{
	struct smb_conf conf;
	struct smb_share *share;
	char text[SMB_TEXT_MAX];
	char root[PATH_MAX];
	const char *path = NULL, *name = NULL, *user = NULL;
	enum smb_write how;
	size_t i;

	for (i = 0; (int)i < argc; i++) {
		if (!strcmp(argv[i], "--name") || !strcmp(argv[i], "--user")) {
			const char **slot = !strcmp(argv[i], "--name") ?
			    &name : &user;

			if ((int)++i >= argc) {
				warn("%s needs a value", argv[i - 1]);
				return 1;
			}
			*slot = argv[i];
		} else if (argv[i][0] == '-') {
			usage();
			return 1;
		} else if (path) {
			warn("serve takes one path");
			return 1;
		} else {
			path = argv[i];
		}
	}
	if (!path || !name) {
		usage();
		return 1;
	}
	if (!smb_name_ok(name)) {
		warn("\"%s\" is not a usable share name.", name);
		warn("Use letters, digits, dot, dash and underscore.");
		return 1;
	}
	if (user && !smb_name_ok(user)) {
		warn("\"%s\" is not a usable user name.", user);
		return 1;
	}

	if (smb_preconditions("serve") < 0)
		return 1;

	/* graph must never expose a directory that is not a repository, and
	 * must not initialize one during serve (§8). */
	if (!is_graph_repo(path)) {
		warn("%s is not a Graph repository.", path);
		warn("Create one with: graph init %s", path);
		return 1;
	}
	/* Samba needs an absolute path, and the one given may be relative. */
	if (!realpath(path, root)) {
		warn("cannot resolve %s", path);
		return 1;
	}

	/* The share sections graph generates depend on Samba's VFS modules.
	 * Without them a share is served but refuses every connection, so it
	 * is treated like any other missing prerequisite (§8). */
	{
		char missing[256];

		if (smb_vfs_modules(missing, sizeof(missing)) == 0) {
			warn("Samba is missing the VFS modules graph needs: %s",
			    missing);
			warn("Without them a share is offered but cannot be");
			warn("connected to. Install them first:");
			warn("  apt install samba-vfs-modules");
			return 1;
		}
	}

	if (smb_read(&conf) < 0) {
		warn("cannot read %s", SMB_CONF);
		return 1;
	}
	/* No address is ever assumed. Samba's own default is every interface,
	 * and exposure must always be an explicit decision (§8). */
	if (conf.naddrs == 0) {
		warn("no listening address is configured.");
		warn("Configure one first: graph config --address <address>");
		return 1;
	}

	share = NULL;
	for (i = 0; i < conf.nshares; i++) {
		if (strcmp(conf.shares[i].name, name) != 0)
			continue;
		/* A share name is a stable SMB identity (§8). Repointing one
		 * would swap the filesystem under a client that has it
		 * mounted, so the name is refused rather than moved. */
		if (strcmp(conf.shares[i].path, root) != 0) {
			warn("the name \"%s\" already serves %s.", name,
			    conf.shares[i].path);
			warn("Choose another name.");
			return 1;
		}
		share = &conf.shares[i];
		break;
	}
	if (!share) {
		if (conf.nshares >= SMB_MAX_SHARES) {
			warn("at most %d repositories can be served",
			    SMB_MAX_SHARES);
			return 1;
		}
		share = &conf.shares[conf.nshares++];
		memset(share, 0, sizeof(*share));
		snprintf(share->name, SMB_NAME_MAX, "%s", name);
		snprintf(share->path, PATH_MAX, "%s", root);
	}

	if (user) {
		if (ensure_user(user) < 0)
			return 1;
		snprintf(share->user, SMB_USER_MAX, "%s", user);
	}

	if (smb_generate(&conf, text, sizeof(text)) < 0) {
		warn("configuration too large");
		return 1;
	}
	if (smb_install(text, &how) < 0) {
		warn("cannot install %s", SMB_CONF);
		warn("The configuration Samba is using has not been changed.");
		return 1;
	}
	smb_report(how);

	/* An unchanged configuration says nothing about whether the service is
	 * running. After a reboot with the unit disabled it is not, and the
	 * share exists only on disk, so serving means starting it rather than
	 * reporting success and leaving it down. */
	if (how == SMB_WRITE_UNCHANGED) {
		if (smb_start() < 0) {
			warn("could not start the SMB service");
			warn("%s is configured but is not being served.", name);
			return 1;
		}
		printf("%s already serves %s\n", name, root);
	} else {
		if (smb_restart() < 0) {
			warn("could not restart the SMB service");
			warn("The new configuration is installed but not in"
			    " use.");
			return 1;
		}
		printf("%s serves %s\n", name, root);
	}
	for (i = 0; i < conf.naddrs; i++)
		printf("  %s\\%s\n", conf.addrs[i], name);
	return 0;
}

int
cmd_unserve(int argc, char *argv[])
{
	struct smb_conf conf;
	char text[SMB_TEXT_MAX];
	const char *name;
	enum smb_write how;
	size_t i, kept;
	int found = 0;

	if (argc != 1) {
		fputs("usage: graph unserve <name>\n", stderr);
		return 1;
	}
	name = argv[0];

	if (smb_preconditions("unserve") < 0)
		return 1;

	if (smb_read(&conf) < 0) {
		warn("cannot read %s", SMB_CONF);
		return 1;
	}

	for (i = 0, kept = 0; i < conf.nshares; i++) {
		if (!strcmp(conf.shares[i].name, name)) {
			found = 1;
			continue;
		}
		if (kept != i)
			conf.shares[kept] = conf.shares[i];
		kept++;
	}
	if (!found) {
		warn("%s is not served.", name);
		return 1;
	}
	conf.nshares = kept;

	if (smb_generate(&conf, text, sizeof(text)) < 0) {
		warn("configuration too large");
		return 1;
	}
	if (smb_install(text, &how) < 0) {
		warn("cannot install %s", SMB_CONF);
		warn("The configuration Samba is using has not been changed.");
		return 1;
	}
	smb_report(how);

	/* The share is gone from the configuration by this point, so it is
	 * reported before anything that can still fail. What follows concerns
	 * the running service, not what is configured, and saying nothing here
	 * would leave a failure below looking as though nothing happened.
	 *
	 * The repository itself is untouched: unserve withdraws a share, not
	 * data. The SMB user is left alone too, since Samba owns users and
	 * graph must not remove one it did not create (§10). */
	printf("%s no longer served\n", name);

	if (conf.nshares > 0) {
		if (smb_restart() < 0) {
			warn("could not restart the SMB service");
			warn("%s may still be served until it restarts.", name);
			return 1;
		}
		return 0;
	}

	/* Nothing remains to offer, so the service is stopped rather than left
	 * listening on an empty configuration. */
	if (smb_stop() < 0) {
		warn("could not stop the SMB service");
		warn("No repositories remain configured, but the service is"
		    " still running.");
		return 1;
	}
	printf("no repositories remain; SMB stopped\n");
	return 0;
}
