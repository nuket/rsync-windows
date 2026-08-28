/*
 * CPU feature level for the port's runtime-dispatched code.  See win32cpu.c.
 *
 * Copyright (C) 2026 Max Vilimpoc, rsync CMake/Windows port.
 * Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
 */
#ifndef WIN32CPU_H
#define WIN32CPU_H

#define WIN32_SIMD_SCALAR 0
#define WIN32_SIMD_SSE2   1
#define WIN32_SIMD_SSSE3  2
#define WIN32_SIMD_AVX2   3	/* AVX2 present and the OS saves YMM state */

int win32_simd_level(void);

#endif
