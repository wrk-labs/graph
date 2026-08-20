/* See LICENSE file for copyright and license details. */
/* graph connect / disconnect — access a remote Graph repository */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/vfs.h>
/* The kernel reports a different superblock magic per SMB generation. A share
 * graph serves negotiates SMB2 or later (§9.6 rules out SMB1), so SMB2_MAGIC
 * is the one seen in practice; CIFS_MAGIC is kept so a share mounted by other
 * means is still recognised as connected rather than treated as an ordinary
 * directory and unmounted by mistake. */
#define CIFS_MAGIC 0xFF534D42	/* SMB1 */
#define SMB2_MAGIC 0xFE534D42	/* SMB2 and later */
#else
#include <sys/mount.h>
#include <sys/param.h>
#endif

#include "graph.h"
#include "smb.h"

/* Reads a line without echoing it, so a password never reaches the terminal,
 * the shell's history, or the process list (§12). */
static int
read_hidden(const char *prompt, char *dst, size_t size)
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
read_line(const char *prompt, char *dst, size_t size)
{
	size_t len;

	fputs(prompt, stdout);
	fflush(stdout);
	if (!fgets(dst, (int)size, stdin))
		return -1;
	len = strlen(dst);
	if (len && dst[len - 1] == '\n')
		dst[len - 1] = '\0';
	return 0;
}

/* Returns 1 if path is a mounted SMB share, 0 if it is not, -1 if it could
 * not be examined. The filesystem type is asked of the kernel rather than
 * read out of a mount table, so a directory that merely looks like a
 * mountpoint cannot be mistaken for one. */
static int
is_smb_mount(const char *path)
{
#ifdef __linux__
	struct statfs st;

	if (statfs(path, &st) < 0)
		return -1;
	/* f_type is a signed word whose width varies; comparing as an unsigned
	 * long keeps the magic from being sign-extended on either. */
	return (unsigned long)st.f_type == (unsigned long)SMB2_MAGIC ||
	    (unsigned long)st.f_type == (unsigned long)CIFS_MAGIC;
#else
	struct statfs st;

	if (statfs(path, &st) < 0)
		return -1;
	return strcmp(st.f_fstypename, "smbfs") == 0;
#endif
}

#ifdef __linux__
/* Writes credentials to a file only root can read, so they are never passed
 * as arguments where the process list would expose them. */
static int
write_credentials(char *path, size_t size, const char *user, const char *pass)
{
	int fd;
	FILE *f;

	snprintf(path, size, "/run/.graph-credentials-XXXXXX");
	if ((fd = mkstemp(path)) < 0) {
		snprintf(path, size, "/tmp/.graph-credentials-XXXXXX");
		if ((fd = mkstemp(path)) < 0)
			return -1;
	}
	if (fchmod(fd, 0600) < 0 || !(f = fdopen(fd, "w"))) {
		close(fd);
		unlink(path);
		return -1;
	}
	fprintf(f, "username=%s\npassword=%s\n", user, pass);
	if (fclose(f) != 0) {
		unlink(path);
		return -1;
	}
	return 0;
}
#endif

int
cmd_connect(int argc, char *argv[])
{
	char user[SMB_USER_MAX], pass[256];
	char source[PATH_MAX], dest[PATH_MAX];
	const char *addr = NULL, *name = NULL, *to = NULL;
	struct stat st;
	size_t i;
	int rc;

	for (i = 0; (int)i < argc; i++) {
		if (!strncmp(argv[i], "--to=", 5)) {
			to = argv[i] + 5;
		} else if (!strcmp(argv[i], "--to")) {
			if ((int)++i >= argc) {
				warn("--to needs a destination");
				return 1;
			}
			to = argv[i];
		} else if (argv[i][0] == '-') {
			fputs("usage: graph connect <address> <name>"
			    " --to=<destination>\n", stderr);
			return 1;
		} else if (!addr) {
			addr = argv[i];
		} else if (!name) {
			name = argv[i];
		} else {
			warn("connect takes one address and one name");
			return 1;
		}
	}
	if (!addr || !name || !to) {
		fputs("usage: graph connect <address> <name>"
		    " --to=<destination>\n", stderr);
		return 1;
	}
	if (!smb_addr_ok(addr)) {
		warn("\"%s\" is not a usable address.", addr);
		return 1;
	}
	if (!smb_name_ok(name)) {
		warn("\"%s\" is not a usable share name.", name);
		return 1;
	}

	/* The destination is always the user's choice; graph never picks a
	 * mount location of its own (§12). It is not created either: a
	 * mistyped path should be reported, not silently brought into being. */
	if (stat(to, &st) < 0) {
		warn("%s does not exist.", to);
		warn("Create it first, then connect to it.");
		return 1;
	}
	if (!S_ISDIR(st.st_mode)) {
		warn("%s is not a directory.", to);
		return 1;
	}
	if (!realpath(to, dest)) {
		warn("cannot resolve %s", to);
		return 1;
	}
	if (is_smb_mount(dest) == 1) {
		warn("%s already has a repository connected.", dest);
		return 1;
	}

	if (geteuid() != 0) {
		warn("graph connect needs root to mount a filesystem.");
		warn("Run it again with sudo.");
		return 1;
	}

	if (read_line("Username: ", user, sizeof(user)) < 0 ||
	    read_hidden("Password: ", pass, sizeof(pass)) < 0) {
		warn("could not read the credentials");
		return 1;
	}
	if (!user[0]) {
		warn("a username is required");
		return 1;
	}

	snprintf(source, sizeof(source), "//%s/%s", addr, name);

#ifdef __linux__
	{
		char creds[PATH_MAX];
		char opts[PATH_MAX + 128];
		char *args[8];
		const char *sudo_uid = getenv("SUDO_UID");
		const char *sudo_gid = getenv("SUDO_GID");

		if (write_credentials(creds, sizeof(creds), user, pass) < 0) {
			warn("could not store the credentials safely");
			memset(pass, 0, sizeof(pass));
			return 1;
		}
		memset(pass, 0, sizeof(pass));

		/* Mounted under the invoking user rather than root: graph is
		 * run with sudo, but the repository belongs to the person who
		 * asked for it, and must stay writable with ordinary tools
		 * (§17.2). */
		if (sudo_uid && sudo_gid)
			snprintf(opts, sizeof(opts),
			    "credentials=%s,uid=%s,gid=%s", creds, sudo_uid,
			    sudo_gid);
		else
			snprintf(opts, sizeof(opts), "credentials=%s", creds);

		args[0] = (char *)"mount";
		args[1] = (char *)"-t";
		args[2] = (char *)"cifs";
		args[3] = source;
		args[4] = dest;
		args[5] = (char *)"-o";
		args[6] = opts;
		args[7] = NULL;
		rc = smb_run(args);

		unlink(creds);
	}
#else
	{
		char url[PATH_MAX];
		char *args[4];

		/* mount_smbfs asks for the password itself, so it is never
		 * handed over as an argument. */
		memset(pass, 0, sizeof(pass));
		snprintf(url, sizeof(url), "//%s@%s/%s", user, addr, name);
		args[0] = (char *)"mount_smbfs";
		args[1] = url;
		args[2] = dest;
		args[3] = NULL;
		rc = smb_run(args);
	}
#endif

	if (rc != 0) {
		warn("could not connect %s to %s", source, dest);
		if (rc == 127)
			warn("SMB mount support is not installed"
			    " (apt install cifs-utils).");
		return 1;
	}
	printf("%s connected at %s\n", source, dest);
	return 0;
}

int
cmd_disconnect(int argc, char *argv[])
{
	char dest[PATH_MAX];
	char *args[3];
	int rc;

	if (argc != 1) {
		fputs("usage: graph disconnect <destination>\n", stderr);
		return 1;
	}
	if (!realpath(argv[0], dest)) {
		warn("cannot resolve %s", argv[0]);
		return 1;
	}

	/* Only a connected repository is released, so that an ordinary local
	 * directory cannot be unmounted by mistake (§12). */
	rc = is_smb_mount(dest);
	if (rc < 0) {
		warn("cannot examine %s: %s", dest, strerror(errno));
		return 1;
	}
	if (rc == 0) {
		warn("%s is not a connected repository.", dest);
		return 1;
	}

	if (geteuid() != 0) {
		warn("graph disconnect needs root to unmount a filesystem.");
		warn("Run it again with sudo.");
		return 1;
	}

	args[0] = (char *)"umount";
	args[1] = dest;
	args[2] = NULL;
	rc = smb_run(args);

	/* An unmount in use is reported rather than forced: forcing one on a
	 * network share can discard writes that have not been flushed (§12). */
	if (rc != 0) {
		warn("could not release %s", dest);
		warn("Something is still using it. Close it and try again.");
		return 1;
	}
	printf("%s released\n", dest);
	return 0;
}
