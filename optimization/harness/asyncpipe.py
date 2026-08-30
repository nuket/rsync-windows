import io

p = 'win32/win32proc.c'
s = io.open(p, encoding='utf-8').read()

def sub(old, new):
    global s
    assert s.count(old) == 1, (s.count(old), old[:70])
    s = s.replace(old, new)

# -------------------------------------------------------------- make_pipe
sub("""/* Create a pipe whose child end is inheritable and whose parent end is not. */
static int make_pipe(HANDLE *parent_end, HANDLE *child_end, int parent_reads)
{
	SECURITY_ATTRIBUTES sa;
	HANDLE rd, wr;

	sa.nLength = sizeof sa;
	sa.lpSecurityDescriptor = NULL;
	sa.bInheritHandle = TRUE;

	if (!CreatePipe(&rd, &wr, &sa, 65536))
		return -1;
""",
"""/*
 * Is `program` Win32-OpenSSH's ssh?  It gets its stdio pipes set up
 * differently (see make_pipe), and the difference is only safe for a child
 * that expects it, so the answer is deliberately narrow: the executable the
 * shell would run has to live in a directory with "OpenSSH" in its path,
 * which is where both the Windows feature (System32\\OpenSSH) and the
 * standalone installer (Program Files\\OpenSSH) put it.  Git's ssh.exe, plink
 * and anything else get ordinary pipes.
 */
static int is_win32_openssh(const char *program)
{
	char path[MAX_PATH], *native = NULL;
	const char *found = NULL;
	char *c;

	if (SearchPathA(NULL, program, ".exe", sizeof path, path, NULL))
		found = path;
	else if ((native = wow64_find_program(program)) != NULL)
		found = native;
	if (!found)
		return 0;

	strlcpy(path, found, sizeof path);
	free(native);
	for (c = path; *c; c++)
		*c = (char)tolower((unsigned char)*c);
	return strstr(path, "openssh") != NULL;
}

#define CHILD_PIPE_SIZE (256 * 1024)

/*
 * Create a pipe whose child end is inheritable and whose parent end is not.
 *
 * With `child_async` the child's end is opened for overlapped I/O.  That is
 * for Win32-OpenSSH: it assumes an inherited stdio pipe cannot do overlapped
 * I/O, and reads one with a thread per read -- measured at 17MB/s feeding a
 * 2.5Gbit link.  Given an overlapped handle and OPENSSH_STDIO_MODE=nonsock
 * (win32_piped_child sets it) it takes its asynchronous path instead.  Only
 * the child's end is overlapped: this side reads and writes its own end
 * synchronously, and a synchronous ReadFile on an overlapped handle is not
 * defined.
 */
static int make_pipe(HANDLE *parent_end, HANDLE *child_end, int parent_reads,
		     int child_async)
{
	static LONG counter;
	SECURITY_ATTRIBUTES sa;
	HANDLE rd, wr;

	sa.nLength = sizeof sa;
	sa.lpSecurityDescriptor = NULL;
	sa.bInheritHandle = TRUE;

	if (!child_async) {
		if (!CreatePipe(&rd, &wr, &sa, 65536))
			return -1;
	} else {
		char name[128];

		snprintf(name, sizeof name, "\\\\\\\\.\\\\pipe\\\\rsync-child-%lu-%ld",
			 (unsigned long)GetCurrentProcessId(),
			 (long)InterlockedIncrement(&counter));

		/* The parent keeps the server end, synchronous; the child's end
		 * is opened on it with FILE_FLAG_OVERLAPPED. */
		if (parent_reads) {
			rd = CreateNamedPipeA(name, PIPE_ACCESS_INBOUND,
					      PIPE_TYPE_BYTE | PIPE_WAIT, 1,
					      CHILD_PIPE_SIZE, CHILD_PIPE_SIZE, 0, &sa);
			if (rd == INVALID_HANDLE_VALUE)
				return -1;
			wr = CreateFileA(name, GENERIC_WRITE, 0, &sa, OPEN_EXISTING,
					 FILE_FLAG_OVERLAPPED, NULL);
			if (wr == INVALID_HANDLE_VALUE) {
				CloseHandle(rd);
				return -1;
			}
		} else {
			wr = CreateNamedPipeA(name, PIPE_ACCESS_OUTBOUND,
					      PIPE_TYPE_BYTE | PIPE_WAIT, 1,
					      CHILD_PIPE_SIZE, CHILD_PIPE_SIZE, 0, &sa);
			if (wr == INVALID_HANDLE_VALUE)
				return -1;
			rd = CreateFileA(name, GENERIC_READ, 0, &sa, OPEN_EXISTING,
					 FILE_FLAG_OVERLAPPED, NULL);
			if (rd == INVALID_HANDLE_VALUE) {
				CloseHandle(wr);
				return -1;
			}
		}
	}
""")

# ------------------------------------------------------ win32_piped_child
sub("""	char *cmdline;
	int in_fd, out_fd;

	if (make_pipe(&to_child_parent, &to_child_child, 0) < 0) {
		errno = EMFILE;
		return -1;
	}
	if (make_pipe(&from_child_parent, &from_child_child, 1) < 0) {""",
"""	char *cmdline;
	int in_fd, out_fd;
	int async = is_win32_openssh(command[0]);
	char saved_mode[64];
	DWORD had_mode = 0;

	if (make_pipe(&to_child_parent, &to_child_child, 0, async) < 0) {
		errno = EMFILE;
		return -1;
	}
	if (make_pipe(&from_child_parent, &from_child_child, 1, async) < 0) {""")

sub("""	si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

	if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {""",
"""	si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

	/* Tell ssh its stdio handles can do overlapped I/O (see make_pipe).
	 * The variable is only for the child, so it goes into our environment
	 * for exactly as long as CreateProcess takes to copy it. */
	if (async) {
		had_mode = GetEnvironmentVariableA("OPENSSH_STDIO_MODE",
						   saved_mode, sizeof saved_mode);
		SetEnvironmentVariableA("OPENSSH_STDIO_MODE", "nonsock");
	}

	if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {""")

sub("""		free(cmdline);
		CloseHandle(to_child_parent);
		CloseHandle(to_child_child);
		CloseHandle(from_child_parent);
		CloseHandle(from_child_child);
		errno = (err == ERROR_FILE_NOT_FOUND) ? ENOENT : EIO;
		return -1;
	}
    started:
	free(cmdline);
""",
"""		if (async)
			SetEnvironmentVariableA("OPENSSH_STDIO_MODE",
				had_mode && had_mode < sizeof saved_mode ? saved_mode : NULL);
		free(cmdline);
		CloseHandle(to_child_parent);
		CloseHandle(to_child_child);
		CloseHandle(from_child_parent);
		CloseHandle(from_child_child);
		errno = (err == ERROR_FILE_NOT_FOUND) ? ENOENT : EIO;
		return -1;
	}
    started:
	if (async)
		SetEnvironmentVariableA("OPENSSH_STDIO_MODE",
			had_mode && had_mode < sizeof saved_mode ? saved_mode : NULL);
	free(cmdline);
""")

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('async child pipes added')
