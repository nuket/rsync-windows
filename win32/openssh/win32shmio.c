/*
 * win32shmio.c -- stdin and stdout of the ssh.exe rsync spawns, carried in a
 * shared-memory ring instead of a pipe.
 *
 * Part of the rsync for Windows port (github.com/nuket/rsync-windows).
 * Compiled into Microsoft's Win32-OpenSSH posix_compat library beside
 * win32pumps.c, which routes the two fds here before it would start a pump.
 *
 * Copyright (c) 2026 Max Vilimpoc.  BSD 2-clause, as contrib/win32.
 *
 * Why
 * ---
 * With the pumps in place, what is left of the cost of a push from this
 * machine is largely the hop between rsync.exe and ssh.exe.  Every byte is
 * copied four times to cross it -- rsync's ring, into the pipe, out of the
 * pipe, our ring -- and each 32KB chunk costs a WriteFile and a ReadFile.
 * Measured on a 20Gbit link, feeding ssh from a pipe rather than a file
 * costs 18% (1275 -> 1045 MB/s) with no rsync in the picture at all, and a
 * standalone parent-to-child benchmark puts a 1MB pipe at 5.7GB/s against
 * 22GB/s for the ring below.
 *
 * What
 * ----
 * rsync creates two single-producer/single-consumer rings in a shared
 * section (win32shmpipe.c), passes their inheritable handle numbers in the
 * environment, and we attach to them before main() -- early, because rsync
 * waits for that answer before it commits to using them, and does not touch
 * stdin until well after we would first have.  Two copies per byte then,
 * and no system call per chunk.
 *
 * A ring is not waitable the way a pipe is, and ssh's main loop learns of
 * everything through one alertable wait.  So each direction gets a notifier
 * thread: it sleeps until the main thread says it is about to block, waits
 * on the ring's event, and queues the same kind of APC a pump would.  It
 * moves no bytes -- read() and write() copy straight out of and into the
 * ring -- and it costs nothing at all while the transfer keeps up, since a
 * select() that finds bytes waiting never asks for it.
 *
 * Everything falls back to the pipes if any of this does not come off: the
 * pipes are created either way, an ssh.exe from before this existed simply
 * ignores an environment variable it does not know, and neither side uses a
 * ring until it has seen the other side agree.
 */

#include <winsock2.h>
#include <Windows.h>
#include <process.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w32fd.h"
#include "debug.h"
#include "misc_internal.h"
#include "win32pumps.h"
#include "win32shmio.h"
#include "win32shmpipe.h"

#define SHM_ENV       "RSYNC_WIN32_SHMPIPE"
#define SHM_DEBUG_ENV "RSYNC_WIN32_SHMPIPE_DEBUG"
#define SHM_GO_MS     8000   /* rsync answers in microseconds; this is a leash */
#define SHM_POLL_MS   50     /* how long a notifier sleeps between looks */

/*
 * fileio.c calls write() with at most WRITE_BUFFER_SIZE (100KB) and expects
 * all of it taken, so "writable" has to promise that much -- as the write
 * pump's SYNC_WPUMP_WRITABLE does.
 */
#define SHM_WRITABLE  (100 * 1024)

struct shmio {
	struct shmpipe *ring;
	int    for_write;
	int    fd;                /* 0 or 1; which of the two this is */

	struct w32_io *ours[8];   /* the fd rsync gave us, and its dups */
	int    n_ours;

	HANDLE notifier;
	HANDLE kick;              /* the main thread is about to block */
	volatile LONG want;       /* ... and wants the APC when that changes */
	volatile LONG stop;
	unsigned long long bytes; /* moved through the ring, for the debug line */
	BOOL   apc_queued;
	BOOL   closed;            /* we told the far side we were done */
	BOOL   detached;          /* the fd was closed; the ring is finished with */
};

static struct shmio shm_in  = { NULL, 0, 0 };   /* stdin, read */
static struct shmio shm_out = { NULL, 1, 1 };   /* stdout, written */
static int shm_attached;      /* the handshake came off */

/* ---- attaching, before main() ---------------------------------------- */

/*
 * The environment says "to-child|from-child|parent-process-handle", each
 * ring as win32shmpipe.c spelled it.  Both ends have to agree before either
 * uses one: we say ready, rsync says go only if it saw that, and we use the
 * rings only if we see go.  An rsync that gave up waiting never says go, so
 * a slow start here costs the speedup, never the transfer.
 */
/* Before main(), and so before ssh's own logging exists: stderr, and only
 * when asked for.  The parent's end of the same switch is in win32proc.c. */
static void
shmio_debug(const char *what)
{
	char on[8];
	DWORD n = GetEnvironmentVariableA(SHM_DEBUG_ENV, on, sizeof on);

	if (n > 0 && n < sizeof on && on[0] && on[0] != '0')
		fprintf(stderr, "ssh: shmpipe: %s\n", what);
}

static void
shmio_attach(void)
{
	char env[512], *bar, *bar2;
	HANDLE parent = NULL;
	struct shmpipe *to_us = NULL, *from_us = NULL;
	DWORD n = GetEnvironmentVariableA(SHM_ENV, env, sizeof env);

	if (n == 0 || n >= sizeof env) {
		shmio_debug("no rings offered");
		return;
	}

	/* Ours to consume: nothing we start should inherit it. */
	SetEnvironmentVariableA(SHM_ENV, NULL);

	if (!(bar = strchr(env, '|'))) {
		shmio_debug("malformed offer");
		return;
	}
	*bar = '\0';
	if ((bar2 = strchr(bar + 1, '|')) != NULL) {
		*bar2 = '\0';
		parent = (HANDLE)(uintptr_t)_strtoui64(bar2 + 1, NULL, 10);
	}
	if (shmpipe_open(&to_us, env) < 0) {
		shmio_debug("could not attach to the inbound ring");
		return;
	}
	if (shmpipe_open(&from_us, bar + 1) < 0) {
		shmio_debug("could not attach to the outbound ring");
		shmpipe_free(to_us);
		return;
	}

	shmpipe_mark_ready(to_us);
	shmpipe_mark_ready(from_us);
	if (shmpipe_wait_go(to_us, SHM_GO_MS) < 0) {
		shmio_debug("rsync did not say go; using the pipes");
		shmpipe_free(to_us);
		shmpipe_free(from_us);
		return;
	}
	shmio_debug("attached to both rings");
	if (parent) {
		shmpipe_set_peer(to_us, parent);
		shmpipe_set_peer(from_us, parent);
	}
	shm_in.ring = to_us;
	shm_out.ring = from_us;
	shm_out.for_write = 1;
	shm_attached = 1;
}

/* exit() with the far side still reading would leave it waiting for an end
 * of file that a closing pipe would have delivered for us. */
static void
shmio_atexit(void)
{
	char msg[96];

	snprintf(msg, sizeof msg, "in %llu bytes, out %llu bytes",
		 shm_in.bytes, shm_out.bytes);
	shmio_debug(msg);
	if (shm_out.ring && !shm_out.closed) {
		shm_out.closed = TRUE;
		shmpipe_close_write(shm_out.ring);
	}
}

static void __cdecl
shmio_early_init(void)
{
	shmio_attach();
	if (shm_attached)
		atexit(shmio_atexit);
}

/*
 * Before main(), because rsync has already spawned us and is waiting to
 * hear whether we understood it, while ssh itself will not look at stdin
 * until the session is up.  The linker directive keeps the pointer alive:
 * a .CRT$XCU entry nothing references is otherwise fair game for /OPT:REF.
 */
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) void (__cdecl *shmio_ctor)(void) = shmio_early_init;
#if defined(_M_IX86)
#pragma comment(linker, "/include:_shmio_ctor")
#else
#pragma comment(linker, "/include:shmio_ctor")
#endif

/* ---- which fd is which ----------------------------------------------- */

/*
 * Which w32_io a ring belongs to.  fd 0 and fd 1 to begin with, and then
 * whatever ssh_session2_open() dup()s them into -- it is those dups the
 * client loop reads and writes, and a dup gets a w32_io and a handle of its
 * own.  Both are kept: a dup does not retire the original, and everything
 * here runs on the main thread in order, so it does not matter which of
 * them a given byte goes through.
 */
static BOOL
is_ours(struct shmio *s, struct w32_io *pio)
{
	int i;

	for (i = 0; i < s->n_ours; i++)
		if (s->ours[i] == pio)
			return TRUE;
	return FALSE;
}

static void
claim(struct shmio *s, struct w32_io *pio)
{
	if (!pio || is_ours(s, pio))
		return;
	if (s->n_ours < (int)(sizeof s->ours / sizeof s->ours[0]))
		s->ours[s->n_ours++] = pio;
}

/* A w32_io is going away: drop it, so a later one at the same address is
 * not mistaken for it. */
static void
forget_io(struct w32_io *pio)
{
	int i, j;

	for (j = 0; j < 2; j++) {
		struct shmio *s = j ? &shm_out : &shm_in;

		for (i = 0; i < s->n_ours; i++)
			if (s->ours[i] == pio)
				s->ours[i] = s->ours[--s->n_ours];
	}
}

void
shmio_note_dup(int oldfd, struct w32_io *newio)
{
	struct shmio *s;

	if (!shm_attached || !newio)
		return;
	if (oldfd != shm_in.fd && oldfd != shm_out.fd)
		return;
	s = (oldfd == shm_out.fd) ? &shm_out : &shm_in;
	if (!s->ring || s->detached)
		return;
	claim(s, w32_io_from_fd(oldfd));   /* the original, if not already */
	claim(s, newio);
	shmio_debug(oldfd == shm_out.fd ? "stdout dup: a ring"
					: "stdin dup: a ring");
}

struct shmio *
shmio_for(struct w32_io *pio, int for_write)
{
	struct shmio *s;

	if (!shm_attached)
		return NULL;
	s = for_write ? &shm_out : &shm_in;
	if (!s->ring || s->detached)
		return NULL;
	if (is_ours(s, pio))
		return s;
	/* The fd itself, before anything has duplicated it. */
	if (w32_io_from_fd(s->fd) == pio) {
		claim(s, pio);
		shmio_debug(for_write ? "stdout is a ring" : "stdin is a ring");
		return s;
	}
	return NULL;
}

/* ---- the notifier ---------------------------------------------------- */

static VOID CALLBACK
ShmWakeAPC(_In_ ULONG_PTR dwParam)
{
	BOOL *queued = (BOOL *)dwParam;
	*queued = FALSE;
}

static BOOL
shmio_ready(struct shmio *s)
{
	if (s->for_write)
		return shmpipe_room(s->ring) >= SHM_WRITABLE;
	return shmpipe_avail(s->ring) > 0 || shmpipe_at_eof(s->ring);
}

/*
 * One thread per direction, and it moves nothing: it exists so that the
 * main thread's alertable wait has something to end it.  It sleeps on `kick`
 * until the main thread says it is about to block, then arms the ring --
 * which is what makes the far side bother to signal at all -- waits for the
 * state to change, and queues the APC.
 */
static unsigned __stdcall
ShmNotifyThread(_In_ LPVOID param)
{
	struct shmio *s = (struct shmio *)param;
	HANDLE evt = s->for_write ? shmpipe_room_event(s->ring)
				  : shmpipe_data_event(s->ring);

	debug5("shmio notifier, fd:%d", s->fd);
	while (!s->stop) {
		if (WaitForSingleObject(s->kick, INFINITE) != WAIT_OBJECT_0)
			break;
		if (s->stop)
			break;

		shmpipe_arm(s->ring, s->for_write, 1);
		while (!s->stop && !shmio_ready(s) && s->want)
			WaitForSingleObject(evt, SHM_POLL_MS);
		shmpipe_arm(s->ring, s->for_write, 0);

		if (InterlockedExchange(&s->want, 0) && !s->apc_queued) {
			s->apc_queued = TRUE;
			if (!QueueUserAPC(ShmWakeAPC, main_thread,
					  (ULONG_PTR)&s->apc_queued))
				s->apc_queued = FALSE;
		}
	}
	return 0;
}

/* Say that the main thread is about to block and wants to hear about it. */
static void
shmio_want_wakeup(struct shmio *s)
{
	if (!s->notifier) {
		if (!(s->kick = CreateEventW(NULL, FALSE, FALSE, NULL)))
			return;
		s->notifier = (HANDLE)_beginthreadex(NULL, 0, ShmNotifyThread,
						     s, 0, NULL);
		if (!s->notifier) {
			CloseHandle(s->kick);
			s->kick = NULL;
			return;
		}
	}
	InterlockedExchange(&s->want, 1);
	SetEvent(s->kick);
}

/* ---- read, write, select --------------------------------------------- */

int
shmio_read(struct shmio *s, struct w32_io *pio, void *buf, size_t len)
{
	for (;;) {
		int n = shmpipe_read(s->ring, buf, len, 1);

		if (n > 0)
			s->bytes += (unsigned)n;
		if (n >= 0 || errno != EAGAIN)
			return n;
		if (!w32_io_is_blocking(pio)) {
			errno = EAGAIN;
			return -1;
		}
		shmio_want_wakeup(s);
		if (shmio_ready(s))
			continue;       /* it arrived while we were asking */
		if (wait_for_any_event(NULL, 0, INFINITE) == -1)
			return -1;
	}
}

int
shmio_write(struct shmio *s, struct w32_io *pio, const void *buf, size_t len)
{
	size_t off = 0;

	/* All or nothing, as the write pump promises fileio_write_wrapper(). */
	for (;;) {
		int n;

		if (off == len)
			return (int)len;
		n = shmpipe_write(s->ring, (const char *)buf + off, len - off, 1);
		if (n > 0) {
			off += (size_t)n;
			s->bytes += (unsigned)n;
			continue;
		}
		if (n < 0 && errno != EAGAIN)
			return off ? (int)off : -1;
		if (!w32_io_is_blocking(pio)) {
			if (off)
				return (int)off;
			errno = EAGAIN;
			return -1;
		}
		shmio_want_wakeup(s);
		if (shmpipe_room(s->ring) > 0)
			continue;
		if (wait_for_any_event(NULL, 0, INFINITE) == -1)
			return off ? (int)off : -1;
	}
}

BOOL
shmio_available(struct shmio *s, int for_write)
{
	(void)for_write;
	if (shmio_ready(s))
		return TRUE;
	shmio_want_wakeup(s);
	/* Between the look and the ask the far side may have moved. */
	return shmio_ready(s);
}

void
shmio_close(struct w32_io *pio)
{
	struct shmio *s;
	int i;

	for (i = 0; i < 2; i++) {
		s = i ? &shm_out : &shm_in;
		if (!s->ring || !is_ours(s, pio))
			continue;
		forget_io(pio);
		if (s->n_ours)
			continue;   /* a dup of it is still open */
		if (s->for_write && !s->closed) {
			s->closed = TRUE;
			shmpipe_close_write(s->ring);
		}
		if (s->notifier) {
			InterlockedExchange(&s->stop, 1);
			SetEvent(s->kick);
			if (WaitForSingleObject(s->notifier, 1000) == WAIT_TIMEOUT)
				debug3("shmio notifier for fd:%d would not stop", s->fd);
			CloseHandle(s->notifier);
			CloseHandle(s->kick);
			s->notifier = NULL;
			s->kick = NULL;
			/* run a wake-up it may have queued before we go */
			SleepEx(0, TRUE);
		}
		s->detached = TRUE;
	}
}
