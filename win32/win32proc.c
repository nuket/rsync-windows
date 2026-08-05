/*
 * Process plumbing: CreateProcess in place of fork()+exec(), plus enough of
 * waitpid()/kill() for rsync's child bookkeeping.
 *
 * rsync only needs to spawn one kind of child on the client side -- the
 * remote shell (ssh) with its stdin/stdout wired to a pipe pair -- so that
 * is what win32_piped_child() does, replacing the fork/dup2/execvp dance in
 * pipe.c wholesale.
 *
 * Copyright (C) 2026 rsync CMake/Windows port.
 * Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
 */

#include "rsync.h"
#include "win32/win32undef.h"

extern int blocking_io;

/* ------------------------------------------------------- child bookkeeping */

#define MAX_CHILDREN 16

static struct {
	pid_t  pid;
	HANDLE handle;
	int    is_thread;   /* the do_recv() receiver half; see win32fork.c */
} children[MAX_CHILDREN];

static void remember_child_kind(pid_t pid, HANDLE h, int is_thread)
{
	int i;

	for (i = 0; i < MAX_CHILDREN; i++) {
		if (!children[i].pid) {
			children[i].pid = pid;
			children[i].handle = h;
			children[i].is_thread = is_thread;
			return;
		}
	}
	CloseHandle(h);   /* table full: we lose the exit status, not the child */
}

static void remember_child(pid_t pid, HANDLE h)
{
	remember_child_kind(pid, h, 0);
}

/* Called by win32_fork_thread() for the receiver half. */
void win32_remember_thread_child(pid_t pid, HANDLE h)
{
	remember_child_kind(pid, h, 1);
}

static HANDLE find_child(pid_t pid, int *slot)
{
	int i;

	for (i = 0; i < MAX_CHILDREN; i++) {
		if (children[i].pid == pid) {
			if (slot)
				*slot = i;
			return children[i].handle;
		}
	}
	return NULL;
}

static int child_is_thread(pid_t pid)
{
	int slot = -1;

	return find_child(pid, &slot) && children[slot].is_thread;
}

/* --------------------------------------------------- command-line quoting */

/*
 * Turn an argv[] into a Windows command line.  Windows hands the raw string
 * to the child, which re-splits it using the CommandLineToArgvW rules, so we
 * have to quote to those rules: backslashes are literal except when they
 * immediately precede a quote, where they must be doubled.
 */
static char *build_command_line(char **argv)
{
	size_t cap = 256, len = 0;
	char *cmd = (char *)malloc(cap);
	int i;

	if (!cmd)
		return NULL;
	cmd[0] = '\0';

	for (i = 0; argv[i]; i++) {
		const char *a = argv[i];
		int needs_quotes = !*a || strpbrk(a, " \t\n\v\"") != NULL;
		size_t need = strlen(a) * 2 + 4;
		size_t j;
		unsigned backslashes = 0;

		if (len + need + 1 > cap) {
			char *bigger;
			while (len + need + 1 > cap)
				cap *= 2;
			bigger = (char *)realloc(cmd, cap);
			if (!bigger) {
				free(cmd);
				return NULL;
			}
			cmd = bigger;
		}

		if (i)
			cmd[len++] = ' ';
		if (needs_quotes)
			cmd[len++] = '"';

		for (j = 0; a[j]; j++) {
			if (a[j] == '\\') {
				backslashes++;
				cmd[len++] = '\\';
				continue;
			}
			if (a[j] == '"') {
				/* Double the run of backslashes, then escape. */
				for (; backslashes; backslashes--)
					cmd[len++] = '\\';
				cmd[len++] = '\\';
			}
			backslashes = 0;
			cmd[len++] = a[j];
		}

		if (needs_quotes) {
			/* Backslashes before the closing quote also double. */
			for (; backslashes; backslashes--)
				cmd[len++] = '\\';
			cmd[len++] = '"';
		}
		cmd[len] = '\0';
	}
	return cmd;
}

/* ------------------------------------------------------------ piped_child */

/* Create a pipe whose child end is inheritable and whose parent end is not. */
static int make_pipe(HANDLE *parent_end, HANDLE *child_end, int parent_reads)
{
	SECURITY_ATTRIBUTES sa;
	HANDLE rd, wr;

	sa.nLength = sizeof sa;
	sa.lpSecurityDescriptor = NULL;
	sa.bInheritHandle = TRUE;

	if (!CreatePipe(&rd, &wr, &sa, 65536))
		return -1;

	if (parent_reads) {
		*parent_end = rd;
		*child_end = wr;
	} else {
		*parent_end = wr;
		*child_end = rd;
	}

	/* The parent's end must not leak into the child. */
	if (!SetHandleInformation(*parent_end, HANDLE_FLAG_INHERIT, 0)) {
		CloseHandle(rd);
		CloseHandle(wr);
		return -1;
	}
	return 0;
}

pid_t win32_piped_child(char **command, int *f_in, int *f_out)
{
	HANDLE to_child_parent, to_child_child;
	HANDLE from_child_parent, from_child_child;
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	char *cmdline;
	int in_fd, out_fd;

	if (make_pipe(&to_child_parent, &to_child_child, 0) < 0) {
		errno = EMFILE;
		return -1;
	}
	if (make_pipe(&from_child_parent, &from_child_child, 1) < 0) {
		CloseHandle(to_child_parent);
		CloseHandle(to_child_child);
		errno = EMFILE;
		return -1;
	}

	cmdline = build_command_line(command);
	if (!cmdline) {
		CloseHandle(to_child_parent);
		CloseHandle(to_child_child);
		CloseHandle(from_child_parent);
		CloseHandle(from_child_child);
		errno = ENOMEM;
		return -1;
	}

	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = to_child_child;
	si.hStdOutput = from_child_child;
	si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

	if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
		DWORD err = GetLastError();
		free(cmdline);
		CloseHandle(to_child_parent);
		CloseHandle(to_child_child);
		CloseHandle(from_child_parent);
		CloseHandle(from_child_child);
		errno = (err == ERROR_FILE_NOT_FOUND) ? ENOENT : EIO;
		return -1;
	}
	free(cmdline);

	/* The child owns its ends now. */
	CloseHandle(to_child_child);
	CloseHandle(from_child_child);
	CloseHandle(pi.hThread);

	in_fd = _open_osfhandle((intptr_t)from_child_parent, _O_RDONLY | _O_BINARY);
	out_fd = _open_osfhandle((intptr_t)to_child_parent, _O_BINARY);
	if (in_fd < 0 || out_fd < 0) {
		CloseHandle(from_child_parent);
		CloseHandle(to_child_parent);
		CloseHandle(pi.hProcess);
		errno = EMFILE;
		return -1;
	}

	remember_child((pid_t)pi.dwProcessId, pi.hProcess);

	*f_in = in_fd;
	*f_out = out_fd;
	return (pid_t)pi.dwProcessId;
}

/* ---------------------------------------------------------------- waitpid */

pid_t win32_waitpid(pid_t pid, int *status, int options)
{
	int slot = -1;
	HANDLE h = find_child(pid, &slot);
	DWORD rc, code = 0;

	if (!h) {
		errno = ECHILD;
		return -1;
	}

	rc = WaitForSingleObject(h, (options & WNOHANG) ? 0 : INFINITE);
	if (rc == WAIT_TIMEOUT)
		return 0;
	if (rc != WAIT_OBJECT_0) {
		errno = ECHILD;
		return -1;
	}

	if (children[slot].is_thread)
		GetExitCodeThread(h, &code);
	else
		GetExitCodeProcess(h, &code);
	CloseHandle(h);
	children[slot].pid = 0;
	children[slot].handle = NULL;
	children[slot].is_thread = 0;

	/* Encode as a Unix wait status: exited normally with `code`. */
	if (status)
		*status = (int)((code & 0xff) << 8);
	return pid;
}

int win32_kill(pid_t pid, int sig)
{
	HANDLE h = find_child(pid, NULL);

	if (!h) {
		errno = ESRCH;
		return -1;
	}
	if (sig == 0)
		return 0;   /* existence check only */

	/* The generator sends SIGUSR2 to stop a receiver that would otherwise
	 * linger in read_final_goodbye().  Our receiver half returns on its
	 * own instead (see receiver_half()), so there is nothing to signal --
	 * and killing the thread outright could strand a CRT lock. */
	if (child_is_thread(pid))
		return 0;

	if (!TerminateProcess(h, 1)) {
		errno = EPERM;
		return -1;
	}
	return 0;
}
