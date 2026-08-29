/*
 * win32pumps.c -- reader and writer threads for the fds that carry bulk
 * data in the Windows OpenSSH client.
 *
 * Part of the rsync for Windows port (github.com/nuket/rsync-windows).
 * Compiled into Microsoft's Win32-OpenSSH posix_compat library beside the
 * files it serves; termio.c, fileio.c and socketio.c hook into it through
 * the patches in win32/openssh/patches.
 *
 * Copyright (c) 2026 Max Vilimpoc.  BSD 2-clause, as contrib/win32.
 *
 * Why
 * ---
 * Microsoft's port does the I/O of a synchronous fd -- a pipe, a file, the
 * console -- with a thread per call: read() starts a thread that reads
 * TERM_IO_BUF_SIZE (3KB) and reports back with an APC, write() starts a
 * thread per write and joins it.  Sized for a console, where a read is a
 * few keystrokes, and what held an upload from ssh.exe at 17MB/s on a
 * 2.5Gbit link: a bulk transfer runs lockstepped on the peer's 98KB window
 * adjusts, and each adjust cost 33 thread round trips before the next 98KB
 * could go out.  Writing had the mirror image, a third of the main thread
 * on a download over a 20Gbit link.  The socket had its own version: one
 * WSASend/WSARecv in flight, with the copy and the TCP work done in the
 * thread that had just finished the crypto, in series with it.
 *
 * What
 * ----
 * A pipe, a file, or a connected socket gets a pump each way: one thread
 * for the fd's lifetime, reading into a ring / writing out of a ring, and
 * read(), write(), recv() and send() copy straight between the caller's
 * buffer and the ring -- no staging buffer in between, no thread and no
 * APC per call.  The socket work overlaps the crypto instead of following
 * it.  The main thread learns of bytes or room the way it learns of
 * everything else in this port, an APC into its alertable wait, queued
 * only when it said it was waiting; and a pump that finds its ring empty
 * (or full) spins briefly before it sleeps, and is signalled only when it
 * is asleep, so at speed neither side makes a kernel call per packet.
 *
 * fileio.c and socketio.c route a pumped fd here before any of their own
 * buffering.  The console keeps the thread-per-read and thread-per-write
 * paths, since its reads are keystrokes and its writes go through
 * WriteConsoleW; listening and connecting sockets keep their overlapped
 * paths.  Rings are drained before shutdown(SD_SEND), on close and at
 * exit, since a write reports its bytes taken once they are in the ring.
 */

#include <winsock2.h>
#include <Windows.h>
#include <process.h>
#include <errno.h>
#include "w32fd.h"
#include "debug.h"
#include "misc_internal.h"
#include "win32pumps.h"

/* The wake-up itself does nothing beyond clearing its flag: returning from
 * the alertable wait is the point, the waiter re-checks what it waited for. */
static VOID CALLBACK
PumpWakeAPC(_In_ ULONG_PTR dwParam)
{
	BOOL *queued = (BOOL *)dwParam;
	*queued = FALSE;
}

/* Spin briefly until *v moves off `stuck' (a full ring for a reader, an
 * empty one for a writer).  ~20-50us: long enough to bridge the gap
 * between packets in a bulk flow, in which the main thread would otherwise
 * pay a SetEvent() per packet to wake a pump that had just gone to sleep
 * (11% of it, measured), short enough to be nothing when the flow has
 * stopped.  The read is unlocked and only a hint; callers re-check under
 * the lock. */
static BOOL
pump_spin(volatile DWORD *v, DWORD stuck)
{
	int i;

	for (i = 0; i < 4000; i++) {
		if (*v != stuck)
			return TRUE;
		YieldProcessor();
	}
	return *v != stuck;
}

/* socketio.c keeps its own copy of this mapping private */
static int
pump_errno_from_wsa(int wsaerrno)
{
	switch (wsaerrno) {
	case WSAEWOULDBLOCK:	return EAGAIN;
	case WSAEFAULT:		return EFAULT;
	case WSAEINVAL:		return EINVAL;
	case WSAECONNABORTED:	return ECONNABORTED;
	case WSAETIMEDOUT:	return ETIMEDOUT;
	case WSAECONNREFUSED:	return ECONNREFUSED;
	case WSAEINPROGRESS:	return EINPROGRESS;
	case WSAESHUTDOWN:	return ECONNRESET;
	case WSAENOTCONN:	return ENOTCONN;
	case WSAECONNRESET:	return ECONNRESET;
	default:		return wsaerrno - 10000;
	}
}

/* ====================================================================== */
/* A pipe or file on a sync fd                                            */
/* ====================================================================== */

#define SYNC_PUMP_RING_SIZE   (1024 * 1024)
#define SYNC_PUMP_CHUNK       (256 * 1024)
#define SYNC_WPUMP_RING_SIZE  (1024 * 1024)
#define SYNC_WPUMP_CHUNK      (256 * 1024)
#define SYNC_WPUMP_FLUSH_MS   10000
/* fileio_write_wrapper() hands fileio_write() at most WRITE_BUFFER_SIZE
 * (100KB) and expects all of it taken, so "writable" promises that much. */
#define SYNC_WPUMP_WRITABLE   (100 * 1024)

/* pio->internal.context for a sync fd: the read pump and the write pump,
 * either of which may exist without the other. */
struct sync_ctx {
	struct sync_pump  *rd;
	struct sync_wpump *wr;
	BOOL pumped;              /* the fd is a pipe or file: GetFileType(), asked once */
};

struct sync_pump {
	HANDLE h;                 /* the pipe or file being read */
	HANDLE thread;
	HANDLE room_evt;          /* auto-reset: the ring has room again */
	CRITICAL_SECTION lock;
	char  *ring;
	DWORD  head, len;         /* head: next byte out */
	BOOL   eof;               /* the source is done; err says how */
	DWORD  err;
	BOOL   waiting;           /* main thread wants bytes; wake it when there are */
	BOOL   apc_queued;        /* a wake-up is already on its way to main_thread */
	BOOL   sleeping;          /* the pump is in its wait: only then is room_evt worth setting */
	volatile LONG stop;
	struct w32_io *pio;       /* NULL once the fd is gone and the thread outlived it */
};

struct sync_wpump {
	HANDLE h;
	HANDLE thread;
	HANDLE data_evt;          /* auto-reset: the ring has bytes */
	CRITICAL_SECTION lock;
	char  *ring;
	DWORD  head, len;         /* head: next byte out */
	DWORD  err;               /* first WriteFile error; the pump is dead after it */
	BOOL   waiting;           /* main thread wants room; wake it when there is */
	BOOL   apc_queued;
	BOOL   sleeping;          /* the pump is in its wait: only then is data_evt worth setting */
	volatile LONG stop;
	struct w32_io *pio;
};

static struct sync_wpump *live_sync_wpumps[64];
static int num_live_sync_wpumps;
static BOOL sync_wpump_atexit_set;

static struct sync_ctx *
sync_ctx_get(struct w32_io *pio)
{
	if (!pio->internal.context)
		pio->internal.context = calloc(1, sizeof(struct sync_ctx));
	return (struct sync_ctx *)pio->internal.context;
}

static BOOL
sync_pump_type_wanted(struct w32_io *pio)
{
	DWORD t = FILETYPE(pio);
	return t == FILE_TYPE_PIPE || t == FILE_TYPE_DISK;
}

/* fileio.c asks this on every read(), write() and select() of a sync fd,
 * and GetFileType() is a syscall (NtQueryVolumeInformationFile: 4% of the
 * main thread at 1GB/s), so the answer is kept with the pumps once the fd
 * is known to be pumped.  A console fd never gets a context and keeps
 * asking, which costs nothing that matters. */
BOOL
syncio_pump_wanted(struct w32_io *pio)
{
	struct sync_ctx *c = (struct sync_ctx *)pio->internal.context;

	if (c)
		return c->pumped;
	if (!sync_pump_type_wanted(pio))
		return FALSE;
	if ((c = sync_ctx_get(pio)) != NULL)
		c->pumped = TRUE;
	return TRUE;
}

/* ---- the read pump --------------------------------------------------- */

static unsigned __stdcall
SyncPumpThread(_In_ LPVOID lpParameter)
{
	struct sync_pump *p = (struct sync_pump *)lpParameter;

	debug5("SyncPump thread, pump:%p", p);
	while (!p->stop) {
		DWORD space, tail, want, got = 0, err = 0;
		BOOL ok, wake = FALSE;

		EnterCriticalSection(&p->lock);
		space = SYNC_PUMP_RING_SIZE - p->len;
		tail = (p->head + p->len) % SYNC_PUMP_RING_SIZE;
		LeaveCriticalSection(&p->lock);
		if (!space) {
			if (pump_spin(&p->len, SYNC_PUMP_RING_SIZE))
				continue;
			EnterCriticalSection(&p->lock);
			if (p->len == SYNC_PUMP_RING_SIZE)
				p->sleeping = TRUE;
			LeaveCriticalSection(&p->lock);
			if (!p->sleeping)
				continue;
			/* timed, so a stop is noticed even if nobody reads */
			WaitForSingleObject(p->room_evt, 200);
			p->sleeping = FALSE;
			continue;
		}

		/* [tail, tail+want) is free and only we append to it */
		want = min(space, SYNC_PUMP_CHUNK);
		want = min(want, SYNC_PUMP_RING_SIZE - tail);
		ok = ReadFile(p->h, p->ring + tail, want, &got, NULL);
		if (!ok || got == 0) {
			err = ok ? ERROR_HANDLE_EOF : GetLastError();
			if (!err)
				err = ERROR_HANDLE_EOF;
		}

		EnterCriticalSection(&p->lock);
		if (err) {
			p->eof = TRUE;
			p->err = err;
		} else
			p->len += got;
		if (p->waiting && !p->apc_queued) {
			p->waiting = FALSE;
			p->apc_queued = TRUE;
			wake = TRUE;
		}
		LeaveCriticalSection(&p->lock);

		if (wake && !QueueUserAPC(PumpWakeAPC, main_thread, (ULONG_PTR)&p->apc_queued)) {
			EnterCriticalSection(&p->lock);
			p->apc_queued = FALSE;
			LeaveCriticalSection(&p->lock);
		}
		if (err)
			break;
	}
	return 0;
}

static struct sync_pump *
sync_pump_start(struct w32_io *pio)
{
	struct sync_pump *p = calloc(1, sizeof *p);

	if (!p)
		goto fail;
	if (!(p->ring = malloc(SYNC_PUMP_RING_SIZE)))
		goto fail;
	if (!(p->room_evt = CreateEventW(NULL, FALSE, FALSE, NULL)))
		goto fail;
	InitializeCriticalSection(&p->lock);
	p->h = WINHANDLE(pio);
	p->pio = pio;
	if (!sync_ctx_get(pio)) {
		DeleteCriticalSection(&p->lock);
		goto fail;
	}
	p->thread = (HANDLE)_beginthreadex(NULL, 0, SyncPumpThread, p, 0, NULL);
	if (!p->thread) {
		DeleteCriticalSection(&p->lock);
		goto fail;
	}
	sync_ctx_get(pio)->rd = p;
	debug4("sync pump started for io:%p", pio);
	return p;

fail:
	errno = errno_from_Win32LastError();
	if (p) {
		if (p->room_evt)
			CloseHandle(p->room_evt);
		free(p->ring);
		free(p);
	}
	return NULL;
}

/* Stop the pump on close.  The thread may be blocked in ReadFile on a source
 * that never delivers; cancel it and give it a moment, and if it still will
 * not go, cut it loose rather than free memory under it. */
static void
sync_pump_stop(struct w32_io *pio)
{
	struct sync_ctx *c = (struct sync_ctx *)pio->internal.context;
	struct sync_pump *p = c ? c->rd : NULL;
	int tries;

	if (!p)
		return;
	c->rd = NULL;

	InterlockedExchange(&p->stop, 1);
	SetEvent(p->room_evt);
	for (tries = 0; ; tries++) {
		CancelIoEx(p->h, NULL);
		if (WaitForSingleObject(p->thread, 50) != WAIT_TIMEOUT)
			break;
		if (tries >= 20) {
			debug3("sync pump for io:%p would not stop; leaking it", pio);
			EnterCriticalSection(&p->lock);
			p->pio = NULL;
			LeaveCriticalSection(&p->lock);
			CloseHandle(p->thread);
			/* drain a wake-up it may have queued for the fd going away */
			SleepEx(0, TRUE);
			return;
		}
	}
	SleepEx(0, TRUE);
	CloseHandle(p->thread);
	CloseHandle(p->room_evt);
	DeleteCriticalSection(&p->lock);
	free(p->ring);
	free(p);
}

/* read() for a pumped fd: straight from the ring into the caller's buffer. */
int
syncio_pump_read(struct w32_io *pio, void *buf, size_t len)
{
	struct sync_ctx *c = sync_ctx_get(pio);
	struct sync_pump *p = c ? c->rd : NULL;
	DWORD n, first;
	BOOL was_full;

	if (!c) {
		errno = ENOMEM;
		return -1;
	}
	if (!p && !(p = sync_pump_start(pio)))
		return -1;

	for (;;) {
		EnterCriticalSection(&p->lock);
		if (p->len)
			break;
		if (p->eof) {
			DWORD err = p->err;
			LeaveCriticalSection(&p->lock);
			if (err == ERROR_HANDLE_EOF || err == ERROR_BROKEN_PIPE) {
				debug4("read - no more data, io:%p", pio);
				errno = 0;
				return 0;
			}
			errno = errno_from_Win32Error(err);
			debug3("read - pump ERROR:%d, io:%p", err, pio);
			return -1;
		}
		p->waiting = TRUE;
		LeaveCriticalSection(&p->lock);
		if (!w32_io_is_blocking(pio)) {
			errno = EAGAIN;
			return -1;
		}
		if (wait_for_any_event(NULL, 0, INFINITE) == -1)
			return -1;
	}
	/* take under the lock, copy outside it, release under it: the pump
	 * only appends past len, so [head, head+n) is ours meanwhile */
	n = (DWORD)min((size_t)p->len, len);
	first = min(n, SYNC_PUMP_RING_SIZE - p->head);
	LeaveCriticalSection(&p->lock);
	memcpy(buf, p->ring + p->head, first);
	if (n > first)
		memcpy((char *)buf + first, p->ring, n - first);
	EnterCriticalSection(&p->lock);
	p->head = (p->head + n) % SYNC_PUMP_RING_SIZE;
	p->len -= n;
	was_full = p->sleeping;
	LeaveCriticalSection(&p->lock);
	/* only a pump in its wait needs the event */
	if (was_full)
		SetEvent(p->room_evt);
	return (int)n;
}

/* select() wants to know: start the pump so there is something to wait on. */
int
syncio_pump_prepare(struct w32_io *pio)
{
	struct sync_ctx *c = sync_ctx_get(pio);

	if (!c) {
		errno = ENOMEM;
		return -1;
	}
	if (!c->rd && !sync_pump_start(pio))
		return -1;
	return 0;
}

/* ---- the write pump -------------------------------------------------- */

static unsigned __stdcall
SyncWpumpThread(_In_ LPVOID lpParameter)
{
	struct sync_wpump *p = (struct sync_wpump *)lpParameter;

	debug5("SyncWpump thread, pump:%p", p);
	for (;;) {
		DWORD head, take, wrote = 0, err = 0;
		BOOL wake = FALSE, empty;

		EnterCriticalSection(&p->lock);
		empty = p->len == 0;
		head = p->head;
		take = min(p->len, SYNC_WPUMP_CHUNK);
		take = min(take, SYNC_WPUMP_RING_SIZE - head);
		LeaveCriticalSection(&p->lock);

		if (empty) {
			if (p->stop)
				break;
			if (pump_spin(&p->len, 0))
				continue;
			EnterCriticalSection(&p->lock);
			if (p->len == 0 && !p->stop)
				p->sleeping = TRUE;   /* under the lock: an append that follows sets the event */
			LeaveCriticalSection(&p->lock);
			if (!p->sleeping)
				continue;
			/* timed, so a stop is noticed even if nobody writes */
			WaitForSingleObject(p->data_evt, 200);
			p->sleeping = FALSE;
			continue;
		}

		/* [head, head+take) is ours until we advance head: the main
		 * thread only ever appends at the tail */
		if (!WriteFile(p->h, p->ring + head, take, &wrote, NULL)) {
			err = GetLastError();
			if (!err)
				err = ERROR_WRITE_FAULT;
		}

		EnterCriticalSection(&p->lock);
		if (err) {
			/* dead: drop what is queued, report on the next write */
			p->err = err;
			p->len = 0;
		} else {
			p->head = (p->head + wrote) % SYNC_WPUMP_RING_SIZE;
			p->len -= wrote;
		}
		if (p->waiting && !p->apc_queued) {
			p->waiting = FALSE;
			p->apc_queued = TRUE;
			wake = TRUE;
		}
		LeaveCriticalSection(&p->lock);
		if (wake && !QueueUserAPC(PumpWakeAPC, main_thread, (ULONG_PTR)&p->apc_queued)) {
			EnterCriticalSection(&p->lock);
			p->apc_queued = FALSE;
			LeaveCriticalSection(&p->lock);
		}
		if (err)
			break;
	}
	return 0;
}

/* Let the pump drain, then stop it.  The thread may be stuck in a WriteFile
 * nobody is reading; cut it loose after a while rather than hang, and if it
 * still will not go, leak the pump rather than free memory under it. */
static void
sync_wpump_stop(struct sync_wpump *p)
{
	int i;

	InterlockedExchange(&p->stop, 1);
	SetEvent(p->data_evt);
	if (WaitForSingleObject(p->thread, SYNC_WPUMP_FLUSH_MS) == WAIT_TIMEOUT) {
		debug3("sync write pump %p: peer not draining, abandoning the rest", p);
		CancelSynchronousIo(p->thread);
		if (WaitForSingleObject(p->thread, 1000) == WAIT_TIMEOUT) {
			EnterCriticalSection(&p->lock);
			p->pio = NULL;
			LeaveCriticalSection(&p->lock);
			CloseHandle(p->thread);
			SleepEx(0, TRUE);
			return;
		}
	}
	/* run a wake-up it may have queued before we free it */
	SleepEx(0, TRUE);
	for (i = 0; i < num_live_sync_wpumps; i++)
		if (live_sync_wpumps[i] == p) {
			live_sync_wpumps[i] = live_sync_wpumps[--num_live_sync_wpumps];
			break;
		}
	CloseHandle(p->thread);
	CloseHandle(p->data_evt);
	DeleteCriticalSection(&p->lock);
	free(p->ring);
	free(p);
}

/* exit() with bytes still in a ring would lose what was reported written. */
static void
sync_wpump_exit(void)
{
	while (num_live_sync_wpumps)
		sync_wpump_stop(live_sync_wpumps[num_live_sync_wpumps - 1]);
}

static struct sync_wpump *
sync_wpump_start(struct w32_io *pio)
{
	struct sync_ctx *c = sync_ctx_get(pio);
	struct sync_wpump *p;

	if (!c || num_live_sync_wpumps == (int)(sizeof live_sync_wpumps / sizeof live_sync_wpumps[0])) {
		errno = ENOMEM;
		return NULL;
	}
	if (!(p = calloc(1, sizeof *p)))
		goto fail;
	if (!(p->ring = malloc(SYNC_WPUMP_RING_SIZE)))
		goto fail;
	if (!(p->data_evt = CreateEventW(NULL, FALSE, FALSE, NULL)))
		goto fail;
	InitializeCriticalSection(&p->lock);
	p->h = WINHANDLE(pio);
	p->pio = pio;
	p->thread = (HANDLE)_beginthreadex(NULL, 0, SyncWpumpThread, p, 0, NULL);
	if (!p->thread) {
		DeleteCriticalSection(&p->lock);
		goto fail;
	}
	if (!sync_wpump_atexit_set) {
		atexit(sync_wpump_exit);
		sync_wpump_atexit_set = TRUE;
	}
	live_sync_wpumps[num_live_sync_wpumps++] = p;
	c->wr = p;
	debug4("sync write pump started for io:%p", pio);
	return p;

fail:
	errno = errno_from_Win32LastError();
	if (p) {
		if (p->data_evt)
			CloseHandle(p->data_evt);
		free(p->ring);
		free(p);
	}
	return NULL;
}

/* write() for a pumped fd: the caller's buffer straight into the ring.
 * All or nothing per call -- fileio_write_wrapper() hands over at most
 * WRITE_BUFFER_SIZE and expects it taken -- so a call waits (or says
 * EAGAIN) until the whole of it fits. */
int
syncio_pump_write(struct w32_io *pio, const void *buf, size_t len)
{
	struct sync_ctx *c = sync_ctx_get(pio);
	struct sync_wpump *p = c ? c->wr : NULL;
	DWORD n = (DWORD)len, tail, first;
	BOOL was_empty;

	if (!c) {
		errno = ENOMEM;
		return -1;
	}
	if (!p && !(p = sync_wpump_start(pio)))
		return -1;
	if (n > SYNC_WPUMP_RING_SIZE)
		n = SYNC_WPUMP_RING_SIZE;

	for (;;) {
		EnterCriticalSection(&p->lock);
		if (p->err) {
			DWORD err = p->err;
			LeaveCriticalSection(&p->lock);
			errno = errno_from_Win32Error(err);
			if (FILETYPE(pio) == FILE_TYPE_PIPE && err == ERROR_BROKEN_PIPE)
				errno = EPIPE;
			debug3("write - pump ERROR:%d, io:%p", err, pio);
			return -1;
		}
		if (SYNC_WPUMP_RING_SIZE - p->len >= n)
			break;
		p->waiting = TRUE;
		LeaveCriticalSection(&p->lock);
		if (!w32_io_is_blocking(pio)) {
			errno = EAGAIN;
			return -1;
		}
		if (wait_for_any_event(NULL, 0, INFINITE) == -1)
			return -1;
	}
	/* reserve under the lock, copy outside it, publish under it: the
	 * region past len is invisible to the pump until len says so */
	tail = (p->head + p->len) % SYNC_WPUMP_RING_SIZE;
	LeaveCriticalSection(&p->lock);
	first = min(n, SYNC_WPUMP_RING_SIZE - tail);
	memcpy(p->ring + tail, buf, first);
	if (n > first)
		memcpy(p->ring, (const char *)buf + first, n - first);
	EnterCriticalSection(&p->lock);
	p->len += n;
	was_empty = p->sleeping;
	LeaveCriticalSection(&p->lock);
	/* only a pump in its wait needs the event */
	if (was_empty)
		SetEvent(p->data_evt);
	return (int)n;
}

/* select()'s view of a pumped fd.  Saying "not ready" also registers the
 * interest, so the pump wakes the waiter when that changes. */
BOOL
syncio_pump_available(struct w32_io *pio, BOOL rd)
{
	struct sync_ctx *c = (struct sync_ctx *)pio->internal.context;
	BOOL ready;

	if (rd) {
		struct sync_pump *p = c ? c->rd : NULL;
		if (!p)
			return FALSE;
		EnterCriticalSection(&p->lock);
		ready = p->len || p->eof;
		if (!ready)
			p->waiting = TRUE;
		LeaveCriticalSection(&p->lock);
	} else {
		struct sync_wpump *p = c ? c->wr : NULL;
		if (!p)
			return TRUE;   /* nothing queued yet: the first write will not block */
		EnterCriticalSection(&p->lock);
		ready = p->err || SYNC_WPUMP_RING_SIZE - p->len >= SYNC_WPUMP_WRITABLE;
		if (!ready)
			p->waiting = TRUE;
		LeaveCriticalSection(&p->lock);
	}
	return ready;
}

/* close: flush the write ring (its bytes were reported written), stop both
 * pumps, drop the context.  Nothing is pending on termio.c's thread paths
 * for a pumped fd, and it must not wait for anything there. */
void
syncio_pump_close(struct w32_io *pio)
{
	struct sync_ctx *c = (struct sync_ctx *)pio->internal.context;

	if (!c)
		return;
	if (c->wr) {
		struct sync_wpump *p = c->wr;
		c->wr = NULL;
		sync_wpump_stop(p);
		pio->write_details.pending = FALSE;
	}
	sync_pump_stop(pio);
	pio->read_details.pending = FALSE;
	free(c);
	pio->internal.context = NULL;
}

/* ====================================================================== */
/* A connected stream socket                                              */
/* ====================================================================== */

#define SOCK_PUMP_RING_SIZE (4 * 1024 * 1024)
#define SOCK_PUMP_CHUNK     (1024 * 1024)
#define SOCK_PUMP_FLUSH_MS  10000

/*
 * The send side is a queue of blocks rather than a ring, so that the
 * packet layer can hand over its output buffer's storage whole instead of
 * copying it (win32sendbuf.c): a block is malloc()ed memory the pump owns,
 * sends from, and then keeps on a short free list for the packet layer to
 * take back in exchange for its next full buffer.  Copied sends -- small
 * packets, and anything not from the packet layer -- go into blocks of
 * their own and coalesce into the tail block while it has room.  The byte
 * limit and "writable" are as for the ring; SOCK_WQUEUE_CAP bounds the
 * count.
 */
#define SOCK_WQUEUE_CAP     256          /* blocks queued at once */
#define SOCK_WFREE_CAP      32           /* blocks kept for reuse */
#define SOCK_WCOPY_BLOCK    (256 * 1024) /* a block for copied sends */
#define SOCK_HANDOFF_MIN    (8 * 1024)   /* below this a copy beats swapping an allocation */

struct sock_wblock {
	char  *d;
	size_t off, len, alloc;   /* len bytes at d+off; alloc bytes in all */
	BOOL   pinned;            /* the main thread is copying into it: not done even at len 0 */
};

struct sock_wpump {
	SOCKET s;
	HANDLE thread;
	HANDLE data_evt;          /* auto-reset: the queue has bytes */
	CRITICAL_SECTION lock;
	struct sock_wblock q[SOCK_WQUEUE_CAP];
	DWORD  qhead, qn;         /* blocks in flight, in order */
	struct sock_wblock freel[SOCK_WFREE_CAP];
	DWORD  nfree;
	DWORD  len;               /* bytes queued, over all blocks */
	int    err;               /* first WSA error; the pump is dead after it */
	BOOL   waiting;           /* main thread wants room; wake it when there is */
	BOOL   apc_queued;
	BOOL   sleeping;          /* the pump is in its wait: only then is data_evt worth setting */
	volatile LONG stop;
};

struct sock_rpump {
	SOCKET s;
	HANDLE thread;
	HANDLE room_evt;          /* auto-reset: the ring has room */
	CRITICAL_SECTION lock;
	char  *ring;
	DWORD  head, len;
	BOOL   eof;               /* the peer is done: err says how (0 = orderly) */
	int    err;
	BOOL   err_reported;
	BOOL   waiting;           /* main thread wants bytes; wake it when there are */
	BOOL   apc_queued;
	BOOL   sleeping;          /* the pump is in its wait: only then is room_evt worth setting */
	volatile LONG stop;
};

struct sock_pumps {
	struct sock_rpump *rd;
	struct sock_wpump *wr;
};

static struct sock_wpump *live_sock_wpumps[64];
static int num_live_sock_wpumps;
static BOOL sock_atexit_set;

static struct sock_pumps *
sock_pumps_get(struct w32_io *pio)
{
	if (!pio->internal.context)
		pio->internal.context = calloc(1, sizeof(struct sock_pumps));
	return (struct sock_pumps *)pio->internal.context;
}

BOOL
sockio_pump_wanted(struct w32_io *pio)
{
	return pio->internal.state == SOCK_READY;
}

/* ---- send ------------------------------------------------------------ */

/* under the lock: a finished block goes to the free list, or away */
static void
sock_wblock_recycle(struct sock_wpump *p, char *d, size_t alloc)
{
	if (p->nfree < SOCK_WFREE_CAP) {
		p->freel[p->nfree].d = d;
		p->freel[p->nfree].alloc = alloc;
		p->nfree++;
	} else
		free(d);
}

/* under the lock: the pump is dead, nothing queued will go */
static void
sock_wpump_drop_all(struct sock_wpump *p)
{
	while (p->qn) {
		free(p->q[p->qhead].d);
		p->qhead = (p->qhead + 1) % SOCK_WQUEUE_CAP;
		p->qn--;
	}
	p->len = 0;
}

static unsigned __stdcall
SockWpumpThread(_In_ LPVOID lpParameter)
{
	struct sock_wpump *p = (struct sock_wpump *)lpParameter;

	for (;;) {
		struct sock_wblock *b;
		char *d;
		size_t off, take;
		BOOL wake = FALSE;
		int sent;

		EnterCriticalSection(&p->lock);
		if (p->len == 0) {
			if (p->stop) {
				LeaveCriticalSection(&p->lock);
				break;
			}
			LeaveCriticalSection(&p->lock);
			if (pump_spin(&p->len, 0))
				continue;
			EnterCriticalSection(&p->lock);
			if (p->len) {
				LeaveCriticalSection(&p->lock);
				continue;
			}
			/* say so under the lock, so an append that follows sees
			 * a pump about to wait and sets the event */
			p->sleeping = TRUE;
			LeaveCriticalSection(&p->lock);
			WaitForSingleObject(p->data_evt, 200);
			p->sleeping = FALSE;
			continue;
		}
		b = &p->q[p->qhead];
		if (b->len == 0) {
			/* a pinned block still being filled at the head, with
			 * published bytes behind it: cannot happen with one
			 * producer, which fills the tail; but never send from
			 * an empty block */
			LeaveCriticalSection(&p->lock);
			SwitchToThread();
			continue;
		}
		d = b->d;
		off = b->off;
		take = min(b->len, SOCK_PUMP_CHUNK);
		LeaveCriticalSection(&p->lock);

		/* [off, off+take) of the head block is ours until we advance
		 * it: the main thread only appends past len, and only pops
		 * nothing -- popping is ours */
		sent = send(p->s, d + off, (int)take, 0);

		EnterCriticalSection(&p->lock);
		if (sent == SOCKET_ERROR) {
			p->err = WSAGetLastError();
			sock_wpump_drop_all(p);
		} else {
			b->off += sent;
			b->len -= sent;
			p->len -= sent;
			if (b->len == 0 && !b->pinned) {
				sock_wblock_recycle(p, b->d, b->alloc);
				p->qhead = (p->qhead + 1) % SOCK_WQUEUE_CAP;
				p->qn--;
			}
		}
		if (p->waiting && !p->apc_queued) {
			p->waiting = FALSE;
			p->apc_queued = TRUE;
			wake = TRUE;
		}
		LeaveCriticalSection(&p->lock);
		if (wake && !QueueUserAPC(PumpWakeAPC, main_thread, (ULONG_PTR)&p->apc_queued)) {
			EnterCriticalSection(&p->lock);
			p->apc_queued = FALSE;
			LeaveCriticalSection(&p->lock);
		}
		if (p->err)
			break;
	}
	return 0;
}

/* Let the ring drain (bounded), then stop the thread.  A send() blocked on
 * a peer that stopped reading is cut loose by the closesocket() the caller
 * does; here we only wait, so a stuck thread costs the flush timeout. */
static void
sock_wpump_drain(struct sock_wpump *p, DWORD ms)
{
	ULONGLONG t0 = GetTickCount64();
	for (;;) {
		BOOL done;
		EnterCriticalSection(&p->lock);
		done = p->len == 0 || p->err;
		LeaveCriticalSection(&p->lock);
		if (done || GetTickCount64() - t0 > ms)
			break;
		Sleep(1);
	}
}

static void
sock_wpump_stop(struct sock_wpump *p, BOOL socket_closed)
{
	int i;

	sock_wpump_drain(p, SOCK_PUMP_FLUSH_MS);
	InterlockedExchange(&p->stop, 1);
	SetEvent(p->data_evt);
	if (WaitForSingleObject(p->thread, socket_closed ? 5000 : 1000) == WAIT_TIMEOUT) {
		/* still in send(): leak the pump rather than free memory under it */
		debug3("sock write pump %p would not stop; leaking it", p);
		return;
	}
	SleepEx(0, TRUE);   /* run a wake-up it may have queued */
	for (i = 0; i < num_live_sock_wpumps; i++)
		if (live_sock_wpumps[i] == p) {
			live_sock_wpumps[i] = live_sock_wpumps[--num_live_sock_wpumps];
			break;
		}
	CloseHandle(p->thread);
	CloseHandle(p->data_evt);
	DeleteCriticalSection(&p->lock);
	sock_wpump_drop_all(p);
	while (p->nfree)
		free(p->freel[--p->nfree].d);
	free(p);
}

static void
sock_wpump_exit(void)
{
	while (num_live_sock_wpumps)
		sock_wpump_stop(live_sock_wpumps[num_live_sock_wpumps - 1], FALSE);
}

static struct sock_wpump *
sock_wpump_start(struct w32_io *pio)
{
	struct sock_pumps *c = sock_pumps_get(pio);
	struct sock_wpump *p;

	if (!c || num_live_sock_wpumps == (int)(sizeof live_sock_wpumps / sizeof live_sock_wpumps[0])) {
		errno = ENOMEM;
		return NULL;
	}
	if (!(p = calloc(1, sizeof *p)))
		goto fail;
	if (!(p->data_evt = CreateEventW(NULL, FALSE, FALSE, NULL)))
		goto fail;
	InitializeCriticalSection(&p->lock);
	p->s = pio->sock;
	p->thread = (HANDLE)_beginthreadex(NULL, 0, SockWpumpThread, p, 0, NULL);
	if (!p->thread) {
		DeleteCriticalSection(&p->lock);
		goto fail;
	}
	if (!sock_atexit_set) {
		atexit(sock_wpump_exit);
		sock_atexit_set = TRUE;
	}
	live_sock_wpumps[num_live_sock_wpumps++] = p;
	c->wr = p;
	debug4("sock write pump started for io:%p", pio);
	return p;

fail:
	errno = ENOMEM;
	if (p) {
		if (p->data_evt)
			CloseHandle(p->data_evt);
		free(p);
	}
	return NULL;
}

/* under the lock: the smallest free block of at least min_alloc bytes */
static BOOL
sock_wpump_take_free(struct sock_wpump *p, size_t min_alloc, char **d, size_t *alloc)
{
	DWORD i, best = SOCK_WFREE_CAP;

	for (i = 0; i < p->nfree; i++)
		if (p->freel[i].alloc >= min_alloc &&
		    (best == SOCK_WFREE_CAP || p->freel[i].alloc < p->freel[best].alloc))
			best = i;
	if (best == SOCK_WFREE_CAP)
		return FALSE;
	*d = p->freel[best].d;
	*alloc = p->freel[best].alloc;
	p->freel[best] = p->freel[--p->nfree];
	return TRUE;
}

/* under the lock: wait-free append of an empty (or given) block at the tail */
static struct sock_wblock *
sock_wpump_push(struct sock_wpump *p, char *d, size_t off, size_t len, size_t alloc)
{
	struct sock_wblock *b = &p->q[(p->qhead + p->qn) % SOCK_WQUEUE_CAP];

	b->d = d;
	b->off = off;
	b->len = len;
	b->alloc = alloc;
	b->pinned = FALSE;
	p->qn++;
	p->len += (DWORD)len;
	return b;
}

/* Wait (or say EAGAIN) until the queue can take more.  Returns with the
 * lock held and the room in bytes, or -1 with errno and the lock released. */
static int
sock_wpump_room(struct w32_io *pio, struct sock_wpump *p, DWORD *space)
{
	for (;;) {
		EnterCriticalSection(&p->lock);
		if (p->err) {
			int err = p->err;
			LeaveCriticalSection(&p->lock);
			errno = pump_errno_from_wsa(err);
			debug3("send - pump ERROR:%d, io:%p", err, pio);
			return -1;
		}
		*space = p->len < SOCK_PUMP_RING_SIZE ? SOCK_PUMP_RING_SIZE - p->len : 0;
		if (*space && p->qn < SOCK_WQUEUE_CAP)
			return 0;
		/* full: block on room, or say so */
		p->waiting = TRUE;
		LeaveCriticalSection(&p->lock);
		if (!w32_io_is_blocking(pio)) {
			errno = EAGAIN;
			return -1;
		}
		if (wait_for_any_event(NULL, 0, INFINITE) == -1)
			return -1;
	}
}

/* send() by copying: into the tail block while it has room, else a block
 * of its own.  The block is pinned while the copy runs outside the lock,
 * so the pump cannot retire it underneath; len says when the bytes are
 * there. */
int
sockio_pump_send(struct w32_io *pio, const void *buf, size_t len)
{
	struct sock_pumps *c = sock_pumps_get(pio);
	struct sock_wpump *p = c ? c->wr : NULL;
	struct sock_wblock *b = NULL;
	DWORD space, n;
	char *dst, *d;
	size_t alloc;
	BOOL was_empty;

	if (!c) {
		errno = ENOMEM;
		return -1;
	}
	if (!p && !(p = sock_wpump_start(pio)))
		return -1;
	if (sock_wpump_room(pio, p, &space) != 0)
		return -1;
	/* holding the lock */
	n = (DWORD)min((size_t)space, len);
	if (p->qn) {
		b = &p->q[(p->qhead + p->qn - 1) % SOCK_WQUEUE_CAP];
		if (b->alloc - (b->off + b->len) < n)
			b = NULL;
	}
	if (b == NULL) {
		if (!sock_wpump_take_free(p, n, &d, &alloc)) {
			alloc = max(n, SOCK_WCOPY_BLOCK);
			if ((d = malloc(alloc)) == NULL) {
				LeaveCriticalSection(&p->lock);
				errno = ENOMEM;
				return -1;
			}
		}
		b = sock_wpump_push(p, d, 0, 0, alloc);
	}
	b->pinned = TRUE;
	dst = b->d + b->off + b->len;
	LeaveCriticalSection(&p->lock);

	memcpy(dst, buf, n);

	EnterCriticalSection(&p->lock);
	b->len += n;
	b->pinned = FALSE;
	p->len += n;
	was_empty = p->sleeping;
	LeaveCriticalSection(&p->lock);
	/* only a pump in its wait needs the event */
	if (was_empty)
		SetEvent(p->data_evt);
	return (int)n;
}

/* ---- zero-copy send: the packet layer's buffer, whole ----------------- */

BOOL
sockio_pump_handoff_ok(struct w32_io *pio, size_t len)
{
	struct sock_pumps *c = sock_pumps_get(pio);
	struct sock_wpump *p = c ? c->wr : NULL;
	BOOL ok;

	if (len < SOCK_HANDOFF_MIN || len > SOCK_PUMP_RING_SIZE)
		return FALSE;
	if (!p && !(p = sock_wpump_start(pio)))
		return FALSE;
	EnterCriticalSection(&p->lock);
	ok = !p->err && p->len + len <= SOCK_PUMP_RING_SIZE && p->qn < SOCK_WQUEUE_CAP;
	LeaveCriticalSection(&p->lock);
	return ok;
}

char *
sockio_pump_spare(struct w32_io *pio, size_t min_alloc, size_t *alloc)
{
	struct sock_pumps *c = (struct sock_pumps *)pio->internal.context;
	struct sock_wpump *p = c ? c->wr : NULL;
	char *d = NULL;

	*alloc = 0;
	if (!p)
		return NULL;
	EnterCriticalSection(&p->lock);
	if (!sock_wpump_take_free(p, min_alloc, &d, alloc))
		d = NULL;
	LeaveCriticalSection(&p->lock);
	return d;
}

void
sockio_pump_spare_back(struct w32_io *pio, char *d, size_t alloc)
{
	struct sock_pumps *c = (struct sock_pumps *)pio->internal.context;
	struct sock_wpump *p = c ? c->wr : NULL;

	if (!p) {
		free(d);
		return;
	}
	EnterCriticalSection(&p->lock);
	sock_wblock_recycle(p, d, alloc);
	LeaveCriticalSection(&p->lock);
}

/* The caller checked sockio_pump_handoff_ok() and nothing else appends
 * meanwhile (one producer), so this cannot fail. */
void
sockio_pump_send_owned(struct w32_io *pio, char *d, size_t off, size_t len, size_t alloc)
{
	struct sock_pumps *c = (struct sock_pumps *)pio->internal.context;
	struct sock_wpump *p = c->wr;
	BOOL was_empty;

	EnterCriticalSection(&p->lock);
	sock_wpump_push(p, d, off, len, alloc);
	was_empty = p->sleeping;
	LeaveCriticalSection(&p->lock);
	if (was_empty)
		SetEvent(p->data_evt);
}

/* ---- recv ------------------------------------------------------------ */

static unsigned __stdcall
SockRpumpThread(_In_ LPVOID lpParameter)
{
	struct sock_rpump *p = (struct sock_rpump *)lpParameter;

	while (!p->stop) {
		DWORD tail, space, take;
		BOOL wake = FALSE;
		int got;

		EnterCriticalSection(&p->lock);
		space = SOCK_PUMP_RING_SIZE - p->len;
		tail = (p->head + p->len) % SOCK_PUMP_RING_SIZE;
		LeaveCriticalSection(&p->lock);
		if (!space) {
			/* full: the main thread is usually a few microseconds
			 * from taking some -- spin before sleeping */
			if (pump_spin(&p->len, SOCK_PUMP_RING_SIZE))
				continue;
			EnterCriticalSection(&p->lock);
			if (p->len == SOCK_PUMP_RING_SIZE)
				p->sleeping = TRUE;
			LeaveCriticalSection(&p->lock);
			if (!p->sleeping)
				continue;
			WaitForSingleObject(p->room_evt, 200);
			p->sleeping = FALSE;
			continue;
		}
		take = min(space, SOCK_PUMP_CHUNK);
		take = min(take, SOCK_PUMP_RING_SIZE - tail);

		/* [tail, tail+take) is free and only we append to it */
		got = recv(p->s, p->ring + tail, (int)take, 0);

		EnterCriticalSection(&p->lock);
		if (got > 0)
			p->len += got;
		else {
			p->eof = TRUE;
			p->err = got == 0 ? 0 : WSAGetLastError();
		}
		if (p->waiting && !p->apc_queued) {
			p->waiting = FALSE;
			p->apc_queued = TRUE;
			wake = TRUE;
		}
		LeaveCriticalSection(&p->lock);
		if (wake && !QueueUserAPC(PumpWakeAPC, main_thread, (ULONG_PTR)&p->apc_queued)) {
			EnterCriticalSection(&p->lock);
			p->apc_queued = FALSE;
			LeaveCriticalSection(&p->lock);
		}
		if (got <= 0)
			break;
	}
	return 0;
}

/* The socket must already be closed (or the peer done): that is what
 * returns the thread from recv(). */
static void
sock_rpump_stop(struct sock_rpump *p)
{
	InterlockedExchange(&p->stop, 1);
	SetEvent(p->room_evt);
	if (WaitForSingleObject(p->thread, 5000) == WAIT_TIMEOUT) {
		debug3("sock read pump %p would not stop; leaking it", p);
		return;
	}
	SleepEx(0, TRUE);
	CloseHandle(p->thread);
	CloseHandle(p->room_evt);
	DeleteCriticalSection(&p->lock);
	free(p->ring);
	free(p);
}

static struct sock_rpump *
sock_rpump_start(struct w32_io *pio)
{
	struct sock_pumps *c = sock_pumps_get(pio);
	struct sock_rpump *p;

	if (!c) {
		errno = ENOMEM;
		return NULL;
	}
	if (!(p = calloc(1, sizeof *p)))
		goto fail;
	if (!(p->ring = malloc(SOCK_PUMP_RING_SIZE)))
		goto fail;
	if (!(p->room_evt = CreateEventW(NULL, FALSE, FALSE, NULL)))
		goto fail;
	InitializeCriticalSection(&p->lock);
	p->s = pio->sock;
	p->thread = (HANDLE)_beginthreadex(NULL, 0, SockRpumpThread, p, 0, NULL);
	if (!p->thread) {
		DeleteCriticalSection(&p->lock);
		goto fail;
	}
	c->rd = p;
	debug4("sock read pump started for io:%p", pio);
	return p;

fail:
	errno = ENOMEM;
	if (p) {
		if (p->room_evt)
			CloseHandle(p->room_evt);
		free(p->ring);
		free(p);
	}
	return NULL;
}

int
sockio_pump_recv(struct w32_io *pio, void *buf, size_t len)
{
	struct sock_pumps *c = sock_pumps_get(pio);
	struct sock_rpump *p = c ? c->rd : NULL;
	DWORD n, first;
	BOOL was_full;

	if (!c) {
		errno = ENOMEM;
		return -1;
	}
	if (!p && !(p = sock_rpump_start(pio)))
		return -1;

	for (;;) {
		EnterCriticalSection(&p->lock);
		if (p->len)
			break;
		if (p->eof) {
			int err = p->err;
			BOOL reported = p->err_reported;
			p->err_reported = TRUE;
			LeaveCriticalSection(&p->lock);
			if (!err || reported) {
				debug4("recv - connection closed, io:%p", pio);
				return 0;
			}
			errno = pump_errno_from_wsa(err);
			debug3("recv - pump ERROR:%d, io:%p", err, pio);
			return -1;
		}
		p->waiting = TRUE;
		LeaveCriticalSection(&p->lock);
		if (!w32_io_is_blocking(pio)) {
			errno = EAGAIN;
			return -1;
		}
		if (wait_for_any_event(NULL, 0, INFINITE) == -1)
			return -1;
	}
	/* take under the lock, copy outside it, release under it: the pump
	 * only appends past len, so [head, head+n) is ours meanwhile */
	n = (DWORD)min((size_t)p->len, len);
	first = min(n, SOCK_PUMP_RING_SIZE - p->head);
	LeaveCriticalSection(&p->lock);
	memcpy(buf, p->ring + p->head, first);
	if (n > first)
		memcpy((char *)buf + first, p->ring, n - first);
	EnterCriticalSection(&p->lock);
	p->head = (p->head + n) % SOCK_PUMP_RING_SIZE;
	p->len -= n;
	was_full = p->sleeping;
	LeaveCriticalSection(&p->lock);
	/* only a pump in its wait needs the event */
	if (was_full)
		SetEvent(p->room_evt);
	return (int)n;
}

/* select()'s view of a pumped socket.  Saying "not ready" also registers
 * the interest, so the pump wakes the waiter when that changes. */
BOOL
sockio_pump_available(struct w32_io *pio, BOOL rd)
{
	struct sock_pumps *c = (struct sock_pumps *)pio->internal.context;
	BOOL ready;

	if (rd) {
		struct sock_rpump *p = c ? c->rd : NULL;
		if (!p)
			return FALSE;
		EnterCriticalSection(&p->lock);
		ready = p->len || p->eof;
		if (!ready)
			p->waiting = TRUE;
		LeaveCriticalSection(&p->lock);
	} else {
		struct sock_wpump *p = c ? c->wr : NULL;
		if (!p)
			return TRUE;   /* nothing queued yet: the first send() will not block */
		EnterCriticalSection(&p->lock);
		ready = p->err || (p->len < SOCK_PUMP_RING_SIZE && p->qn < SOCK_WQUEUE_CAP);
		if (!ready)
			p->waiting = TRUE;
		LeaveCriticalSection(&p->lock);
	}
	return ready;
}

/* select() is interested in reads: the read pump starts on the first ask
 * and keeps recv() running from then on. */
void
sockio_pump_on_select(struct w32_io *pio)
{
	struct sock_pumps *c = sock_pumps_get(pio);

	if (c && !c->rd && !sock_rpump_start(pio)) {
		/* nothing to hand the error to yet; recv() will retry the start */
		errno = 0;
	}
}

/* what send() reported written must reach the wire before the FIN */
void
sockio_pump_shutdown(struct w32_io *pio, int how)
{
	struct sock_pumps *c = (struct sock_pumps *)pio->internal.context;

	if (how != SD_RECEIVE && c && sockio_pump_wanted(pio) && c->wr)
		sock_wpump_drain(c->wr, SOCK_PUMP_FLUSH_MS);
}

/* Drain what send() has reported written, then let closesocket() return
 * the threads from their blocking calls before joining them. */
BOOL
sockio_pump_close(struct w32_io *pio)
{
	struct sock_pumps *c = (struct sock_pumps *)pio->internal.context;

	if (pio->internal.state != SOCK_READY || !c)
		return FALSE;
	pio->internal.context = NULL;
	if (c->wr)
		sock_wpump_drain(c->wr, SOCK_PUMP_FLUSH_MS);
	closesocket(pio->sock);
	if (c->wr)
		sock_wpump_stop(c->wr, TRUE);
	if (c->rd)
		sock_rpump_stop(c->rd);
	free(c);
	return TRUE;
}
