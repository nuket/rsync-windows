/*
 * win32shmpipe.h -- a byte pipe between rsync.exe and the ssh.exe it spawns
 * that does not go through the kernel.
 *
 * A transfer's bulk data crosses that boundary through an anonymous pipe,
 * and every byte is copied four times to get there: rsync's own write ring,
 * the kernel's copy into the pipe, the kernel's copy back out, and ssh's
 * read ring.  Measured on a 20 Gbit link, feeding ssh from a pipe instead of
 * a file costs 18% (1275 -> 1045 MB/s) with no rsync involved at all.
 *
 * This is a single-producer / single-consumer ring in a shared section
 * instead: the producer copies in, the consumer copies out, and nothing
 * else touches the bytes.  Two copies rather than four, and no system call
 * per chunk -- only an event when a side has to wait.
 *
 * The parent creates one (inheritable section and events), passes the
 * handle numbers to the child in the environment, and the child opens it.
 * Both ends fall back to the pipe if any of that fails, so an ssh.exe that
 * does not know about this still works.
 *
 * Copyright (C) 2026 Max Vilimpoc, rsync CMake/Windows port.
 * Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
 */
#ifndef WIN32SHMPIPE_H
#define WIN32SHMPIPE_H

#include <windows.h>

struct shmpipe;

/* Parent side: make a ring of `bytes` usable capacity.  The section and the
 * two events are inheritable so the child gets them by handle value. */
int shmpipe_create(struct shmpipe **out, size_t bytes);

/* The text to hand the child, "section:bytes:data_evt:room_evt" in decimal.
 * Valid until shmpipe_free(); the caller does not own it. */
const char *shmpipe_spec(struct shmpipe *sp);

/* Child side: attach to what the parent described. */
int shmpipe_open(struct shmpipe **out, const char *spec);

/* The handshake.  The child calls shmpipe_mark_ready() once it has both
 * rings open and will really use them; the parent calls
 * shmpipe_wait_ready() before its first byte and keeps the pipes instead if
 * the answer does not come.  Without this an ssh.exe built before any of
 * this existed -- the one an old release drops into C:\Tools\rsync, say --
 * would sit reading a pipe nobody writes to. */
void shmpipe_mark_ready(struct shmpipe *sp);
int shmpipe_wait_ready(struct shmpipe *sp, DWORD ms);

/* The other half of it, so the two ends can never disagree: the parent says
 * "go" only after it has seen "ready", and the child uses the ring only after
 * it has seen "go".  A parent whose wait ran out never says go, so a child
 * that was merely slow falls back to the pipes as well. */
void shmpipe_mark_go(struct shmpipe *sp);
int shmpipe_wait_go(struct shmpipe *sp, DWORD ms);

/*
 * A pipe reports end of file when the last handle to its far end closes,
 * even if the process holding it died without a word.  A section does not,
 * so tell this end which process is on the other: if that process is gone
 * and the ring is empty, reads report end of file and writes EPIPE, exactly
 * as the pipe would have.  Pass NULL to forget it; the handle stays the
 * caller's to close.
 */
void shmpipe_set_peer(struct shmpipe *sp, HANDLE proc);

/* Both sides.  Return the count moved, 0 at end of file (read only), or -1
 * with errno set.  A non-blocking call that would wait returns -1/EAGAIN. */
int shmpipe_read(struct shmpipe *sp, void *buf, size_t len, int nonblock);
int shmpipe_write(struct shmpipe *sp, const void *buf, size_t len, int nonblock);

/* Tell the far side no more bytes are coming; its reads then drain what is
 * left and report end of file. */
void shmpipe_close_write(struct shmpipe *sp);

/* Set while the ring holds bytes (or the writer has finished), so a
 * select()-like wait has something to block on. */
HANDLE shmpipe_data_event(struct shmpipe *sp);
/* Set while the ring has room. */
HANDLE shmpipe_room_event(struct shmpipe *sp);

/*
 * A side that is about to block on one of those events has to say so first:
 * the other side only pays for a SetEvent when someone is actually waiting,
 * which is what keeps a busy ring system-call free.  Arm, re-check, wait,
 * disarm -- the re-check is what stops a publish that landed in between from
 * being missed.
 */
void shmpipe_arm(struct shmpipe *sp, int for_write, int on);

/* Bytes available to read right now, without waiting. */
size_t shmpipe_avail(struct shmpipe *sp);
/* Bytes that can be written right now, without waiting. */
size_t shmpipe_room(struct shmpipe *sp);
/* True once the writer has closed and the ring is empty. */
int shmpipe_at_eof(struct shmpipe *sp);

void shmpipe_free(struct shmpipe *sp);

#endif /* WIN32SHMPIPE_H */
