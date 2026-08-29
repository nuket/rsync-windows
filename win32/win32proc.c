/*
 * Process plumbing: CreateProcess in place of fork()+exec(), plus enough of
 * waitpid()/kill() for rsync's child bookkeeping.
 *
 * rsync only needs to spawn one kind of child on the client side -- the
 * remote shell (ssh) with its stdin/stdout wired to a pipe pair -- so that
 * is what win32_piped_child() does, replacing the fork/dup2/execvp dance in
 * pipe.c wholesale.
 *
 * Copyright (C) 2026 Max Vilimpoc, rsync CMake/Windows port.
 * Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
 */

#include "rsync.h"
#include "win32/win32undef.h"
#include "win32/win32shmpipe.h"

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

/* --------------------------------------------------- command-line quoting */

/*
 * Turn an argv[] into a Windows command line.  Windows hands the raw string
 * to the child, which re-splits it using the CommandLineToArgvW rules, so we
 * have to quote to those rules: backslashes are literal except when they
 * immediately precede a quote, where they must be doubled.
 */
/* --------------------------------------------------- WOW64 program lookup */

/*
 * On 64-bit Windows a 32-bit process sees %WINDIR%\System32 redirected to
 * %WINDIR%\SysWOW64 -- the WOW64 File System Redirector.  Windows' own
 * OpenSSH lives in System32\OpenSSH and has no SysWOW64 counterpart, so
 * rsync-x86.exe searching PATH for "ssh" finds nothing and every remote
 * transfer fails with "Failed to exec ssh: No such file or directory",
 * while the x64 build of the same source works.
 *
 * %WINDIR%\Sysnative is the escape hatch: a virtual directory that exists
 * only for a 32-bit process on 64-bit Windows and names the real System32.
 * The lookup below is retried through it -- and only after the ordinary
 * launch has already failed to find the program, so nothing that works
 * today reaches this code at all.
 */

static int under_wow64(void)
{
	BOOL wow = FALSE;

	/* Failing counts as "no": a 64-bit process, or 32-bit on 32-bit
	 * Windows, has no redirector to defeat. */
	return IsWow64Process(GetCurrentProcess(), &wow) && wow;
}

/* Strip trailing slashes in place, returning the new length. */
static size_t rstrip_slashes(char *s, size_t len)
{
	while (len && (s[len-1] == '\\' || s[len-1] == '/'))
		s[--len] = '\0';
	return len;
}

/* Does `dir` name the redirected System32, so that Sysnative reaches the real
 * one?  Case-insensitive: PATH spells it however it likes. */
static int is_system32_dir(const char *dir, const char *windir, size_t wdlen)
{
	static const char sys32[] = "\\system32";
	char after;

	if (_strnicmp(dir, windir, wdlen) != 0)
		return 0;
	if (_strnicmp(dir + wdlen, sys32, sizeof sys32 - 1) != 0)
		return 0;
	after = dir[wdlen + sizeof sys32 - 1];
	return after == '\0' || after == '\\' || after == '/';
}

static int is_file(const char *path)
{
	DWORD a = GetFileAttributesA(path);

	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* dir + "\" + name, and again with ".exe" when name carries no extension --
 * the search CreateProcess would have done.  Returns a malloc'd path that
 * exists, or NULL. */
static char *try_join(const char *dir, const char *name)
{
	static const char *const exts[] = { "", ".exe" };
	int has_ext = strchr(name, '.') != NULL;
	unsigned e;

	for (e = 0; e < sizeof exts / sizeof *exts; e++) {
		char *p;
		size_t need;

		if (*exts[e] && has_ext)
			continue;
		need = strlen(dir) + 1 + strlen(name) + strlen(exts[e]) + 1;
		if (!(p = (char *)malloc(need)))
			return NULL;
		snprintf(p, need, "%s\\%s%s", dir, name, exts[e]);
		if (is_file(p))
			return p;
		free(p);
	}
	return NULL;
}

/*
 * Find `prog` the way the ordinary launch would have, but through Sysnative.
 * Returns a malloc'd absolute path for the caller to free, or NULL when this
 * is not the problem -- which includes every case on a 64-bit build.
 */
static char *wow64_find_program(const char *prog)
{
	char windir[MAXPATHLEN], sysnative[MAXPATHLEN];
	size_t wdlen, snlen;
	const char *path, *p;
	int n;

	if (!prog || !*prog || !under_wow64())
		return NULL;

	wdlen = (size_t)GetWindowsDirectoryA(windir, sizeof windir);
	if (!wdlen || wdlen >= sizeof windir)
		return NULL;
	wdlen = rstrip_slashes(windir, wdlen);

	n = snprintf(sysnative, sizeof sysnative, "%s\\Sysnative", windir);
	if (n < 0 || (size_t)n >= sizeof sysnative)
		return NULL;
	snlen = (size_t)n;

	/* An explicit path into System32 -- "C:\Windows\System32\OpenSSH\ssh.exe"
	 * out of RSYNC_RSH, say -- is redirected just the same, so rewrite its
	 * prefix rather than searching for it. */
	if (is_system32_dir(prog, windir, wdlen)) {
		const char *rest = prog + wdlen + sizeof "\\system32" - 1;
		size_t need = snlen + strlen(rest) + 1;
		char *full = (char *)malloc(need);

		if (!full)
			return NULL;
		snprintf(full, need, "%s%s", sysnative, rest);
		if (is_file(full))
			return full;
		free(full);
		return NULL;
	}

	/* Anything else carrying a directory is not a PATH lookup. */
	if (strpbrk(prog, "\\/"))
		return NULL;

	if (!(path = getenv("PATH")))
		return NULL;

	for (p = path; *p; ) {
		const char *end = strchr(p, ';');
		size_t dlen = end ? (size_t)(end - p) : strlen(p);
		char dir[MAXPATHLEN], alt[MAXPATHLEN];

		if (dlen && dlen < sizeof dir) {
			memcpy(dir, p, dlen);
			dir[dlen] = '\0';
			dlen = rstrip_slashes(dir, dlen);
			if (is_system32_dir(dir, windir, wdlen)) {
				/* The same directory, reached unredirected. */
				n = snprintf(alt, sizeof alt, "%s%s", sysnative,
					     dir + wdlen + sizeof "\\system32" - 1);
				if (n >= 0 && (size_t)n < sizeof alt) {
					char *hit = try_join(alt, prog);
					if (hit)
						return hit;
				}
			}
		}
		if (!end)
			break;
		p = end + 1;
	}
	return NULL;
}

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

/*
 * The ssh.exe that ships beside rsync.exe, if one does.
 *
 * Windows' own OpenSSH client reads a pipe on its stdin 3KB at a time with a
 * thread per read, which holds a push from this machine at about 17MB/s
 * however fast the link is (WINDOWS-PORT.md, "Moar Speed!").  The release
 * carries a build of the same client with that fixed, next to rsync.exe.
 * A bare "ssh" -- the default remote shell, or -e ssh -- resolves to it
 * when it is there; a remote shell given with a path is used as given.
 */
/* The path of the ssh.exe beside rsync.exe, or NULL if there is none. */
static char *bundled_ssh_path(void)
{
	static char path[MAXPATHLEN];
	static int looked;
	const char *base;

	if (!looked) {
		DWORD n = GetModuleFileNameA(NULL, path, sizeof path);

		looked = 1;
		if (!n || n >= sizeof path
		 || !(base = strrchr(path, '\\'))
		 || (size_t)(base + 1 - path) + sizeof "ssh.exe" > sizeof path) {
			path[0] = '\0';
		} else {
			strcpy((char *)base + 1, "ssh.exe");
			if (!is_file(path))
				path[0] = '\0';
		}
	}
	return path[0] ? path : NULL;
}

static char *bundled_ssh(const char *program)
{
	if (!program || strpbrk(program, "\\/"))
		return NULL;
	if (_stricmp(program, "ssh") != 0 && _stricmp(program, "ssh.exe") != 0)
		return NULL;
	return bundled_ssh_path();
}

/* Is this remote shell our own ssh.exe -- the only one that knows about the
 * shared-memory rings?  A bare "ssh" resolves to it, and so does the same
 * file named with a path, which is how a script that wants a particular
 * build (or a measurement that pins the cipher) usually spells it. */
static int is_bundled_ssh(const char *program)
{
	char full[MAXPATHLEN];
	const char *ours = bundled_ssh_path();
	DWORD n;

	if (!ours)
		return 0;
	if (bundled_ssh(program))
		return 1;
	if (!program || !*program)
		return 0;
	n = GetFullPathNameA(program, sizeof full, full, NULL);
	if (!n || n >= sizeof full)
		return 0;
	if (_stricmp(full, ours) == 0)
		return 1;
	/* "...\ssh" with the extension left off */
	if (strlen(full) + sizeof ".exe" <= sizeof full) {
		strcat(full, ".exe");
		if (_stricmp(full, ours) == 0)
			return 1;
	}
	return 0;
}

/* Create a pipe whose child end is inheritable and whose parent end is not.
 * 1MB rather than the 64KB default: the bulk data of a transfer goes through
 * here in 64KB writes, and a larger buffer lets the reader fall behind by a
 * few of them without the writer stalling. */
static int make_pipe(HANDLE *parent_end, HANDLE *child_end, int parent_reads)
{
	SECURITY_ATTRIBUTES sa;
	HANDLE rd, wr;

	sa.nLength = sizeof sa;
	sa.lpSecurityDescriptor = NULL;
	sa.bInheritHandle = TRUE;

	if (!CreatePipe(&rd, &wr, &sa, 1024 * 1024))
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

/* ------------------------------------------------------- shared-memory ring */

/*
 * The bulk of a transfer crosses to ssh through the pipes above, and the
 * kernel copies every byte twice to get it there.  When the child is the
 * ssh.exe we ship -- the only one that knows about any of this -- offer it a
 * pair of shared-memory rings instead (win32shmpipe.h): the section and its
 * events are inheritable, so the child needs nothing but their handle
 * numbers, which go in the environment.  Everything stays as it was if the
 * child does not answer: the pipes are created either way and the fds are
 * the same fds.
 */
/*
 * One ring's worth of buffering, per direction.  Measured on its own the
 * ring holds ~22.5 GB/s anywhere from 128 KB to 2 MB, gives up 5% at 4 MB
 * and falls to 13.8 GB/s at 8 MB, which is this machine's 6 MB L3 showing
 * through: past it, every byte is copied out of memory rather than cache.
 * End to end the difference is under 2% either way, since only the busy
 * direction's ring is ever touched -- but 1 MB is free, is what the pipe it
 * replaced held, and leaves the cliff a long way off.
 */
#define SHM_RING_BYTES  (1024 * 1024)
#define SHM_ENV         "RSYNC_WIN32_SHMPIPE"
#define SHM_READY_MS    5000

/* RSYNC_WIN32_SHMPIPE_DEBUG=1 says on stderr which transport was settled on.
 * Not rprintf(): this file is linked into the C test helpers, which supply
 * no logging symbols. */
/*
 * How big each ring is.  The copy in and the copy out are what is left of
 * the hop's cost, and they run at cache speed or memory speed depending on
 * whether the two rings fit alongside everything else in L3 -- 30 GB/s
 * against 13 GB/s measured on this machine -- so the size is worth being
 * able to move without a rebuild.  RSYNC_WIN32_SHMPIPE_KB overrides it.
 */
static size_t shm_ring_bytes(void)
{
	const char *kb = getenv(SHM_ENV "_KB");
	long n = kb && *kb ? atol(kb) : 0;

	if (n < 16 || n > 64 * 1024)
		return SHM_RING_BYTES;
	return (size_t)n * 1024;
}

static int shm_disabled(void)
{
	const char *off = getenv("RSYNC_WIN32_NO_SHMPIPE");

	return off && *off && *off != '0';
}

static void shm_debug(const char *what)
{
	const char *on = getenv(SHM_ENV "_DEBUG");

	if (on && *on && *on != '0')
		fprintf(stderr, "rsync: shmpipe: %s\n", what);
}

/*
 * "to-child|from-child|us", the two rings as shmpipe_spec() gives them and
 * an inheritable handle to this process.  The child needs the last one for
 * the same reason a pipe needs no such thing: if we die without closing, it
 * has to find out somehow, and a section never breaks.
 */
static char *shm_env_value(struct shmpipe *to_child, struct shmpipe *from_child)
{
	static char buf[384];
	HANDLE self = NULL;

	if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(),
			     GetCurrentProcess(), &self, SYNCHRONIZE, TRUE, 0))
		self = NULL;
	snprintf(buf, sizeof buf, "%s|%s|%llu",
		 shmpipe_spec(to_child), shmpipe_spec(from_child),
		 (unsigned long long)(uintptr_t)self);
	return buf;
}

pid_t win32_piped_child(char **command, int *f_in, int *f_out)
{
	HANDLE to_child_parent, to_child_child;
	HANDLE from_child_parent, from_child_child;
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	struct shmpipe *shm_to = NULL, *shm_from = NULL;
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

	/* Only for our own ssh.exe: anything else would inherit the handles,
	 * ignore the variable, and cost us the handshake wait for nothing.
	 * RSYNC_WIN32_NO_SHMPIPE=1 keeps the pipes, for measuring one against
	 * the other and as a way out if a ring ever misbehaves. */
	if (is_bundled_ssh(command[0]) && !shm_disabled()) {
		size_t bytes = shm_ring_bytes();

		if (shmpipe_create(&shm_to, bytes) < 0)
			shm_to = NULL;
		else if (shmpipe_create(&shm_from, bytes) < 0) {
			shmpipe_free(shm_to);
			shm_to = NULL;
		}
		if (shm_to)
			SetEnvironmentVariableA(SHM_ENV,
						shm_env_value(shm_to, shm_from));
	}

	/* Naming the image leaves cmdline, and so the child's argv, as given. */
	if (!CreateProcessA(bundled_ssh(command[0]), cmdline, NULL, NULL, TRUE, 0,
			    NULL, NULL, &si, &pi)) {
		DWORD err = GetLastError();
		char *native = NULL;

		/* Not found, and we are a 32-bit process on 64-bit Windows: the
		 * program may be one the WOW64 redirector hid from us (ssh is,
		 * since Windows keeps it in System32\OpenSSH).  Name the image
		 * explicitly via Sysnative and try once more.  cmdline is left
		 * alone, so the child still sees the argv it was given.
		 *
		 * Silent by design: this file is linked into the C test helpers,
		 * which supply no logging symbols, so reaching for rprintf() here
		 * would break every one of them at link time.  piped_child() in
		 * win32pipe.c already prints the argv under --debug=CMD. */
		if (err == ERROR_FILE_NOT_FOUND
		 && (native = wow64_find_program(command[0])) != NULL) {
			if (CreateProcessA(native, cmdline, NULL, NULL, TRUE, 0,
					   NULL, NULL, &si, &pi)) {
				free(native);
				goto started;
			}
			err = GetLastError();
			free(native);
		}

		free(cmdline);
		SetEnvironmentVariableA(SHM_ENV, NULL);
		shmpipe_free(shm_to);
		shmpipe_free(shm_from);
		CloseHandle(to_child_parent);
		CloseHandle(to_child_child);
		CloseHandle(from_child_parent);
		CloseHandle(from_child_child);
		errno = (err == ERROR_FILE_NOT_FOUND) ? ENOENT : EIO;
		return -1;
	}
    started:
	free(cmdline);
	SetEnvironmentVariableA(SHM_ENV, NULL);

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

	/*
	 * The handshake.  Our ssh marks the rings ready as its first act, so
	 * this normally costs a couple of milliseconds; the process wait is
	 * there so an ssh that dies immediately -- a bad host, a missing key --
	 * does not cost the full timeout.  Saying go only after seeing ready
	 * is what keeps both ends on the same transport.
	 */
	if (shm_to) {
		DWORD waited = 0;
		int ok = 0;

		for (;;) {
			if (shmpipe_wait_ready(shm_to, 0) == 0) {
				ok = 1;
				break;
			}
			if (waited >= SHM_READY_MS)
				break;
			if (WaitForSingleObject(pi.hProcess, 5) == WAIT_OBJECT_0) {
				ok = shmpipe_wait_ready(shm_to, 0) == 0;
				break;
			}
			waited += 5;
		}
		shm_debug(ok ? "child answered; using shared memory"
			     : "no answer from the child; using pipes");
		if (ok) {
			shmpipe_mark_go(shm_to);
			shmpipe_mark_go(shm_from);
			shmpipe_set_peer(shm_to, pi.hProcess);
			shmpipe_set_peer(shm_from, pi.hProcess);
			win32_shm_attach(in_fd, shm_from, 0);
			win32_shm_attach(out_fd, shm_to, 1);
		} else {
			shmpipe_free(shm_to);
			shmpipe_free(shm_from);
		}
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
	int slot = -1;
	HANDLE h = find_child(pid, &slot);

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
	if (children[slot].is_thread)
		return 0;

	if (!TerminateProcess(h, 1)) {
		errno = EPERM;
		return -1;
	}
	return 0;
}
