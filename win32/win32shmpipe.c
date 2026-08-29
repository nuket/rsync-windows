/*
 * win32shmpipe.c -- see win32shmpipe.h.
 *
 * Copyright (C) 2026 Max Vilimpoc, rsync CMake/Windows port.
 * Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
 *
 * One producer, one consumer, one ring.  head is owned by the consumer and
 * tail by the producer; each side only ever advances its own index, so no
 * lock is needed -- just a release before publishing an index and an
 * acquire after reading the other side's.  The events exist only so a side
 * that has to wait can block instead of spinning, and are signalled only
 * when the other side is actually waiting.
 */

/* sscanf() below; the ssh build does not define this for us as rsync's does */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "win32shmpipe.h"   /* beside this file; the ssh build has no repo root */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHM_SPIN 2000        /* brief spin before sleeping, as the pumps do */

/* Shared header, at the start of the section; the ring follows it. */
struct shm_hdr {
    volatile LONG head;      /* consumer: next byte out */
    volatile LONG tail;      /* producer: next byte in */
    volatile LONG closed;    /* producer has finished */
    volatile LONG rd_wait;   /* consumer is blocked on data_evt */
    volatile LONG wr_wait;   /* producer is blocked on room_evt */
    volatile LONG ready;     /* the child has opened it and means to use it */
    volatile LONG go;        /* the parent saw that, and is using it too */
    LONG capacity;
    char pad[64 - (8 * sizeof(LONG)) % 64];
};

struct shmpipe {
    HANDLE section;
    HANDLE data_evt;         /* the ring stopped being empty */
    HANDLE room_evt;         /* the ring stopped being full */
    HANDLE peer;             /* the process at the other end, or NULL */
    struct shm_hdr *hdr;
    char *ring;
    size_t capacity;
    DWORD peer_checked;      /* tick of the last look at it */
    int peer_dead;
    int owner;               /* created it, so it closes the handles */
    char spec[128];
};

#define PEER_CHECK_MS 50     /* how often an idle side looks for a corpse */

/* Has the far end died without closing?  Cheap to ask often: the answer is
 * only refreshed a few times a second, and only ever asked when this side
 * has nothing to do anyway. */
static int peer_gone(struct shmpipe *sp)
{
    DWORD now;

    if (!sp->peer)
        return 0;
    if (sp->peer_dead)
        return 1;
    now = GetTickCount();
    if (sp->peer_checked && (DWORD)(now - sp->peer_checked) < PEER_CHECK_MS)
        return 0;
    sp->peer_checked = now;
    if (WaitForSingleObject(sp->peer, 0) == WAIT_OBJECT_0)
        sp->peer_dead = 1;
    return sp->peer_dead;
}

void shmpipe_set_peer(struct shmpipe *sp, HANDLE proc)
{
    sp->peer = proc;
    sp->peer_dead = 0;
    sp->peer_checked = 0;
}

static size_t used_of(struct shm_hdr *h, size_t cap)
{
    LONG t = h->tail, hd = h->head;
    MemoryBarrier();
    return (size_t)((t - hd + (LONG)cap) % (LONG)cap);
}

static int map_view(struct shmpipe *sp, size_t bytes)
{
    sp->hdr = (struct shm_hdr *)MapViewOfFile(sp->section, FILE_MAP_ALL_ACCESS,
                                              0, 0, sizeof(struct shm_hdr) + bytes);
    if (!sp->hdr)
        return -1;
    sp->ring = (char *)(sp->hdr + 1);
    sp->capacity = bytes;
    return 0;
}

int shmpipe_create(struct shmpipe **out, size_t bytes)
{
    SECURITY_ATTRIBUTES sa;
    struct shmpipe *sp = (struct shmpipe *)calloc(1, sizeof *sp);
    size_t total = sizeof(struct shm_hdr) + bytes;

    if (!sp)
        return -1;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    sp->section = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                     (DWORD)(((ULONGLONG)total) >> 32),
                                     (DWORD)(total & 0xffffffffu), NULL);
    sp->data_evt = CreateEventA(&sa, FALSE, FALSE, NULL);
    sp->room_evt = CreateEventA(&sa, FALSE, FALSE, NULL);
    if (!sp->section || !sp->data_evt || !sp->room_evt || map_view(sp, bytes) < 0) {
        shmpipe_free(sp);
        return -1;
    }
    memset(sp->hdr, 0, sizeof *sp->hdr);
    sp->hdr->capacity = (LONG)bytes;
    sp->owner = 1;
    snprintf(sp->spec, sizeof sp->spec, "%llu:%llu:%llu:%llu",
             (unsigned long long)(uintptr_t)sp->section,
             (unsigned long long)bytes,
             (unsigned long long)(uintptr_t)sp->data_evt,
             (unsigned long long)(uintptr_t)sp->room_evt);
    *out = sp;
    return 0;
}

const char *shmpipe_spec(struct shmpipe *sp) { return sp->spec; }

int shmpipe_open(struct shmpipe **out, const char *spec)
{
    unsigned long long sect = 0, bytes = 0, de = 0, re = 0;
    struct shmpipe *sp;

    if (!spec || sscanf(spec, "%llu:%llu:%llu:%llu", &sect, &bytes, &de, &re) != 4)
        return -1;
    if (!sect || !bytes || !de || !re)
        return -1;
    if (!(sp = (struct shmpipe *)calloc(1, sizeof *sp)))
        return -1;
    sp->section = (HANDLE)(uintptr_t)sect;
    sp->data_evt = (HANDLE)(uintptr_t)de;
    sp->room_evt = (HANDLE)(uintptr_t)re;
    if (map_view(sp, (size_t)bytes) < 0) {
        free(sp);
        return -1;
    }
    if (sp->hdr->capacity != (LONG)bytes) {   /* not ours, or not mapped */
        UnmapViewOfFile(sp->hdr);
        free(sp);
        return -1;
    }
    *out = sp;
    return 0;
}

void shmpipe_mark_ready(struct shmpipe *sp)
{
    InterlockedExchange(&sp->hdr->ready, 1);
    SetEvent(sp->data_evt);          /* in case the parent is already waiting */
}

int shmpipe_wait_ready(struct shmpipe *sp, DWORD ms)
{
    DWORD waited = 0;
    while (!sp->hdr->ready && waited < ms) {
        Sleep(2);
        waited += 2;
    }
    return sp->hdr->ready ? 0 : -1;
}

void shmpipe_mark_go(struct shmpipe *sp)
{
    InterlockedExchange(&sp->hdr->go, 1);
}

int shmpipe_wait_go(struct shmpipe *sp, DWORD ms)
{
    DWORD waited = 0;
    while (!sp->hdr->go && waited < ms) {
        Sleep(2);
        waited += 2;
    }
    return sp->hdr->go ? 0 : -1;
}

HANDLE shmpipe_data_event(struct shmpipe *sp) { return sp->data_evt; }
HANDLE shmpipe_room_event(struct shmpipe *sp) { return sp->room_evt; }
size_t shmpipe_avail(struct shmpipe *sp) { return used_of(sp->hdr, sp->capacity); }

size_t shmpipe_room(struct shmpipe *sp)
{
    return sp->capacity - 1 - used_of(sp->hdr, sp->capacity);
}

void shmpipe_arm(struct shmpipe *sp, int for_write, int on)
{
    InterlockedExchange(for_write ? &sp->hdr->wr_wait : &sp->hdr->rd_wait,
                        on ? 1 : 0);
}

int shmpipe_at_eof(struct shmpipe *sp)
{
    if (used_of(sp->hdr, sp->capacity) != 0)
        return 0;
    return sp->hdr->closed || peer_gone(sp);
}

int shmpipe_read(struct shmpipe *sp, void *buf, size_t len, int nonblock)
{
    struct shm_hdr *h = sp->hdr;
    size_t cap = sp->capacity, avail, n, first;
    LONG head;
    int spins = 0;

    for (;;) {
        avail = used_of(h, cap);
        if (avail)
            break;
        if (h->closed) {
            MemoryBarrier();
            if (used_of(h, cap) == 0)
                return 0;                     /* end of file */
            continue;
        }
        if (nonblock) {
            if (peer_gone(sp))
                return 0;                     /* died mid-sentence */
            errno = EAGAIN;
            return -1;
        }
        if (++spins < SHM_SPIN) {
            YieldProcessor();
            continue;
        }
        InterlockedExchange(&h->rd_wait, 1);
        MemoryBarrier();
        if (used_of(h, cap) == 0 && !h->closed)
            WaitForSingleObject(sp->data_evt, PEER_CHECK_MS);
        InterlockedExchange(&h->rd_wait, 0);
        if (used_of(h, cap) == 0 && !h->closed && peer_gone(sp))
            return 0;
        spins = 0;
    }

    head = h->head;
    n = avail < len ? avail : len;
    first = cap - (size_t)head;
    if (first > n)
        first = n;
    memcpy(buf, sp->ring + head, first);
    if (n > first)
        memcpy((char *)buf + first, sp->ring, n - first);

    MemoryBarrier();                          /* the copy lands before head moves */
    InterlockedExchange(&h->head, (LONG)(((size_t)head + n) % cap));
    if (h->wr_wait)
        SetEvent(sp->room_evt);
    return (int)n;
}

int shmpipe_write(struct shmpipe *sp, const void *buf, size_t len, int nonblock)
{
    struct shm_hdr *h = sp->hdr;
    size_t cap = sp->capacity, room, n, first;
    LONG tail;
    int spins = 0;

    for (;;) {
        /* one byte is left unused so full and empty stay distinguishable */
        room = cap - 1 - used_of(h, cap);
        if (room)
            break;
        if (nonblock) {
            if (peer_gone(sp)) {
                errno = EPIPE;
                return -1;
            }
            errno = EAGAIN;
            return -1;
        }
        if (++spins < SHM_SPIN) {
            YieldProcessor();
            continue;
        }
        InterlockedExchange(&h->wr_wait, 1);
        MemoryBarrier();
        if (cap - 1 - used_of(h, cap) == 0)
            WaitForSingleObject(sp->room_evt, PEER_CHECK_MS);
        InterlockedExchange(&h->wr_wait, 0);
        if (cap - 1 - used_of(h, cap) == 0 && peer_gone(sp)) {
            errno = EPIPE;
            return -1;
        }
        spins = 0;
    }

    tail = h->tail;
    n = room < len ? room : len;
    first = cap - (size_t)tail;
    if (first > n)
        first = n;
    memcpy(sp->ring + tail, buf, first);
    if (n > first)
        memcpy(sp->ring, (const char *)buf + first, n - first);

    MemoryBarrier();
    InterlockedExchange(&h->tail, (LONG)(((size_t)tail + n) % cap));
    if (h->rd_wait)
        SetEvent(sp->data_evt);
    return (int)n;
}

void shmpipe_close_write(struct shmpipe *sp)
{
    InterlockedExchange(&sp->hdr->closed, 1);
    SetEvent(sp->data_evt);
}

void shmpipe_free(struct shmpipe *sp)
{
    if (!sp)
        return;
    if (sp->hdr)
        UnmapViewOfFile(sp->hdr);
    if (sp->section)
        CloseHandle(sp->section);
    if (sp->data_evt)
        CloseHandle(sp->data_evt);
    if (sp->room_evt)
        CloseHandle(sp->room_evt);
    free(sp);
}
