/*
 * Windows implementations of the POSIX calls rsync makes: file metadata,
 * links, users/groups and assorted odds and ends.
 *
 * Copyright (C) 2026 rsync CMake/Windows port.
 * Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
 */

#include "rsync.h"
#include "win32/win32undef.h"

#include <lmcons.h>
#include <locale.h>
#include <winioctl.h>   /* FSCTL_GET_REPARSE_POINT (WIN32_LEAN_AND_MEAN omits it) */

/* --------------------------------------------------------------- startup */

void win32_init(void)
{
	WSADATA wsa;

	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		fprintf(stderr, "rsync: WSAStartup failed\n");
		exit(1);
	}
	/* rsync's protocol is binary; never let the CRT translate newlines. */
	_setmode(0, _O_BINARY);
	_setmode(1, _O_BINARY);

	/* The embedded manifest already selects UTF-8 as the active code page;
	 * line these up with it so the CRT and the console agree. */
	setlocale(LC_ALL, ".UTF8");
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);
}

int win32_no_fork(void)
{
	errno = ENOSYS;
	return -1;
}

/*
 * rsync splits and rebuilds paths around '/', so translate the user's
 * backslashes once, here, for the local (non-remote) operands only -- a
 * remote spec's path belongs to the peer and is left untouched.  Called from
 * main() via the platform_fix_path_args() hook once popt has consumed the
 * options and argv holds just the path operands.
 */
void win32_fix_path_args(int argc, char *argv[])
{
	int i;

	for (i = 0; i < argc; i++) {
		char *host = NULL;
		int port = 0;

		if (!check_for_hostspec(argv[i], &host, &port))
			win32_normalize_path(argv[i]);
		if (host)
			free(host);
	}
}

void win32_normalize_path(char *path)
{
	char *p;

	for (p = path; *p; p++) {
		if (*p == '\\')
			*p = '/';
	}
}

/* ------------------------------------------------------------------ open */

int win32_open(const char *path, int flags, ...)
{
	va_list ap;
	int mode = 0;
	int fd;

	if (flags & _O_CREAT) {
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}

	/* These have no Windows equivalent and must not reach the CRT. */
	flags &= ~(O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC | O_NOATIME | O_NONBLOCK);
	flags |= _O_BINARY;

	fd = _open(path, flags, mode ? _S_IREAD | _S_IWRITE : 0);
	return fd;
}

/* ------------------------------------------------------------------ stat */

/* Windows reports a symlink only via a reparse point plus its tag. */
static int path_is_symlink(const char *path)
{
	WIN32_FIND_DATAA fd;
	HANDLE h;
	DWORD attrs = GetFileAttributesA(path);

	if (attrs == INVALID_FILE_ATTRIBUTES
	 || !(attrs & FILE_ATTRIBUTE_REPARSE_POINT))
		return 0;

	h = FindFirstFileA(path, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return 0;
	FindClose(h);

	return fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK
	    || fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT;
}

/* Give rsync a usable st_ino/st_nlink so that --hard-links and the
 * "file is its own basis" checks behave. */
static void fill_file_ids(const char *path, struct _stat64 *st)
{
	BY_HANDLE_FILE_INFORMATION bhfi;
	HANDLE h = CreateFileA(path, 0,
			       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			       NULL, OPEN_EXISTING,
			       FILE_FLAG_BACKUP_SEMANTICS, NULL);

	if (h == INVALID_HANDLE_VALUE)
		return;

	if (GetFileInformationByHandle(h, &bhfi)) {
		st->st_dev = (unsigned int)bhfi.dwVolumeSerialNumber;
		st->st_ino = ((unsigned __int64)bhfi.nFileIndexHigh << 32)
			   | bhfi.nFileIndexLow;
		st->st_nlink = (short)bhfi.nNumberOfLinks;
	}
	CloseHandle(h);
}

/* Trailing slashes upset _stat64 on plain files; drop all but a root's. */
static const char *trim_path(const char *path, char *buf, size_t bufsz)
{
	size_t len = strlen(path);

	if (len < 2 || len >= bufsz)
		return path;
	if (path[len - 1] != '/' && path[len - 1] != '\\')
		return path;
	/* Keep "C:/" and "/" intact. */
	if (len == 3 && path[1] == ':')
		return path;

	memcpy(buf, path, len - 1);
	buf[len - 1] = '\0';
	return buf;
}

int win32_stat64(const char *path, struct _stat64 *st)
{
	char buf[MAXPATHLEN];
	const char *p = trim_path(path, buf, sizeof buf);
	int rc = _stat64(p, st);

	if (rc == 0)
		fill_file_ids(p, st);
	return rc;
}

int win32_lstat64(const char *path, struct _stat64 *st)
{
	char buf[MAXPATHLEN];
	const char *p = trim_path(path, buf, sizeof buf);
	int rc = _stat64(p, st);

	if (rc != 0) {
		/* A symlink to a missing target still exists as a link. */
		if (path_is_symlink(p)) {
			memset(st, 0, sizeof *st);
			st->st_mode = S_IFLNK | 0777;
			st->st_nlink = 1;
			return 0;
		}
		return rc;
	}

	if (path_is_symlink(p))
		st->st_mode = (st->st_mode & ~(unsigned)_S_IFMT) | S_IFLNK;
	else
		fill_file_ids(p, st);

	return 0;
}

/* ------------------------------------------------------------------ links */

/* The reparse-point payload.  Declared here because the SDK only exposes it
 * to kernel-mode headers. */
typedef struct {
	ULONG  ReparseTag;
	USHORT ReparseDataLength;
	USHORT Reserved;
	union {
		struct {
			USHORT SubstituteNameOffset;
			USHORT SubstituteNameLength;
			USHORT PrintNameOffset;
			USHORT PrintNameLength;
			ULONG  Flags;
			WCHAR  PathBuffer[1];
		} SymbolicLinkReparseBuffer;
		struct {
			USHORT SubstituteNameOffset;
			USHORT SubstituteNameLength;
			USHORT PrintNameOffset;
			USHORT PrintNameLength;
			WCHAR  PathBuffer[1];
		} MountPointReparseBuffer;
	} u;
} RSYNC_REPARSE_BUFFER;

/*
 * Read the target *as stored in the link*, which is what rsync puts on the
 * wire.  GetFinalPathNameByHandle() would resolve the whole chain instead,
 * turning a relative link into an absolute one and following it to the end.
 */
int win32_readlink(const char *path, char *buf, size_t bufsiz)
{
	union {
		RSYNC_REPARSE_BUFFER rb;
		char space[16 * 1024];
	} data;
	HANDLE h;
	DWORD got = 0;
	const WCHAR *wname;
	USHORT woff, wlen;
	int n;

	h = CreateFileA(path, 0,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			NULL, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
			NULL);
	if (h == INVALID_HANDLE_VALUE) {
		errno = ENOENT;
		return -1;
	}

	if (!DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0,
			     &data, sizeof data, &got, NULL)) {
		CloseHandle(h);
		errno = EINVAL;     /* not a reparse point */
		return -1;
	}
	CloseHandle(h);

	/* Prefer the print name: it is the text the user wrote, without the
	 * "\??\" device prefix that the substitute name carries. */
	if (data.rb.ReparseTag == IO_REPARSE_TAG_SYMLINK) {
		woff = data.rb.u.SymbolicLinkReparseBuffer.PrintNameOffset;
		wlen = data.rb.u.SymbolicLinkReparseBuffer.PrintNameLength;
		wname = data.rb.u.SymbolicLinkReparseBuffer.PathBuffer;
		if (!wlen) {
			woff = data.rb.u.SymbolicLinkReparseBuffer.SubstituteNameOffset;
			wlen = data.rb.u.SymbolicLinkReparseBuffer.SubstituteNameLength;
		}
	} else if (data.rb.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) {
		woff = data.rb.u.MountPointReparseBuffer.PrintNameOffset;
		wlen = data.rb.u.MountPointReparseBuffer.PrintNameLength;
		wname = data.rb.u.MountPointReparseBuffer.PathBuffer;
		if (!wlen) {
			woff = data.rb.u.MountPointReparseBuffer.SubstituteNameOffset;
			wlen = data.rb.u.MountPointReparseBuffer.SubstituteNameLength;
		}
	} else {
		errno = EINVAL;
		return -1;
	}

	/* The manifest selects UTF-8, so the narrow encoding is UTF-8 too. */
	n = WideCharToMultiByte(CP_UTF8, 0, wname + woff / sizeof(WCHAR),
				wlen / sizeof(WCHAR), buf, (int)bufsiz, NULL, NULL);
	if (n <= 0) {
		errno = ENAMETOOLONG;
		return -1;
	}

	/* rsync speaks '/'; a Windows link stores '\'. */
	{
		int i;
		for (i = 0; i < n; i++) {
			if (buf[i] == '\\')
				buf[i] = '/';
		}
	}
	return n;
}

/*
 * Windows needs to know at creation time whether a symlink points at a
 * directory, and it records that in the link itself.  A relative target is
 * relative to the *link's* directory, not to our cwd, so resolve it that way
 * before asking.  Guessing wrong leaves a link that resolves to the wrong
 * kind of object.
 */
static int symlink_target_is_dir(const char *target, const char *linkpath)
{
	char probe[MAXPATHLEN];
	const char *slash;
	DWORD attrs;
	size_t dirlen;

	if (IS_ABS_PATH(target)) {
		attrs = GetFileAttributesA(target);
		return attrs != INVALID_FILE_ATTRIBUTES
		    && (attrs & FILE_ATTRIBUTE_DIRECTORY);
	}

	slash = strrchr(linkpath, '/');
	if (!slash)
		slash = strrchr(linkpath, '\\');
	dirlen = slash ? (size_t)(slash - linkpath) + 1 : 0;

	if (dirlen + strlen(target) >= sizeof probe)
		return 0;
	memcpy(probe, linkpath, dirlen);
	strcpy(probe + dirlen, target);

	attrs = GetFileAttributesA(probe);
	return attrs != INVALID_FILE_ATTRIBUTES
	    && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

int win32_symlink(const char *target, const char *linkpath)
{
	DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;

	if (symlink_target_is_dir(target, linkpath))
		flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;

	if (!CreateSymbolicLinkA(linkpath, target, flags)) {
		DWORD err = GetLastError();

		/* Creating symlinks needs Developer Mode or the
		 * SeCreateSymbolicLinkPrivilege; without either, this is the
		 * failure users will hit. */
		switch (err) {
		case ERROR_PRIVILEGE_NOT_HELD:  errno = EPERM;  break;
		case ERROR_ALREADY_EXISTS:      errno = EEXIST; break;
		case ERROR_PATH_NOT_FOUND:      errno = ENOENT; break;
		default:                        errno = EIO;    break;
		}
		return -1;
	}
	return 0;
}

int win32_link(const char *oldpath, const char *newpath)
{
	if (!CreateHardLinkA(newpath, oldpath, NULL)) {
		errno = (GetLastError() == ERROR_ALREADY_EXISTS) ? EEXIST : EIO;
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------- attributes */

int win32_chown(const char *path, uid_t uid, gid_t gid)
{
	(void)path; (void)uid; (void)gid;
	return 0;   /* no Unix ownership model to honour */
}

int win32_chmod(const char *path, mode_t mode)
{
	DWORD attrs = GetFileAttributesA(path);

	if (attrs == INVALID_FILE_ATTRIBUTES) {
		errno = ENOENT;
		return -1;
	}

	/* The only bit Windows can represent is "read-only". */
	if (mode & S_IWUSR)
		attrs &= ~(DWORD)FILE_ATTRIBUTE_READONLY;
	else
		attrs |= FILE_ATTRIBUTE_READONLY;

	if (!SetFileAttributesA(path, attrs)) {
		errno = EACCES;
		return -1;
	}
	return 0;
}

int win32_mkdir(const char *path, mode_t mode)
{
	(void)mode;
	if (_mkdir(path) != 0)
		return -1;
	return 0;
}

int win32_unlink(const char *path)
{
	DWORD attrs = GetFileAttributesA(path);

	if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
		SetFileAttributesA(path, attrs & ~(DWORD)FILE_ATTRIBUTE_READONLY);

	if (attrs != INVALID_FILE_ATTRIBUTES
	 && (attrs & FILE_ATTRIBUTE_DIRECTORY)
	 && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
		if (RemoveDirectoryA(path))
			return 0;
		errno = EACCES;
		return -1;
	}

	if (!DeleteFileA(path)) {
		switch (GetLastError()) {
		case ERROR_FILE_NOT_FOUND:
		case ERROR_PATH_NOT_FOUND: errno = ENOENT; break;
		case ERROR_ACCESS_DENIED:  errno = EACCES; break;
		default:                   errno = EIO;    break;
		}
		return -1;
	}
	return 0;
}

int win32_rename(const char *from, const char *to)
{
	/* POSIX rename() replaces the destination; MoveFile does not. */
	if (!MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
		switch (GetLastError()) {
		case ERROR_FILE_NOT_FOUND:
		case ERROR_PATH_NOT_FOUND: errno = ENOENT; break;
		case ERROR_ACCESS_DENIED:  errno = EACCES; break;
		case ERROR_NOT_SAME_DEVICE: errno = EXDEV; break;
		default:                   errno = EIO;    break;
		}
		return -1;
	}
	return 0;
}

int win32_ftruncate64(int fd, off64_t length)
{
	return _chsize_s(fd, length) == 0 ? 0 : -1;
}

int win32_fsync(int fd)
{
	HANDLE h = (HANDLE)_get_osfhandle(fd);

	if (h == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return -1;
	}
	if (!FlushFileBuffers(h)) {
		errno = EIO;
		return -1;
	}
	return 0;
}

/* --------------------------------------------------------------- mkstemp */

int win32_mkstemp64(char *tmpl)
{
	size_t len = strlen(tmpl);
	int tries;

	if (len < 6 || strcmp(tmpl + len - 6, "XXXXXX") != 0) {
		errno = EINVAL;
		return -1;
	}

	for (tries = 0; tries < 256; tries++) {
		static const char chars[] =
			"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
		unsigned int r;
		int i, fd;

		for (i = 0; i < 6; i++) {
			rand_s(&r);
			tmpl[len - 6 + i] = chars[r % (sizeof chars - 1)];
		}

		fd = _open(tmpl, _O_RDWR | _O_CREAT | _O_EXCL | _O_BINARY,
			   _S_IREAD | _S_IWRITE);
		if (fd >= 0)
			return fd;
		if (errno != EEXIST)
			return -1;
	}
	errno = EEXIST;
	return -1;
}

/* ----------------------------------------------------------------- times */

static void timeval_to_filetime(const struct timeval *tv, FILETIME *ft)
{
	/* Unix epoch -> Windows epoch, in 100ns ticks. */
	unsigned __int64 ticks =
		((unsigned __int64)tv->tv_sec + 11644473600ULL) * 10000000ULL
		+ (unsigned __int64)tv->tv_usec * 10ULL;

	ft->dwLowDateTime = (DWORD)(ticks & 0xFFFFFFFF);
	ft->dwHighDateTime = (DWORD)(ticks >> 32);
}

int win32_utimes(const char *path, const struct timeval tv[2])
{
	FILETIME atime, mtime;
	char buf[MAXPATHLEN];
	const char *p;
	size_t len;
	HANDLE h;
	BOOL ok;

	/* rsync hands us "dir/." for the transfer root, which CreateFile
	 * rejects; reduce it to the directory itself. */
	len = strlen(path);
	if (len >= 2 && path[len-1] == '.'
	 && (path[len-2] == '/' || path[len-2] == '\\')) {
		len -= (len == 2) ? 1 : 2;   /* keep "/" itself */
		if (len >= sizeof buf)
			len = sizeof buf - 1;
		memcpy(buf, path, len);
		buf[len] = '\0';
		p = buf;
	} else
		p = path;

	h = CreateFileA(p, FILE_WRITE_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		errno = (GetLastError() == ERROR_ACCESS_DENIED) ? EACCES : ENOENT;
		return -1;
	}

	timeval_to_filetime(&tv[0], &atime);
	timeval_to_filetime(&tv[1], &mtime);

	ok = SetFileTime(h, NULL, &atime, &mtime);
	CloseHandle(h);

	if (!ok) {
		errno = EACCES;
		return -1;
	}
	return 0;
}

/* Only these six are valid for the CRT's signal(); everything else rsync
 * asks for (SIGUSR1/2, SIGCHLD, SIGPIPE, ...) is silently ignored. */
win32_sighandler_t win32_signal(int sig, win32_sighandler_t handler)
{
	switch (sig) {
	case SIGABRT:
	case SIGFPE:
	case SIGILL:
	case SIGINT:
	case SIGSEGV:
	case SIGTERM:
		return (win32_sighandler_t)signal(sig, (void (*)(int))handler);
	default:
		return NULL;
	}
}

struct tm *win32_localtime_r(const time_t *timep, struct tm *result)
{
	if (localtime_s(result, timep) != 0)
		return NULL;
	return result;
}

/* Read a password with console echo turned off. */
char *win32_getpass(const char *prompt)
{
	static char buf[256];
	HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
	DWORD saved_mode = 0;
	size_t len;

	fputs(prompt, stderr);
	fflush(stderr);

	if (GetConsoleMode(in, &saved_mode))
		SetConsoleMode(in, saved_mode & ~(DWORD)ENABLE_ECHO_INPUT);

	if (!fgets(buf, sizeof buf, stdin))
		buf[0] = '\0';

	if (saved_mode)
		SetConsoleMode(in, saved_mode);

	fputs("\n", stderr);

	len = strlen(buf);
	while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[--len] = '\0';

	return buf;
}

int win32_gettimeofday(struct timeval *tv, void *tz)
{
	FILETIME ft;
	unsigned __int64 ticks;

	(void)tz;
	GetSystemTimeAsFileTime(&ft);
	ticks = ((unsigned __int64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	ticks -= 11644473600ULL * 10000000ULL;

	tv->tv_sec = (long)(ticks / 10000000ULL);
	tv->tv_usec = (long)((ticks % 10000000ULL) / 10);
	return 0;
}

unsigned int win32_sleep(unsigned int seconds)
{
	Sleep(seconds * 1000);
	return 0;
}

int win32_usleep(unsigned int usec)
{
	Sleep(usec / 1000);
	return 0;
}

int win32_gethostname(char *name, size_t len)
{
	DWORD sz = (DWORD)len;

	if (!GetComputerNameA(name, &sz)) {
		strncpy(name, "localhost", len - 1);
		name[len - 1] = '\0';
	}
	return 0;
}

/* --------------------------------------------------------- users & groups */

/* Windows has SIDs, not uid/gid.  rsync only needs a stable name for the
 * current user plus lookups that fail cleanly, so synthesise one entry. */

static char  cur_user[UNLEN + 1];
static char  empty_str[] = "";
static char *no_members[] = { NULL };

static const char *current_user(void)
{
	DWORD sz = sizeof cur_user;

	if (!cur_user[0] && !GetUserNameA(cur_user, &sz))
		strcpy(cur_user, "user");
	return cur_user;
}

struct passwd *win32_getpwuid(uid_t uid)
{
	static struct passwd pw;

	if (uid != WIN32_FAKE_UID)
		return NULL;

	pw.pw_name = (char *)current_user();
	pw.pw_passwd = empty_str;
	pw.pw_uid = WIN32_FAKE_UID;
	pw.pw_gid = WIN32_FAKE_GID;
	pw.pw_gecos = empty_str;
	pw.pw_dir = getenv("USERPROFILE") ? getenv("USERPROFILE") : empty_str;
	pw.pw_shell = empty_str;
	return &pw;
}

struct passwd *win32_getpwnam(const char *name)
{
	if (name && _stricmp(name, current_user()) == 0)
		return win32_getpwuid(WIN32_FAKE_UID);
	return NULL;
}

struct group *win32_getgrgid(gid_t gid)
{
	static struct group gr;

	if (gid != WIN32_FAKE_GID)
		return NULL;

	gr.gr_name = (char *)current_user();
	gr.gr_passwd = empty_str;
	gr.gr_gid = WIN32_FAKE_GID;
	gr.gr_mem = no_members;
	return &gr;
}

struct group *win32_getgrnam(const char *name)
{
	if (name && _stricmp(name, current_user()) == 0)
		return win32_getgrgid(WIN32_FAKE_GID);
	return NULL;
}
