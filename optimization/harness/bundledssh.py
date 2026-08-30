import io

p = 'C:/Users/Claude/devsrc/rsync-windows/win32/win32proc.c'
s = io.open(p, encoding='utf-8').read()

def sub(old, new):
    global s
    assert s.count(old) == 1, (s.count(old), old[:70])
    s = s.replace(old, new)

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
 * The ssh.exe that ships beside rsync.exe, if one does.
 *
 * Windows' own OpenSSH client reads a pipe on its stdin 3KB at a time with a
 * thread per read, which holds a push from this machine at about 17MB/s
 * however fast the link is (WINDOWS-PORT.md, "Moar Speed!").  The release
 * carries a build of the same client with that fixed, next to rsync.exe.
 * A bare "ssh" -- the default remote shell, or -e ssh -- resolves to it
 * when it is there; a remote shell given with a path is used as given.
 */
static char *bundled_ssh(const char *program)
{
	static char path[MAXPATHLEN];
	static int looked;
	const char *base;

	if (!program || strpbrk(program, "\\\\/"))
		return NULL;
	if (_stricmp(program, "ssh") != 0 && _stricmp(program, "ssh.exe") != 0)
		return NULL;

	if (!looked) {
		DWORD n = GetModuleFileNameA(NULL, path, sizeof path);

		looked = 1;
		if (!n || n >= sizeof path
		 || !(base = strrchr(path, '\\\\'))
		 || (size_t)(base + 1 - path) + sizeof "ssh.exe" > sizeof path) {
			path[0] = '\\0';
		} else {
			strcpy((char *)base + 1, "ssh.exe");
			if (!is_file(path))
				path[0] = '\\0';
		}
	}
	return path[0] ? path : NULL;
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
""")

sub("""	if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
		DWORD err = GetLastError();
		char *native = NULL;
""",
"""	/* Naming the image leaves cmdline, and so the child's argv, as given. */
	if (!CreateProcessA(bundled_ssh(command[0]), cmdline, NULL, NULL, TRUE, 0,
			    NULL, NULL, &si, &pi)) {
		DWORD err = GetLastError();
		char *native = NULL;
""")

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('bundled ssh + 1MB pipes added')
