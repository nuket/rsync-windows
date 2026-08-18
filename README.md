RSYNC ON WINDOWS
----------------

This branch builds rsync natively on Windows with MSVC — no Cygwin, no MSYS,
no emulation layer — and adds a CMake build that replaces autoconf on every
platform. Push, pull, local copies, the delta algorithm, `--delete`, filters,
batch mode and UTF-8 file names all work; daemon mode, ACLs, xattrs, devices,
Unix ownership and link *creation* do not (see BUILD-CMAKE.md).

    windows-build-and-test.bat

builds it and runs the test suite, for x64 (`build\rsync.exe`) and x86
(`build-x86\rsync-x86.exe`) in turn. Either result is standalone, importing
only `WS2_32`, `ADVAPI32` and `KERNEL32` — all shipped with Windows — with the
C runtime linked in and the usual exploit mitigations enabled (Control Flow
Guard, CET shadow stack, strict `/GS`, Spectre v1 hardening, ASLR, DEP, and a
System32-only DLL search path).

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
