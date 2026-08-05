/*
 * Undo the POSIX-name macros from win32compat.h.
 *
 * The shim implementations need to reach the genuine CRT/Winsock functions,
 * so every win32/*.c includes this right after rsync.h.
 *
 * Copyright (C) 2026 Max Vilimpoc, rsync CMake/Windows port.
 * Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
 */

#ifndef RSYNC_WIN32UNDEF_H
#define RSYNC_WIN32UNDEF_H

#undef read
#undef write
#undef close
#undef open
#undef open64
#undef pipe
#undef socketpair
#undef select
#undef fcntl
#undef dup
#undef dup2

#undef socket
#undef accept
#undef bind
#undef connect
#undef listen
#undef setsockopt
#undef getsockopt
#undef getpeername
#undef getsockname
#undef shutdown

#undef stat64
#undef fstat64
#undef lstat64
#undef lseek64
#undef ftruncate64
#undef mkstemp64

#undef readlink
#undef symlink
#undef link
#undef chown
#undef lchown
#undef chmod
#undef lchmod
#undef mkdir
#undef rename
#undef unlink
#undef utimes
#undef fsync
#undef rmdir
#undef chdir
#undef getcwd

#undef opendir
#undef readdir
#undef closedir

#undef getpwuid
#undef getpwnam
#undef getgrgid
#undef getgrnam

#undef waitpid
#undef kill
#undef fork
#undef execvp
#undef getpid

#undef gettimeofday
#undef sleep
#undef usleep
#undef gethostname
#undef isatty
#undef umask
#undef mknod
#undef mkfifo
#undef fchmod
#undef fchown

/* Critical: win32_signal() forwards to the CRT's signal(); without this it
 * would call itself forever. */
#undef signal
#undef localtime_r
#undef getpass
#undef access
#undef alarm
#undef setsid
#undef nice
#undef fchdir
#undef chroot
#undef openlog
#undef closelog
#undef syslog
#undef strcasecmp
#undef strncasecmp
#undef strdup
#undef getuid
#undef geteuid
#undef getgid
#undef getegid
#undef setuid
#undef seteuid
#undef setgid
#undef setegid
#undef getgroups
#undef setgroups
#undef initgroups

#endif /* RSYNC_WIN32UNDEF_H */
