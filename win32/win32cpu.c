/*
 * Which x86 vector extensions this CPU -- and the OS running it -- can use.
 * Shared by the SIMD get_checksum1() (win32checksum.c) and the AVX2 build
 * of xxHash (win32xxh.c), so both make the same decision from one probe.
 *
 * Copyright (C) 2026 Max Vilimpoc, rsync CMake/Windows port.
 * Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
 */

#include "rsync.h"
#include <intrin.h>
#include "win32cpu.h"

static int simd_level = -1;

int win32_simd_level(void)
{
	if (simd_level < 0) {
		int r[4], maxleaf, sse2, ssse3, avx2 = 0;
		__cpuid(r, 0);
		maxleaf = r[0];
		__cpuid(r, 1);
		sse2 = (r[3] >> 26) & 1;	/* part of x64; a 32-bit build asks */
		ssse3 = (r[2] >> 9) & 1;
		if (maxleaf >= 7) {
			int osxsave = (r[2] >> 27) & 1;
			int avx = (r[2] >> 28) & 1;
			__cpuidex(r, 7, 0);
			avx2 = (r[1] >> 5) & 1;
			/* XGETBV is only legal once the OS has set OSXSAVE, and
			 * bits 1 and 2 of XCR0 say it saves XMM and YMM state:
			 * without that, AVX instructions fault however the CPU
			 * advertises them. */
			if (!(avx2 && osxsave && avx) || (_xgetbv(0) & 6) != 6)
				avx2 = 0;
		}
		simd_level = avx2 ? WIN32_SIMD_AVX2 : ssse3 ? WIN32_SIMD_SSSE3
			   : sse2 ? WIN32_SIMD_SSE2 : WIN32_SIMD_SCALAR;
	}
	return simd_level;
}
