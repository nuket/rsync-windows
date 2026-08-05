/*
 * Windows replacement for pipe.c.
 *
 * This file is linked instead of pipe.c, not alongside it, so the two are
 * alternative implementations of the same small interface:
 *
 *   piped_child()          spawn the remote shell on a pipe pair
 *   local_child()          spawn the in-process server for a local copy
 *   spawn_receiver_half()  split do_recv() into generator and receiver
 *   receiver_half_finish() end the receiving half
 *   inc_recurse_when_receiving
 *
 * pipe.c does all of that with fork(); none of it exists on Windows, so the
 * remote shell goes through CreateProcess and the generator/receiver split
 * goes through a thread (win32/win32fork.c).  Keeping both versions whole,
 * rather than interleaving them with #ifdef, is what lets the shared sources
 * stay platform-agnostic.
 *
 * Copyright (C) 2026 rsync CMake/Windows port.
 * Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
 */

#include "rsync.h"
#include "win32/win32undef.h"

extern int protocol_version;
extern int am_server;
extern RSYNC_TLS int kluge_around_eof;
extern RSYNC_TLS BOOL shutting_down;

/*
 * The receiver half is a thread, so it shares the generator's heap.  With
 * incremental recursion both halves independently parse and append file-list
 * chunks after the split, which needs the private address spaces fork()
 * would have given them; in one heap they collide.  Clearing this makes the
 * whole list arrive before the split instead.
 */
int inc_recurse_when_receiving = 0;

/* ------------------------------------------------------------ piped_child */

pid_t piped_child(char **command, int *f_in, int *f_out)
{
	pid_t pid;

	if (DEBUG_GTE(CMD, 1))
		print_child_argv("opening connection using:", command);

	/* CreateProcess does the pipe setup and the exec in one step. */
	pid = win32_piped_child(command, f_in, f_out);
	if (pid == -1) {
		rsyserr(FERROR, errno, "Failed to exec %s", command[0]);
		exit_cleanup(RERR_IPC);
	}

	set_blocking(*f_out);
	return pid;
}

/* ------------------------------------------------------------ local_child */

pid_t local_child(int argc, char **argv, int *f_in, int *f_out,
		  int (*child_main)(int, char*[]))
{
	(void)argc; (void)argv; (void)f_in; (void)f_out; (void)child_main;

	rprintf(FERROR,
		"Local (non-remote) copies are not supported on Windows.\n"
		"Use a remote source or destination, e.g. user@host:/path\n");
	exit_cleanup(RERR_UNSUPPORTED);
	return -1;
}

/* -------------------------------------------------- generator/receiver split */

struct recv_half_args {
	int f_in, f_out;
	char *local_name;
	int error_pipe_r, error_pipe_w;
};

static void receiver_half_entry(void *arg)
{
	struct recv_half_args *a = (struct recv_half_args *)arg;

	receiver_half(a->f_in, a->f_out, a->local_name,
		      a->error_pipe_r, a->error_pipe_w);
}

pid_t spawn_receiver_half(int f_in, int f_out, char *local_name,
			  int error_pipe_r, int error_pipe_w)
{
	/* Outlives this call: the thread reads it after we return. */
	static struct recv_half_args args;

	args.f_in = f_in;
	args.f_out = f_out;
	args.local_name = local_name;
	args.error_pipe_r = error_pipe_r;
	args.error_pipe_w = error_pipe_w;

	return win32_fork_thread(receiver_half_entry, &args);
}

/*
 * End the receiving half.
 *
 * pipe.c's version parks in read_final_goodbye() draining keep-alives until
 * the generator sends SIGUSR2.  A thread has no such signal, so instead we
 * perform just the protocol-31 exchange that read_final_goodbye() would have
 * done -- the generator is still waiting on that final MSG_DONE -- and then
 * return, letting the generator's wait_process() collect this half.
 */
void receiver_half_finish(int f_in, int f_out)
{
	if (protocol_version >= 29) {
		uchar fnamecmp_type;
		char xname[MAXPATHLEN];
		int iflags, xlen, i;

		kluge_around_eof = -1;
		shutting_down = True;

		i = read_ndx_and_attrs(f_in, f_out, &iflags, &fnamecmp_type,
				       xname, &xlen);
		if (protocol_version >= 31 && i == NDX_DONE)
			write_int(f_out, NDX_DONE);
		io_flush(FULL_FLUSH);
	}

	/* The forked version prints this from sigusr2_handler(), because the
	 * transfer counters live in this half rather than the generator's. */
	if (!am_server)
		output_summary();
}
