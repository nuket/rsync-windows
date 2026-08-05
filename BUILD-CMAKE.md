# Building rsync with CMake (including Windows)

`CMakeLists.txt` is an alternative to the autoconf build. On Unix it is a
drop-in replacement for `./configure && make`; on Windows it is the only way
to build, since there is no shell, no `awk` and no autoconf there.

```sh
cmake -B build -G Ninja
cmake --build build
```

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
compatibility layer lives entirely in `win32/`; changes to the shared sources
are small and `#ifdef _WIN32`-guarded.

```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\rsync.exe --version
```

`rsync.exe` is self-contained — no Cygwin or MSYS DLLs. It uses the Windows
OpenSSH client (`C:\Windows\System32\OpenSSH\ssh.exe`) as its remote shell.

## What works

* Transfers to a remote host over ssh (`rsync -rt local/ user@host:/path/`),
  including recursion, the delta algorithm, `--delete`, `--exclude`,
  compression and `--stats`.
* Drive-letter paths (`C:\dir`, `C:/dir`) and backslash separators.
* File names outside the legacy ANSI code page. The executable embeds a
  manifest selecting the UTF-8 active code page, so names like
  `naïve-café-日本.txt` round-trip byte-for-byte to a UTF-8 Unix peer.
* Names containing spaces, quotes and shell metacharacters.

## What does not work yet

* **Pulling** (`rsync user@host:/path/ C:\local\`). The receiving side of
  rsync forks into a generator and a receiver process that run concurrently
  (`do_recv()` in `main.c`), and there is no `fork()` on Windows. This exits
  with `fork failed in do_recv: Function not implemented`. Emulating it means
  running the two halves as threads, which requires making rsync's I/O state
  per-thread — a substantial change that is not attempted here.
* **Local-to-local copies** (`rsync C:\a\ C:\b\`), for the same reason:
  `local_child()` forks an in-process server. Exits with a clear message.
* **Daemon mode** (`--daemon`, `rsync://`), which needs `fork()`, `chroot()`
  and Unix users.
* Symlinks, hard links, ACLs, xattrs, devices and Unix ownership are compiled
  out. `--archive` therefore behaves like `-rt` plus permissions; use `-rt`
  to be explicit. Only the read-only bit of a file's mode is representable.

## Design notes

| File | Responsibility |
| --- | --- |
| `win32/win32compat.h` | POSIX types, `S_IS*`, open flags, and the macros that route POSIX calls to `win32_*` wrappers. Pulled in by `config.h`, so it is visible tree-wide. |
| `win32/win32undef.h` | Undoes those macros. Every `win32/*.c` includes it so the shims can reach the real CRT/Winsock functions. |
| `win32/win32io.c` | fd routing. CRT fds serve files and pipes; sockets get pseudo-fds above `WIN32_SOCK_BASE` with a side table, because Winsock `SOCKET`s are not CRT fds. |
| `win32/win32proc.c` | `CreateProcess` in place of `fork()`+`execvp()` for the ssh child, plus `waitpid()`/`kill()`. |
| `win32/win32dir.c` | `opendir`/`readdir` over `FindFirstFile`. |
| `win32/win32compat.c` | stat/link/attribute/time calls, users and groups, `getpass`, signal filtering. |
| `win32/rsync.manifest` | Selects the UTF-8 active code page. |

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

## Known rough edges

* `st_ino` is 16 bits in MSVC's `struct _stat64`, so the 64-bit NTFS file
  index is truncated when stored. This only matters for features that are
  already disabled (`--hard-links`), but `rsync --version` reports
  "16-bit inums".
* `--iconv` is unavailable, so `--protect-args`/`--secluded-args` and
  charset conversion behave as on a build without iconv.
