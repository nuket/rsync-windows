/*
 * Move bytes from one process to another, the way rsync moves them to ssh,
 * two ways: through an anonymous pipe and through the shared-memory ring.
 * Same chunk size, same processes, same machine -- the only difference is
 * the transport.
 *
 *   shmbench <pipe|shm> <seconds> [chunkKB]
 *
 * Prints "t,mbps" once a second, then a summary line.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32/win32shmpipe.h"

static double now(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

/* ---------------------------------------------------------------- child */
static int child_pipe(size_t chunk)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    char *buf = (char *)_aligned_malloc(chunk, 64);
    DWORD got;
    for (;;) {
        if (!ReadFile(in, buf, (DWORD)chunk, &got, NULL) || got == 0)
            break;
    }
    return 0;
}

static int child_shm(const char *spec, size_t chunk)
{
    struct shmpipe *sp;
    char *buf = (char *)_aligned_malloc(chunk, 64);
    if (shmpipe_open(&sp, spec) < 0) {
        fprintf(stderr, "child: shmpipe_open(%s) failed\n", spec);
        return 1;
    }
    for (;;) {
        int n = shmpipe_read(sp, buf, chunk, 0);
        if (n <= 0)
            break;
    }
    return 0;
}

/* --------------------------------------------------------------- parent */
static int spawn(char *cmdline, HANDLE child_stdin, PROCESS_INFORMATION *pi)
{
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (child_stdin) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = child_stdin;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }
    return CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, pi);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "pipe";
    double secs = argc > 2 ? atof(argv[2]) : 10.0;
    size_t chunk = (argc > 3 ? (size_t)atoi(argv[3]) : 32) << 10;
    char self[MAX_PATH], cmdline[512];

    if (!strcmp(mode, "child-pipe"))
        return child_pipe(chunk);
    if (!strcmp(mode, "child-shm"))
        return child_shm(argv[3], chunk);

    GetModuleFileNameA(NULL, self, sizeof self);

    char *buf = (char *)_aligned_malloc(chunk, 64);
    memset(buf, 0xa5, chunk);
    unsigned long long bytes = 0, at_mark = 0;
    double t0, mark;
    PROCESS_INFORMATION pi;

    if (!strcmp(mode, "pipe")) {
        SECURITY_ATTRIBUTES sa;
        HANDLE rd, wr;
        DWORD put;
        sa.nLength = sizeof sa;
        sa.lpSecurityDescriptor = NULL;
        sa.bInheritHandle = TRUE;
        if (!CreatePipe(&rd, &wr, &sa, 1024 * 1024)) {
            fprintf(stderr, "CreatePipe failed\n");
            return 1;
        }
        SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
        snprintf(cmdline, sizeof cmdline, "\"%s\" child-pipe 0 %llu",
                 self, (unsigned long long)(chunk >> 10));
        if (!spawn(cmdline, rd, &pi)) {
            fprintf(stderr, "spawn failed %lu\n", GetLastError());
            return 1;
        }
        CloseHandle(rd);
        t0 = mark = now();
        for (;;) {
            if (!WriteFile(wr, buf, (DWORD)chunk, &put, NULL) || put == 0)
                break;
            bytes += put;
            double t = now();
            if (t - mark >= 1.0) {
                printf("%.2f,%.0f\n", t - t0, (double)(bytes - at_mark) / (t - mark) / 1e6);
                fflush(stdout);
                mark = t; at_mark = bytes;
            }
            if (t - t0 >= secs)
                break;
        }
        CloseHandle(wr);
    } else {
        struct shmpipe *sp;
        /* ring size in KB, since whether the two of them stay in L3 is the
         * question; 1 MB by default, matching the pipe buffer it replaces */
        size_t ring = (size_t)(argc > 4 ? atoi(argv[4]) : 1024) << 10;
        if (shmpipe_create(&sp, ring) < 0) {
            fprintf(stderr, "shmpipe_create failed\n");
            return 1;
        }
        snprintf(cmdline, sizeof cmdline, "\"%s\" child-shm %llu %s",
                 self, (unsigned long long)(chunk >> 10), shmpipe_spec(sp));
        if (!spawn(cmdline, NULL, &pi)) {
            fprintf(stderr, "spawn failed %lu\n", GetLastError());
            return 1;
        }
        t0 = mark = now();
        for (;;) {
            size_t off = 0;
            while (off < chunk) {
                int n = shmpipe_write(sp, buf + off, chunk - off, 0);
                if (n <= 0)
                    goto done;
                off += (size_t)n;
            }
            bytes += chunk;
            double t = now();
            if (t - mark >= 1.0) {
                printf("%.2f,%.0f\n", t - t0, (double)(bytes - at_mark) / (t - mark) / 1e6);
                fflush(stdout);
                mark = t; at_mark = bytes;
            }
            if (t - t0 >= secs)
                break;
        }
done:
        shmpipe_close_write(sp);
    }

    {
        double el = now() - t0;
        printf("SUMMARY %s %zuKB chunks: %.0f MB/s over %.1f s\n",
               mode, chunk >> 10, (double)bytes / el / 1e6, el);
    }
    WaitForSingleObject(pi.hProcess, 5000);
    TerminateProcess(pi.hProcess, 0);
    return 0;
}
