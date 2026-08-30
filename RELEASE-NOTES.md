rsync for Windows -- release notes
==================================

What this is
------------

rsync 3.5.0 built natively for Windows with MSVC -- no Cygwin, no MSYS --
together with the ssh.exe it runs.  Push, pull, local copies, the delta
algorithm, --delete, filters, batch mode and UTF-8 file names all work.
Daemon mode, ACLs, xattrs, devices, Unix ownership and symlink creation do
not.


What is in the zip
------------------

    rsync.exe                 the program
    ssh.exe                   the ssh client rsync runs (see "Speed" below)
    setup-windows-rsync.bat   installer: double-click it
    setup-windows-rsync.ps1   the script the .bat runs
    COPYING.txt               rsync's licence (GPL v3)
    NOTICE-ssh.txt            licences of what ssh.exe is built from
    RELEASE-NOTES.txt         this file

rsync-windows-x64.zip is for 64-bit Windows.  rsync-windows-x86.zip is for
32-bit Windows only: its ssh.exe cannot use the 64-bit libcrypto.dll that
64-bit Windows provides.


Install
-------

Unpack the zip anywhere and double-click setup-windows-rsync.bat.  It asks
for elevation, opens a new window that stays open when it is done, and:

  1. installs the Windows OpenSSH Client component if it is missing
     (ssh.exe needs its libcrypto.dll),
  2. sets the ssh-agent service to start automatically,
  3. installs and starts the OpenSSH Server, with a firewall rule for
     port 22, so this machine can receive transfers
     (add -SkipServer to leave that out),
  4. copies rsync.exe and ssh.exe to C:\Tools\rsync and puts that
     directory on the machine PATH.

Open a new console afterwards: the PATH change reaches only shells started
after it.  Re-running the installer is safe; it skips what is already done.

To skip the installer, unpack onto a directory on your PATH and keep
rsync.exe and ssh.exe together: rsync uses the ssh.exe beside it.

Requires Windows 10 1809 or later, Windows 11, or Windows Server 2019 or
later.


Speed
-----

Both directions run at the wire on 1 GbE and 2.5 GbE links, and delta
transfers of files that are edited in place (disk images, databases) no
longer take many times longer than a full copy.

  - Ask for AES-GCM.  OpenSSH's default cipher, chacha20-poly1305, has no
    hardware acceleration and caps a transfer at roughly 150-170 MB/s on a
    laptop; AES-GCM uses AES-NI and reaches the wire.  The cipher is chosen
    by the side that starts the transfer:

        rsync -rt -e "ssh -c aes128-gcm@openssh.com" src/ user@host:dst/

  - The ssh.exe in the zip is Microsoft's own Win32-OpenSSH client with its
    I/O fixed.  The one Windows ships reads 3 KB at a time and holds any
    transfer *from* a Windows machine at about 17 MB/s; this one reaches the
    wire on Ethernet, and ~1.5 GB/s sending / ~1.26 GB/s receiving over
    20 Gbit Thunderbolt networking (rsync ~1 GB/s pushing, ~980 MB/s
    pulling; a laptop core running AES-GCM is the limit there).  Same
    ~/.ssh, same ssh-agent, same known_hosts.

  - On Thunderbolt networking set the MTU to the maximum on both ends
    (65330 on Windows with Intel driver 1.41.1423, `ip link set dev
    thunderbolt0 mtu 65330` on Linux): at 1500 the Linux end's per-packet
    work capped a transfer into it at ~420 MB/s.


What changed since v3.5.0-gf800ace2
-----------------------------------

  - rsync.exe and the ssh.exe it starts now pass a transfer's bulk data
    through a shared-memory ring rather than a kernel pipe: two copies per
    byte instead of four, and no system call per chunk.  A fifth of the
    processor time of a push, gone.  Both ends fall back to the pipe if the
    other does not answer, so an older ssh.exe still works;
    RSYNC_WIN32_NO_SHMPIPE=1 keeps the pipes.

  - The ssh.exe no longer creates a thread for every write to stdout, nor a
    named pipe for every pass through its main loop (OpenSSH's portable
    pselect() fallback did that), its socket sends and receives run on
    their own threads, overlapping the crypto instead of following it, and
    two staging copies per byte are gone.  A packet buffer that was freed
    and reallocated for every packet is kept, and AES-GCM runs through
    Intel's ISA-L assembly rather than LibreSSL's EVP.  Cold that is worth
    about 4%; the reason it is the default is what happens when the machine
    is not cold.  Held at line rate for three minutes on a 15W laptop,
    Windows CNG gives up 6-11% of its throughput as the processor throttles
    and ISA-L gives up 1%, at the same clock and the same temperature.
    SSH_AESGCM_BACKEND=cng picks CNG, =libcrypto the original EVP path;
    ssh -vvv says which one a build is using.  The assembly is 64-bit, so
    this is the x64 download; the x86 one keeps CNG, as does any machine
    without AVX2, AES-NI and PCLMULQDQ.
    The random padding drawn for every packet no longer takes a kernel
    mutex to do it, and each encrypted packet is handed to the socket
    thread rather than copied to it.  None of it shows on Ethernet; on a
    20 Gbit link the client went from ~340 MB/s sending and ~220 MB/s
    receiving to ~1.5 GB/s sending and ~1.26 GB/s receiving (rsync: ~1
    GB/s pushing, ~980 MB/s pulling).
  - ssh.exe run with -n, or with its stdin from NUL, no longer spins a
    thread per pass of its main loop asking NUL for input; that cost a
    plain "ssh host command > file" a sixth of its time.
  - The I/O threads sleep between batches instead of spinning: ssh.exe
    now uses about 1.3-1.8 cores during a transfer rather than 2.4-2.9,
    at the same speed, and on a slow cipher or link the difference is a
    core and a half of nothing.  rsync.exe's own pipe threads stopped
    signalling their state on every 32KB, and it stopped asking Windows
    what an fd is on every read and write, which took another 8% off its
    processor time during a transfer.

What changed in v3.5.0-gf800ace2
--------------------------------

  - Delta transfers of files edited in place are up to 16x faster: the
    sender no longer rolls its checksum byte by byte through every changed
    block.  A 2.2 GB file with most of its blocks changed pulled to Linux in
    6.7 s instead of 106 s.
  - The block checksum and xxHash use SSE2/SSSE3/AVX2 chosen at run time
    (rsync --version now says SIMD-roll); 3.7x and 1.6x faster on one core.
  - The installer ships inside the zip and installs from it, no download.
  - A read past the end of a file in the sender's window management, which
    could make the receiver redo a file, is fixed.

Earlier in this port (v3.5.0-g00786d79 and before): rsync waited on pipes
instead of sleeping on them (7 MB/s to line rate on a transfer into
Windows), xxHash was bundled so transfers no longer fell back to MD5, and
the ssh.exe above was added (17 MB/s to line rate on a transfer out of
Windows).


Known limitations
-----------------

  - No daemon mode (rsync://), no ACLs, no xattrs, no symlink creation.
  - A *local* transfer with --protocol=30 hangs; protocol 31 and later,
    the default, are fine.


Checking a download
-------------------

Each zip has a .sha256 beside it on the releases page:

    certutil -hashfile rsync-windows-x64.zip SHA256

Source, build instructions and issues: https://github.com/nuket/rsync-windows
(branch windows-cmake-port).  rsync itself: https://rsync.samba.org/
