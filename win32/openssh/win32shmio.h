/*
 * win32shmio.h -- stdin and stdout of the ssh.exe rsync spawns, carried in a
 * shared-memory ring instead of a pipe.  See win32shmio.c.
 *
 * Part of the rsync for Windows port (github.com/nuket/rsync-windows).
 * Compiled into Microsoft's Win32-OpenSSH posix_compat library; fileio.c
 * reaches it through win32pumps.c, which asks shmio_for() before it starts
 * a pump.  Include after "w32fd.h".
 *
 * Copyright (c) 2026 Max Vilimpoc.  Same licence as the file that includes
 * it (BSD 2-clause, contrib/win32).
 */
#ifndef WIN32SHMIO_H
#define WIN32SHMIO_H

struct w32_io;
struct shmio;

/*
 * The ring behind this fd, or NULL -- which is the answer for every fd but
 * the two rsync handed us, and for every ssh that was not started by rsync.
 * `for_write` picks the direction: stdout is written, stdin is read.
 */
struct shmio *shmio_for(struct w32_io *pio, int for_write);

/*
 * A dup() of stdin or stdout carries the ring with it.  ssh_session2_open()
 * hands the session channel dup()s of fd 0, 1 and 2 and it is those the
 * client loop reads and writes, so without this the rings would sit unused
 * while the bytes went back to the pipes.  Called from w32_dup2() in
 * w32fd.c, which is the one place a fd is duplicated; harmless for anything
 * that is not one of ours.
 */
void shmio_note_dup(int oldfd, struct w32_io *newio);

/* read()/write() for a ring-backed fd, with the same return convention as
 * the pump paths: bytes moved, 0 at end of file, or -1 with errno set.  A
 * non-blocking fd with nothing to do says EAGAIN. */
int  shmio_read(struct shmio *s, struct w32_io *pio, void *buf, size_t len);
int  shmio_write(struct shmio *s, struct w32_io *pio, const void *buf, size_t len);

/* select()'s view.  Saying "not ready" also registers the interest, so the
 * notifier thread wakes the main thread once that changes. */
BOOL shmio_available(struct shmio *s, int for_write);

/* close(): the far side is told no more bytes are coming.  The ring itself
 * outlives the fd -- it is process-wide -- so this only detaches. */
void shmio_close(struct w32_io *pio);

#endif /* WIN32SHMIO_H */
