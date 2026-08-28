/*
 * The pieces of the MSVC get_checksum1() port that live in more than one
 * translation unit: the AVX2 routine is compiled apart with /arch:AVX2 (see
 * win32checksum.c for why), and the test program in win32checksum.c checks
 * every routine against the scalar one.  Each takes the running (s1, s2)
 * pair, consumes as many whole 32- or 64-byte groups from buf[i] as its
 * loop bound allows, and returns the index it stopped at; the caller chains
 * them and finishes with get_checksum1_default_1().
 *
 * Copyright (C) 2026 Max Vilimpoc, rsync CMake/Windows port.
 * Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
 */
#ifndef WIN32CHECKSUM_H
#define WIN32CHECKSUM_H

int32 get_checksum1_avx2_64(schar *buf, int32 len, int32 i, uint32 *ps1, uint32 *ps2);
int32 get_checksum1_ssse3_32(schar *buf, int32 len, int32 i, uint32 *ps1, uint32 *ps2);
int32 get_checksum1_sse2_32(schar *buf, int32 len, int32 i, uint32 *ps1, uint32 *ps2);
int32 get_checksum1_default_1(schar *buf, int32 len, int32 i, uint32 *ps1, uint32 *ps2);

#endif
