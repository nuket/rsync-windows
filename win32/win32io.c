/*
 * Windows fd routing: pipes, sockets, and select()/poll() over both.
 *
 * rsync assumes a Unix fd space where read/write/select work uniformly over
 * files, pipes and sockets.  Windows splits those into CRT fds and Winsock
 * SOCKETs, and select() only accepts the latter.  This file bridges the gap.
 *
 * Copyright (C) 2026 Max Vilimpoc, rsync CMake/Windows port.
 * Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
 */

#include "rsync.h"
#include "win32/win32undef.h"
#include <poll.h>	/* struct pollfd and nfds_t for win32_poll() below */

/* Set RSYNC_WIN32_DEBUG=1 to trace the shim's fd operations to stderr. */
static int win32_trace(void)
{
	static int on = -1;

	if (on < 0) {
		const char *e = getenv("RSYNC_WIN32_DEBUG");
		on = e && *e && *e != '0';
	}
	return on;
}

#define TRACE(...) \
	do { if (win32_trace()) { \
		fprintf(stderr, "[w32] " __VA_ARGS__); \
		fflush(stderr); \
	} } while (0)

/* ------------------------------------------------------------ socket table */

#define MAX_SOCKFDS 256

static SOCKET sock_table[MAX_SOCKFDS];
static int    sock_used[MAX_SOCKFDS];

int win32_is_sockfd(int fd)
{
	return fd >= WIN32_SOCK_BASE && fd < WIN32_SOCK_BASE + MAX_SOCKFDS;
}

SOCKET win32_sockfd_handle(int fd)
{
	int idx = fd - WIN32_SOCK_BASE;

	if (!win32_is_sockfd(fd) || !sock_used[idx])
		return INVALID_SOCKET;
	return sock_table[idx];
}

int win32_sockfd_alloc(SOCKET s)
{
	int i;

	if (s == INVALID_SOCKET)
		return -1;

	for (i = 0; i < MAX_SOCKFDS; i++) {
		if (!sock_used[i]) {
			sock_used[i] = 1;
			sock_table[i] = s;
			return WIN32_SOCK_BASE + i;
		}
	}
	closesocket(s);
	errno = EMFILE;
	return -1;
}

static void sockfd_free(int fd)
{
	int idx = fd - WIN32_SOCK_BASE;

	if (win32_is_sockfd(fd))
		sock_used[idx] = 0;
}

/* Map the most common Winsock errors onto the errno values rsync checks. */
static int wsa_to_errno(int err)
{
	switch (err) {
	case WSAEWOULDBLOCK:   return EAGAIN;
	case WSAEINTR:         return EINTR;
	case WSAECONNRESET:    return ECONNRESET;
	case WSAECONNREFUSED:  return ECONNREFUSED;
	case WSAECONNABORTED:  return ECONNABORTED;
	case WSAENOTCONN:      return ENOTCONN;
	case WSAEADDRINUSE:    return EADDRINUSE;
	case WSAETIMEDOUT:     return ETIMEDOUT;
	case WSAEHOSTUNREACH:  return EHOSTUNREACH;
	case WSAENETUNREACH:   return ENETUNREACH;
	case WSAEAFNOSUPPORT:  return EAFNOSUPPORT;
	case WSAEINVAL:        return EINVAL;
	case WSAEACCES:        return EACCES;
	case WSAEMFILE:        return EMFILE;
	default:               return EIO;
	}
}

static int sock_fail(void)
{
	errno = wsa_to_errno(WSAGetLastError());
	return -1;
}

/* --------------------------------------------------------------- CRT fds */

/* Non-blocking state for CRT fds, tracked separately because Windows has no
 * F_GETFL equivalent for pipe handles. */
#define MAX_CRTFDS 2048
static unsigned char fd_nonblock[MAX_CRTFDS];

static HANDLE fd_handle(int fd)
{
	intptr_t h;

	if (fd < 0 || fd >= MAX_CRTFDS)
		return INVALID_HANDLE_VALUE;
	h = _get_osfhandle(fd);
	return h == -1 ? INVALID_HANDLE_VALUE : (HANDLE)h;
}

static int handle_is_pipe(HANDLE h)
{
	return h != INVALID_HANDLE_VALUE && GetFileType(h) == FILE_TYPE_PIPE;
}

/* ------------------------------------------------------------- pipe pumps */

/*
 * Windows cannot wait for a pipe to become readable.  PeekNamedPipe answers
 * only "is there data right now", which leaves win32_select() polling, and a
 * poll needs an interval to wait between attempts.  Every choice of interval
 * is wrong on a fast link: Sleep() rounds up to the system timer tick, 15.6ms
 * on a machine where nothing has asked for better, which throttles a bulk
 * transfer to one pipe buffer per tick; spinning instead keeps up, but burns
 * the core ssh needs for its own crypto.
 *
 * So give each polled pipe a pump: a thread doing ordinary blocking ReadFile
 * into a ring buffer, signalling an event whenever the ring stops being
 * empty.  "Readable" becomes a real waitable object, so win32_select() can
 * block on it and wake the instant bytes land -- no interval, no spin.  The
 * ring also decouples the two sides, letting the peer go on filling it while
 * rsync is busy checksumming and writing, rather than stalling on a full
 * pipe until rsync comes back for more.
 *
 * A pump owns its fd once it exists: the bytes have left the pipe, so every
 * later read of that fd has to come out of the ring instead.
 */

#define PUMP_RING_SIZE (1024 * 1024)
#define PUMP_CHUNK     (64 * 1024)

struct pipe_pump {
	HANDLE h;                /* the pipe being drained */
	HANDLE thread;
	HANDLE data_evt;         /* set while the ring holds bytes, or at EOF */
	HANDLE room_evt;         /* set while the ring has space to fill */
	CRITICAL_SECTION lock;
	char  *ring;
	size_t head, tail, len;  /* head: next byte out; tail: next byte in */
	int    eof;
	DWORD  err;
	volatile LONG stop;
};

static struct pipe_pump *pumps[MAX_CRTFDS];

static unsigned __stdcall pump_thread(void *arg)
{
	struct pipe_pump *p = (struct pipe_pump *)arg;
	char chunk[PUMP_CHUNK];

	while (!p->stop) {
		DWORD got = 0, want;
		size_t space, first;

		EnterCriticalSection(&p->lock);
		space = PUMP_RING_SIZE - p->len;
		if (!space)
			ResetEvent(p->room_evt);
		LeaveCriticalSection(&p->lock);

		if (!space) {
			WaitForSingleObject(p->room_evt, INFINITE);
			continue;
		}

		want = (DWORD)(space < PUMP_CHUNK ? space : PUMP_CHUNK);
		if (!ReadFile(p->h, chunk, want, &got, NULL) || got == 0) {
			EnterCriticalSection(&p->lock);
			p->err = got ? 0 : GetLastError();
			p->eof = 1;
			SetEvent(p->data_evt);   /* EOF is a readable event too */
			LeaveCriticalSection(&p->lock);
			break;
		}

		EnterCriticalSection(&p->lock);
		first = PUMP_RING_SIZE - p->tail;
		if (first > got)
			first = got;
		memcpy(p->ring + p->tail, chunk, first);
		if (got > first)
			memcpy(p->ring, chunk + first, got - first);
		p->tail = (p->tail + got) % PUMP_RING_SIZE;
		p->len += got;
		SetEvent(p->data_evt);
		LeaveCriticalSection(&p->lock);
	}
	return 0;
}

/* Return fd's pump, starting one if `create` and fd is a pipe.  A NULL back
 * from a create call means fd is not something a pump can drain, and the
 * caller should read or poll it directly as before. */
static struct pipe_pump *pump_for(int fd, int create)
{
	struct pipe_pump *p;
	HANDLE h;
	DWORD mode;

	if (fd < 0 || fd >= MAX_CRTFDS)
		return NULL;
	if (pumps[fd] || !create)
		return pumps[fd];

	h = fd_handle(fd);
	if (!handle_is_pipe(h))
		return NULL;

	if (!(p = calloc(1, sizeof *p)))
		return NULL;
	if (!(p->ring = malloc(PUMP_RING_SIZE))) {
		free(p);
		return NULL;
	}
	p->h = h;
	p->data_evt = CreateEventA(NULL, TRUE, FALSE, NULL);  /* manual reset */
	p->room_evt = CreateEventA(NULL, TRUE, TRUE, NULL);
	if (!p->data_evt || !p->room_evt) {
		if (p->data_evt) CloseHandle(p->data_evt);
		if (p->room_evt) CloseHandle(p->room_evt);
		free(p->ring);
		free(p);
		return NULL;
	}
	InitializeCriticalSection(&p->lock);

	/* The pump wants ReadFile to block, whatever O_NONBLOCK the fd carries;
	 * non-blocking reads are served from the ring instead (pump_read). */
	mode = PIPE_READMODE_BYTE | PIPE_WAIT;
	SetNamedPipeHandleState(h, &mode, NULL, NULL);

	p->thread = (HANDLE)_beginthreadex(NULL, 0, pump_thread, p, 0, NULL);
	if (!p->thread) {
		CloseHandle(p->data_evt);
		CloseHandle(p->room_evt);
		DeleteCriticalSection(&p->lock);
		free(p->ring);
		free(p);
		return NULL;
	}

	TRACE("pump started for fd %d\n", fd);
	pumps[fd] = p;
	return p;
}

static void pump_stop(int fd)
{
	struct pipe_pump *p;

	if (fd < 0 || fd >= MAX_CRTFDS || !(p = pumps[fd]))
		return;
	pumps[fd] = NULL;

	InterlockedExchange(&p->stop, 1);
	SetEvent(p->room_evt);
	CancelIoEx(p->h, NULL);        /* unblock a ReadFile already in flight */
	WaitForSingleObject(p->thread, 2000);

	CloseHandle(p->thread);
	CloseHandle(p->data_evt);
	CloseHandle(p->room_evt);
	DeleteCriticalSection(&p->lock);
	free(p->ring);
	free(p);
}

/* Serve a read out of the ring, blocking or not as the fd asks. */
static int pump_read(struct pipe_pump *p, int fd, void *buf, unsigned int count)
{
	for (;;) {
		size_t n = 0, first;
		int eof;
		DWORD err;

		EnterCriticalSection(&p->lock);
		if (p->len) {
			n = p->len < count ? p->len : count;
			first = PUMP_RING_SIZE - p->head;
			if (first > n)
				first = n;
			memcpy(buf, p->ring + p->head, first);
			if (n > first)
				memcpy((char *)buf + first, p->ring, n - first);
			p->head = (p->head + n) % PUMP_RING_SIZE;
			p->len -= n;
			if (!p->len && !p->eof)
				ResetEvent(p->data_evt);
			SetEvent(p->room_evt);
		}
		eof = p->eof;
		err = p->err;
		LeaveCriticalSection(&p->lock);

		if (n)
			return (int)n;
		if (eof) {
			if (err && err != ERROR_BROKEN_PIPE
			 && err != ERROR_PIPE_NOT_CONNECTED
			 && err != ERROR_OPERATION_ABORTED) {
				errno = EIO;
				return -1;
			}
			return 0;
		}
		if (fd >= 0 && fd < MAX_CRTFDS && fd_nonblock[fd]) {
			errno = EAGAIN;
			return -1;
		}
		WaitForSingleObject(p->data_evt, INFINITE);
	}
}

/* ------------------------------------------------------------ read/write */

int win32_read(int fd, void *buf, unsigned int count)
{
	HANDLE h;
	DWORD got = 0;

	if (win32_is_sockfd(fd)) {
		int n = recv(win32_sockfd_handle(fd), (char *)buf, (int)count, 0);
		if (n == SOCKET_ERROR)
			return sock_fail();
		return n;
	}

	h = fd_handle(fd);
	if (h == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return -1;
	}

	if (!handle_is_pipe(h))
		return _read(fd, buf, count);

	/* Once a pump has taken the fd over the bytes are in its ring, not in
	 * the pipe -- reading the pipe here would block or lose data. */
	{
		struct pipe_pump *pp = pump_for(fd, 0);
		if (pp)
			return pump_read(pp, fd, buf, count);
	}

	if (!ReadFile(h, buf, count, &got, NULL)) {
		TRACE("read(%d,%u) ReadFile err=%lu\n", fd, count, GetLastError());
		DWORD err = GetLastError();
		if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED)
			return 0;  /* EOF */
		if (err == ERROR_NO_DATA) {
			errno = EAGAIN;
			return -1;
		}
		errno = EIO;
		return -1;
	}
	TRACE("read(%d,%u) = %lu\n", fd, count, got);
	/* A PIPE_NOWAIT pipe with nothing queued succeeds with zero bytes. */
	if (got == 0 && fd < MAX_CRTFDS && fd_nonblock[fd]) {
		errno = EAGAIN;
		return -1;
	}
	return (int)got;
}

int win32_write(int fd, const void *buf, unsigned int count)
{
	HANDLE h;
	DWORD put = 0;

	if (win32_is_sockfd(fd)) {
		int n = send(win32_sockfd_handle(fd), (const char *)buf, (int)count, 0);
		if (n == SOCKET_ERROR)
			return sock_fail();
		return n;
	}

	h = fd_handle(fd);
	if (h == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return -1;
	}

	if (!handle_is_pipe(h))
		return _write(fd, buf, count);

	if (!WriteFile(h, buf, count, &put, NULL)) {
		DWORD err = GetLastError();
		if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) {
			errno = EPIPE;
			return -1;
		}
		errno = EIO;
		return -1;
	}
	TRACE("write(%d,%u) = %lu\n", fd, count, put);
	if (put == 0 && count > 0 && fd < MAX_CRTFDS && fd_nonblock[fd]) {
		errno = EAGAIN;
		return -1;
	}
	return (int)put;
}

int win32_close(int fd)
{
	if (win32_is_sockfd(fd)) {
		SOCKET s = win32_sockfd_handle(fd);
		sockfd_free(fd);
		if (s != INVALID_SOCKET && closesocket(s) == SOCKET_ERROR)
			return sock_fail();
		return 0;
	}
	pump_stop(fd);
	if (fd >= 0 && fd < MAX_CRTFDS)
		fd_nonblock[fd] = 0;
	return _close(fd);
}

int win32_dup(int fd)
{
	if (win32_is_sockfd(fd)) {
		errno = ENOSYS;
		return -1;
	}
	return _dup(fd);
}

int win32_dup2(int oldfd, int newfd)
{
	if (win32_is_sockfd(oldfd) || win32_is_sockfd(newfd)) {
		errno = ENOSYS;
		return -1;
	}
	return _dup2(oldfd, newfd);
}

/* ------------------------------------------------------------------ pipes */

/* Anonymous pipes can't be switched to non-blocking mode, so build the pair
 * out of a uniquely-named pipe instead; that lets set_nonblocking() work. */
int win32_pipe(int fd[2])
{
	static LONG counter;
	char name[128];
	HANDLE rd, wr;
	SECURITY_ATTRIBUTES sa;

	sa.nLength = sizeof sa;
	sa.lpSecurityDescriptor = NULL;
	sa.bInheritHandle = TRUE;

	snprintf(name, sizeof name, "\\\\.\\pipe\\rsync-%lu-%ld",
		 (unsigned long)GetCurrentProcessId(),
		 (long)InterlockedIncrement(&counter));

	rd = CreateNamedPipeA(name, PIPE_ACCESS_INBOUND,
			      PIPE_TYPE_BYTE | PIPE_WAIT, 1,
			      65536, 65536, 0, &sa);
	if (rd == INVALID_HANDLE_VALUE) {
		errno = EMFILE;
		return -1;
	}

	wr = CreateFileA(name, GENERIC_WRITE, 0, &sa, OPEN_EXISTING,
			 FILE_ATTRIBUTE_NORMAL, NULL);
	if (wr == INVALID_HANDLE_VALUE) {
		CloseHandle(rd);
		errno = EMFILE;
		return -1;
	}

	fd[0] = _open_osfhandle((intptr_t)rd, _O_RDONLY | _O_BINARY);
	fd[1] = _open_osfhandle((intptr_t)wr, _O_BINARY);
	if (fd[0] < 0 || fd[1] < 0) {
		if (fd[0] >= 0) _close(fd[0]); else CloseHandle(rd);
		if (fd[1] >= 0) _close(fd[1]); else CloseHandle(wr);
		errno = EMFILE;
		return -1;
	}
	return 0;
}

/* rsync only uses socketpair() for local child plumbing; a connected pair of
 * loopback TCP sockets behaves closely enough. */
int win32_socketpair(int domain, int type, int protocol, int sv[2])
{
	SOCKET listener = INVALID_SOCKET, a = INVALID_SOCKET, b = INVALID_SOCKET;
	struct sockaddr_in addr;
	int len = sizeof addr;

	(void)domain; (void)protocol;

	listener = socket(AF_INET, type, 0);
	if (listener == INVALID_SOCKET)
		return sock_fail();

	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;

	if (bind(listener, (struct sockaddr *)&addr, sizeof addr) == SOCKET_ERROR
	 || listen(listener, 1) == SOCKET_ERROR
	 || getsockname(listener, (struct sockaddr *)&addr, &len) == SOCKET_ERROR)
		goto fail;

	a = socket(AF_INET, type, 0);
	if (a == INVALID_SOCKET)
		goto fail;
	if (connect(a, (struct sockaddr *)&addr, sizeof addr) == SOCKET_ERROR)
		goto fail;

	b = accept(listener, NULL, NULL);
	if (b == INVALID_SOCKET)
		goto fail;

	closesocket(listener);

	/* sockfd_alloc() closes the socket it was handed if the table is full,
	 * so only the half that succeeded still needs releasing. */
	sv[0] = win32_sockfd_alloc(a);
	sv[1] = win32_sockfd_alloc(b);
	if (sv[0] < 0 || sv[1] < 0) {
		if (sv[0] >= 0)
			win32_close(sv[0]);
		if (sv[1] >= 0)
			win32_close(sv[1]);
		sv[0] = sv[1] = -1;
		errno = EMFILE;
		return -1;
	}
	return 0;

fail:
	{
		int saved = WSAGetLastError();
		if (listener != INVALID_SOCKET) closesocket(listener);
		if (a != INVALID_SOCKET) closesocket(a);
		if (b != INVALID_SOCKET) closesocket(b);
		WSASetLastError(saved);
	}
	return sock_fail();
}

/* ----------------------------------------------------------------- fcntl */

int win32_fcntl(int fd, int cmd, ...)
{
	long arg = 0;

	/* Only the setters are called with a third argument; reading one that
	 * was never passed is undefined, however well it happens to work. */
	if (cmd == F_SETFL || cmd == F_SETFD) {
		va_list ap;

		va_start(ap, cmd);
		arg = va_arg(ap, long);
		va_end(ap);
	}

	switch (cmd) {
	case F_GETFL:
		if (win32_is_sockfd(fd))
			return 0;
		return (fd >= 0 && fd < MAX_CRTFDS && fd_nonblock[fd]) ? O_NONBLOCK : 0;

	case F_SETFL: {
		int want = (arg & O_NONBLOCK) ? 1 : 0;

		if (win32_is_sockfd(fd)) {
			u_long mode = want;
			if (ioctlsocket(win32_sockfd_handle(fd), FIONBIO, &mode) == SOCKET_ERROR)
				return sock_fail();
			return 0;
		}
		{
			HANDLE h = fd_handle(fd);
			/* A pumped fd stays PIPE_WAIT for its pump thread; the
			 * recorded flag is enough, pump_read() honours it. */
			if (handle_is_pipe(h) && !pump_for(fd, 0)) {
				DWORD mode = PIPE_READMODE_BYTE
					   | (want ? PIPE_NOWAIT : PIPE_WAIT);
				if (!SetNamedPipeHandleState(h, &mode, NULL, NULL)) {
					/* Not our named pipe (e.g. inherited stdio);
					 * remember the intent and let read/write cope. */
				}
			}
			if (fd >= 0 && fd < MAX_CRTFDS)
				fd_nonblock[fd] = (unsigned char)want;
		}
		return 0;
	}

	case F_GETFD:
		return 0;
	case F_SETFD:
		return 0;
	case F_SETLK:
		return 0;   /* rsync only uses this for advisory daemon locks */
	default:
		errno = EINVAL;
		return -1;
	}
}

/* ---------------------------------------------------------------- select */

/* Can this CRT fd be read without blocking?  Returns 1 ready, 0 not ready. */
static int crtfd_readable(int fd)
{
	HANDLE h = fd_handle(fd);
	DWORD avail = 0;
	struct pipe_pump *p = pump_for(fd, 0);

	if (p) {
		int ready;
		EnterCriticalSection(&p->lock);
		ready = p->len > 0 || p->eof;
		LeaveCriticalSection(&p->lock);
		return ready;
	}

	if (h == INVALID_HANDLE_VALUE)
		return 1;   /* report ready so the caller gets a real error */

	switch (GetFileType(h)) {
	case FILE_TYPE_PIPE:
		if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL))
			return 1;   /* broken pipe: readable, yields EOF */
		return avail > 0;
	case FILE_TYPE_CHAR:
		return WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
	default:
		return 1;   /* regular files are always ready */
	}
}

/* Pipes are writable unless the peer is gone; anonymous byte pipes give us no
 * free-space query, so treat them as ready and let a PIPE_NOWAIT WriteFile
 * return EAGAIN if the buffer happens to be full. */
static int crtfd_writable(int fd)
{
	HANDLE h = fd_handle(fd);

	return h != INVALID_HANDLE_VALUE;
}

/* ------------------------------------------------------------- poll waits */

/*
 * Windows offers no way to wait on a pipe becoming readable -- PeekNamedPipe
 * only answers "is there data now" -- so the mixed path below has to poll,
 * and how long it waits between attempts sets a ceiling on throughput.
 *
 * Sleep() is the obvious wait and the wrong one: its argument is rounded up
 * to the system timer tick, which is 15.6ms on a machine where nothing has
 * asked for better, so Sleep(1) really sleeps ~16ms.  rsync reads the pipe
 * IO_BUFFER_SIZE (32KB) at a time, so one sleep per empty pipe caps a bulk
 * transfer at a couple of MB/s no matter how fast the link is.
 *
 * What the loop actually waits for, during a transfer, is the peer refilling
 * a pipe it is writing to continuously -- microseconds away.  So spin first,
 * yielding rather than burning the core, and only sleep once the wait has
 * gone on long enough to mean the peer is genuinely idle.  Those sleeps use
 * a high-resolution waitable timer, which is not quantised to the tick, so
 * even the fallback costs tens of microseconds rather than sixteen ms.
 */

/* Sleep for `usec`, without rounding up to the system timer tick.  Falls
 * back to Sleep() if the high-resolution timer is unavailable (pre-1803). */
static void hires_sleep(DWORD usec)
{
	static RSYNC_TLS HANDLE timer;
	static RSYNC_TLS int timer_failed;
	LARGE_INTEGER due;

	if (!timer && !timer_failed) {
		timer = CreateWaitableTimerExW(NULL, NULL,
					       CREATE_WAITABLE_TIMER_MANUAL_RESET
					       | CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
					       TIMER_ALL_ACCESS);
		if (!timer)
			timer_failed = 1;
	}
	if (!timer) {
		Sleep(usec < 1000 ? 1 : usec / 1000);
		return;
	}

	due.QuadPart = -(LONGLONG)usec * 10;   /* negative: relative, 100ns units */
	if (!SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
		Sleep(usec < 1000 ? 1 : usec / 1000);
		return;
	}
	WaitForSingleObject(timer, INFINITE);
}

/*
 * Back off by one step.  `waited` counts the attempts made so far in this
 * win32_select() call, so a burst of traffic never pays for the idling the
 * previous lull did.
 *
 * The first two bands are yields, not sleeps: SwitchToThread() gives the
 * rest of this timeslice to another runnable thread and returns as soon as
 * there is nothing to give it to, which keeps the wait in the microseconds
 * an active transfer needs.  Roughly: yield for the first ~256 attempts,
 * then 250us apiece, then settle to 5ms once it is clear nobody is talking.
 */
static void poll_backoff(unsigned waited)
{
	if (waited < 64)
		YieldProcessor();
	else if (waited < 256)
		SwitchToThread();
	else if (waited < 1024)
		hires_sleep(250);
	else
		hires_sleep(5000);
}

/* ------------------------------------------------------------------ poll */

/* io.c polls a handful of fds at a time (three at most), mixing the socket
 * with pipes, so this translates into the select() above rather than calling
 * WSAPoll -- which would see only the socket.  The exception set is left out
 * deliberately: win32_select() drops CRT fds from it, and a broken pipe
 * already surfaces as readable-then-EOF, which is how the select()-based code
 * detected it before poll() arrived. */
int win32_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
	fd_set rf, wf;
	struct timeval tv, *tvp;
	nfds_t i;
	int rc, count = 0;

	FD_ZERO(&rf);
	FD_ZERO(&wf);

	for (i = 0; i < nfds; i++) {
		fds[i].revents = 0;
		if (fds[i].fd < 0)	/* POSIX: a negative fd is ignored */
			continue;
		if (fds[i].events & (POLLIN | POLLPRI))
			FD_SET((SOCKET)fds[i].fd, &rf);
		if (fds[i].events & POLLOUT)
			FD_SET((SOCKET)fds[i].fd, &wf);
	}

	if (timeout < 0)
		tvp = NULL;
	else {
		tv.tv_sec  = timeout / 1000;
		tv.tv_usec = (timeout % 1000) * 1000;
		tvp = &tv;
	}

	/* With no fds this is poll()-as-sleep, which win32_select() also
	 * implements for the empty-set case. */
	rc = win32_select(0, &rf, &wf, NULL, tvp);
	if (rc <= 0)
		return rc;

	for (i = 0; i < nfds; i++) {
		if (fds[i].fd < 0)
			continue;
		if (FD_ISSET((SOCKET)fds[i].fd, &rf))
			fds[i].revents |= POLLIN;
		if (FD_ISSET((SOCKET)fds[i].fd, &wf))
			fds[i].revents |= POLLOUT;
		if (fds[i].revents)
			count++;
	}

	TRACE("poll nfds=%lu timeout=%d -> %d\n", (unsigned long)nfds, timeout, count);
	return count;
}

int win32_select(int nfds, fd_set *rfds, fd_set *wfds, fd_set *efds,
		 struct timeval *tv)
{
	fd_set sock_r, sock_w, sock_e;
	int    crt_r[FD_SETSIZE], crt_w[FD_SETSIZE];
	int    n_crt_r = 0, n_crt_w = 0;
	int    have_socks = 0;
	unsigned int i;
	DWORD  deadline = 0;
	int    infinite = (tv == NULL);
	unsigned waited = 0;

	(void)nfds;

	TRACE("select nfds=%d r=%u w=%u e=%u tv=%ld.%06ld\n", nfds,
	      rfds ? rfds->fd_count : 0, wfds ? wfds->fd_count : 0,
	      efds ? efds->fd_count : 0,
	      tv ? (long)tv->tv_sec : -1L, tv ? (long)tv->tv_usec : 0L);

	FD_ZERO(&sock_r);
	FD_ZERO(&sock_w);
	FD_ZERO(&sock_e);

	/* Partition the caller's sets into socket and CRT halves. */
	if (rfds) {
		for (i = 0; i < rfds->fd_count; i++) {
			int fd = (int)rfds->fd_array[i];
			if (win32_is_sockfd(fd)) {
				FD_SET(win32_sockfd_handle(fd), &sock_r);
				have_socks = 1;
			} else if (n_crt_r < FD_SETSIZE)
				crt_r[n_crt_r++] = fd;
		}
	}
	if (wfds) {
		for (i = 0; i < wfds->fd_count; i++) {
			int fd = (int)wfds->fd_array[i];
			if (win32_is_sockfd(fd)) {
				FD_SET(win32_sockfd_handle(fd), &sock_w);
				have_socks = 1;
			} else if (n_crt_w < FD_SETSIZE)
				crt_w[n_crt_w++] = fd;
		}
	}
	if (efds) {
		for (i = 0; i < efds->fd_count; i++) {
			int fd = (int)efds->fd_array[i];
			if (win32_is_sockfd(fd)) {
				FD_SET(win32_sockfd_handle(fd), &sock_e);
				have_socks = 1;
			}
		}
	}

	/* select() with no fds at all is rsync's portable millisecond sleep. */
	if (!have_socks && n_crt_r == 0 && n_crt_w == 0) {
		if (rfds) FD_ZERO(rfds);
		if (wfds) FD_ZERO(wfds);
		if (efds) FD_ZERO(efds);
		if (tv)
			Sleep((DWORD)(tv->tv_sec * 1000 + tv->tv_usec / 1000));
		else
			Sleep(INFINITE);
		return 0;
	}

	if (!infinite)
		deadline = GetTickCount() + (DWORD)(tv->tv_sec * 1000 + tv->tv_usec / 1000);

	/* Sockets only: hand straight to Winsock. */
	if (have_socks && n_crt_r == 0 && n_crt_w == 0) {
		int rc = select(0, rfds ? &sock_r : NULL, wfds ? &sock_w : NULL,
				efds ? &sock_e : NULL, tv);
		if (rc == SOCKET_ERROR)
			return sock_fail();

		/* Translate the results back into pseudo-fd sets. */
		{
			fd_set out_r, out_w, out_e;
			FD_ZERO(&out_r); FD_ZERO(&out_w); FD_ZERO(&out_e);
			if (rfds) {
				for (i = 0; i < rfds->fd_count; i++) {
					int fd = (int)rfds->fd_array[i];
					if (FD_ISSET(win32_sockfd_handle(fd), &sock_r))
						FD_SET((SOCKET)fd, &out_r);
				}
				*rfds = out_r;
			}
			if (wfds) {
				for (i = 0; i < wfds->fd_count; i++) {
					int fd = (int)wfds->fd_array[i];
					if (FD_ISSET(win32_sockfd_handle(fd), &sock_w))
						FD_SET((SOCKET)fd, &out_w);
				}
				*wfds = out_w;
			}
			if (efds) {
				for (i = 0; i < efds->fd_count; i++) {
					int fd = (int)efds->fd_array[i];
					if (FD_ISSET(win32_sockfd_handle(fd), &sock_e))
						FD_SET((SOCKET)fd, &out_e);
				}
				*efds = out_e;
			}
		}
		return rc;
	}

	/*
	 * Pipes only, waiting to read: this is the shape rsync-over-ssh has
	 * whenever it is waiting for the peer, and the one that has to be
	 * fast.  Every fd here can be pumped, so wait on the pumps' events --
	 * a real block, woken the moment bytes arrive.
	 *
	 * A write fd in the set skips this: pipe writability is not something
	 * Windows reports, so crtfd_writable() calls them all ready and the
	 * loop below returns without waiting at all.
	 */
	if (!have_socks && n_crt_w == 0 && n_crt_r > 0) {
		HANDLE evts[MAXIMUM_WAIT_OBJECTS];
		int    evt_fd[MAXIMUM_WAIT_OBJECTS];
		int    n_evt = 0, unpumped = 0, j;

		for (j = 0; j < n_crt_r; j++) {
			struct pipe_pump *p = pump_for(crt_r[j], 1);

			if (!p || n_evt == MAXIMUM_WAIT_OBJECTS) {
				unpumped = 1;
				break;
			}
			evts[n_evt] = p->data_evt;
			evt_fd[n_evt++] = crt_r[j];
		}

		/* Anything a pump could not take (a console, a file) leaves the
		 * set unwaitable, so fall through to the polling loop. */
		if (!unpumped) {
			for (;;) {
				fd_set out_r;
				DWORD rc, ms;
				int count = 0;

				FD_ZERO(&out_r);
				for (j = 0; j < n_evt; j++) {
					if (crtfd_readable(evt_fd[j])) {
						FD_SET((SOCKET)evt_fd[j], &out_r);
						count++;
					}
				}
				if (count) {
					TRACE("select -> %d (pump)\n", count);
					if (rfds) *rfds = out_r;
					if (wfds) FD_ZERO(wfds);
					if (efds) FD_ZERO(efds);
					return count;
				}

				if (infinite)
					ms = INFINITE;
				else {
					DWORD now = GetTickCount();
					if ((long)(now - deadline) >= 0)
						ms = 0;
					else
						ms = deadline - now;
				}

				rc = WaitForMultipleObjects((DWORD)n_evt, evts, FALSE, ms);
				if (rc == WAIT_TIMEOUT) {
					if (rfds) FD_ZERO(rfds);
					if (wfds) FD_ZERO(wfds);
					if (efds) FD_ZERO(efds);
					return 0;
				}
				if (rc == WAIT_FAILED) {
					errno = EIO;
					return -1;
				}
			}
		}
	}

	/* Mixed, or a set a pump could not cover: poll, backing off as
	 * described above poll_backoff(). */
	for (;;) {
		fd_set out_r, out_w, out_e;
		int count = 0;
		int j;

		FD_ZERO(&out_r);
		FD_ZERO(&out_w);
		FD_ZERO(&out_e);

		for (j = 0; j < n_crt_r; j++) {
			if (crtfd_readable(crt_r[j])) {
				FD_SET((SOCKET)crt_r[j], &out_r);
				count++;
			}
		}
		for (j = 0; j < n_crt_w; j++) {
			if (crtfd_writable(crt_w[j])) {
				FD_SET((SOCKET)crt_w[j], &out_w);
				count++;
			}
		}

		if (have_socks) {
			fd_set try_r = sock_r, try_w = sock_w, try_e = sock_e;
			struct timeval zero;
			int rc;

			zero.tv_sec = 0;
			zero.tv_usec = 0;
			rc = select(0, rfds ? &try_r : NULL, wfds ? &try_w : NULL,
				    efds ? &try_e : NULL, &zero);
			if (rc == SOCKET_ERROR)
				return sock_fail();
			if (rc > 0) {
				if (rfds) {
					for (i = 0; i < rfds->fd_count; i++) {
						int fd = (int)rfds->fd_array[i];
						if (win32_is_sockfd(fd)
						 && FD_ISSET(win32_sockfd_handle(fd), &try_r)) {
							FD_SET((SOCKET)fd, &out_r);
							count++;
						}
					}
				}
				if (wfds) {
					for (i = 0; i < wfds->fd_count; i++) {
						int fd = (int)wfds->fd_array[i];
						if (win32_is_sockfd(fd)
						 && FD_ISSET(win32_sockfd_handle(fd), &try_w)) {
							FD_SET((SOCKET)fd, &out_w);
							count++;
						}
					}
				}
				if (efds) {
					for (i = 0; i < efds->fd_count; i++) {
						int fd = (int)efds->fd_array[i];
						if (win32_is_sockfd(fd)
						 && FD_ISSET(win32_sockfd_handle(fd), &try_e)) {
							FD_SET((SOCKET)fd, &out_e);
							count++;
						}
					}
				}
			}
		}

		if (count > 0) {
			TRACE("select -> %d (poll)\n", count);
			if (rfds) *rfds = out_r;
			if (wfds) *wfds = out_w;
			if (efds) *efds = out_e;
			return count;
		}

		if (!infinite) {
			DWORD now = GetTickCount();
			if ((long)(now - deadline) >= 0) {
				if (rfds) FD_ZERO(rfds);
				if (wfds) FD_ZERO(wfds);
				if (efds) FD_ZERO(efds);
				return 0;
			}
		}

		poll_backoff(waited);
		if (waited < 0x7fffffffu)
			waited++;
	}
}

/* --------------------------------------------------------- socket wrappers */

/* Winsock reports failure the same way everywhere, so the wrappers below are
 * all the same shape: hand the call its SOCKET, translate SOCKET_ERROR. */
static int sock_result(int rc)
{
	return rc == SOCKET_ERROR ? sock_fail() : 0;
}

int win32_socket(int af, int type, int protocol)
{
	SOCKET s = socket(af, type, protocol);

	if (s == INVALID_SOCKET)
		return sock_fail();
	return win32_sockfd_alloc(s);
}

int win32_accept(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
	SOCKET s = accept(win32_sockfd_handle(fd), addr, addrlen);

	if (s == INVALID_SOCKET)
		return sock_fail();
	return win32_sockfd_alloc(s);
}

int win32_bind(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
	return sock_result(bind(win32_sockfd_handle(fd), addr, addrlen));
}

int win32_connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
	return sock_result(connect(win32_sockfd_handle(fd), addr, addrlen));
}

int win32_listen(int fd, int backlog)
{
	return sock_result(listen(win32_sockfd_handle(fd), backlog));
}

int win32_setsockopt(int fd, int level, int opt, const void *val, socklen_t len)
{
	return sock_result(setsockopt(win32_sockfd_handle(fd), level, opt, (const char *)val, len));
}

int win32_getsockopt(int fd, int level, int opt, void *val, socklen_t *len)
{
	return sock_result(getsockopt(win32_sockfd_handle(fd), level, opt, (char *)val, len));
}

int win32_getpeername(int fd, struct sockaddr *addr, socklen_t *len)
{
	return sock_result(getpeername(win32_sockfd_handle(fd), addr, len));
}

int win32_getsockname(int fd, struct sockaddr *addr, socklen_t *len)
{
	return sock_result(getsockname(win32_sockfd_handle(fd), addr, len));
}

int win32_shutdown(int fd, int how)
{
	return sock_result(shutdown(win32_sockfd_handle(fd), how));
}
