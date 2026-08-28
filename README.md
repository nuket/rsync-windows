RSYNC ON WINDOWS
----------------

This branch builds rsync natively on Windows with MSVC — no Cygwin, no MSYS,
no emulation layer — and adds a CMake build that replaces autoconf on every
platform. Push, pull, local copies, the delta algorithm, `--delete`, filters,
batch mode and UTF-8 file names all work; daemon mode, ACLs, xattrs, devices,
Unix ownership and link *creation* do not (see BUILD-CMAKE.md).

    windows-build-and-test.bat

builds it and runs the test suite, for x64 (`build-x64\rsync.exe`) and x86
(`build-x86\rsync-x86.exe`) in turn. Either result is standalone, importing
only `WS2_32`, `ADVAPI32` and `KERNEL32` — all shipped with Windows — with the
C runtime linked in and the usual exploit mitigations enabled (Control Flow
Guard, CET shadow stack, strict `/GS`, Spectre v1 hardening, ASLR, DEP, and a
System32-only DLL search path).

### Installing on Windows

Building is optional. Every release on [the releases page][w1] carries
`rsync-windows-x64.zip` and `rsync-windows-x86.zip` with a `.sha256` beside
each. A zip holds `rsync.exe` and `ssh.exe`, both self-contained — unpack it
somewhere on your `PATH`, keep the two together, and it runs. It also holds
`setup-windows-rsync.bat` (and the `.ps1` it runs): double-click that from
the unpacked folder and it installs those files to `C:\Tools\rsync`, puts the
directory on the machine `PATH` and sets up ssh, as described below — the same
script, just fed from disk instead of downloading.

The `ssh.exe` is there because rsync does not speak ssh itself; it runs an ssh
client, and the one Windows ships reads its stdin 3KB at a time with a thread
per read, which holds a transfer *from* a Windows machine at about 17MB/s
however fast the link is. The one in the zip is Microsoft's own client, built
from the pinned `openssh/` submodule with that fixed (see `win32/openssh/`),
and `rsync.exe` prefers it whenever it sits in the same directory. It uses the
same `~/.ssh`, agent and `known_hosts` as the system one — and the same
`libcrypto.dll`, the one the Windows OpenSSH Client component (9.5 or later)
installs in System32, so that component has to be present. An `-e` naming
another ssh by path is used as given.

The sender's delta search is also faster than upstream's on files that are
modified in place — disk images, databases, anything a program rewrites
sectors of. Upstream rolls its checksum through every changed block one byte
at a time, with a cache-missing table lookup per byte, which held a laptop
core at 100% for ~30MB/s on a VM image pulled to Linux. This port keys the
lookup table on the full weak checksum and prefetches it, hoists the window
management out of the per-byte loop, and — before rolling through a changed
block at all — checks whether the next few blocks still sit where the basis
file has them, which on such files they almost always do. Same wire format,
same result file; on a 2.2GB image with three quarters of its blocks touched
the pull went from 106s to 6.7s, i.e. from CPU-bound to line rate.

The checksums themselves are vectorised too. Upstream's SIMD block checksum
is GCC/Clang-only (function multiversioning), so `win32/win32checksum*.c`
carries the same SSE2/SSSE3/AVX2 arithmetic as plain C with the CPU chosen
once from CPUID (`rsync --version` says `SIMD-roll`), and the bundled xxHash
is compiled a second time under `/arch:AVX2` and picked at runtime by
`win32/win32xxh.c` — xxHash's own dispatcher add-on is slower than the
baseline build under MSVC, which cannot keep AVX and legacy-SSE code apart
inside one function. On an i5-8350U that is 1.8 → 7.2 GB/s for the block
checksum and 9 → 16 GB/s for xxh128.

[w1]: https://github.com/nuket/rsync-windows/releases

To take a fresh box all the way to sending *and* receiving over rsync-over-ssh,
there is `setup-windows-rsync.ps1`. To run it:

1. Get the script onto the machine — clone this repository, or download just
   that one file from it.
2. Open a PowerShell window. Windows PowerShell 5.1 or PowerShell 7, either
   is fine, and it does **not** need to be an elevated one.
3. Change to the directory holding the script and run

       powershell -ExecutionPolicy Bypass -File .\setup-windows-rsync.ps1

4. Answer the UAC prompt. The script relaunches itself elevated in a **new**
   console window and does its work there; that window stays open at the end
   so you can read what it did.
5. Open a fresh PowerShell window before using `rsync`: the `PATH` change
   reaches only shells started after it.

`-ExecutionPolicy Bypass` is there only because the script is unsigned; in a
session whose policy already allows local scripts, `.\setup-windows-rsync.ps1`
on its own does the same. `Get-Help .\setup-windows-rsync.ps1 -Full` prints
the same instructions and every option.

Elevated, the script then:

1. installs the **OpenSSH Client** capability — rsync does not speak ssh
   itself, it execs an `ssh` binary, and on Windows that is the one this
   capability provides;
2. sets **ssh-agent** to Automatic and starts it, so keys loaded with `ssh-add`
   outlive the shell that loaded them. The service ships *Disabled*, so the
   start type has to be changed before it can be started at all;
3. installs **OpenSSH Server**, sets it Automatic, starts it, and widens the
   capability's own `OpenSSH-Server-In-TCP` firewall rule to *all* profiles.
   A VM's host-only or bridged adapter is routinely classified Public, and
   that is the usual reason a plainly running `sshd` is plainly unreachable;
4. resolves the **latest release**, picks the zip matching the OS bitness,
   verifies it against the published SHA-256, unpacks `rsync.exe` and
   `ssh.exe` into `C:\Tools\rsync`, and puts that directory on the
   **machine** `PATH`;
5. optionally authorises a public key for inbound ssh, with the ACLs
   Win32-OpenSSH requires before it will honour one.

What it does is adjustable — each of these goes on the end of the command
above:

    -AuthorizedKey $HOME\.ssh\id_ed25519.pub   # also authorise a key for inbound ssh
    -SkipServer                                # client side only: no sshd, no firewall rule
    -InstallDir D:\bin                         # somewhere other than C:\Tools\rsync
    -Tag v3.5.0-g00786d79                      # a particular release rather than the latest

Re-running is safe: each step checks before it acts, and rsync is re-downloaded
only when the installed binary is not the release being asked for. The run
below is a repeat run, which is why every step reports what was already in
place:

    ==> OpenSSH Client
        already installed: OpenSSH.Client~~~~0.0.1.0

    ==> ssh-agent service
        ssh-agent: Automatic + running

    ==> OpenSSH Server (sshd)
        already installed: OpenSSH.Server~~~~0.0.1.0
        sshd: Automatic + running
        firewall: widened OpenSSH-Server-In-TCP to all profiles

    ==> rsync for Windows (nuket/rsync-windows)
        release: v3.5.0-g521ad8ad
        already current: rsync 3.5.0-g521ad8ad at C:\Tools\rsync\rsync.exe
        C:\Tools\rsync already on the machine PATH
        rsync  version 3.5.0-g521ad8ad  protocol version 32

    ==> Done

      host   : DESKTOP (192.168.178.107)
      user   : Max
      rsync  : C:\Tools\rsync\rsync.exe
      sshd   : Running on TCP 22

      Receive - run this on the sending (Linux) box:
          rsync -av ./data/ Max@192.168.178.107:data/

      Send - run this here:
          ssh-add $HOME\.ssh\id_ed25519      # once; ssh-agent keeps it across boots
          rsync -av ./data/ user@linuxbox:/srv/data/

      No key is authorised for inbound ssh yet - password auth only.
      Re-run with the public key to fix that:
          .\setup-windows-rsync.ps1 -AuthorizedKey $HOME\.ssh\id_ed25519.pub

      Open a NEW shell before using rsync elsewhere: the machine PATH change
      does not reach shells that were already running.

Three details in there are worth knowing even if you set the box up by hand,
because each fails in a way that does not point at its own cause:

- **The `PATH` entry has to be the machine one, not the user one.** The remote
  end of a transfer runs as `rsync --server` in a non-interactive session with
  no login shell, and Win32-OpenSSH builds that session's environment from the
  registry rather than from a profile. A user-`PATH` entry is invisible to it,
  and the symptom is the confusing one: rsync works when you ssh in and type
  it, and is "not found" when rsync itself is the caller.
- **`sshd` caches the environment it started with**, so a `PATH` change does
  not reach a service that is already running. The script restarts it; by hand,
  remember to.
- **An administrator's `~/.ssh/authorized_keys` is ignored.** The default
  `sshd_config` routes every member of the Administrators group to
  `C:\ProgramData\ssh\administrators_authorized_keys` instead, and rejects
  either file if it is writable by anyone but SYSTEM and its owner. It fails
  closed and near-silently — the client simply falls back to asking for a
  password.

Install path is `C:\Tools\rsync` rather than anywhere under `Program Files` on
purpose: the remote end is invoked as a bare command line, and a path with a
space in it makes the client-side `--rsync-path` escape hatch painful to quote.

### The guiding constraint

The shared sources contain **no `#ifdef _WIN32`**. Platform variation is
expressed three other ways instead, so that upstream code stays readable and
merges stay clean:

1. **Platform hooks** — `rsync.h` defines a handful of macros right after
   `#include "config.h"`, each defaulting to the POSIX behaviour. A port
   overrides whichever it needs from a header named by
   `RSYNC_PLATFORM_INCLUDE`; a platform that needs none changes nothing.
2. **Stand-in headers** — `win32/include/` supplies `pwd.h`, `grp.h`,
   `dirent.h` and friends, so shared sources include them unconditionally.
3. **Conditional linking** — `pipe.c` and `win32/win32pipe.c` are alternative
   implementations of one interface; exactly one is linked.

### Changes to non-Windows files

22 shared files change, by 397 lines — and 123 of those are a single
`RSYNC_TLS` token added to a declaration, which expands to nothing off
Windows. No file gains a Windows conditional.

| File(s) | Change | Why |
| --- | --- | --- |
| `rsync.h` | +73: hook macros (`RSYNC_TLS`, `IS_ABS_PATH`, `IS_DRIVE_PATH`, `platform_init`, `platform_fix_path_args`, `close_sibling_fd`, `CHDIR_VIA_DIRFD`, `LOCAL_SERVER_SHARES_STATE`), each `#ifndef`-guarded with the POSIX default | the one place the port hooks into; every other shared change follows from these |
| `pipe.c` | +54: `spawn_receiver_half()`, `receiver_half_finish()`, `inc_recurse_when_receiving`, `local_server_shares_memory` | factors the `do_recv()` fork out of `main.c` so a port can replace it wholesale |
| `main.c` | +70/−75: `receiver_half()` extracted and de-static'd, `platform_init()` and `platform_fix_path_args()` call sites, `close_sibling_fd()`, a `SIGACTMASK` fallback where `sigaction()` is absent | the fork site and process startup |
| `io.c` | +75/−41: `io_fork_child_fixup()` (deep-copies the protocol buffers a real fork would have duplicated), plus 38 `RSYNC_TLS` marks | the largest concentration of state the two halves diverge on |
| `options.c` | +40/−9: `--backup`, `--no-backup`, `--append-verify`, `--no-append` moved from popt targets to `OPT_` switch codes | popt takes its target's address in a static initializer, which a thread-local cannot supply |
| `exclude.c` | +7/−6: `local_server` → `LOCAL_SERVER_SHARES_STATE` at five sites | a local server that is a separate process must actually be *sent* the filter list; without this `--delete --exclude` deleted the excluded files |
| `compat.c` | +9/−1: honour `inc_recurse_when_receiving` | incremental recursion needs the private address spaces `fork()` gives; threads share one heap |
| `batch.c` | +7/−2: `batch_fd` marked `RSYNC_TLS`, with the reason | it is set to −1 at input EOF in the receiving half only; sharing it deadlocked `--read-batch` |
| `util1.c` `main.c` `options.c` | `*p == '/'` → `IS_ABS_PATH(p)` at the three sites that test an operator-supplied *local* path (`change_dir`, the `--link-dest`/`--copy-dest`/`--compare-dest` roots, `--confine-root`); `CHDIR_VIA_DIRFD` guards the receiver's dirfd chdir | `C:\dir` is absolute too, and Windows has no directory fds to `fchdir()` to |
| `checksum.c` `cleanup.c` `clientserver.c` `delete.c` `flist.c` `generator.c` `log.c` `match.c` `progress.c` `receiver.c` `rsync.c` `sender.c` `xattrs.c` | `RSYNC_TLS` on declarations only | state `fork()` would have made private, marked so a thread-based split gets its own copy |

### Windows support files

All new, all Windows-only (`win32/`, ~3,100 lines):

| File | Lines | Role |
| --- | --- | --- |
| `win32io.c` | 769 | fd routing: CRT fds for files and pipes, pseudo-fds with a side table for Winsock sockets, and `select()`/`poll()` over both |
| `win32compat.c` | 621 | `stat`/`chmod`/`rename`/`unlink`, users and groups, times, signals, `getpass` |
| `win32compat.h` | 593 | POSIX types, `S_IS*`, open flags, the platform hooks, `struct win32_stat` (64-bit `st_ino`), and the macros routing POSIX names to `win32_*` |
| `win32proc.c` | 301 | `CreateProcess` for `fork()`+`execvp()`, `CommandLineToArgvW` quoting, `waitpid()`/`kill()` |
| `win32pipe.c` | 177 | replaces `pipe.c`: process plumbing and the generator/receiver split |
| `win32harden.c` | 154 | runtime exploit mitigations — DLL search order, extension points, image-load policy |
| `win32links.c` | 142 | records symlinks and hard links met, and lists them as the run ends |
| `win32fork.c` | 120 | `fork()` stand-in: a thread that starts from a copy of the parent's TLS block |
| `win32undef.h` | 113 | undoes every shim macro, so `win32/*.c` can reach the real CRT and Winsock |
| `win32dir.c` | 111 | `opendir`/`readdir` over `FindFirstFile` |
| `win32args.c` | 37 | the `platform_fix_path_args()` hook |
| `win32helperstubs.c` | 21 | log-level arrays the C test helpers need |
| `win32/include/` | 9 files | stand-ins: `pwd.h`, `grp.h`, `dirent.h`, `syslog.h`, `unistd.h`, `netinet/{in,tcp}.h`, `arpa/inet.h`, `sys/ioctl.h` |
| `rsync.manifest`, `rsync.rc` | 37 | selects the UTF-8 active code page, embedded as a resource |
| `win32/tests/` | 24 tests | covers what the port supports and skips what Windows cannot do; `testsuite/` is untouched |

### Build files

| File | Lines | Role |
| --- | --- | --- |
| `CMakeLists.txt` | 1032 | full autoconf replacement: feature probes, generated headers, source lists, test helpers, mitigation and static-CRT options |
| `BUILD-CMAKE.md` | 571 | build instructions, design notes, and what does not work |
| `cmake/gen-headers.py` | 304 | Python ports of the `awk` generators (`proto.h`, `daemon-parm.h`, `help-*.h`, `default-*.h`), byte-exact against real `awk` |
| `cmake/config.h.in` | 257 | the `config.h` template |
| `windows-build-and-test.bat` | 392 | one-command build and test of both architectures; finds MSVC via `vswhere`, so no Developer Command Prompt is needed |
| `setup-linux.sh` | 272 | installs the build dependencies on Ubuntu 22.04 and 26.04 |
| `setup-windows-rsync.ps1` | 460 | provisions a Windows box to send and receive: OpenSSH client and server, ssh-agent, and the latest release on the machine `PATH` |

Python 3.6+ is the only new hard requirement, and only because it replaces
`awk` for header generation.

### Testing

`win32/tests/` holds 24 tests that run on Windows, including push, pull and
delta over ssh to a Linux peer. The upstream suite in `testsuite/` is left
untouched and still runs on Linux under both build systems.

Upstream has ~350 tests to this port's two dozen, which invites the question of
whether the difference is coverage or just packaging. Mostly it is packaging:
upstream writes one file per narrow regression, this port writes one file per
topic with many assertions inside. The rest is upstream testing things Windows
does not do at all.

Rather than assert that in prose, `win32/tests/coverage_map.py` computes it.
Every upstream test is either given a documented reason it cannot apply here,
or mapped to the port test covering the same ground, or listed as a gap:

| upstream tests | count | status |
| --- | ---: | --- |
| **total in `testsuite/`** | 345 | |
| symlink creation | 69 | not applicable -- the port reports links rather than creating them: making one needs a privilege most accounts lack |
| daemon / chroot mode | 67 | not applicable -- daemon mode is not supported on Windows |
| real-TCP transport | 43 | not applicable -- needs the real-TCP transport; upstream skips these under the default `make check` too (testsuite/skiplist/common.txt) |
| rrsync wrapper | 19 | not applicable -- rrsync is a Perl/POSIX wrapper script, not part of the port |
| ACLs and xattrs | 14 | not applicable -- ACLs and xattrs are not supported on Windows |
| Unix ids, devices, fake-super | 13 | not applicable -- Unix ownership and special files have no Windows equivalent |
| rsync-ssl / proxy | 5 | not applicable -- rsync-ssl needs stunnel/openssl and a daemon to talk to |
| AddressSanitizer build | 2 | not applicable -- needs an AddressSanitizer build |
| **applicable here** | 113 | 111 covered, 2 not |

| port test | upstream tests it covers |
| --- | --- |
| `altdest` | 7: `alt-dest`, `alt-dest-deep`, `link-dest-module-escape`, `link-dest-pathroot`, `operator-path-compare-dest`, `operator-path-copy-dest` and 1 more |
| `backup` | 5: `backup`, `backup-deep`, `backup-dir-relative`, `backup-dir-repeated-separator-delete`, `operator-path-backup-dir` |
| `basics` | 12: `00-hello`, `atimes`, `crtimes`, `dirs`, `executability`, `hands` and 6 more |
| `batch` | 5: `batch-mode`, `batch-only-remove-source-regression`, `operator-path-write-batch`, `write-batch-filter-injection`, `write-batch-quoting` |
| `compress` | 3: `compare`, `compress-options`, `compress-zlib-insert` |
| `delete_exclude` | 4: `delete`, `delete-deep`, `delete-missing-args-files-from`, `scanner-delete-delay-overread` |
| `delta` | 7: `append`, `append-shortsum`, `change-shrink`, `change-vanish`, `growing-file`, `hashsearch-chain` and 1 more |
| `filenames` | 2: `ki58-log-format-percent`, `log-control-chars` |
| `files_from` | 4: `files-from`, `files-from-depth`, `files-from-path-clamp`, `operator-path-files-from` |
| `filters` | 9: `cvs-exclude`, `exclude-lsh`, `filter-depth`, `filter-merge-content-echo`, `filter-merge-recursion`, `ki73-cvs-clear-list` and 3 more |
| `fuzzy` | 2: `fuzzy`, `fuzzy-basis` |
| `helpers` | 12: `clean-fname-collapse`, `clean-fname-underflow`, `hashtable-overflow`, `iwildmatch-fold`, `max-alloc-zero-rejected`, `recv-discard-nullderef` and 6 more |
| `inplace_append` | 5: `inplace`, `partial`, `partial-dir-abs-delta`, `partial_nowrite`, `readonly-partial-abort-mode-regression` |
| `itemize` | 3: `ki62-io-error-mask`, `metadata-depth`, `output-options` |
| `links_reported` | 2: `hardlinks`, `hardlinks-deep` |
| `local_copy` | 1: `reverse-daemon-delta` |
| `options` | 14: `authenticate-no-ocloexec-build-regression`, `chmod`, `chmod-option`, `chmod-temp-dir`, `connect-prog-host-quoting`, `connect-prog-nested-singlequote-host-injection` and 8 more |
| `paths` | 2: `operator-path-insecure-links-refused`, `operator-path-log-file` |
| `relative` | 3: `relative`, `relative-content`, `relative-implied` |
| `remote` | 3: `sender-remove-source-relative-anchor`, `sender-remove-source-root-anchor`, `ssh-basic` |
| `shims` | Windows-specific; no upstream equivalent |
| `transfer_control` | 6: `delay-updates`, `delay-updates-deep`, `operator-path-inplace-backup-dir`, `operator-path-partial-dir`, `operator-path-temp-dir`, `temp-dir` |
| `unsafe_symlink` | Windows-specific; no upstream equivalent |
| `wildmatch` | Windows-specific; no upstream equivalent |

Regenerate the table with `python win32/tests/coverage_map.py --markdown`, and
see the whole picture with no arguments.

The part that matters is `--check`, which the CI build runs: it exits non-zero
when an upstream test is applicable to Windows and is neither covered nor
listed in `KNOWN_GAPS`. A rebase onto a new upstream release brings new tests
with it, and this is what stops them from being absorbed silently — the mapping
has to be extended, or the gap accepted in writing, before the build is green.

The "covered" claims are topic-level, not line-for-line: `filters` covers
upstream's dozen filter tests by exercising the same rules, not by
reimplementing each file.

WHAT IS RSYNC?
--------------

Rsync is a fast and extraordinarily versatile file copying tool for
both remote and local files.

Rsync uses a delta-transfer algorithm which provides a very fast method
for bringing remote files into sync.  It does this by sending just the
differences in the files across the link, without requiring that both
sets of files are present at one of the ends of the link beforehand.  At
first glance this may seem impossible because the calculation of diffs
between two files normally requires local access to both files.

A technical report describing the rsync algorithm is included with this
package.


USAGE
-----

Basically you use rsync just like scp, but rsync has many additional
options.  To get a complete list of supported options type:

    rsync --help

See the [manpage][0] for more detailed information.

[0]: https://download.samba.org/pub/rsync/rsync.1

BUILDING AND INSTALLING
-----------------------

If you need to build rsync yourself, check out the [INSTALL][1] page for
information on what libraries and packages you can use to get the maximum
features in your build.

[1]: https://github.com/RsyncProject/rsync/blob/master/INSTALL.md

SETUP
-----

Rsync normally uses ssh or rsh for communication with remote systems.
It does not need to be setuid and requires no special privileges for
installation.  You must, however, have a working ssh or rsh system.
Using ssh is recommended for its security features.

Alternatively, rsync can run in `daemon' mode, listening on a socket.
This is generally used for public file distribution, although
authentication and access control are available.

To install rsync, first run the "configure" script.  This will create a
Makefile and config.h appropriate for your system.  Then type "make".

Note that on some systems you will have to force configure not to use
gcc because gcc may not support some features (such as 64 bit file
offsets) that your system may support.  Set the environment variable CC
to the name of your native compiler before running configure in this
case.

Once built put a copy of rsync in your search path on the local and
remote systems (or use "make install").  That's it!


RSYNC DAEMONS
-------------

Rsync can also talk to "rsync daemons" which can provide anonymous or
authenticated rsync.  See the rsyncd.conf(5) manpage for details on how
to setup an rsync daemon.  See the rsync(1) manpage for info on how to
connect to an rsync daemon.


WEB SITE
--------

For more information, visit the [main rsync web site][2].

[2]: https://rsync.samba.org/

You'll find a FAQ list, downloads, resources, HTML versions of the
manpages, etc.


MAILING LISTS
-------------

There is a mailing list for the discussion of rsync and its applications
that is open to anyone to join.  New releases are announced on this
list, and there is also an announcement-only mailing list for those that
want official announcements.  See the [mailing-list page][3] for full
details.

[3]: https://rsync.samba.org/lists.html


DISCORD
-------

There is also an rsync [Discord server][d] for real-time chat about rsync
and its development.

[d]: https://discord.gg/Avfvy9zhdp


BUG REPORTS
-----------

The [bug-tracking web page][4] has full details on bug reporting.

[4]: https://rsync.samba.org/bug-tracking.html

That page contains links to the current bug list, and information on how to
do a good job when reporting a bug.  You might also like to try searching
the Internet for the error message you've received, or looking in the
[mailing list archives][5].

[5]: https://mail-archive.com/rsync@lists.samba.org/

To send a bug report, follow the instructions on the bug-tracking
page of the web site.

Alternately, email your bug report to <rsync@lists.samba.org>.

For security issues please email details of the issue to <rsync.project@gmail.com>.

GIT REPOSITORY
--------------

If you want to get the very latest version of rsync direct from the
source code repository, then you will need to use git.  The git repo
is hosted [on GitHub][6] and [on Samba's site][7].

[6]: https://github.com/RsyncProject/rsync
[7]: https://git.samba.org/?p=rsync.git;a=summary

See [the download page][8] for full details on all the ways to grab the
source.

[8]: https://rsync.samba.org/download.html


COPYRIGHT
---------

Rsync was originally written by Andrew Tridgell and Paul Mackerras.  Many
people from around the world have helped to maintain and improve it.

Special thanks go to Wayne Davison, who maintained rsync from 2004 to 2024.

Rsync may be used, modified and redistributed only under the terms of
the GNU General Public License, found in the file [COPYING][9] in this
distribution, or at [the Free Software Foundation][10].

[9]: https://github.com/RsyncProject/rsync/blob/master/COPYING
[10]: https://www.fsf.org/licenses/gpl.html
