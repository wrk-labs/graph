/* See LICENSE file for copyright and license details. */
/* graph config — configure the SMB server */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "graph.h"
#include "smb.h"

static void
usage(void)
{
	fputs("usage: graph config --address <address> [--address <address>]\n",
	    stderr);
}

/* Shared by config and serve: both configure Samba, and both must refuse
 * rather than work around a machine that cannot support them. */
int
smb_preconditions(const char *cmd)
{
	if (!smb_on_linux()) {
		warn("graph %s configures Samba and runs on Linux only.", cmd);
		warn("macOS runs its own SMB implementation rather than Samba.");
		return -1;
	}
	if (!smb_present()) {
		warn("Samba is not installed.");
		warn("Install it first: apt install samba");
		return -1;
	}
	/* Graph does not escalate privileges on its own. */
	if (geteuid() != 0) {
		warn("graph %s needs root to configure Samba.", cmd);
		warn("Run it again with sudo.");
		return -1;
	}
	return 0;
}

/* Reports what installing a configuration did, so that replacing someone
 * else's Samba configuration is never silent. */
void
smb_report(enum smb_write how)
{
	if (how != SMB_WRITE_DISPLACED)
		return;
	printf("notice: replaced the existing Samba configuration\n");
	printf("        the original is preserved at %s\n", SMB_CONF_PRESERVED);
}

int
cmd_config(int argc, char *argv[])
{
	struct smb_conf conf;
	char text[SMB_TEXT_MAX];
	char have[512];
	enum smb_write how;
	size_t i;
	int n = 0;

	/* Each run states the whole address list. What is passed is what is
	 * served, so an address is dropped by leaving it out rather than by a
	 * removal flag; that keeps the CLI to its few fixed operations. */
	for (i = 0; (int)i < argc; i++) {
		if (strcmp(argv[i], "--address") != 0) {
			usage();
			return 1;
		}
		if ((int)++i >= argc) {
			warn("--address needs an address");
			return 1;
		}
		if (n >= SMB_MAX_ADDRS) {
			warn("at most %d addresses", SMB_MAX_ADDRS);
			return 1;
		}
		n++;
	}
	if (n == 0) {
		usage();
		return 1;
	}

	if (smb_preconditions("config") < 0)
		return 1;

	/* An address this machine does not have cannot be served on. Samba
	 * accepts such a configuration and then binds nothing, so a client
	 * meets a refused connection with nothing to explain it; refusing here
	 * keeps a configuration that cannot work from being installed. */
	for (i = 0; (int)i < argc; i += 2) {
		const char *addr = argv[i + 1];

		if (smb_local_address(addr) == 1)
			continue;
		warn("%s is not an address of this machine.", addr);
		if (smb_addresses(have, sizeof(have)) == 0 && have[0])
			warn("This machine has: %s", have);
		warn("Use 0.0.0.0 to serve on every interface.");
		return 1;
	}

	/* Carry forward the shares serve established: config sets the address
	 * the server listens on, and must not disturb what is served. */
	if (smb_read(&conf) < 0) {
		warn("cannot read %s", SMB_CONF);
		return 1;
	}

	conf.naddrs = 0;
	for (i = 0; (int)i < argc; i += 2)
		snprintf(conf.addrs[conf.naddrs++], SMB_ADDR_MAX, "%s",
		    argv[i + 1]);

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

	/* Unchanged does not mean running: the address may already be on disk
	 * while the service is down. Starting is idempotent, so it costs
	 * nothing when it is already up. */
	if (how == SMB_WRITE_UNCHANGED) {
		if (smb_start() < 0) {
			warn("could not start the SMB service");
			warn("The configuration is in place but not in use.");
			return 1;
		}
		printf("already listening on");
	} else {
		if (smb_restart() < 0) {
			warn("could not restart the SMB service");
			warn("The new configuration is installed but not in use.");
			return 1;
		}
		printf("listening on");
	}
	for (i = 0; i < conf.naddrs; i++)
		printf("%s %s", i ? "," : "", conf.addrs[i]);
	printf("\n");

	if (conf.nshares == 0)
		printf("no repositories are served yet\n");

	return 0;
}
