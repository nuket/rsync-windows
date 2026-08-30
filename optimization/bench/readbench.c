/*
 * What does rsync's source-file read actually cost, and what would moving it
 * off the main thread buy?  Reads a file the way map_ptr() does -- sequential
 * 256KB read() into a buffer -- and simulates the per-chunk work rsync does
 * between reads, so the question is not raw read throughput but how much of
 * the read can be taken off the critical path.
 *
 *   plain   : read() straight into the buffer, as now
 *   thread  : a reader thread fills a ring; the main thread memcpy()s out
 *   overlap : ReadFile on a FILE_FLAG_OVERLAPPED handle, double-buffered,
 *             the main thread memcpy()s out of the completed buffer
 *
 * Reports wall time and the main thread's own processor time; the second is
 * the number that matters, since rsync's main thread is the bottleneck.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <process.h>

#define CHUNK (256 * 1024)
#define NBUF  4

static volatile unsigned long long sink;

/* Stand in for the checksumming and buffer-filling rsync does per chunk. */
static void work(const char *p, int n, int passes)
{
	unsigned long long acc = 0;
	int k;
	for (k = 0; k < passes; k++) {
		const unsigned long long *q = (const unsigned long long *)p;
		int i, m = n / 8;
		for (i = 0; i < m; i++)
			acc ^= q[i];
	}
	sink += acc;
}

static double thread_cpu_seconds(void)
{
	FILETIME c, e, k, u;
	ULONGLONG kt, ut;
	GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
	kt = ((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime;
	ut = ((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime;
	return (kt + ut) / 1e7;
}

/* ---------------- thread mode: a reader thread and a ring ---------------- */

struct ring {
	HANDLE h;
	char *buf[NBUF];
	volatile LONG filled[NBUF];   /* bytes in each slot, -1 = EOF */
	HANDLE slot_free[NBUF], slot_full[NBUF];
	volatile LONG stop;
};

static unsigned __stdcall reader(void *arg)
{
	struct ring *r = (struct ring *)arg;
	int i = 0;
	for (;;) {
		DWORD got = 0;
		WaitForSingleObject(r->slot_free[i], INFINITE);
		if (r->stop)
			return 0;
		if (!ReadFile(r->h, r->buf[i], CHUNK, &got, NULL))
			got = 0;
		r->filled[i] = got ? (LONG)got : -1;
		SetEvent(r->slot_full[i]);
		if (!got)
			return 0;
		i = (i + 1) % NBUF;
	}
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
	const char *path = argv[1];
	const char *mode = argv[2];
	int passes = argc > 3 ? atoi(argv[3]) : 6;
	char *dst = (char *)_aligned_malloc(CHUNK, 64);
	LARGE_INTEGER f, t0, t1;
	double cpu0, cpu1;
	ULONGLONG total = 0;

	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t0);
	cpu0 = thread_cpu_seconds();

	if (strcmp(mode, "plain") == 0) {
		HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
				       OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
		for (;;) {
			DWORD got = 0;
			if (!ReadFile(h, dst, CHUNK, &got, NULL) || !got)
				break;
			total += got;
			work(dst, got, passes);
		}
		CloseHandle(h);
	} else if (strcmp(mode, "thread") == 0) {
		struct ring r;
		HANDLE th;
		int i = 0;
		memset(&r, 0, sizeof r);
		r.h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
				  OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
		for (i = 0; i < NBUF; i++) {
			r.buf[i] = (char *)_aligned_malloc(CHUNK, 64);
			r.slot_free[i] = CreateEventA(NULL, FALSE, TRUE, NULL);
			r.slot_full[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
		}
		th = (HANDLE)_beginthreadex(NULL, 0, reader, &r, 0, NULL);
		for (i = 0; ; i = (i + 1) % NBUF) {
			LONG n;
			WaitForSingleObject(r.slot_full[i], INFINITE);
			n = r.filled[i];
			if (n <= 0)
				break;
			memcpy(dst, r.buf[i], n);      /* the copy rsync's read() owes */
			SetEvent(r.slot_free[i]);
			total += n;
			work(dst, n, passes);
		}
		r.stop = 1;
		for (i = 0; i < NBUF; i++)
			SetEvent(r.slot_free[i]);
		WaitForSingleObject(th, 2000);
		CloseHandle(th);
		CloseHandle(r.h);
	} else if (strcmp(mode, "overlap") == 0) {
		HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
				       OPEN_EXISTING,
				       FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
		char *buf[2];
		OVERLAPPED ov[2];
		int inflight[2] = {0, 0};
		ULONGLONG off = 0;
		int i;
		for (i = 0; i < 2; i++) {
			buf[i] = (char *)_aligned_malloc(CHUNK, 64);
			memset(&ov[i], 0, sizeof ov[i]);
			ov[i].hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
		}
		for (i = 0; i < 2; i++) {   /* prime both */
			ov[i].Offset = (DWORD)off;
			ov[i].OffsetHigh = (DWORD)(off >> 32);
			ResetEvent(ov[i].hEvent);
			if (ReadFile(h, buf[i], CHUNK, NULL, &ov[i]) || GetLastError() == ERROR_IO_PENDING)
				inflight[i] = 1;
			off += CHUNK;
		}
		for (i = 0; ; i ^= 1) {
			DWORD got = 0;
			if (!inflight[i])
				break;
			if (!GetOverlappedResult(h, &ov[i], &got, TRUE) || !got)
				break;
			memcpy(dst, buf[i], got);      /* same copy */
			total += got;
			ov[i].Offset = (DWORD)off;
			ov[i].OffsetHigh = (DWORD)(off >> 32);
			ResetEvent(ov[i].hEvent);
			inflight[i] = (ReadFile(h, buf[i], CHUNK, NULL, &ov[i])
				       || GetLastError() == ERROR_IO_PENDING);
			off += CHUNK;
			work(dst, got, passes);
		}
		CloseHandle(h);
	} else {
		fprintf(stderr, "modes: plain thread overlap\n");
		return 2;
	}

	cpu1 = thread_cpu_seconds();
	QueryPerformanceCounter(&t1);
	{
		double wall = (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
		double mb = total / 1048576.0;
		printf("%-8s %6.0f MB  wall %5.2f s = %6.0f MB/s   main-thread cpu %5.2f s (%3.0f%%)\n",
		       mode, mb, wall, mb / wall, cpu1 - cpu0, 100 * (cpu1 - cpu0) / wall);
	}
	return 0;
}
