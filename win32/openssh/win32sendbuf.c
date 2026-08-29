/*
 * win32sendbuf.c -- write() for the packet layer's output buffer that hands
 * the buffer's storage to the socket pump instead of copying it.
 *
 * Part of the rsync for Windows port (github.com/nuket/rsync-windows).
 * Compiled into Win32-OpenSSH's libssh beside packet.c, which calls it from
 * ssh_packet_write_poll() through patch 0008.
 *
 * Copyright (c) 2026 Max Vilimpoc.  ISC, as packet.c.
 *
 * With the socket pumped (win32pumps.c), every packet the client sends was
 * encrypted into the packet layer's output sshbuf and then copied from
 * there into the pump's ring, from which the thread send()s it: one memcpy
 * of every byte, 7% of the sending thread on a 20Gbit link.  Instead, the
 * sshbuf's storage itself goes to the pump, which sends it as it stands
 * and keeps the block for reuse, and the sshbuf takes a block the pump has
 * finished with in exchange (sshbuf_swap_storage(), patch 0005).  The
 * sshbuf's bookkeeping does not change: the caller consumes the bytes as
 * after any write().  Small writes -- window adjusts, keepalives, the
 * handshake -- are copied as before; swapping a 40KB allocation to send
 * 100 bytes would only churn the pool.
 */

#include "includes.h"

#include <sys/types.h>
#include <errno.h>
#include <unistd.h>

#include "sshbuf.h"
#include "ssherr.h"
#include "w32fd.h"
#include "win32pumps.h"
#include "win32sendbuf.h"

/* SSH_SENDBUF_COPY in the environment keeps the copying path, for
 * comparison and as a fallback */
static int
handoff_wanted(void)
{
	static int wanted = -1;

	if (wanted < 0)
		wanted = getenv("SSH_SENDBUF_COPY") == NULL;
	return wanted;
}

int
w32_write_sshbuf(int fd, struct sshbuf *buf)
{
	struct w32_io *pio = w32_io_from_fd(fd);
	size_t len = sshbuf_len(buf), alloc = 0, off = 0;
	u_char *d;

	if (!handoff_wanted() || pio == NULL || pio->type != SOCK_FD ||
	    !sockio_pump_wanted(pio) || !sockio_pump_handoff_ok(pio, len))
		return write(fd, sshbuf_mutable_ptr(buf), len);

	/* a block the pump is done with, if it has one that fits; the
	 * sshbuf allocates its own otherwise */
	d = (u_char *)sockio_pump_spare(pio, len, &alloc);
	if (sshbuf_swap_storage(buf, &d, &alloc, &off) != 0) {
		if (d != NULL)
			sockio_pump_spare_back(pio, (char *)d, alloc);
		return write(fd, sshbuf_mutable_ptr(buf), len);
	}
	sockio_pump_send_owned(pio, (char *)d, off, len, alloc);
	return (int)len;
}
