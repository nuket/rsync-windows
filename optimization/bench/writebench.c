/*
 * Does the size of a write matter?  The receiver writes what arrives, which
 * is one mux frame at a time, and NtWriteFile was 36% of rsync.exe's running
 * samples during a pull.  If bigger writes are cheaper per byte, coalescing
 * them in the win32 layer is worth doing; if not, it is not.
 *
 *   writebench <path> <MB> <chunkKB> [seq]
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

static double cpu_seconds(void)
{
    FILETIME c, e, k, u;
    ULONGLONG kk, uu;

    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    kk = ((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime;
    uu = ((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime;
    return (double)(kk + uu) / 1e7;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "wb.tmp";
    ULONGLONG mb = argc > 2 ? (ULONGLONG)atoi(argv[2]) : 1024;
    size_t chunk = (size_t)(argc > 3 ? atoi(argv[3]) : 32) << 10;
    int seq = argc > 4 ? atoi(argv[4]) : 0;
    int prealloc = argc > 5 ? atoi(argv[5]) : 0;
    char *buf = (char *)_aligned_malloc(chunk, 64);
    ULONGLONG total = mb << 20, done = 0;
    double t0, cpu0, el, cpu;
    HANDLE h;

    memset(buf, 0x5a, chunk);
    h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL |
                    (seq ? FILE_FLAG_SEQUENTIAL_SCAN : 0), NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("open %s failed: %lu\n", path, GetLastError());
        return 1;
    }

    if (prealloc) {
        /* the whole point: give the file its final size once, instead of
         * extending it on every write */
        LARGE_INTEGER end;
        end.QuadPart = (LONGLONG)total;
        SetFilePointerEx(h, end, NULL, FILE_BEGIN);
        SetEndOfFile(h);
        end.QuadPart = 0;
        SetFilePointerEx(h, end, NULL, FILE_BEGIN);
    }

    t0 = now();
    cpu0 = cpu_seconds();
    while (done < total) {
        DWORD put = 0;
        DWORD want = (DWORD)((total - done) < chunk ? (total - done) : chunk);

        if (!WriteFile(h, buf, want, &put, NULL) || put == 0)
            break;
        done += put;
    }
    el = now() - t0;
    cpu = cpu_seconds() - cpu0;
    CloseHandle(h);
    DeleteFileA(path);

    printf("%7zu KB chunks%s : %6.0f MB/s   %.2f CPU-s   (%.3f CPU-s per GB)\n",
           chunk >> 10, seq ? ", seq hint" : "         ",
           (double)done / el / 1e6, cpu, cpu / ((double)done / 1e9));
    _aligned_free(buf);
    return 0;
}
