/*
 * The platform_fix_path_args() hook: translate path separators in the
 * command line, and turn away the one option this port cannot honour.
 *
 * This lives apart from win32compat.c because it calls check_for_hostspec(),
 * which belongs to rsync's option parsing.  The test helpers (tls, trimslash,
 * ...) link the compat layer but not options.c, and a one-function file keeps
 * that dependency out of their way.  main() calls the hook immediately after
 * parse_arguments(), which is also the earliest point at which the options are
 * known -- so it is the natural place for a port to refuse one.
 *
 * Copyright (C) 2026 Max Vilimpoc, rsync CMake/Windows port.
 * Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
 */

#include "rsync.h"
#include "win32/win32undef.h"

extern int read_batch;

/*
 * --read-batch cannot work here, and the failure it produces without this
 * check is the worst kind: every file is written correctly and then the run
 * hangs forever on the closing handshake.
 *
 * Replaying a batch has no sender, so the generator's index stream loops back
 * to the receiver through a pipe (batch_gen_fd).  How much of that stream
 * recv_files() consumes depends on incremental recursion -- and this port has
 * to force that off while receiving, because the receiver is a thread rather
 * than a forked process and the two halves would otherwise append file-list
 * chunks to one shared heap (see inc_recurse_when_receiving in win32pipe.c).
 * With it off, the receiver stops reading batch_gen_fd at the first negative
 * index, so the generator's del-stats marker is left unread and each half
 * ends up waiting for the other.
 *
 * Writing a batch is unaffected and fully supported: --write-batch and
 * --only-write-batch produce a correct batch file, and any rsync that can
 * fork -- including this one running on the far end of an ssh transfer -- can
 * replay it.
 */
static void refuse_read_batch(void)
{
	if (!read_batch)
		return;

	/* One call per line: rprintf() escapes an embedded newline as \#012,
	 * so a single multi-line string comes out unreadable. */
	rprintf(FERROR, "--read-batch is not supported by this Windows build.\n");
	rprintf(FERROR, "Replaying a batch needs the generator and receiver to be separate\n");
	rprintf(FERROR, "processes; here the receiver is a thread sharing the generator's memory.\n");
	rprintf(FERROR, "--write-batch does work, and an rsync that forks can replay its output.\n");
	exit_cleanup(RERR_SYNTAX);
}

/*
 * rsync splits and rebuilds paths around '/', so translate the user's
 * backslashes once, here, for the local (non-remote) operands only -- a
 * remote spec's path belongs to the peer and is left untouched.  main()
 * calls this once popt has consumed the options and argv holds just the
 * path operands.
 */
void win32_fix_path_args(int argc, char *argv[])
{
	int i;

	refuse_read_batch();

	for (i = 0; i < argc; i++) {
		char *host = NULL;
		int port = 0;

		if (!check_for_hostspec(argv[i], &host, &port))
			win32_normalize_path(argv[i]);
		if (host)
			free(host);
	}
}
