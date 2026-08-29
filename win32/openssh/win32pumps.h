/*
 * win32pumps.h -- reader and writer threads for the fds that carry bulk
 * data in the Windows OpenSSH client: a pipe or file on stdin/stdout, and
 * the connection socket.  See win32pumps.c.
 *
 * Part of the rsync for Windows port (github.com/nuket/rsync-windows); it
 * is compiled into Microsoft's Win32-OpenSSH posix_compat library beside
 * the files it serves, and those files hook into it through the patches in
 * win32/openssh/patches (0001 for termio.c/fileio.c, 0002 for socketio.c).
 * Include after "w32fd.h".
 *
 * Copyright (c) 2026 Max Vilimpoc.  Same licence as the file that includes
 * it (BSD 2-clause, contrib/win32).
 */
#ifndef WIN32PUMPS_H
#define WIN32PUMPS_H

struct w32_io;

/* ---- a pipe or file on a NONSOCK_SYNC_FD (termio.c, fileio.c) --------- */

/* TRUE for a pipe or a disk file; a console keeps the original paths.
 * Cached on the fd after the first call (GetFileType() is a syscall). */
BOOL syncio_pump_wanted(struct w32_io *pio);
int  syncio_pump_read(struct w32_io *pio, void *buf, size_t len);
int  syncio_pump_write(struct w32_io *pio, const void *buf, size_t len);
/* select()'s view: ready, or interest registered for a wake-up */
BOOL syncio_pump_available(struct w32_io *pio, BOOL rd);
/* select() is asking about reads: make sure the read pump runs */
int  syncio_pump_prepare(struct w32_io *pio);
/* on close: flush and stop both pumps, free the context; a no-op if the
 * fd never had one */
void syncio_pump_close(struct w32_io *pio);

#define SYNCIO_PUMPED(pio) ((pio)->type == NONSOCK_SYNC_FD && syncio_pump_wanted(pio))

/* ---- a connected stream socket (socketio.c) --------------------------- */

BOOL sockio_pump_wanted(struct w32_io *pio);
int  sockio_pump_recv(struct w32_io *pio, void *buf, size_t len);
int  sockio_pump_send(struct w32_io *pio, const void *buf, size_t len);
BOOL sockio_pump_available(struct w32_io *pio, BOOL rd);
/* select() is asking about reads: make sure the read pump runs */
void sockio_pump_on_select(struct w32_io *pio);
/* before shutdown(): what send() reported written reaches the wire first */
void sockio_pump_shutdown(struct w32_io *pio, int how);
/* close: drains, closes the socket, stops the threads.  FALSE if the
 * socket was never pumped, in which case the caller closes it. */
BOOL sockio_pump_close(struct w32_io *pio);

/* ---- zero-copy send (win32sendbuf.c) --------------------------------- */

/* The w32_io behind a fd (w32fd.c); NULL if there is none. */
struct w32_io *w32_io_from_fd(int fd);
/* TRUE if a buffer of len bytes is worth handing over whole and there is
 * room for it in the queue: the caller then swaps the buffer's storage
 * for a spare and passes the storage to sockio_pump_send_owned(). */
BOOL sockio_pump_handoff_ok(struct w32_io *pio, size_t len);
/* A spare block of at least min_alloc bytes from the pump's free list, or
 * NULL; *alloc gets its size.  Return an unused one with sockio_pump_spare_back(). */
char *sockio_pump_spare(struct w32_io *pio, size_t min_alloc, size_t *alloc);
void sockio_pump_spare_back(struct w32_io *pio, char *d, size_t alloc);
/* Queue a malloc()ed block for sending: len bytes at d+off, alloc bytes
 * in all.  The pump owns d from here and frees or recycles it. */
void sockio_pump_send_owned(struct w32_io *pio, char *d, size_t off, size_t len, size_t alloc);

#endif /* WIN32PUMPS_H */
