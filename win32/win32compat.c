/*
 * Windows implementations of the POSIX calls rsync makes: file metadata,
 * links, users/groups and assorted odds and ends.
 *
 * Copyright (C) 2026 Max Vilimpoc, rsync CMake/Windows port.
 * Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
 */

#include "rsync.h"
#include "win32/win32undef.h"

#include <lmcons.h>
#include <locale.h>

/* --------------------------------------------------------------- startup */

void win32_init(void)
{
	WSADATA wsa;

	/* First, so that the policies are in force before anything below can
	 * cause a DLL to be loaded -- WSAStartup pulls in the Winsock catalog. */
	win32_harden();

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

	/* Must precede any stat() call, which is what records links. */
	win32_links_init();
}

int win32_no_fork(void)
{
	errno = ENOSYS;
	return -1;
}

void win32_normalize_path(char *path)
{
	char *p;

	for (p = path; *p; p++) {
		if (*p == '\\')
			*p = '/';
	}
}

/* ----------------------------------------------------------------- errors */

/*
 * The file APIs report through GetLastError(), so every wrapper below ends up
 * translating it.  One table beats four hand-written switches that had drifted
 * apart -- ERROR_INVALID_NAME meant ENOENT in stat() and EIO in unlink(), for
 * instance.  This is the Win32 counterpart of wsa_to_errno() in win32io.c.
 */
int win32_oserr_to_errno(unsigned long err)
{
	switch (err) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_NAME:
	case ERROR_BAD_NETPATH:
	case ERROR_BAD_PATHNAME:        return ENOENT;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:      return EACCES;
	case ERROR_ALREADY_EXISTS:
	case ERROR_FILE_EXISTS:         return EEXIST;
	case ERROR_NOT_SAME_DEVICE:     return EXDEV;
	case ERROR_PRIVILEGE_NOT_HELD:  return EPERM;
	case ERROR_DIR_NOT_EMPTY:       return ENOTEMPTY;
	case ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:         return ENOMEM;
	case ERROR_DISK_FULL:           return ENOSPC;
	case ERROR_WRITE_PROTECT:       return EROFS;
	case ERROR_INVALID_HANDLE:      return EBADF;
	default:                        return EIO;
	}
}

/* Set errno from the last Win32 error and return -1, the way sock_fail()
 * does for Winsock. */
static int os_fail(void)
{
	errno = win32_oserr_to_errno(GetLastError());
	return -1;
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
	flags &= ~(O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC | O_NOATIME | O_NOCTTY | O_NONBLOCK);
	flags |= _O_BINARY;

	/* A file opened only for reading is, in rsync, a file about to be read
	 * from start to finish and never looked at again: _O_SEQUENTIAL passes
	 * FILE_FLAG_SEQUENTIAL_SCAN, which tells the cache manager to read
	 * further ahead and to drop what is behind rather than keeping it.
	 * RSYNC_WIN32_NO_SEQSCAN=1 turns it off. */
	if ((flags & (_O_WRONLY | _O_RDWR)) == 0) {
		static int off = -1;
		if (off < 0) {
			const char *e = getenv("RSYNC_WIN32_NO_SEQSCAN");
			off = e && *e && *e != '0';
		}
		if (!off)
			flags |= _O_SEQUENTIAL;
	}

	/* Windows keeps one bit of a Unix mode: whether the file is writable.
	 * Carry that bit across rather than making every created file
	 * read-write and relying on the chmod that follows. */
	fd = _open(path, flags, _S_IREAD | ((mode & 0200) ? _S_IWRITE : 0));
	return fd;
}

/* ------------------------------------------------------------------ stat */

/*
 * Windows reports a symlink only via a reparse point plus its tag.  The tag
 * matters: plenty of things are reparse points without being links -- a
 * OneDrive placeholder, a deduplicated file -- and calling those symlinks
 * would be wrong twice over, in what gets reported and in what readdir()
 * claims.  Shared with win32dir.c for exactly that reason.
 */
int win32_attrs_are_symlink(unsigned long attrs, unsigned long tag)
{
	return (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
	    && (tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT);
}

/* Trailing slashes upset the file APIs on plain files; drop all but a root's. */
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

/* 100ns ticks since 1601 -> seconds since 1970. */
static __time64_t filetime_to_time(const FILETIME *ft)
{
	unsigned __int64 ticks =
		((unsigned __int64)ft->dwHighDateTime << 32) | ft->dwLowDateTime;

	if (ticks < 116444736000000000ULL)
		return 0;
	return (__time64_t)((ticks - 116444736000000000ULL) / 10000000ULL);
}

/*
 * Fill in a stat buffer from an open handle.  GetFileInformationByHandle is
 * what gives us a usable st_ino: the 64-bit NTFS file index, which is what
 * --hard-links compares.  It also reports the real link count.
 */
static int stat_by_handle(HANDLE h, const char *path,
			  struct win32_stat *st, int is_link)
{
	BY_HANDLE_FILE_INFORMATION bhfi;

	if (!GetFileInformationByHandle(h, &bhfi))
		return os_fail();

	memset(st, 0, sizeof *st);

	st->st_dev = (dev_t)bhfi.dwVolumeSerialNumber;
	st->st_rdev = st->st_dev;
	st->st_ino = ((unsigned __int64)bhfi.nFileIndexHigh << 32)
		   | bhfi.nFileIndexLow;
	st->st_nlink = (nlink_t)(bhfi.nNumberOfLinks ? bhfi.nNumberOfLinks : 1);
	if (bhfi.nNumberOfLinks > 1 && path)
		win32_note_link(path, WIN32_LINK_HARDLINK);
	st->st_size = ((__int64)bhfi.nFileSizeHigh << 32) | bhfi.nFileSizeLow;
	st->st_uid = WIN32_FAKE_UID;
	st->st_gid = WIN32_FAKE_GID;

	st->st_atime = filetime_to_time(&bhfi.ftLastAccessTime);
	st->st_mtime = filetime_to_time(&bhfi.ftLastWriteTime);
	st->st_ctime = filetime_to_time(&bhfi.ftCreationTime);

	/* Windows has one permission bit we can represent, so synthesise a
	 * plausible mode from it and let --chmod do the rest. */
	if (is_link)
		st->st_mode = S_IFLNK | 0777;
	else if (bhfi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		st->st_mode = _S_IFDIR | 0777;
	else
		st->st_mode = _S_IFREG | 0666;

	if (!is_link && (bhfi.dwFileAttributes & FILE_ATTRIBUTE_READONLY))
		st->st_mode &= ~(mode_t)0222;

	return 0;
}

static int stat_path(const char *path, struct win32_stat *st, int follow)
{
	static const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE
				 | FILE_SHARE_DELETE;
	char buf[MAXPATHLEN];
	const char *p = trim_path(path, buf, sizeof buf);
	FILE_ATTRIBUTE_TAG_INFO tag;
	int is_link = 0;
	HANDLE h;
	int rc;

	/*
	 * Open without following first.  Asking the handle what it is answers
	 * both questions -- "is this a link" and "what are its attributes" --
	 * from one path resolution, where testing the path beforehand cost a
	 * second (and a third, FindFirstFile, to read the reparse tag).  stat()
	 * runs once per file in the tree, so that is the walk's inner loop.
	 */
	h = CreateFileA(p, FILE_READ_ATTRIBUTES, share, NULL, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
			NULL);
	if (h == INVALID_HANDLE_VALUE)
		return os_fail();

	if (GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof tag)
	 && win32_attrs_are_symlink(tag.FileAttributes, tag.ReparseTag)) {
		win32_note_link(p, WIN32_LINK_SYMLINK);
		if (follow) {
			/* stat() reports the target, so pay for a second open
			 * -- but only for the links, which are the rare case. */
			CloseHandle(h);
			h = CreateFileA(p, FILE_READ_ATTRIBUTES, share, NULL,
					OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
					NULL);
			if (h == INVALID_HANDLE_VALUE)
				return os_fail();
		} else
			is_link = 1;
	}

	rc = stat_by_handle(h, p, st, is_link);
	CloseHandle(h);
	return rc;
}

int win32_stat(const char *path, struct win32_stat *st)
{
	return stat_path(path, st, 1);
}

int win32_lstat(const char *path, struct win32_stat *st)
{
	return stat_path(path, st, 0);
}

int win32_fstat(int fd, struct win32_stat *st)
{
	HANDLE h = (HANDLE)_get_osfhandle(fd);

	if (h == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return -1;
	}
	return stat_by_handle(h, NULL, st, 0);
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

	if (attrs == INVALID_FILE_ATTRIBUTES)
		return os_fail();

	/* The only bit Windows can represent is "read-only". */
	if (mode & S_IWUSR)
		attrs &= ~(DWORD)FILE_ATTRIBUTE_READONLY;
	else
		attrs |= FILE_ATTRIBUTE_READONLY;

	if (!SetFileAttributesA(path, attrs))
		return os_fail();
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
		return os_fail();
	}

	if (!DeleteFileA(path))
		return os_fail();
	return 0;
}

int win32_rename(const char *from, const char *to)
{
	/*
	 * POSIX rename() replaces the destination; MoveFile does not, hence
	 * REPLACE_EXISTING.  COPY_ALLOWED then makes a cross-volume move work
	 * by copying, which is why the EXDEV case below cannot actually be
	 * reached: robust_rename() would otherwise catch EXDEV and do the copy
	 * itself.  Letting Windows do it keeps --temp-dir on another drive
	 * working; the cost is that what looks like a rename can move a whole
	 * file's worth of bytes.
	 */
	if (!MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
		return os_fail();
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

struct tm *win32_gmtime_r(const time_t *timep, struct tm *result)
{
	if (gmtime_s(result, timep) != 0)
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
