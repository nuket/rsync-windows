/*
 * opendir/readdir/closedir on top of FindFirstFile/FindNextFile.
 *
 * Copyright (C) 2026 rsync CMake/Windows port.
 * Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
 */

#include "rsync.h"
#include "win32/win32undef.h"

struct DIR {
	HANDLE           handle;
	WIN32_FIND_DATAA find;
	int              pending;   /* find data not yet returned */
	struct dirent    ent;
};

DIR *win32_opendir(const char *path)
{
	DIR *dirp;
	char pattern[MAXPATHLEN];
	size_t len = strlen(path);

	if (len == 0 || len + 3 >= sizeof pattern) {
		errno = len ? ENAMETOOLONG : ENOENT;
		return NULL;
	}

	memcpy(pattern, path, len);
	if (pattern[len - 1] != '/' && pattern[len - 1] != '\\')
		pattern[len++] = '\\';
	pattern[len++] = '*';
	pattern[len] = '\0';

	dirp = (DIR *)calloc(1, sizeof *dirp);
	if (!dirp) {
		errno = ENOMEM;
		return NULL;
	}

	dirp->handle = FindFirstFileA(pattern, &dirp->find);
	if (dirp->handle == INVALID_HANDLE_VALUE) {
		DWORD err = GetLastError();
		free(dirp);
		errno = (err == ERROR_PATH_NOT_FOUND || err == ERROR_FILE_NOT_FOUND)
		      ? ENOENT : EACCES;
		return NULL;
	}
	dirp->pending = 1;
	return dirp;
}

struct dirent *win32_readdir(DIR *dirp)
{
	if (!dirp) {
		errno = EBADF;
		return NULL;
	}

	if (!dirp->pending) {
		if (!FindNextFileA(dirp->handle, &dirp->find))
			return NULL;   /* end of directory */
	}
	dirp->pending = 0;

	strlcpy(dirp->ent.d_name, dirp->find.cFileName, sizeof dirp->ent.d_name);
	dirp->ent.d_ino = 0;

	if (dirp->find.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
		dirp->ent.d_type = DT_LNK;
	else if (dirp->find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		dirp->ent.d_type = DT_DIR;
	else
		dirp->ent.d_type = DT_REG;

	return &dirp->ent;
}

int win32_closedir(DIR *dirp)
{
	if (!dirp) {
		errno = EBADF;
		return -1;
	}
	if (dirp->handle != INVALID_HANDLE_VALUE)
		FindClose(dirp->handle);
	free(dirp);
	return 0;
}
