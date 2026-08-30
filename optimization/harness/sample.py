"""Poor-man's sampling profiler: suspend each thread of a child process every
~1 ms, read RIP, and attribute it to a function via the linker .map file.
Needs no elevation (same user), unlike ETW/xperf."""
import ctypes, ctypes.wintypes as W, subprocess, sys, time, bisect, collections, os

exe, mapfile, args = sys.argv[1], sys.argv[2], sys.argv[3:]

# --callers SUBSTRING: for every sample whose leaf function matches, scan the
# thread's stack for return addresses back into the main module and report who
# was underneath.  A scan, not a real unwind: it walks the top of the stack and
# keeps the addresses that land in the module's code, nearest first, so a stale
# slot can show up as a frame.  Enough to answer "which of our functions is
# sitting in this system call", which an unwind through PDB-less system DLLs
# would not manage anyway.
want_callers = None
if args and args[0] == "--callers":
    want_callers, args = args[1], args[2:]

# ---- symbols: a linker .map ("0001:offset  name  absolute f obj"), or a
# .pdb resolved through dbghelp's SymFromAddr.
funcs = []
base_pref = None
use_pdb = mapfile.lower().endswith(".pdb")
if not use_pdb:
    for line in open(mapfile, errors="replace"):
        p = line.split()
        if len(p) >= 3 and p[0] == "Preferred" and p[1] == "load":
            base_pref = int(p[-1], 16)
        if len(p) >= 4 and p[0].startswith("0001:") and p[3] == "f":
            try:
                funcs.append((int(p[2], 16), p[1]))
            except ValueError:
                pass
    funcs.sort()
faddr = [a for a, _ in funcs]

class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [("SizeOfStruct", ctypes.c_uint32), ("TypeIndex", ctypes.c_uint32),
                ("Reserved", ctypes.c_uint64 * 2), ("Index", ctypes.c_uint32),
                ("Size", ctypes.c_uint32), ("ModBase", ctypes.c_uint64),
                ("Flags", ctypes.c_uint32), ("Value", ctypes.c_uint64),
                ("Address", ctypes.c_uint64), ("Register", ctypes.c_uint32),
                ("Scope", ctypes.c_uint32), ("Tag", ctypes.c_uint32),
                ("NameLen", ctypes.c_uint32), ("MaxNameLen", ctypes.c_uint32),
                ("Name", ctypes.c_char * 512)]

dbghelp = None
sym_cache = {}
def pdb_symbol(hproc, addr):
    """Function name for an address in the main module, via the PDB."""
    if addr in sym_cache:
        return sym_cache[addr]
    si = SYMBOL_INFO()
    si.SizeOfStruct = 88          # sizeof(SYMBOL_INFO) without the name tail
    si.MaxNameLen = 511
    disp = ctypes.c_uint64()
    ok = dbghelp.SymFromAddr(hproc, ctypes.c_uint64(addr), ctypes.byref(disp), ctypes.byref(si))
    name = si.Name.decode(errors="replace") if ok else "?"
    sym_cache[addr] = name
    return name

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

THREAD_ALL = 0x1F03FF
TH32CS_SNAPTHREAD = 4
CONTEXT_CONTROL = 0x100001
CTX_SIZE = 1232
OFF_FLAGS = 0x30
OFF_RSP = 0x98
OFF_RIP = 0xF8

class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", W.DWORD), ("cntUsage", W.DWORD), ("th32ThreadID", W.DWORD),
                ("th32OwnerProcessID", W.DWORD), ("tpBasePri", ctypes.c_long),
                ("tpDeltaPri", ctypes.c_long), ("dwFlags", W.DWORD)]

class MODULEINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", ctypes.c_void_p), ("SizeOfImage", W.DWORD), ("EntryPoint", ctypes.c_void_p)]

k32.OpenThread.restype = W.HANDLE
k32.OpenThread.argtypes = [W.DWORD, W.BOOL, W.DWORD]
k32.SuspendThread.argtypes = [W.HANDLE]
k32.ResumeThread.argtypes = [W.HANDLE]
k32.GetThreadContext.argtypes = [W.HANDLE, ctypes.c_void_p]
k32.CreateToolhelp32Snapshot.restype = W.HANDLE
k32.CloseHandle.argtypes = [W.HANDLE]
k32.OpenProcess.restype = W.HANDLE
k32.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
k32.ReadProcessMemory.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
psapi.EnumProcessModules.argtypes = [W.HANDLE, ctypes.c_void_p, W.DWORD, ctypes.POINTER(W.DWORD)]
psapi.GetModuleInformation.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_void_p, W.DWORD]
psapi.GetModuleBaseNameW.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_wchar_p, W.DWORD]

def threads_of(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32(); te.dwSize = ctypes.sizeof(te)
    out = []
    if k32.Thread32First(snap, ctypes.byref(te)):
        while True:
            if te.th32OwnerProcessID == pid:
                out.append(te.th32ThreadID)
            if not k32.Thread32Next(snap, ctypes.byref(te)):
                break
    k32.CloseHandle(snap)
    return out

def modules_of(hproc):
    arr = (W.HMODULE * 512)()
    needed = W.DWORD()
    psapi.EnumProcessModules(hproc, arr, ctypes.sizeof(arr), ctypes.byref(needed))
    mods = []
    for i in range(needed.value // ctypes.sizeof(W.HMODULE)):
        mi = MODULEINFO()
        psapi.GetModuleInformation(hproc, arr[i], ctypes.byref(mi), ctypes.sizeof(mi))
        name = ctypes.create_unicode_buffer(260)
        psapi.GetModuleBaseNameW(hproc, arr[i], name, 260)
        mods.append((mi.lpBaseOfDll, mi.SizeOfImage, name.value))
    return mods

class Attached:
    """Stand-in for Popen when attaching to a process that already runs."""
    def __init__(self, pid):
        self.pid = pid
        self.returncode = None
        self.h = k32.OpenProcess(0x100000 | 0x0410, False, pid)  # SYNCHRONIZE too
    def poll(self):
        if k32.WaitForSingleObject(self.h, 0) == 0:
            self.returncode = 0
        return self.returncode

if args and args[0] == "--attach":
    # Wait for a process with the exe's basename to appear (e.g. one sshd
    # spawns), then sample it until it exits.
    import ctypes.wintypes
    class PROCESSENTRY32(ctypes.Structure):
        _fields_ = [("dwSize", W.DWORD), ("cntUsage", W.DWORD), ("th32ProcessID", W.DWORD),
                    ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", W.DWORD),
                    ("cntThreads", W.DWORD), ("th32ParentProcessID", W.DWORD),
                    ("pcPriClassBase", ctypes.c_long), ("dwFlags", W.DWORD),
                    ("szExeFile", ctypes.c_char * 260)]
    want = os.path.basename(exe).lower().encode()
    pid = None
    deadline = time.time() + float(args[1]) if len(args) > 1 else time.time() + 60
    while pid is None and time.time() < deadline:
        snap = k32.CreateToolhelp32Snapshot(2, 0)
        pe = PROCESSENTRY32(); pe.dwSize = ctypes.sizeof(pe)
        if k32.Process32First(snap, ctypes.byref(pe)):
            while True:
                if pe.szExeFile.lower() == want and pe.th32ProcessID != os.getpid():
                    pid = pe.th32ProcessID
                    break
                if not k32.Process32Next(snap, ctypes.byref(pe)):
                    break
        k32.CloseHandle(snap)
        if pid is None:
            time.sleep(0.05)
    if pid is None:
        sys.exit("no process named %s appeared" % want.decode())
    proc = Attached(pid)
    print("attached to pid", pid)
    time.sleep(1.0)  # let it get past startup so the module list is complete
else:
    stdin = None
    stdout = subprocess.DEVNULL
    while args and args[0] in ("--stdin", "--stdout"):
        if args[0] == "--stdin":               # feed a file to the child's stdin
            stdin = open(args[1], "rb")
        else:                                   # a real file, not NUL (a char device)
            stdout = open(args[1], "wb")
        args = args[2:]
    proc = subprocess.Popen([exe] + args, stdin=stdin, stdout=stdout, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
hproc = k32.OpenProcess(0x0410, False, proc.pid)  # QUERY_INFORMATION | VM_READ
mods = modules_of(hproc)
main = [m for m in mods if m[2].lower() == os.path.basename(exe).lower()][0]
mbase, msize = main[0], main[1]

if use_pdb:
    dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)
    dbghelp.SymInitializeW.argtypes = [W.HANDLE, ctypes.c_wchar_p, W.BOOL]
    dbghelp.SymLoadModuleExW.restype = ctypes.c_uint64
    dbghelp.SymLoadModuleExW.argtypes = [W.HANDLE, W.HANDLE, ctypes.c_wchar_p, ctypes.c_wchar_p,
                                         ctypes.c_uint64, W.DWORD, ctypes.c_void_p, W.DWORD]
    dbghelp.SymFromAddr.argtypes = [W.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.c_void_p]
    dbghelp.SymSetOptions(0x00000002 | 0x00000004)   # SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS
    if not dbghelp.SymInitializeW(hproc, os.path.dirname(os.path.abspath(mapfile)), False):
        sys.exit("SymInitialize failed: %d" % ctypes.get_last_error())
    if not dbghelp.SymLoadModuleExW(hproc, None, os.path.abspath(exe), None, mbase, msize, None, 0):
        sys.exit("SymLoadModuleEx failed: %d" % ctypes.get_last_error())
    # System DLLs too: no PDBs, but their export tables name the syscall
    # stubs (NtWriteFile, NtDeviceIoControlFile, ...), which is what matters.
    for b, s, nm in mods:
        if b != mbase:
            full = ctypes.create_unicode_buffer(1024)
            psapi.GetModuleFileNameExW.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_wchar_p, W.DWORD]
            psapi.GetModuleFileNameExW(hproc, b, full, 1024)
            dbghelp.SymLoadModuleExW(hproc, None, full.value, None, b, s, None, 0)

raw = ctypes.create_string_buffer(CTX_SIZE + 16)
addr = ctypes.addressof(raw)
ctx = (addr + 15) & ~15
ctypes.memset(ctx, 0, CTX_SIZE)
counts = collections.Counter()
per_thread = collections.Counter()
caller_counts = collections.Counter()
caller_span = {}
STACK_QWORDS = 96
stack_buf = (ctypes.c_uint64 * STACK_QWORDS)()
got = ctypes.c_size_t()

def stack_frames(rsp):
    """Main-module return addresses on top of the stack, nearest first."""
    if not k32.ReadProcessMemory(hproc, ctypes.c_void_p(rsp), stack_buf,
                                 ctypes.sizeof(stack_buf), ctypes.byref(got)):
        return []
    out = []
    for i in range(got.value // 8):
        v = stack_buf[i]
        if mbase <= v < mbase + msize:
            nm = pdb_symbol(hproc, v) if use_pdb else "?"
            if not out or out[-1] != nm:
                out.append(nm)
            if len(out) >= 4:
                break
    return out

n = 0
handles = {}
t0 = time.time()
while proc.poll() is None:
    for tid in threads_of(proc.pid):
        h = handles.get(tid)
        if h is None:
            h = handles[tid] = k32.OpenThread(THREAD_ALL, False, tid)
            if not h:
                continue
        if k32.SuspendThread(h) == 0xFFFFFFFF:
            continue
        ctypes.c_uint32.from_address(ctx + OFF_FLAGS).value = CONTEXT_CONTROL
        ok = k32.GetThreadContext(h, ctx)
        rip = ctypes.c_uint64.from_address(ctx + OFF_RIP).value
        rsp = ctypes.c_uint64.from_address(ctx + OFF_RSP).value
        k32.ResumeThread(h)
        if not ok:
            continue
        if mbase <= rip < mbase + msize:
            if use_pdb:
                name = pdb_symbol(hproc, rip)
            else:
                rva = rip - mbase + base_pref
                i = bisect.bisect_right(faddr, rva) - 1
                name = funcs[i][1] if i >= 0 else "?"
        else:
            name = "<other>"
            for b, s, nm in mods:
                if b <= rip < b + s:
                    name = "<" + nm + ">"
                    if use_pdb:
                        name = nm + "!" + pdb_symbol(hproc, rip)
                    break
        counts[(tid, name)] += 1
        per_thread[tid] += 1
        if want_callers and want_callers in name:
            key = (tid, " <- ".join(stack_frames(rsp)) or "(nothing in the module)")
            caller_counts[key] += 1
            el_now = time.time() - t0
            span = caller_span.get(key)
            caller_span[key] = (min(span[0], el_now), max(span[1], el_now)) if span else (el_now, el_now)
        n += 1
    time.sleep(0.001)
el = time.time() - t0
print(f"exit {proc.returncode}, {n} samples over {el:.1f}s")
# Idle threads sit in ntdll wait functions the whole time; show every thread but
# the busy one is what matters.
for tid, tot in per_thread.most_common():
    print(f"thread {tid}: {tot} samples")
    for (t, name), c in counts.most_common():
        if t == tid and c * 100 >= tot:
            print(f"   {c*100/tot:5.1f}%  {name}")
if want_callers:
    tot = sum(caller_counts.values())
    print(f"\nstacks under {want_callers!r} ({tot} samples), nearest frame first:")
    for (tid, chain), c in caller_counts.most_common(12):
        lo, hi = caller_span[(tid, chain)]
        print(f"   {c:4d}  thread {tid}  seen {lo:.2f}s..{hi:.2f}s of {el:.2f}s  {chain}")
