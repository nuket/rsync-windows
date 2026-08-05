#
# CMake build for rsync.
#
# This is an alternative to the autoconf build (configure && make).  It exists
# mainly so that rsync can be built on Windows with MSVC/clang-cl, where there
# is no shell, no awk and no autoconf -- but it is a full replacement on
# Linux/macOS/BSD too.
#
#   cmake -B build -G Ninja
#   cmake --build build
#
# Copyright (C) 2026 Max Vilimpoc, rsync CMake/Windows port.
# Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
#
cmake_minimum_required(VERSION 3.20)

project(rsync
        VERSION 3.4.4
        DESCRIPTION "fast incremental file transfer"
        HOMEPAGE_URL "https://rsync.samba.org/"
        LANGUAGES C)

include(CheckCSourceCompiles)
include(CheckCSourceRuns)
include(CheckFunctionExists)
include(CheckIncludeFile)
include(CheckStructHasMember)
include(CheckSymbolExists)
include(CheckTypeSize)
include(CMakePushCheckState)
include(TestBigEndian)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(RSYNC_ENABLE_ACLS      "Support POSIX ACLs"              ON)
option(RSYNC_ENABLE_XATTRS    "Support extended attributes"     ON)
option(RSYNC_ENABLE_OPENSSL   "Use OpenSSL for MD4/MD5"         OFF)
option(RSYNC_ENABLE_ZSTD      "Support zstd compression"        OFF)
option(RSYNC_ENABLE_LZ4       "Support LZ4 compression"         OFF)
option(RSYNC_ENABLE_XXHASH    "Support xxhash checksums"        OFF)
option(RSYNC_ENABLE_ICONV     "Support --iconv character conversion" OFF)
option(RSYNC_ENABLE_SIMD      "Use SIMD/asm checksum acceleration" OFF)
option(RSYNC_EXTERNAL_ZLIB    "Link the system zlib instead of the bundled copy" OFF)
# Note: the test suite (runtests.py) and its C helpers are driven by the
# autoconf Makefile's "make check"; CMake builds only rsync itself.

set(RSYNC_PATH      "rsync" CACHE STRING "Default remote rsync command")
set(RSYNC_RSH       "ssh"   CACHE STRING "Default remote shell")
set(NOBODY_USER     "nobody" CACHE STRING "Unprivileged user for the daemon")
set(NOBODY_GROUP    "nogroup" CACHE STRING "Unprivileged group for the daemon")

set(RSYNC_PLATFORM_INCLUDE "")
set(RSYNC_PLATFORM_INCLUDE_DIR "")

if(WIN32)
    set(RSYNCD_SYSCONF  "rsyncd.conf")
    set(POPT_SYSCONFDIR ".")
    set(RSYNC_PLATFORM_INCLUDE "#include \"win32/win32compat.h\"")
    # Shim headers (pwd.h, dirent.h, syslog.h, ...) so the shared sources
    # can include them unconditionally.
    set(RSYNC_PLATFORM_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/win32/include")
else()
    set(RSYNCD_SYSCONF  "/etc/rsyncd.conf")
    set(POPT_SYSCONFDIR "/etc")
endif()

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}")

# rsync.h does #include "config.h", and a quoted include is resolved against
# the including file's own directory before any -I path.  So a config.h left
# in the source tree by a previous ./configure silently wins over ours, and
# the mismatch shows up much later as bizarre link errors.
foreach(stale config.h proto.h daemon-parm.h rounding.h)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${stale}")
        message(FATAL_ERROR
            "Found ${stale} in the source tree, left over from an autoconf "
            "build.  It would shadow the copy CMake generates.  Run "
            "'make distclean' (or delete the generated *.h files) first.")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Generated headers (these replace the awk rules in Makefile.in)
#
# They are produced at configure time because the rounding.h probe below has
# to be able to #include rsync.h, which in turn needs proto.h.
# ---------------------------------------------------------------------------
find_package(Python3 3.6 REQUIRED COMPONENTS Interpreter)

set(GEN_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen-headers.py")

function(rsync_generate name)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${GEN_SCRIPT} ${ARGN}
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "Failed to generate ${name}:\n${_out}${_err}")
    endif()
endfunction()

rsync_generate("daemon-parm.h" daemon-parm
    -o "${GEN_DIR}/daemon-parm.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/daemon-parm.txt")

rsync_generate("help-rsync.h" help
    -o "${GEN_DIR}/help-rsync.h" "${CMAKE_CURRENT_SOURCE_DIR}/rsync.1.md")
rsync_generate("help-rsyncd.h" help
    -o "${GEN_DIR}/help-rsyncd.h" "${CMAKE_CURRENT_SOURCE_DIR}/rsync.1.md")
rsync_generate("default-cvsignore.h" define
    -o "${GEN_DIR}/default-cvsignore.h" "${CMAKE_CURRENT_SOURCE_DIR}/rsync.1.md")
rsync_generate("default-dont-compress.h" define
    -o "${GEN_DIR}/default-dont-compress.h" "${CMAKE_CURRENT_SOURCE_DIR}/rsync.1.md")

file(GLOB PROTO_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
list(SORT PROTO_SOURCES)
rsync_generate("proto.h" proto
    -o "${GEN_DIR}/proto.h"
    ${PROTO_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/compat.c"
    "${GEN_DIR}/daemon-parm.h")

# Re-run cmake when any generator input changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/daemon-parm.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/rsync.1.md"
    "${GEN_SCRIPT}")

# git-version.h (mkgitver's job)
set(RSYNC_GITVER "")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --always --dirty
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    OUTPUT_VARIABLE RSYNC_GITVER
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
endif()
if(RSYNC_GITVER)
    file(WRITE "${GEN_DIR}/git-version.h" "#define RSYNC_GITVER \"${RSYNC_GITVER}\"\n")
else()
    file(WRITE "${GEN_DIR}/git-version.h" "")
endif()

# ---------------------------------------------------------------------------
# Platform feature detection
# ---------------------------------------------------------------------------
if(NOT WIN32)
    # _GNU_SOURCE et al. must be visible to every probe as well as the build.
    add_compile_definitions(_GNU_SOURCE)
    set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
endif()

set(CHECK_HEADERS
    ctype.h dirent.h dl.h fcntl.h grp.h iconv.h inttypes.h langinfo.h
    libcharset.h limits.h locale.h malloc.h memory.h ndir.h netdb.h
    netgroup.h stdint.h stdlib.h strings.h string.h utime.h unistd.h
    compat.h bsd/string.h
    netinet/in_systm.h netinet/ip.h
    sys/acl.h sys/attr.h sys/dir.h sys/extattr.h sys/fcntl.h sys/file.h
    sys/filio.h sys/ioctl.h sys/mode.h sys/ndir.h sys/param.h sys/select.h
    sys/socket.h sys/stat.h sys/time.h sys/types.h sys/un.h sys/wait.h
    sys/xattr.h attr/xattr.h linux/falloc.h)

foreach(hdr IN LISTS CHECK_HEADERS)
    string(TOUPPER "HAVE_${hdr}" var)
    string(REGEX REPLACE "[/.-]" "_" var "${var}")
    check_include_file("${hdr}" ${var})
endforeach()

check_c_source_compiles("
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <float.h>
int main(void) { return 0; }" STDC_HEADERS)

set(CHECK_FUNCS
    asprintf chmod fchmod ftruncate getaddrinfo getattrlist getcwd getegid
    geteuid getgrouplist getgroups getpass getpgrp hstrerror inet_ntop
    inet_pton initgroups innetgr lchmod lchown link linkat locale_charset
    lseek64 lutimes mallinfo mallinfo2 memmove mkfifo mkfifoat mknod mknodat
    mkstemp64 mktime nanosleep nl_langinfo open64 putenv readlink setattrlist
    setenv seteuid setgroups setlocale setsid setvbuf sigaction sigprocmask
    snprintf socketpair strchr strerror strlcat strlcpy strpbrk strtol
    tcgetpgrp unsetenv usleep utime utimensat utimes vasprintf vsnprintf
    wait4 waitpid fallocate posix_fallocate)

foreach(fn IN LISTS CHECK_FUNCS)
    string(TOUPPER "HAVE_${fn}" var)
    check_function_exists("${fn}" ${var})
endforeach()

if(HAVE_POSIX_FALLOCATE)
    set(HAVE_EFFICIENT_POSIX_FALLOCATE 1)
endif()
if(HAVE_FALLOCATE)
    set(HAVE_SYS_FALLOCATE 1)
endif()

# MSVC's CRT exports a `setmode` that has nothing to do with BSD setmode(3),
# and an `open64`-free but `_stat64`-based large-file API.  Don't let the
# generic probes above mistake those for the POSIX functions rsync wants.
if(WIN32)
    set(HAVE_SETMODE 0)
    set(HAVE_GETCWD 1)
    set(HAVE_CHMOD 1)
    # win32_utimes() can set times on directories (via CreateFile with
    # FILE_FLAG_BACKUP_SEMANTICS); the CRT's _utime() cannot.
    set(HAVE_UTIMES 1)
    set(HAVE_UTIME 1)
    set(HAVE_MKTIME 1)
    set(HAVE_STRCHR 1)
    set(HAVE_STRERROR 1)
    set(HAVE_STRPBRK 1)
    set(HAVE_STRTOL 1)
    set(HAVE_MEMMOVE 1)
    set(HAVE_PUTENV 1)
    set(HAVE_SETVBUF 1)
    set(HAVE_SETLOCALE 1)
    set(HAVE_VSNPRINTF 1)
    set(HAVE_SNPRINTF 1)
    set(HAVE_C99_VSNPRINTF 1)
    set(HAVE_ERRNO_DECL 1)
    set(HAVE_STRUCT_UTIMBUF 1)
    set(HAVE_GETADDRINFO 1)
    set(HAVE_INET_NTOP 1)
    set(HAVE_INET_PTON 1)
    set(HAVE_STRUCT_ADDRINFO 1)
    set(HAVE_STRUCT_SOCKADDR_STORAGE 1)
    # The win32 shim implements these on top of the Win32 API.
    set(HAVE_WAITPID 1)
    set(HAVE_SOCKETPAIR 1)
    set(HAVE_USLEEP 1)
    set(HAVE_STRUCT_STAT_ST_RDEV 1)
    # 64-bit file offsets come from the stat64 family, exactly like the
    # large-file path rsync already supports on 32-bit Unix.
    set(HAVE_STRUCT_STAT64 1)
    set(HAVE_LSEEK64 1)
    set(HAVE_OPEN64 1)
    set(HAVE_MKSTEMP64 1)
    set(HAVE_SECURE_MKSTEMP 1)
    set(INET6 1)
    set(SIGNED_CHAR_OK 1)
    set(GETPGRP_VOID 1)
    # Supplied by the CRT or by win32compat.h, so rsync.h must not re-typedef.
    set(HAVE_ID_T 1)
    set(HAVE_PID_T 1)
    set(HAVE_MODE_T 1)
    set(HAVE_OFF_T 1)
    set(HAVE_SIZE_T 1)
    # win32_gettimeofday() takes the (tv, tz) form.
    set(HAVE_GETTIMEOFDAY_TZ 1)
    # win32compat.c implements getpass() with the console API, so the
    # termios-based lib/getpass.c is not needed.
    set(HAVE_GETPASS 1)
    # win32/include supplies these, so rsync.h can include them plainly.
    set(HAVE_DIRENT_H 1)
    # Deliberately NOT setting HAVE_READLINK or HAVE_LINK.  Symlinks and hard
    # links are not a first-class concept for ordinary Windows users --
    # creating a symlink needs Developer Mode or a privilege most accounts
    # lack -- so rsync treats them as the plain files they resolve to and
    # win32/win32links.c lists any it met at the end of the run.
    # _chsize_s() behind the POSIX name; enables --inplace/--append.
    set(HAVE_FTRUNCATE 1)
else()
    check_function_exists(setmode HAVE_SETMODE)
endif()

check_symbol_exists(errno "errno.h" HAVE_ERRNO_DECL)

# --- types -----------------------------------------------------------------
foreach(t id_t mode_t off_t pid_t size_t)
    string(TOUPPER "HAVE_${t}" var)
    check_type_size("${t}" ${var}_SIZE LANGUAGE C)
    if(${var}_SIZE)
        set(${var} 1)
    endif()
endforeach()

check_type_size("long long"     SIZEOF_LONG_LONG)
check_type_size("long double"   SIZEOF_LONG_DOUBLE)
check_type_size("int"           SIZEOF_INT)
check_type_size("short"         SIZEOF_SHORT)
check_type_size("long"          SIZEOF_LONG)
check_type_size("char *"        SIZEOF_CHARP)
check_type_size("time_t"        SIZEOF_TIME_T)
check_type_size("int16_t"       SIZEOF_INT16_T)
check_type_size("uint16_t"      SIZEOF_UINT16_T)
check_type_size("int32_t"       SIZEOF_INT32_T)
check_type_size("uint32_t"      SIZEOF_UINT32_T)
check_type_size("int64_t"       SIZEOF_INT64_T)
check_type_size("off_t"         SIZEOF_OFF_T)

if(SIZEOF_LONG_LONG)
    set(HAVE_LONG_LONG 1)
endif()
if(SIZEOF_LONG_DOUBLE)
    set(HAVE_LONG_DOUBLE 1)
endif()

if(WIN32)
    # MSVC's off_t is a 32-bit long; the 64-bit variant is __int64.
    set(SIZEOF_OFF64_T 8)
else()
    cmake_push_check_state()
    set(CMAKE_EXTRA_INCLUDE_FILES sys/types.h)
    check_type_size("off64_t" SIZEOF_OFF64_T)
    cmake_pop_check_state()
    if(NOT SIZEOF_OFF64_T)
        set(SIZEOF_OFF64_T 0)
    endif()

    cmake_push_check_state()
    set(CMAKE_EXTRA_INCLUDE_FILES sys/types.h sys/stat.h)
    check_type_size("struct stat64" SIZEOF_STRUCT_STAT64)
    cmake_pop_check_state()
    if(SIZEOF_STRUCT_STAT64)
        set(HAVE_STRUCT_STAT64 1)
    endif()
endif()

foreach(v SIZEOF_INT SIZEOF_SHORT SIZEOF_LONG SIZEOF_LONG_LONG SIZEOF_CHARP
          SIZEOF_OFF_T SIZEOF_TIME_T SIZEOF_INT16_T SIZEOF_UINT16_T
          SIZEOF_INT32_T SIZEOF_UINT32_T SIZEOF_INT64_T)
    if(NOT ${v})
        set(${v} 0)
    endif()
endforeach()

# --- struct members --------------------------------------------------------
if(NOT WIN32)
    check_struct_has_member("struct stat" st_rdev "sys/types.h;sys/stat.h"
                            HAVE_STRUCT_STAT_ST_RDEV)
    check_struct_has_member("struct stat" st_mtim.tv_nsec "sys/types.h;sys/stat.h"
                            HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC)
    check_struct_has_member("struct stat" st_mtimensec "sys/types.h;sys/stat.h"
                            HAVE_STRUCT_STAT_ST_MTIMENSEC)
    check_struct_has_member("struct stat" st_mtimespec.tv_nsec "sys/types.h;sys/stat.h"
                            HAVE_STRUCT_STAT_ST_MTIMESPEC_TV_NSEC)
    check_struct_has_member("struct sockaddr_in" sin_len "sys/types.h;netinet/in.h"
                            HAVE_SOCKADDR_IN_LEN)
    check_struct_has_member("struct sockaddr_in6" sin6_scope_id "sys/types.h;netinet/in.h"
                            HAVE_SOCKADDR_IN6_SCOPE_ID)
    check_struct_has_member("struct sockaddr_un" sun_len "sys/types.h;sys/un.h"
                            HAVE_SOCKADDR_UN_LEN)
    check_struct_has_member("struct utimbuf" actime "utime.h"
                            HAVE_STRUCT_UTIMBUF)
    cmake_push_check_state()
    set(CMAKE_EXTRA_INCLUDE_FILES sys/types.h sys/socket.h netdb.h)
    check_type_size("struct addrinfo" SIZEOF_STRUCT_ADDRINFO)
    cmake_pop_check_state()
    if(SIZEOF_STRUCT_ADDRINFO)
        set(HAVE_STRUCT_ADDRINFO 1)
    endif()
    check_symbol_exists(AF_INET6 "sys/types.h;sys/socket.h;netinet/in.h" INET6)

    check_c_source_runs("
#include <sys/types.h>
#include <sys/socket.h>
int main(void) { struct sockaddr_storage x; (void)x; return 0; }"
        HAVE_STRUCT_SOCKADDR_STORAGE)

    # Some old systems take only one argument.
    check_c_source_compiles("
#include <sys/time.h>
int main(void) {
    struct timeval tv; struct timezone tz;
    return gettimeofday(&tv, &tz);
}" HAVE_GETTIMEOFDAY_TZ)

    check_c_source_runs("int main(void) { signed char c = -1; return c > 0; }"
        SIGNED_CHAR_OK)
    check_c_source_compiles("
#include <unistd.h>
int main(void) { return getpgrp() < 0; }" GETPGRP_VOID)
endif()

test_big_endian(WORDS_BIGENDIAN)

check_c_source_compiles("
#include <stdarg.h>
void f(va_list a) { va_list b; va_copy(b, a); va_end(b); }
int main(void) { return 0; }" HAVE_VA_COPY)
check_c_source_compiles("
#include <stdarg.h>
void f(va_list a) { va_list b; __va_copy(b, a); va_end(b); }
int main(void) { return 0; }" HAVE___VA_COPY)

check_symbol_exists(major "sys/sysmacros.h" MAJOR_IN_SYSMACROS)
if(NOT MAJOR_IN_SYSMACROS)
    check_symbol_exists(major "sys/mkdev.h" MAJOR_IN_MKDEV)
endif()

check_symbol_exists(FALLOC_FL_PUNCH_HOLE "linux/falloc.h" HAVE_FALLOC_FL_PUNCH_HOLE)
check_symbol_exists(FALLOC_FL_ZERO_RANGE "linux/falloc.h" HAVE_FALLOC_FL_ZERO_RANGE)

if(HAVE_GETGROUPS)
    set(GETGROUPS_T "gid_t")
else()
    set(GETGROUPS_T "int")
endif()
if(WIN32)
    set(GETGROUPS_T "int")
endif()

if(HAVE_LOCALE_H AND HAVE_SETLOCALE)
    set(CONFIG_LOCALE 1)
endif()

# rsync's flexible-array usage; C99 compilers are fine with it.
set(USE_FLEXIBLE_ARRAY 1)

# --- symlink/hardlink behaviour --------------------------------------------
# The autoconf build runs these as runtime probes.  Assume the common POSIX
# answers when cross-compiling or on Windows.
if(NOT WIN32)
    set(CAN_HARDLINK_SYMLINK 1)
    set(CAN_HARDLINK_SPECIAL 1)
    if(APPLE)
        set(CAN_SET_SYMLINK_TIMES 1)
        set(CAN_CHMOD_SYMLINK 1)
    endif()
    if(HAVE_LUTIMES OR HAVE_UTIMENSAT)
        set(CAN_SET_SYMLINK_TIMES 1)
    endif()
    if(HAVE_LCHOWN)
        set(CAN_CHOWN_SYMLINK 1)
    endif()
    if(HAVE_LCHMOD)
        set(CAN_CHMOD_SYMLINK 1)
    endif()
    if(HAVE_UTIMENSAT OR HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC)
        set(CAN_SET_NSEC 1)
    endif()
    set(MKNOD_CREATES_FIFOS 1)
    set(MKNOD_CREATES_SOCKETS 1)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(NO_SYMLINK_USER_XATTRS 1)
    endif()
endif()

# ---------------------------------------------------------------------------
# Optional third-party features
# ---------------------------------------------------------------------------
set(RSYNC_EXTRA_LIBS "")

if(RSYNC_ENABLE_ACLS AND HAVE_SYS_ACL_H AND NOT WIN32)
    set(SUPPORT_ACLS 1)
    set(HAVE_POSIX_ACLS 1)
    check_include_file("acl/libacl.h" HAVE_ACL_LIBACL_H)
    find_library(ACL_LIBRARY acl)
    if(ACL_LIBRARY)
        list(APPEND RSYNC_EXTRA_LIBS ${ACL_LIBRARY})
    endif()
    if(APPLE)
        set(HAVE_OSX_ACLS 1)
        unset(HAVE_POSIX_ACLS)
    endif()
endif()

if(RSYNC_ENABLE_XATTRS AND NOT WIN32)
    if(APPLE)
        set(SUPPORT_XATTRS 1)
        set(HAVE_OSX_XATTRS 1)
    elseif(HAVE_SYS_XATTR_H OR HAVE_ATTR_XATTR_H)
        set(SUPPORT_XATTRS 1)
        set(HAVE_LINUX_XATTRS 1)
    elseif(HAVE_SYS_EXTATTR_H)
        set(SUPPORT_XATTRS 1)
        set(HAVE_FREEBSD_XATTRS 1)
    endif()
endif()

if(RSYNC_ENABLE_OPENSSL)
    find_package(OpenSSL REQUIRED)
    set(USE_OPENSSL 1)
    set(SET_OPENSSL_CONF 1)
    list(APPEND RSYNC_EXTRA_LIBS OpenSSL::Crypto)
endif()

if(RSYNC_ENABLE_ZSTD)
    find_package(zstd REQUIRED)
    set(SUPPORT_ZSTD 1)
    list(APPEND RSYNC_EXTRA_LIBS zstd::libzstd)
endif()

if(RSYNC_ENABLE_LZ4)
    find_library(LZ4_LIBRARY lz4 REQUIRED)
    set(SUPPORT_LZ4 1)
    list(APPEND RSYNC_EXTRA_LIBS ${LZ4_LIBRARY})
endif()

if(RSYNC_ENABLE_XXHASH)
    find_library(XXHASH_LIBRARY xxhash REQUIRED)
    set(SUPPORT_XXHASH 1)
    set(SUPPORT_XXH3 1)
    list(APPEND RSYNC_EXTRA_LIBS ${XXHASH_LIBRARY})
endif()

if(RSYNC_ENABLE_ICONV AND HAVE_ICONV_H)
    set(USE_ICONV_OPEN 1)
    set(ICONV_CONST "")
    set(ICONV_OPTION "NULL")
    find_library(ICONV_LIBRARY iconv)
    if(ICONV_LIBRARY)
        list(APPEND RSYNC_EXTRA_LIBS ${ICONV_LIBRARY})
    endif()
endif()

if(RSYNC_EXTERNAL_ZLIB)
    find_package(ZLIB REQUIRED)
    set(EXTERNAL_ZLIB 1)
    list(APPEND RSYNC_EXTRA_LIBS ZLIB::ZLIB)
endif()

# ---------------------------------------------------------------------------
# config.h
# ---------------------------------------------------------------------------
configure_file(cmake/config.h.in "${GEN_DIR}/config.h")

# ---------------------------------------------------------------------------
# rounding.h -- probe for the padding the compiler puts in file_extras[]
# ---------------------------------------------------------------------------
set(EXTRA_ROUNDING_FOUND OFF)
foreach(r 0 1 3)
    try_compile(ROUNDING_OK
        SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/rounding.c"
        COMPILE_DEFINITIONS "-DEXTRA_ROUNDING=${r}"
        C_STANDARD 99
        CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${GEN_DIR};${CMAKE_CURRENT_SOURCE_DIR};${RSYNC_PLATFORM_INCLUDE_DIR}"
        OUTPUT_VARIABLE ROUNDING_OUTPUT)
    if(ROUNDING_OK)
        file(WRITE "${GEN_DIR}/rounding.h" "#define EXTRA_ROUNDING ${r}\n")
        message(STATUS "EXTRA_ROUNDING is ${r}")
        set(EXTRA_ROUNDING_FOUND ON)
        break()
    endif()
    unset(ROUNDING_OK CACHE)
endforeach()
if(NOT EXTRA_ROUNDING_FOUND)
    message(FATAL_ERROR
        "Failed to determine EXTRA_ROUNDING (could not build rounding.c):\n"
        "${ROUNDING_OUTPUT}")
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(RSYNC_SOURCES
    flist.c rsync.c generator.c receiver.c cleanup.c sender.c exclude.c
    util1.c util2.c main.c checksum.c match.c syscall.c android.c log.c
    backup.c delete.c
    options.c io.c compat.c hlink.c token.c uidlist.c socket.c hashtable.c
    usage.c fileio.c batch.c clientname.c chmod.c acls.c xattrs.c
    progress.c
    params.c loadparm.c clientserver.c access.c connection.c authenticate.c)

set(RSYNC_LIB_SOURCES
    lib/wildmatch.c lib/compat.c lib/snprintf.c lib/mdfour.c lib/md5.c
    lib/permstring.c lib/pool_alloc.c lib/sysacls.c lib/sysxattrs.c)

if(NOT HAVE_GETADDRINFO OR NOT HAVE_STRUCT_ADDRINFO)
    list(APPEND RSYNC_LIB_SOURCES lib/getaddrinfo.c)
endif()
if(NOT HAVE_INET_NTOP)
    list(APPEND RSYNC_LIB_SOURCES lib/inet_ntop.c)
endif()
if(NOT HAVE_INET_PTON)
    list(APPEND RSYNC_LIB_SOURCES lib/inet_pton.c)
endif()
if(NOT HAVE_GETPASS)
    list(APPEND RSYNC_LIB_SOURCES lib/getpass.c)
endif()

set(POPT_SOURCES
    popt/popt.c popt/poptconfig.c popt/popthelp.c popt/poptparse.c
    popt/poptint.c)

set(ZLIB_SOURCES
    zlib/deflate.c zlib/inffast.c zlib/inflate.c zlib/inftrees.c
    zlib/trees.c zlib/zutil.c zlib/adler32.c zlib/compress.c zlib/crc32.c)

# pipe.c and win32/win32pipe.c are alternative implementations of the same
# interface (process plumbing + the generator/receiver split), so exactly
# one of them is linked.
if(WIN32)
    list(APPEND RSYNC_SOURCES win32/win32pipe.c)
else()
    list(APPEND RSYNC_SOURCES pipe.c)
endif()

set(RSYNC_ALL_SOURCES ${RSYNC_SOURCES} ${RSYNC_LIB_SOURCES} ${POPT_SOURCES})
if(NOT RSYNC_EXTERNAL_ZLIB)
    list(APPEND RSYNC_ALL_SOURCES ${ZLIB_SOURCES})
endif()

if(WIN32)
    enable_language(RC)   # for win32/rsync.rc
    list(APPEND RSYNC_ALL_SOURCES
        win32/win32compat.c
        win32/win32proc.c
        win32/win32io.c
        win32/win32dir.c
        win32/win32fork.c
        win32/win32args.c
        win32/win32links.c)

endif()

if(RSYNC_ENABLE_SIMD AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(USE_ROLL_SIMD 1)
    list(APPEND RSYNC_ALL_SOURCES simd-checksum-x86_64.cpp)
endif()

add_executable(rsync ${RSYNC_ALL_SOURCES})

target_include_directories(rsync PRIVATE
    "${RSYNC_PLATFORM_INCLUDE_DIR}"   # a port's stand-ins for missing headers
    "${GEN_DIR}"                      # -I. : generated headers win
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/popt")

if(NOT RSYNC_EXTERNAL_ZLIB)
    target_include_directories(rsync PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/zlib")
endif()

# popt/system.h (and others) only pull in config.h when this is set; the
# autoconf build passes it in CFLAGS for every translation unit.
target_compile_definitions(rsync PRIVATE HAVE_CONFIG_H)

target_link_libraries(rsync PRIVATE ${RSYNC_EXTRA_LIBS})

if(WIN32)
    target_link_libraries(rsync PRIVATE ws2_32 advapi32 shell32 userenv)
    target_compile_definitions(rsync PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
        _WINSOCK_DEPRECATED_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX)
    if(MSVC)
        # Keep the noise down: rsync is old-school C and trips a lot of
        # MSVC's signed/unsigned and deprecation warnings.
        target_compile_options(rsync PRIVATE /W2 /wd4996 /wd4267 /wd4244 /wd4018)
        # Embed the manifest (UTF-8 active code page) as a resource, and
        # turn off the linker's own manifest so there is exactly one.
        target_sources(rsync PRIVATE win32/rsync.rc)
        target_link_options(rsync PRIVATE "/MANIFEST:NO")
    endif()
endif()

if(NOT WIN32)
    find_library(M_LIBRARY m)
    if(M_LIBRARY)
        target_link_libraries(rsync PRIVATE ${M_LIBRARY})
    endif()
endif()

# ---------------------------------------------------------------------------
# Test helpers
#
# runtests.py refuses to start without these; they are the C parts of the
# suite (Makefile.in's CHECK_PROGS).  Small enough to build unconditionally.
# ---------------------------------------------------------------------------
set(HELPER_WIN32_SOURCES "")
if(WIN32)
    # The compat layer these need; win32pipe.c and win32fork.c are left out
    # because they reach into main.c/io.c, which no helper links.
    set(HELPER_WIN32_SOURCES
        win32/win32compat.c win32/win32io.c win32/win32dir.c
        win32/win32proc.c win32/win32links.c)
endif()

function(rsync_test_helper name)
    add_executable(${name} ${ARGN} ${HELPER_WIN32_SOURCES})
    target_include_directories(${name} PRIVATE
        "${RSYNC_PLATFORM_INCLUDE_DIR}"
        "${GEN_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/popt")
    target_compile_definitions(${name} PRIVATE HAVE_CONFIG_H)
    target_link_libraries(${name} PRIVATE ${RSYNC_EXTRA_LIBS})
    # Land beside rsync itself: runtests.py looks for them in one place.
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32 advapi32 shell32 userenv)
        target_compile_definitions(${name} PRIVATE
            _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS
            _WINSOCK_DEPRECATED_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
        if(MSVC)
            target_compile_options(${name} PRIVATE
                /W2 /wd4996 /wd4267 /wd4244 /wd4018)
            # Same UTF-8 code page as rsync, so they agree about file names.
            target_sources(${name} PRIVATE win32/rsync.rc)
            target_link_options(${name} PRIVATE "/MANIFEST:NO")
        endif()
    endif()
endfunction()

set(HELPER_COMMON syscall.c android.c util2.c t_stub.c
                  lib/compat.c lib/snprintf.c lib/permstring.c)

# Makefile.in links tls and trimslash against syscall.c but not util1.c.
# That works where AT_FDCWD exists, because syscall.c then compiles the
# openat()-based secure_relative_open(); Windows has no AT_FDCWD, so it takes
# the portable fallback instead, which calls pathjoin() from util1.c.  util1.c
# in turn wants wildmatch and the log-level arrays that those two helpers --
# unlike the other three -- do not declare themselves.
set(HELPER_PATHJOIN "")
if(WIN32)
    set(HELPER_PATHJOIN util1.c lib/wildmatch.c win32/win32helperstubs.c)
endif()

# tls prints a Unix-style listing -- mode bits, uid/gid and a symlink's
# target -- and calls do_readlink() unconditionally, which this build does
# not provide because it has no symlink support.  It is only needed by
# testsuite/runtests.py, which does not run on Windows; win32/tests/ compares
# trees directly instead.
if(NOT WIN32)
    rsync_test_helper(tls    tls.c ${HELPER_COMMON} ${HELPER_PATHJOIN}
                             lib/sysxattrs.c ${POPT_SOURCES})
endif()
rsync_test_helper(trimslash  trimslash.c ${HELPER_COMMON} ${HELPER_PATHJOIN})
rsync_test_helper(t_unsafe   t_unsafe.c ${HELPER_COMMON}
                             util1.c lib/wildmatch.c)
rsync_test_helper(t_chmod_secure  t_chmod_secure.c ${HELPER_COMMON}
                             util1.c lib/wildmatch.c)
rsync_test_helper(t_secure_relpath t_secure_relpath.c ${HELPER_COMMON}
                             util1.c lib/wildmatch.c)
# No lib/wildmatch.c here: wildtest.c #includes it directly.
rsync_test_helper(wildtest   wildtest.c lib/compat.c lib/snprintf.c
                             ${POPT_SOURCES})
rsync_test_helper(getgroups  getgroups.c)
rsync_test_helper(getfsdev   getfsdev.c)

# ---------------------------------------------------------------------------
# The Windows test suite
#
# testsuite/ assumes Unix semantics throughout, so win32/tests/ covers what
# this port supports instead and skips what Windows genuinely cannot do:
#
#     cmake --build build --target win32-tests
#
# Set RSYNC_WIN_TEST_HOST=user@host first to include the ssh transfer tests.
# ---------------------------------------------------------------------------
if(WIN32)
    add_custom_target(win32-tests
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/win32/tests/run.py"
                --rsync-bin $<TARGET_FILE:rsync>
                --tooldir  $<TARGET_FILE_DIR:rsync>
                --srcdir   "${CMAKE_CURRENT_SOURCE_DIR}"
        DEPENDS rsync trimslash t_unsafe t_chmod_secure t_secure_relpath
                wildtest getgroups getfsdev
        COMMENT "Running the Windows test suite"
        USES_TERMINAL)
endif()

install(TARGETS rsync RUNTIME DESTINATION bin)

message(STATUS "")
message(STATUS "rsync ${PROJECT_VERSION} configuration:")
message(STATUS "  system .............. ${CMAKE_SYSTEM_NAME}")
message(STATUS "  compiler ............ ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  ACLs ................ ${SUPPORT_ACLS}")
message(STATUS "  xattrs .............. ${SUPPORT_XATTRS}")
message(STATUS "  OpenSSL ............. ${USE_OPENSSL}")
message(STATUS "  bundled zlib ........ ${RSYNC_EXTERNAL_ZLIB}")
message(STATUS "")
