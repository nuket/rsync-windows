# Building rsync with CMake (including Windows)

`CMakeLists.txt` is an alternative to the autoconf build. On Unix it is a
drop-in replacement for `./configure && make`; on Windows it is the only way
to build, since there is no shell, no `awk` and no autoconf there.

```sh
cmake -B build -G Ninja
cmake --build build
```

On Ubuntu, `./setup-linux.sh` installs everything either build system
needs (tested on 22.04.5 and 26.04).

The only hard requirement beyond a C compiler is **Python 3.6+**, which
replaces `awk` for generating `proto.h`, `daemon-parm.h`, `help-*.h` and
`default-*.h` (see `cmake/gen-headers.py`). Those generators are byte-exact
reimplementations of the `.awk` scripts — verified by diffing their output
against real `awk` on a Linux host.

## Options

| Option | Default | Meaning |
| --- | --- | --- |
| `RSYNC_ENABLE_ACLS` | `ON` | POSIX ACL support (Unix only) |
| `RSYNC_ENABLE_XATTRS` | `ON` | Extended attributes (Unix only) |
| `RSYNC_ENABLE_OPENSSL` | `OFF` | Use OpenSSL for MD4/MD5 |
| `RSYNC_ENABLE_ZSTD` | `OFF` | zstd compression |
| `RSYNC_ENABLE_LZ4` | `OFF` | LZ4 compression |
| `RSYNC_ENABLE_XXHASH` | `ON` | xxh64/xxh3/xxh128 checksums (bundled copy) |
| `RSYNC_ENABLE_ICONV` | `OFF` | `--iconv` character conversion |
| `RSYNC_ENABLE_SIMD` | `ON` on MSVC, `OFF` elsewhere | SIMD `get_checksum1()`: SSE2/SSSE3/AVX2 chosen at runtime. MSVC builds `win32/win32checksum*.c`, a C port with its own CPUID dispatch; other compilers build upstream's `simd-checksum-x86_64.cpp` and need C++ |
| `RSYNC_XXH_DISPATCH` | `ON` on MSVC, `OFF` elsewhere | Compile the bundled `xxhash.c` a second time under `/arch:AVX2` and let `win32/win32xxh.c` pick it at runtime, so XXH3/xxh128 use AVX2 when the CPU has it (bundled xxhash, x86/x64 only) |
| `RSYNC_EXTERNAL_ZLIB` | `OFF` | Link system zlib instead of the bundled copy |
| `RSYNC_EXTERNAL_XXHASH` | `OFF` | Link system xxhash instead of the bundled copy |
| `RSYNC_STATIC_CRT` | `ON` | Link the C runtime statically (MSVC only; see below) |
| `RSYNC_HARDEN` | `ON` | Exploit mitigations: CFG, CET, strict `/GS`, Spectre (MSVC only) |
| `RSYNC_STRICT_MITIGATIONS` | `OFF` | Also ACG, CIG and strict handle checks (see below) |
| `RSYNC_ENABLE_ASAN` | `OFF` | AddressSanitizer, for testing |

`RSYNC_PATH`, `RSYNC_RSH`, `NOBODY_USER` and `NOBODY_GROUP` are cache strings
with the same meaning as the corresponding `configure` options.

---

# The Windows port

Built and tested with **MSVC 2022 + Ninja** on Windows 10, for x64 and x86.
The compatibility layer lives entirely in `win32/`; the shared sources carry
no `#ifdef _WIN32` at all (see Design notes).

```powershell
windows-build-and-test.bat
```

That finds the MSVC toolchain with `vswhere` (so no Developer Command Prompt
is needed), then configures, builds and tests **both** architectures in turn:

| Architecture | Build directory | Binary |
| --- | --- | --- |
| x64 (default) | `build-x64\` | `rsync.exe` |
| x86 | `build-x86\` | `rsync-x86.exe` |

Ninja generates for one compiler, and which compiler that is comes from the
environment vcvars set up, so the two cannot share a configure — the script
calls itself once per architecture instead, each call with its own vcvars and
its own build directory. `--arch x64` or `--arch x86` builds just the one.
Other useful options: `--clean`, `--config Debug`, `--build-dir DIR`,
`--no-tests`, `--tests PATTERN`, and `--host USER@HOST` to include the ssh
transfer tests. It exits 0 on success, 1 on failure, 2 on a usage error.

Or drive CMake directly, from a shell where vcvars has already run — the
architecture is whichever that targets, and the output name follows from it:

```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build-x64\rsync.exe --version
```

Both binaries are self-contained — no Cygwin or MSYS DLLs — and identical but
for the architecture they were compiled for. They use the Windows OpenSSH
client (`C:\Windows\System32\OpenSSH\ssh.exe`) as their remote shell. Take the
x64 build unless you are on 32-bit Windows, or something in the way you are
invoking rsync will only load a 32-bit image.

`rsync-x86.exe` reaches that ssh through `%WINDIR%\Sysnative`. On 64-bit
Windows the WOW64 redirector points a 32-bit process at `SysWOW64` whenever it
names `System32`, and OpenSSH ships only in the real `System32` — so a plain
PATH lookup finds nothing and every remote transfer fails with "Failed to exec
ssh". `Sysnative` is the alias that reaches the true `System32` from a 32-bit
process, and `win32_piped_child()` retries there when, and only when, the
ordinary launch has already come back with "file not found".

### A standalone binary

`RSYNC_STATIC_CRT` (on by default) links the C runtime with `/MT` rather than
`/MD`. A `/MD` build imports from `VCRUNTIME140.dll` and the
`api-ms-win-crt-*.dll` UCRT stubs, so it only runs where the Visual C++
redistributable is installed and the UCRT is current — which is not something
you can assume of a machine you have just copied a binary onto. With `/MT`
the only imports left are part of Windows itself:

```
> dumpbin /dependents build-x64\rsync.exe
    WS2_32.dll
    ADVAPI32.dll
    KERNEL32.dll
```

It costs about 250 KB. Turn it off with `-DRSYNC_STATIC_CRT=OFF` if you are
packaging rsync alongside other `/MD` binaries and would rather they share one
runtime. Note that mixing the two in a single process is the thing to avoid —
each CRT has its own heap and its own fd table — but rsync links no C libraries
outside its own tree, so there is nothing here to mix with.

### The ssh.exe that ships with it

rsync runs its remote end through an ssh client, and on Windows that is
Microsoft's Win32-OpenSSH. Its client reads a pipe on stdin 3 KB at a time
with a thread per read (`TERM_IO_BUF_SIZE`, sized for a console), which holds a
transfer *from* a Windows machine at about 17 MB/s regardless of the link —
the receive direction is unaffected. The release therefore carries its own
`ssh.exe`, built from the same source with that fixed, and `rsync.exe` prefers
an `ssh.exe` in its own directory over the one on `PATH` (a remote shell given
with a path is used as given).

The source is the `openssh/` submodule, Win32-OpenSSH pinned to a commit, plus
the patches in `win32/openssh/patches/`, which is where the fix lives. To build
it:

    git submodule update --init --depth 1 openssh
    .\win32\openssh\build-openssh.ps1 -Arch both

The script applies the patches once, fetches the prebuilt LibreSSL 3.8.2 and
zlib SDKs Microsoft publishes for Win32-OpenSSH (pinned by SHA-256), runs
MSBuild on the OpenSSH projects, and leaves `build-x64\ssh.exe` beside
`rsync.exe` — which means the ssh transfer tests, run with `--host`, exercise
the bundled client — and `build-x86\ssh-x86.exe` (the release packs each
pair as `rsync.exe` + `ssh.exe` in a per-architecture zip). Both carry the same
mitigations as rsync.exe (CFG, CET, `/sdl`, `/Qspectre`; see below) and a
static CRT, with zlib linked in.

LibreSSL is *not* linked in. `ssh.exe` loads the `libcrypto.dll` that the
Windows **OpenSSH Client** component installs in `System32` — Windows' own
build of LibreSSL 3.8.2, the same version the SDK provides headers for. Two
reasons. Windows' build uses AES-NI and runs AES at 5 GB/s on this machine,
where LibreSSL 4.2 built from source with Microsoft's vcpkg port ran it in
software at 140 MB/s (its CPU-feature probe is a GCC constructor that MSVC
compiles away). And rsync.exe's `PreferSystem32Images` hardening
(`win32/win32harden.c`) is inherited by the `ssh.exe` it starts, so under
rsync that process takes the System32 copy whatever sits beside it. So no
DLL ships; the requirement is the OpenSSH Client component at 9.5 or later,
which `setup-windows-rsync.ps1` installs and checks. The 32-bit build is for
32-bit Windows, whose System32 holds a 32-bit `libcrypto.dll`; 64-bit
Windows has none, and that case is not supported.

What is distributed, and under what: OpenSSH and its openbsd-compat code
(the BSD and ISC licences in its `LICENCE`), Microsoft's Win32 compatibility
layer (BSD, in the file headers), and zlib (the zlib licence). All permit
static linking and redistribution with the notices retained, and the script
writes every text into `NOTICE-ssh.txt` beside the binary, which the release
attaches. Nothing here is GPL, and nothing here is linked with rsync — the two
are separate programs in one directory, so rsync's GPL is not engaged.

### Hardening

rsync parses input it does not control — a file list, a checksum stream and a
set of deltas, all produced by the far end — in C. `RSYNC_HARDEN` (on by
default) is worth the few kilobytes it costs.

Baked into the image by the toolchain:

| Mitigation | Flag | What it buys |
| --- | --- | --- |
| Control Flow Guard | `/guard:cf` | indirect calls are checked against a table of legitimate targets, so a clobbered function pointer cannot be aimed anywhere |
| CET shadow stack | `/CETCOMPAT` | the CPU keeps its own copy of return addresses, which defeats ROP at the return |
| Strict stack cookies | `/GS /sdl` | `/sdl` puts a cookie on functions the default `/GS` heuristic skips |
| Spectre v1 | `/Qspectre` | speculation barriers on bounds-check patterns |
| ASLR + DEP | `/DYNAMICBASE /NXCOMPAT`, plus `/HIGHENTROPYVA` on x64 | MSVC defaults, pinned so they cannot quietly lapse |
| Import path | `/DEPENDENTLOADFLAG:0x800` | static imports resolve from System32 alone, so a DLL planted beside the exe cannot win |

The x86 build gets all of that except `/HIGHENTROPYVA`, which is a hard error
on a 32-bit target (`LNK1246`) rather than a warning — there is no high half
of the address space to rebase into. It takes `/LARGEADDRESSAWARE` instead, so
it can use the top 2 GB of its own address space rather than being capped at
half of it; rsync's file list is the one structure large enough to care.

Switched on at runtime by `win32/win32harden.c`, before `WSAStartup()` can pull
in the Winsock catalog:

* `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)` — the `LoadLibrary`
  counterpart to `/DEPENDENTLOADFLAG`. rsync.exe gets dropped into downloads
  folders, shares and USB sticks, and the default search order looks beside the
  executable before System32.
* Extension points disabled — AppInit_DLLs, `SetWindowsHookEx`, IMEs, legacy
  Winsock LSPs. A console file-transfer tool needs none of them.
* Image-load policy — `NoRemoteImages`, `NoLowMandatoryLabelImages`,
  `PreferSystem32Images`. The first matters most here: rsync is routinely
  pointed at a UNC path, and this stops a DLL being loaded from the very share
  it is copying to or from.
* Heap termination on corruption (already the 64-bit default; stated anyway).
* `SetSearchPathMode(SAFE_SEARCHMODE)` — `SearchPath()` otherwise looks in the
  current directory first, which is the same class of problem.

Verify what actually landed:

```powershell
dumpbin /headers build-x64\rsync.exe    # Control Flow Guard, CET compatible,
                                    # Dynamic base, NX compatible, High Entropy
dumpbin /loadconfig build-x64\rsync.exe # Dependent Load Flag 0800, Guard CF count
```

**`RSYNC_STRICT_MITIGATIONS`** (off) adds three more that are genuinely useful
but can break a working setup on someone else's machine, which is a poor trade
for a file copier:

* **ACG** (`ProhibitDynamicCode`) — no page may become executable after the
  fact. rsync generates no code, but an injected profiler or anti-virus DLL may,
  and it would then fail inside this process.
* **CIG** (`MicrosoftSignedOnly`) — only Microsoft-signed images may be loaded
  from that point on. It does not apply to rsync.exe itself, which is already
  mapped, so an unsigned build is unaffected; what it stops is an unsigned DLL
  being injected later. It is also what most often collides with EDR.
* **Strict handle checks** — using a closed handle raises instead of returning
  an error. Good discipline and a good way to find a double-close, but it turns
  a latent bug into a crash.
* **Side-channel isolation** — `SmtBranchTargetIsolation` (STIBP, Spectre v2),
  `SpeculativeStoreBypassDisable` (SSBD, Spectre v4), `IsolateSecurityDomain`,
  `DisablePageCombine`. See the note on Spectre below.
* **CFG strict mode** — every image loaded must itself be built with CFG, so
  one DLL without it cannot become the gadget source.

All twelve tests pass with these on, but that is one machine — treat it as an
opt-in for a controlled environment.

### On Spectre

`/Qspectre` handles **variant 1** (bounds-check bypass) and is on by default:
it is a codegen mitigation, so the compiler can insert the barriers.

**Variant 2** (branch target injection) has no MSVC flag, and asking for one is
a category error — it is mitigated in microcode and by the kernel (IBRS/eIBRS,
IBPB, STIBP), not in the code a compiler emits. Retpoline exists but Microsoft
does not expose it for user-mode builds. What a *process* can ask for is that
its branch predictors not be shared with the sibling SMT thread, which is the
`SmtBranchTargetIsolation` field above; **variant 4** (speculative store
bypass) is `SpeculativeStoreBypassDisable` next to it.

Both are off by default, for two reasons. They are not free — STIBP gives up
most of what SMT buys, and rsync spends its time in checksums — and they
address a threat this program rarely faces: code already running on the same
machine, trying to read a daemon password out of rsync's address space.

Two further compiler options were evaluated and not adopted:
`/Qspectre-load` and `/Qspectre-load-cf` harden *every* load rather than the
ones the heuristic picks, at a cost that is real and a benefit that is
speculative here; `/guard:xfg` (eXtended Flow Guard) is accepted by cl.exe but
XFG never shipped as a supported OS mitigation. `/guard:ehcont` is likewise
skipped — it protects exception continuation records, and this is C with no
structured exception handling.

Two runtime policies are *not* attempted because a process cannot apply them to
itself; both were measured returning `ERROR_INVALID_PARAMETER` and
`ERROR_ACCESS_DENIED` on Windows 10 22H2. `ProcessASLRPolicy` is fixed at
process creation — the `/DYNAMICBASE` and `/HIGHENTROPYVA` image flags are what
cover rsync.exe. `ProcessUserShadowStackPolicy` likewise: `/CETCOMPAT` opts in,
and the loader turns the shadow stack on from that where the CPU has one.

`ProcessChildProcessPolicy` (`NoChildProcessCreation`) is an obvious candidate
and is simply incompatible with rsync, which spawns `ssh.exe` and re-execs
itself as `--server` for local copies.

The largest remaining step is not a flag: **Authenticode signing**. It is what
SmartScreen reputation is built on, what lets an administrator apply CIG or
WDAC policy to rsync elsewhere, and a precondition for `/INTEGRITYCHECK`
(which makes the loader refuse to run the binary unless the signature
verifies).

### AddressSanitizer

`-DRSYNC_ENABLE_ASAN=ON` builds with ASan. It is a testing configuration, not a
shipping one — several times slower, and it needs the ASan runtime DLL beside
the toolchain, so `/DEPENDENTLOADFLAG` is dropped for that build (otherwise the
loader refuses to look outside System32 and the binary dies at startup with
`STATUS_DLL_NOT_FOUND`).

```powershell
cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRSYNC_ENABLE_ASAN=ON
cmake --build build-asan
python win32\tests\run.py --rsync-bin build-asan\rsync.exe
```

The full suite passes clean under it, ssh transfers included.

## What works

* Transfers in **both directions** over ssh — `rsync -rt local/ user@host:/path/`
  and `rsync -rt user@host:/path/ C:\local\` — including recursion, the delta
  algorithm, `--delete`, `--exclude`, compression and `--stats`.
* Drive-letter paths (`C:\dir`, `C:/dir`) and backslash separators.
* File names outside the legacy ANSI code page. The executable embeds a
  manifest selecting the UTF-8 active code page, so names like
  `naïve-café-日本.txt` round-trip byte-for-byte to a UTF-8 Unix peer.
* Names containing spaces, quotes and shell metacharacters.
* **Local-to-local copies** (`rsync -rt C:\a\ D:\b\`), including `--delete`,
  `--exclude` and `--remove-source-files`.

## Links

Neither symlinks nor hard links are a first-class concept for ordinary
Windows users — creating a symlink needs Developer Mode or the
SeCreateSymbolicLinkPrivilege, which most accounts do not hold — so this
build leaves `SUPPORT_LINKS` and `SUPPORT_HARD_LINKS` off and treats each as
the ordinary file it resolves to:

* a **symlink** (or directory junction) is followed, and its referent copied;
* files sharing an inode through a **hard link** are copied as that many
  independent files.

Flattening a tree silently would be a poor outcome, so any links met during
a run are listed as it ends:

```
rsync: 1 symlink was followed and copied as an ordinary file:
    junc
rsync: 2 hard-linked files were copied as independent files:
    a.txt
    b.txt
This build does not reproduce links: Windows needs a privilege most
accounts lack to create a symlink, so neither is treated as first-class.
```

A transfer that meets no links prints nothing. Detection lives in the stat
layer (`win32/win32compat.c`), which already sees every file rsync looks at,
and the report in `win32/win32links.c` — so this costs the shared sources
nothing.

## What does not work

* **Daemon mode** (`--daemon`, `rsync://`), which needs `fork()`, `chroot()`
  and Unix users.
* ACLs, xattrs, devices and Unix ownership are compiled out; none of them map
  onto Windows. `--archive` therefore behaves like `-rt` plus permissions,
  and only the read-only bit of a file's mode is representable.
Batch mode (`--write-batch`, `--only-write-batch`, `--read-batch`) does work,
and the batch is portable in both directions:
`win32/tests/test_batch.py` replays one on a Linux peer and compares the
result against the source.

## Design notes

The shared sources contain **no `#ifdef _WIN32`**. (Two such conditionals
exist in `options.c` and `rsync.h`, but both predate this port and belong to
upstream.) The platform variation is expressed three other ways instead.

**1. Stand-in headers.** `win32/include/` holds `pwd.h`, `grp.h`, `dirent.h`,
`syslog.h`, `unistd.h`, `netinet/in.h`, `netinet/tcp.h`, `arpa/inet.h` and
`sys/ioctl.h`. Each just pulls in `win32compat.h`, which already defines what
rsync uses from them. That directory goes on the include path only on
Windows, so `rsync.h`, `socket.c` and popt include those headers
unconditionally, exactly as upstream wrote them.

**2. Platform hooks.** `rsync.h` defines a handful of macros right after it
includes `config.h`, each defaulting to the POSIX behaviour:

| Hook | Default | Windows |
| --- | --- | --- |
| `RSYNC_TLS` | *(empty)* | `__declspec(thread)` |
| `IS_ABS_PATH(p)` | leading `/` | also `C:/` and `C:\` |
| `IS_DRIVE_PATH(s)` | always false | `C:\dir` is a drive, not `HOST:PATH` |
| `platform_init()` | no-op | Winsock, binary stdio, UTF-8 |
| `platform_fix_path_args()` | no-op | translate `\` in local operands |
| `close_sibling_fd(fd)` | `close(fd)` | no-op (threads share one fd table) |

A port overrides whichever it needs from a header named by
`RSYNC_PLATFORM_INCLUDE` in `cmake/config.h.in`; a target that needs none of
them changes nothing.

**3. Conditional linking.** `pipe.c` and `win32/win32pipe.c` are alternative
implementations of the same interface — `piped_child()`, `local_child()`,
`spawn_receiver_half()`, `receiver_half_finish()` and
`inc_recurse_when_receiving` — and exactly one is linked. Everything that
differs about process handling lives in whole, readable functions in one file
or the other, rather than interleaved.

| File | Responsibility |
| --- | --- |
| `win32/win32compat.h` | POSIX types, `S_IS*`, open flags, the platform hooks, and the macros that route POSIX calls to `win32_*` wrappers. Pulled in by `config.h`, so it is visible tree-wide. |
| `win32/win32undef.h` | Undoes those macros. Every `win32/*.c` includes it so the shims can reach the real CRT/Winsock functions. |
| `win32/win32io.c` | fd routing. CRT fds serve files and pipes; sockets get pseudo-fds above `WIN32_SOCK_BASE` with a side table, because Winsock `SOCKET`s are not CRT fds. |
| `win32/win32proc.c` | `CreateProcess` in place of `fork()`+`execvp()` for the ssh child, plus `waitpid()`/`kill()`. |
| `win32/win32dir.c` | `opendir`/`readdir` over `FindFirstFile`. |
| `win32/win32compat.c` | stat/link/attribute/time calls, users and groups, `getpass`, signal filtering. |
| `win32/win32fork.c` | `fork()` stand-in for the generator/receiver split (below). |
| `win32/win32pipe.c` | Replaces `pipe.c`: process plumbing and the split. |
| `win32/win32links.c` | Records the links met and lists them as the run ends. |
| `win32/win32args.c` | The `platform_fix_path_args()` hook. |
| `win32/tests/` | The Windows test suite. |
| `win32/include/` | Stand-ins for headers MSVC lacks. |
| `win32/rsync.manifest` | Selects the UTF-8 active code page. |

### Local copies

`local_child()` normally forks a child that keeps the parent's parsed options
in memory and jumps straight into `child_main()`. This port starts a second
copy of the executable instead and hands it the options the way a remote
server would receive them: `server_options()` builds exactly the argument
vector `do_cmd()` would have built for an ssh invocation, so the child is an
ordinary `--server` run that happens to be on this machine.

That makes the local server a separate process, which matters for more than
startup. `local_server` in the shared code means two things at once — "the
peer is on this machine" and "the peer inherited our memory, so don't bother
transmitting what it already has". Only the first still holds.
`LOCAL_SERVER_SHARES_STATE` (`rsync.h`) separates them, and the three places
that skip transmission now test that instead: the filter list in
`exclude.c`, the shape of a `MSG_SUCCESS` in `io.c`, and the protected-args
terminator in `options.c`. Both ends evaluate it identically, which is what
keeps the protocol in step.

This is not cosmetic. Without it, `--delete --exclude=PATTERN` would delete
the excluded files, because the server would never receive the filter list
that protects them.

### The generator/receiver split

Receiving forks into a generator and a receiver that run concurrently over
one socket (`do_recv()` in `main.c`). With no `fork()`, the receiver runs as
a thread — but a thread shares every global, where fork would have given the
child private copies.

The state the two halves modify independently is therefore tagged
`RSYNC_TLS` (`rsync.h`), which is `__declspec(thread)` on Windows and
**nothing at all** on every other platform, so Unix builds are untouched.
A new thread would normally start from the TLS *template* (the initial
values); fork gives the child the parent's *current* values, so
`win32_fork_thread()` copies the parent's whole TLS block into the child
before any rsync code runs. Pointers in that block still refer to shared
heap, so the buffers holding in-flight protocol data are deep-copied by
`io_fork_child_fixup()`.

Deciding what belongs in that set is the subtle part, and the symptom of
getting it wrong is a hang rather than a wrong answer. `batch_fd` is the
clearest example: `io.c` sets it to -1 when the input stream reaches EOF,
which under `--read-batch` happens in the receiving half only. Sharing one
copy meant the receiver finishing the batch also stopped the *generator*
selecting for input —

```c
if (iobuf.in_fd >= 0 && iobuf.in.size - iobuf.in.len) {
    if (!read_batch || batch_fd >= 0) {     /* <-- generator blinded here */
        FD_SET(iobuf.in_fd, &r_fds);
```

— so the generator sat in a `select()` with an empty read set, waiting for a
`MSG_DONE` it had made itself unable to hear, with every file already
correctly written. The line above it already tests `am_generator`, which is
`RSYNC_TLS`: this variable was always fork-private, and the port simply had
not marked it.

Two consequences worth knowing:

* **Incremental recursion is forced off when receiving** (`compat.c`). With
  it on, each half independently parses and appends file-list chunks *after*
  the split; in two address spaces that is fine, in one heap the two halves
  collide. Building the whole list up front avoids it, at the cost of a
  slower start on very large trees.
* **fds are process-wide**, so the hand-off does not close the sibling's pipe
  ends the way the forked version does, and the receiver half returns rather
  than lingering in `read_final_goodbye()` waiting for a `SIGUSR2` that a
  thread cannot receive. It still performs that function's protocol-31
  `MSG_DONE` exchange first, which the generator waits for.

Residual risk: this is emulation, not fork. Both halves share one heap, so
the pre-split file list is shared rather than copied. That is read-mostly and
transfers verify byte-for-byte in testing (see below), but it is a weaker
guarantee than the Unix version's shared-nothing processes. Treat Windows
pulls as well-tested rather than proven.

Two details worth knowing:

* **Waiting on pipes.** Windows `select()` only accepts sockets, but a client
  talking to ssh has nothing *but* pipes. `win32_select()` splits the fd sets,
  hands socket fds to Winsock and polls pipe fds with `PeekNamedPipe`, backing
  off from a spin to 5 ms so an active transfer stays responsive without
  burning a core when idle. rsync 3.5.0 moved `io.c` from `select()` to
  `poll()`, to escape the `FD_SETSIZE` ceiling; `WSAPoll()` is no use here
  because it too sees only sockets, so `win32_poll()` (in `win32/include/poll.h`
  and `win32io.c`) translates the `pollfd` array into that same split wait.
* **Large files.** MSVC's `off_t` is 32-bit, so the build takes the
  `stat64`/`off64_t` path that rsync already supports for 32-bit Unix
  (`USE_STAT64_FUNCS`). Offsets are 64-bit.

Set `RSYNC_WIN32_DEBUG=1` to trace the shim's fd operations to stderr.

## Testing

`testsuite/` is written against Unix semantics throughout -- ACLs, xattrs,
uid/gid, devices, a POSIX shell for its remote-shell stand-in -- so most of it
cannot run here, and bending it into shape would mean editing shared test code
for one platform's benefit. `win32/tests/` covers what this port supports
instead, and **skips** what Windows genuinely cannot do rather than failing:

```powershell
cmake --build build --target win32-tests
```

or directly, to pick individual tests:

```powershell
python win32\tests\run.py --rsync-bin build-x64\rsync.exe [TEST ...]
```

Set `RSYNC_WIN_TEST_HOST=user@host` (key-based login, rsync installed) to
include the ssh tests; they skip without it. All 23 currently pass.

Platform behaviour -- the part `testsuite/` could not reach even in principle:

| Test | Covers |
| --- | --- |
| `basics` | `--version`/`--help`, 64-bit capabilities, exact sizes, `-t` mtimes |
| `paths` | drive letters, backslashes, trailing slashes, `h:relative` |
| `filenames` | Unicode, spaces and shell metacharacters through the narrow API |
| `local_copy` | local copy, incremental no-op, `--delete` |
| `delete_exclude` | `--delete` must not delete what `--exclude` protects |
| `links_reported` | links followed and copied as plain files, and listed at the end |
| `remote` | push, pull and delta over ssh |

Ordinary rsync behaviour, ported from the equivalent `testsuite/` tests:

| Test | Covers | Upstream equivalent |
| --- | --- | --- |
| `helpers` | `secure_relative_open()` path validation, `trimslash` | `secure-relpath-validation`, `trimslash` |
| `filters` | `--filter`, dir-merge, `-F`/`-F -F`, `--exclude-from`, `-C`, `--max-size`/`--min-size`, `-m` | `exclude`, `merge`, `filter-depth`, `cvs-exclude`, `size-filter`, `prune-empty-dirs` |
| `files_from` | `--files-from` incl. implied `--relative`, `-0`, stdin, `--delete` | `files-from`, `files-from-depth` |
| `relative` | `-R`, `./` cut points, `--no-implied-dirs`, `-d`, `--mkpath`, dedupe, missing args | `relative`, `relative-implied`, `dirs`, `mkpath`, `duplicates`, `missing` |
| `transfer_control` | `--temp-dir`, `--partial-dir`, `--delay-updates`, `--update`, `--sparse` | `temp-dir`, `partial`, `delay-updates`, `update`, `sparse` |
| `altdest` | `--compare-dest`, `--copy-dest` | `alt-dest`, `alt-dest-deep` |
| `backup` | `--backup-dir` at depth, `--suffix`, backup-on-delete | `backup`, `backup-deep` |
| `itemize` | `-i` change strings, `--out-format`, `--stats`, `--info`, `-q` | `itemize`, `output-options` |
| `fuzzy` | `--fuzzy` basis selection, and a repetitive delta basis | `fuzzy`, `fuzzy-basis`, `hashsearch-chain` |
| `compress` | `--compress-choice`, `--compress-level`, `--skip-compress` | `compress-options` |
| `batch` | `--write-batch`, `--only-write-batch`, replay on a Linux peer | `batch-mode` |
| `delta` | block matching, and byte-exactness after a delta | `hands` |
| `inplace_append` | `--inplace` (including truncation) and `--append` | `inplace`, `append` |
| `options` | `--remove-source-files`, `--dry-run`, filters, `-z`, `--backup` | assorted |
| `wildmatch` | rsync's own wildmatch unit test, all 12 option sets | `wildmatch` |
| `unsafe_symlink` | rsync's own `unsafe_symlink()` unit test, plus a drive-letter case | `unsafe-links` |

`wildmatch`, `unsafe_symlink` and `helpers` drive rsync's own C unit tests:
`wildmatch()`, `unsafe_symlink()`, `secure_relative_open()` and `trimslash`
are pure string and path arithmetic, so the upstream helpers run unmodified.
CMake builds all of `Makefile.in`'s `CHECK_PROGS`, so `runtests.py` on a Unix
host can use a CMake build tree too.

`t_chmod_secure` is built but not driven: every one of its cases either
builds a parent symlink that escapes the tree or checks that a legitimate one
still resolves, and a build without symlink support cannot set any of that up.

Two upstream assertions do not carry over, and the tests here say so rather
than quietly weakening:

* The upstream `fuzzy` test proves the basis was chosen by looking for
  `--debug=FUZZY` output. For a local copy this port re-execs itself as
  `--server` instead of forking, and `server_options()` forwards `--info` but
  not `--debug` -- so a debug level a forked generator would have inherited in
  memory never reaches ours. `test_fuzzy.py` asserts on `--stats`
  matched-versus-literal bytes instead, which is the stronger claim: it shows
  the basis was not merely found but actually used.
* The upstream `partial` test interrupts a transfer with a signal to leave a
  partial file behind. `TerminateProcess` gives rsync no chance to run the
  cleanup handler that moves the partial into place, so
  `test_transfer_control.py` exercises the resume path from the other end: it
  plants a partial and checks the generator adopts and consumes it.

### Manual verification

Beyond the suite, against an Ubuntu 22.04 host running rsync 3.2.7
(protocol 31), over ssh:

* Push and pull of a 443-file / 23 MB tree, compared byte-for-byte with an
  MD5 manifest on both sides; the Windows→Linux→Windows round trip is
  identical to the original.
* Delta transfer in both directions (a modified block in a 300 KB file moves
  ~700 literal bytes against ~299 KB matched).
* Incremental runs with remote modifications, additions, renames and
  deletions, verified against the remote's own manifest each round.
* Five repeated pulls with and without `-z`, all byte-exact.
* Unicode, spaces and shell metacharacters in file names.
* A symlink read back with its stored target (not the resolved path) and
  pushed to Linux verbatim.
* Hard links in both directions: a linked set arrives linked and unrelated
  files do not, and `-H` over the 457-file tree produces no false links —
  the check that the 16-bit `st_ino` would have failed.
* Local-to-local copy of a 457-file tree, byte-for-byte identical to the
  source; `--delete --exclude` correctly spares the excluded files;
  `--remove-source-files` empties the source.
* Symlinks and hard links met during a transfer are followed/copied as plain
  files and listed at the end of the run.

The `RSYNC_TLS` changes are inert off Windows, and the CMake-built binary
still passes rsync's own test suite there: on both Ubuntu 22.04.5 and
26.04, 108 passed, 5 skipped, 0 failures.

### File metadata

`STRUCT_STAT` is `struct win32_stat` (`win32/win32compat.h`) rather than the
CRT's `struct _stat64`, which keeps `st_ino` in **16 bits** — nowhere near
enough for an NTFS file index, so rsync would see collisions and hard-link
unrelated files. `rsync.h` only picks a `STRUCT_STAT` when a platform header
has not already supplied one.

The three stat calls are built on `GetFileInformationByHandle`, which returns
the full 64-bit file index as `st_ino` and the real link count as
`st_nlink` — between them, that is what `-H` compares. `rsync --version` now
reports "64-bit inums".

## Known rough edges

* Only the read-only bit of a mode is representable, so `st_mode` is
  synthesised (0666/0777, minus write when read-only). Permission-preserving
  options have little to work with.
* `struct timeval` has a 32-bit `tv_sec` here, so times set through
  `utimes()` are subject to the 2038 limit even though the stat side is
  64-bit.
* `--iconv` is unavailable, so `--secluded-args` and charset conversion
  behave as on a build without iconv.

---

Copyright (C) 2026 Max Vilimpoc, rsync CMake/Windows port.
Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
