/*
 * Where a process's threads are, sampled from outside.  No admin rights, so
 * no ETW: suspend each thread, read its instruction pointer, resume, and
 * count the answers by symbol.  Leaf frames only -- enough to say which code
 * is running, not who called it, which is the question here.
 *
 *   sampler <exe-name|pid> <seconds> [hz]
 *
 * Prints a flat profile: percentage of samples, module, symbol.
 */
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "dbghelp.lib")

#define MAX_SYMS 4096

struct bucket {
    char name[220];
    unsigned long long hits;
};

static struct bucket buckets[MAX_SYMS];
static int nbuckets;
static unsigned long long total, unknown;

static void tally(const char *name)
{
    int i;

    for (i = 0; i < nbuckets; i++) {
        if (strcmp(buckets[i].name, name) == 0) {
            buckets[i].hits++;
            return;
        }
    }
    if (nbuckets < MAX_SYMS) {
        strncpy(buckets[nbuckets].name, name, sizeof buckets[0].name - 1);
        buckets[nbuckets].hits = 1;
        nbuckets++;
    }
}

static int by_hits(const void *a, const void *b)
{
    const struct bucket *x = a, *y = b;
    return x->hits < y->hits ? 1 : x->hits > y->hits ? -1 : 0;
}

static DWORD find_pid(const char *what)
{
    DWORD pid = (DWORD)atoi(what);
    HANDLE snap;
    PROCESSENTRY32 pe = { sizeof pe };
    DWORD best = 0;
    FILETIME newest = { 0, 0 }, c, e, k, u;

    if (pid)
        return pid;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, what) != 0)
                continue;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!h)
                continue;
            if (GetProcessTimes(h, &c, &e, &k, &u)) {
                if (CompareFileTime(&c, &newest) > 0) {
                    newest = c;
                    best = pe.th32ProcessID;   /* the most recently started */
                }
            }
            CloseHandle(h);
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return best;
}

/*
 * The threads worth sampling.  A process like this one has a dozen threads
 * and most of them are parked in the thread pool; sampling them all buries
 * the answer under ZwWaitForWorkViaWorkerFactory.  So each refresh asks how
 * much processor time every thread has used since the last one and keeps
 * only those actually running -- which is also the honest denominator, since
 * a thread that is asleep is not what limits anything.
 */
struct thr {
    DWORD tid;
    ULONGLONG cpu100ns;      /* kernel + user, at the last refresh */
    ULONGLONG busy;          /* total sampled while it was running */
};
static struct thr known[256];
static int nknown;

static ULONGLONG thread_cpu(DWORD tid)
{
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    FILETIME c, e, k, u;
    ULONGLONG v = 0;

    if (!h)
        return 0;
    if (GetThreadTimes(h, &c, &e, &k, &u))
        v = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) +
            (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
    CloseHandle(h);
    return v;
}

static struct thr *known_find(DWORD tid)
{
    int i;

    for (i = 0; i < nknown; i++)
        if (known[i].tid == tid)
            return &known[i];
    if (nknown == 256)
        return NULL;
    known[nknown].tid = tid;
    known[nknown].cpu100ns = thread_cpu(tid);
    known[nknown].busy = 0;
    return &known[nknown++];
}

/* threads that used more than `pct` of a core since the last refresh */
static int collect_threads(DWORD pid, DWORD *tids, int max, ULONGLONG since_ms,
                           double pct)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = { sizeof te };
    int n = 0;

    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    if (Thread32First(snap, &te)) {
        do {
            struct thr *t;
            ULONGLONG now, used;

            if (te.th32OwnerProcessID != pid || n >= max)
                continue;
            if (!(t = known_find(te.th32ThreadID)))
                continue;
            now = thread_cpu(te.th32ThreadID);
            used = now > t->cpu100ns ? now - t->cpu100ns : 0;
            t->cpu100ns = now;
            /* 100ns units against wall-clock ms */
            if (since_ms && (double)used / 10000.0 >= pct * since_ms)
                tids[n++] = te.th32ThreadID;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return n;
}

int main(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "rsync.exe";
    double secs = argc > 2 ? atof(argv[2]) : 10.0;
    int hz = argc > 3 ? atoi(argv[3]) : 500;
    DWORD pid, tids[256];
    HANDLE proc;
    int ntids = 0, i;
    ULONGLONG t0, tend, next_refresh = 0;
    char line[220];

    while ((pid = find_pid(what)) == 0) {
        Sleep(20);   /* wait for it to appear */
    }
    proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) {
        fprintf(stderr, "OpenProcess(%lu) failed: %lu\n", pid, GetLastError());
        return 1;
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    if (!SymInitialize(proc, NULL, TRUE))
        fprintf(stderr, "SymInitialize: %lu (symbols may be missing)\n", GetLastError());

    fprintf(stderr, "sampling pid %lu (%s) for %.0f s at %d Hz\n", pid, what, secs, hz);
    t0 = GetTickCount64();
    tend = t0 + (ULONGLONG)(secs * 1000);
    while (GetTickCount64() < tend) {
        if (GetTickCount64() >= next_refresh) {
            static ULONGLONG last_refresh;
            ULONGLONG now = GetTickCount64();
            ntids = collect_threads(pid, tids, 256,
                                    last_refresh ? now - last_refresh : 0, 0.02);
            last_refresh = now;
            next_refresh = now + 200;
            if (nknown == 0)
                break;   /* gone */
        }
        for (i = 0; i < ntids; i++) {
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                                   FALSE, tids[i]);
            CONTEXT ctx;
            DWORD64 pc;
            char buf[sizeof(SYMBOL_INFO) + 256];
            PSYMBOL_INFO sym = (PSYMBOL_INFO)buf;
            IMAGEHLP_MODULE64 mod;
            DWORD64 disp = 0;

            if (!th)
                continue;
            if (SuspendThread(th) == (DWORD)-1) {
                CloseHandle(th);
                continue;
            }
            memset(&ctx, 0, sizeof ctx);
            ctx.ContextFlags = CONTEXT_FULL;
            if (!GetThreadContext(th, &ctx)) {
                ResumeThread(th);
                CloseHandle(th);
                continue;
            }
            pc = ctx.Rip;

            /*
             * Walk a little way up as well: a leaf like ZwClose says what is
             * running but not why, and the first frame back in rsync's own
             * code is the answer to that.  The thread stays suspended for the
             * walk, which is why the walk is short.
             */
            {
                STACKFRAME64 sf;
                char leaf[160] = "", caller[160] = "";
                int depth;

                memset(&sf, 0, sizeof sf);
                sf.AddrPC.Offset = ctx.Rip;      sf.AddrPC.Mode = AddrModeFlat;
                sf.AddrFrame.Offset = ctx.Rbp;   sf.AddrFrame.Mode = AddrModeFlat;
                sf.AddrStack.Offset = ctx.Rsp;   sf.AddrStack.Mode = AddrModeFlat;

                for (depth = 0; depth < 24; depth++) {
                    char one[160];

                    if (depth && !StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th,
                                              &sf, &ctx, NULL,
                                              SymFunctionTableAccess64,
                                              SymGetModuleBase64, NULL))
                        break;
                    {
                        DWORD64 at = depth ? sf.AddrPC.Offset : pc;

                        if (!at)
                            break;
                        memset(buf, 0, sizeof buf);
                        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                        sym->MaxNameLen = 255;
                        memset(&mod, 0, sizeof mod);
                        mod.SizeOfStruct = sizeof mod;
                        SymGetModuleInfo64(proc, at, &mod);
                        if (SymFromAddr(proc, at, &disp, sym))
                            snprintf(one, sizeof one, "%s", sym->Name);
                        else
                            snprintf(one, sizeof one, "(no symbol)");
                        if (!depth)
                            snprintf(leaf, sizeof leaf, "%-13s %s",
                                     mod.ModuleName[0] ? mod.ModuleName : "?", one);
                        else if (!caller[0] && mod.ModuleName[0]
                                 && _stricmp(mod.ModuleName, "rsync") == 0) {
                            snprintf(caller, sizeof caller, "%s", one);
                            break;
                        }
                    }
                }
                ResumeThread(th);
                CloseHandle(th);

                total++;
                if (strstr(leaf, "(no symbol)"))
                    unknown++;
                snprintf(line, sizeof line, "%-40s  <-  %s",
                         leaf[0] ? leaf : "?", caller[0] ? caller : "?");
                tally(line);
            }
        }
        Sleep(1000 / hz > 0 ? 1000 / hz : 1);
    }

    qsort(buckets, nbuckets, sizeof buckets[0], by_hits);
    printf("\n%llu samples over %d threads (%.1f%% without symbols)\n\n",
           total, ntids, total ? 100.0 * unknown / total : 0.0);
    printf("  %%      samples  where\n");
    for (i = 0; i < nbuckets && i < 40; i++) {
        if (buckets[i].hits * 400 < total)     /* below 0.25%: not interesting */
            break;
        printf("%6.2f  %8llu  %s\n",
               100.0 * buckets[i].hits / total, buckets[i].hits, buckets[i].name);
    }
    SymCleanup(proc);
    CloseHandle(proc);
    return 0;
}
