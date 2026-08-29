/*
 * win32sendbuf.h -- write() for the packet layer's output buffer that hands
 * the buffer's storage to the socket pump instead of copying it.  See
 * win32sendbuf.c; packet.c reaches it through patch 0008.
 *
 * Part of the rsync for Windows port (github.com/nuket/rsync-windows).
 * Copyright (c) 2026 Max Vilimpoc.  ISC, as packet.c.
 */
#ifndef WIN32SENDBUF_H
#define WIN32SENDBUF_H

struct sshbuf;

/* As write(fd, sshbuf_ptr(buf), sshbuf_len(buf)): returns the bytes taken,
 * which the caller consumes from buf, or -1 with errno (EAGAIN when the
 * pump is full).  Falls back to a plain write() for anything that is not
 * a pumped socket, or too small to be worth handing over. */
int w32_write_sshbuf(int fd, struct sshbuf *buf);

#endif /* WIN32SENDBUF_H */
