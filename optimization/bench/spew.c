/*
 * Feed ssh's stdin from memory for N seconds, so one connection stays busy
 * for the whole run: no disk, no rsync, no gaps between transfers -- just
 * the cipher, held at whatever rate the machine can manage, for long enough
 * that the machine gets hot.
 *
 *   spew <seconds> [chunkKB]
 *
 * Writes "t,MBps" to stderr once a second: the achieved rate, which is what
 * throttling shows up in.
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

int main(int argc, char **argv)
{
    double secs = argc > 1 ? atof(argv[1]) : 60.0;
    size_t chunk = (size_t)(argc > 2 ? atoi(argv[2]) : 256) << 10;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    char *buf = (char *)_aligned_malloc(chunk, 64);
    unsigned long long total = 0, at_mark = 0;
    double t0, mark;
    unsigned i;

    /* not all one byte: a compressible stream would be a different test */
    for (i = 0; i < chunk; i++)
        buf[i] = (char)(i * 31 + (i >> 8));

    t0 = mark = now();
    for (;;) {
        DWORD put = 0;
        double t;

        if (!WriteFile(out, buf, (DWORD)chunk, &put, NULL) || put == 0)
            break;
        total += put;
        t = now();
        if (t - mark >= 1.0) {
            fprintf(stderr, "%.1f,%.0f\n", t - t0,
                    (double)(total - at_mark) / (t - mark) / 1e6);
            fflush(stderr);
            mark = t;
            at_mark = total;
        }
        if (t - t0 >= secs)
            break;
    }
    fprintf(stderr, "SUMMARY %.0f MB/s over %.1f s\n",
            (double)total / (now() - t0) / 1e6, now() - t0);
    fflush(stderr);
    return 0;
}
