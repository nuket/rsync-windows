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
| `RSYNC_ENABLE_XXHASH` | `OFF` | xxhash checksums |
| `RSYNC_ENABLE_ICONV` | `OFF` | `--iconv` character conversion |
| `RSYNC_ENABLE_SIMD` | `OFF` | SIMD/asm checksum acceleration (x86-64) |
| `RSYNC_EXTERNAL_ZLIB` | `OFF` | Link system zlib instead of the bundled copy |

`RSYNC_PATH`, `RSYNC_RSH`, `NOBODY_USER` and `NOBODY_GROUP` are cache strings
with the same meaning as the corresponding `configure` options.

---

# The Windows port

Built and tested with **MSVC 2022 (x64) + Ninja** on Windows 10. The
compatibility layer lives entirely in `win32/`; the shared sources carry no
`#ifdef _WIN32` at all (see Design notes).

```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\rsync.exe --version
```

`rsync.exe` is self-contained — no Cygwin or MSYS DLLs. It uses the Windows
OpenSSH client (`C:\Windows\System32\OpenSSH\ssh.exe`) as its remote shell.

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
* **Symlinks** (`-l`). Reading always works, including of directory
  junctions. *Creating* one needs Developer Mode or the
  SeCreateSymbolicLinkPrivilege — without either, Windows refuses, and rsync
  reports the `EPERM` per link and finishes the rest of the transfer.

* **Hard links** (`-H`), via `CreateHardLink` on NTFS.

## What does not work

* **Daemon mode** (`--daemon`, `rsync://`), which needs `fork()`, `chroot()`
  and Unix users.
* ACLs, xattrs, devices and Unix ownership are compiled out; none of them map
  onto Windows. `--archive` therefore behaves like `-rlt` plus permissions,
  and only the read-only bit of a file's mode is representable.

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

* **`select()` over pipes.** Windows `select()` only accepts sockets, but a
  client talking to ssh has nothing *but* pipes. `win32_select()` splits the
  fd sets, hands socket fds to Winsock and polls pipe fds with
  `PeekNamedPipe`, backing off from a spin to 5 ms so an active transfer stays
  responsive without burning a core when idle.
* **Large files.** MSVC's `off_t` is 32-bit, so the build takes the
  `stat64`/`off64_t` path that rsync already supports for 32-bit Unix
  (`USE_STAT64_FUNCS`). Offsets are 64-bit.

Set `RSYNC_WIN32_DEBUG=1` to trace the shim's fd operations to stderr.

## Testing

Against an Ubuntu 22.04 host running rsync 3.2.7 (protocol 31), over ssh:

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
