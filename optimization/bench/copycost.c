/*
 * What one copy of a byte actually costs on this machine, at the sizes the
 * transfer uses: a 32 KB chunk copied inside a 1 MB ring stays in L2, a
 * 4 MB one does not.  The point is to price "one memcpy per byte" against
 * the other things a push does per byte.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double now(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

static void run(const char *what, size_t working, size_t chunk)
{
    char *src = (char *)_aligned_malloc(working, 64);
    char *dst = (char *)_aligned_malloc(working, 64);
    double t0, el;
    unsigned long long moved = 0;
    size_t off = 0;

    memset(src, 0x5a, working);
    memset(dst, 0, working);
    /* warm both */
    memcpy(dst, src, working);

    t0 = now();
    do {
        for (int i = 0; i < 64; i++) {
            memcpy(dst + off, src + off, chunk);
            off += chunk;
            if (off + chunk > working)
                off = 0;
            moved += chunk;
        }
        el = now() - t0;
    } while (el < 1.0);

    printf("%-34s %6.1f GB/s   (%.1f%% of a core per GB/s moved)\n",
           what, (double)moved / el / 1e9, 100.0 / ((double)moved / el / 1e9));
    _aligned_free(src);
    _aligned_free(dst);
}

int main(void)
{
    run("32 KB chunks inside 1 MB", 1u << 20, 32u << 10);
    run("64 KB chunks inside 1 MB", 1u << 20, 64u << 10);
    run("32 KB chunks inside 4 MB", 4u << 20, 32u << 10);
    run("256 KB chunks inside 64 MB", 64u << 20, 256u << 10);
    return 0;
}
