/*
 * Report the symlinks and hard links met during a run.
 *
 * Neither is a first-class concept for ordinary Windows users: creating a
 * symlink needs Developer Mode or SeCreateSymbolicLinkPrivilege, which most
 * accounts do not hold, and hard links are rare outside developer tooling.
 * This build therefore leaves SUPPORT_LINKS and SUPPORT_HARD_LINKS off, so
 * rsync treats each one as the ordinary file it resolves to:
 *
 *   - a symlink is followed, and its referent's contents are copied;
 *   - hard links to one inode are copied as that many independent files.
 *
 * That is the sensible default here, but it silently loses structure the
 * user may have meant to keep.  The stat layer (win32/win32compat.c) notices
 * both cases as it walks a tree, records them here, and this prints a
 * summary as the process exits -- so the outcome is stated plainly rather
 * than discovered later.
 *
 * Copyright (C) 2026 Max Vilimpoc, rsync CMake/Windows port.
 * Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
 */

#include "rsync.h"
#include "win32/win32undef.h"

/* Enough to make the point without turning into a second file listing. */
#define MAX_NOTED 100

struct noted {
	char *path;
	int   kind;
};

static struct noted noted[MAX_NOTED];
static int noted_cnt;          /* entries actually stored, both kinds */
static int noted_of_kind[2];   /* ...and how many of those are each kind */
static int seen_cnt[2];        /* everything seen, per kind, stored or not */
static int printer_registered;
static CRITICAL_SECTION lock;
static int lock_ready;

void win32_links_init(void)
{
	InitializeCriticalSection(&lock);
	lock_ready = 1;
}

static void print_summary(void)
{
	int kind, i;

	if (!noted_cnt)
		return;

	fprintf(stderr, "\n");
	for (kind = 0; kind < 2; kind++) {
		if (!seen_cnt[kind])
			continue;
		if (kind == WIN32_LINK_SYMLINK) {
			if (seen_cnt[kind] == 1)
				fprintf(stderr, "rsync: 1 symlink was followed "
						"and copied as an ordinary file:\n");
			else
				fprintf(stderr, "rsync: %d symlinks were followed "
						"and copied as ordinary files:\n",
					seen_cnt[kind]);
		} else {
			if (seen_cnt[kind] == 1)
				fprintf(stderr, "rsync: 1 hard-linked file was "
						"copied as an independent file:\n");
			else
				fprintf(stderr, "rsync: %d hard-linked files were "
						"copied as independent files:\n",
					seen_cnt[kind]);
		}
		for (i = 0; i < noted_cnt; i++) {
			if (noted[i].kind == kind)
				fprintf(stderr, "    %s\n", noted[i].path);
		}
		/* Against this kind's own stored count, not the combined one:
		 * a run with both kinds fills the table between them, and
		 * comparing against the total hid the truncation. */
		if (seen_cnt[kind] > noted_of_kind[kind])
			fprintf(stderr, "    ... and %d more not listed\n",
				seen_cnt[kind] - noted_of_kind[kind]);
	}
	fprintf(stderr,
		"This build does not reproduce links: Windows needs a privilege "
		"most\naccounts lack to create a symlink, so neither is treated "
		"as first-class.\n");
	fflush(stderr);
}

/*
 * Record one path.  Called from the stat layer, which sees every file rsync
 * looks at, so it must stay cheap and must not care about being called more
 * than once for the same path.
 */
void win32_note_link(const char *path, int kind)
{
	int i;

	if (kind != WIN32_LINK_SYMLINK && kind != WIN32_LINK_HARDLINK)
		return;
	if (!lock_ready)          /* a helper that never called win32_init() */
		return;

	EnterCriticalSection(&lock);

	for (i = 0; i < noted_cnt; i++) {
		if (noted[i].kind == kind && strcmp(noted[i].path, path) == 0) {
			LeaveCriticalSection(&lock);
			return;    /* already known; stat gets called repeatedly */
		}
	}

	/* Past MAX_NOTED the paths are no longer kept, so this loop can no
	 * longer recognise a repeat and the count becomes "times seen" rather
	 * than "distinct paths".  It only ever appears as "and N more", so an
	 * approximation there is better than paying to keep every path. */
	seen_cnt[kind]++;

	if (noted_cnt < MAX_NOTED) {
		char *copy = strdup(path);
		if (copy) {
			noted[noted_cnt].path = copy;
			noted[noted_cnt].kind = kind;
			noted_cnt++;
			noted_of_kind[kind]++;
		}
	}

	if (!printer_registered) {
		/* exit_cleanup() ends with exit(), so atexit runs; the one path
		 * that uses _exit() is the signal handler, where a tidy summary
		 * is not the priority anyway. */
		atexit(print_summary);
		printer_registered = 1;
	}

	LeaveCriticalSection(&lock);
}

/* rsync 3.5.0's t_secure_relpath helper builds a symlink to check that an
 * in-tree ".." climb through one still resolves.  Creating a symlink needs
 * Developer Mode or SeCreateSymbolicLinkPrivilege, which is exactly the
 * privilege this port declines to require (see the file header), so this
 * reports the honest answer rather than half-creating something.  rsync
 * itself never reaches here: SUPPORT_LINKS is off, so do_symlink() is
 * compiled out. */
int win32_symlink(const char *target, const char *linkpath)
{
	(void)target;
	(void)linkpath;
	errno = ENOSYS;
	return -1;
}
