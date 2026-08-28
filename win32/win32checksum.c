/*
 * SSE2/SSSE3/AVX2 get_checksum1() for MSVC -- the algorithm of
 * simd-checksum-x86_64.cpp, which that file's GCC/Clang-only function
 * multiversioning keeps out of a Visual C++ build.
 *
 * Copyright (C) 1996 Andrew Tridgell
 * Copyright (C) 1996 Paul Mackerras
 * Copyright (C) 2004-2020 Wayne Davison
 * Copyright (C) 2020 Jorrit Jongma
 * Copyright (C) 2026 Max Vilimpoc, rsync CMake/Windows port
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, visit the http://fsf.org website.
 */
/*
 * How this differs from the .cpp it is ported from, and why:
 *
 * MSVC lets any intrinsic be used in any translation unit -- there is no
 * per-function target attribute, and none is needed for the compiler to
 * accept _mm_maddubs_epi16() in a file built for baseline x64.  What it
 * does not do is pick an implementation for the CPU at hand, so the choice
 * is made once from CPUID, in win32cpu.c.
 *
 * The AVX2 routine lives in its own file, win32checksum-avx2.c, compiled
 * with /arch:AVX2.  Compiled into this baseline file it would work, but the
 * compiler would then mix VEX-encoded AVX2 with legacy-SSE code in the same
 * function, and the AVX/SSE transition penalty on Intel CPUs costs more
 * than the vectorisation gains.  Nothing in that file runs unless
 * win32_simd_level() has established that AVX2 is usable.
 *
 * The 32-byte SSE2 and SSSE3 loops, and the scalar tail, are otherwise a
 * line-for-line translation: same arithmetic, same loop bounds, so a block
 * checksummed here matches one checksummed by an upstream sender or
 * generator on any other platform -- which is the whole point, since this
 * weak checksum is compared across the wire.
 *
 * The test program at the bottom (compiled from this same file with
 * TEST_WIN32CHECKSUM defined, as the t_win32checksum helper) checks every
 * routine against the scalar one and for over-reads, the way upstream's
 * TEST_SIMD_CHECKSUM1 section does.
 */

#include "rsync.h"

#ifdef USE_ROLL_SIMD

#include <intrin.h>
#include "win32checksum.h"
#include "win32cpu.h"

/* Compatibility macros to let the SSSE3 algorithm run with only SSE2. */
#define SSE2_INTERLEAVE_ODD_EPI16(a, b) _mm_packs_epi32(_mm_srai_epi32(a, 16), _mm_srai_epi32(b, 16))
#define SSE2_INTERLEAVE_EVEN_EPI16(a, b) SSE2_INTERLEAVE_ODD_EPI16(_mm_slli_si128(a, 2), _mm_slli_si128(b, 2))
#define SSE2_MULU_ODD_EPI8(a, b) _mm_mullo_epi16(_mm_srli_epi16(a, 8), _mm_srai_epi16(b, 8))
#define SSE2_MULU_EVEN_EPI8(a, b) _mm_mullo_epi16(_mm_and_si128(a, _mm_set1_epi16(0xFF)), _mm_srai_epi16(_mm_slli_si128(b, 1), 8))

#define SSE2_HADDS_EPI16(a, b) _mm_adds_epi16(SSE2_INTERLEAVE_EVEN_EPI16(a, b), SSE2_INTERLEAVE_ODD_EPI16(a, b))
#define SSE2_MADDUBS_EPI16(a, b) _mm_adds_epi16(SSE2_MULU_EVEN_EPI8(a, b), SSE2_MULU_ODD_EPI8(a, b))

/*
  Original loop per 4 bytes:
    s2 += 4*(s1 + buf[i]) + 3*buf[i+1] + 2*buf[i+2] + buf[i+3] + 10*CHAR_OFFSET;
    s1 += buf[i] + buf[i+1] + buf[i+2] + buf[i+3] + 4*CHAR_OFFSET;

  SSE2/SSSE3 loop per 32 bytes:
    int16 t1[8];
    int16 t2[8];
    for (int j = 0; j < 8; j++) {
      t1[j] = buf[j*4 + i] + buf[j*4 + i+1] + buf[j*4 + i+2] + buf[j*4 + i+3];
      t2[j] = 4*buf[j*4 + i] + 3*buf[j*4 + i+1] + 2*buf[j*4 + i+2] + buf[j*4 + i+3];
    }
    s2 += 32*s1 + (uint32)(
              28*t1[0] + 24*t1[1] + 20*t1[2] + 16*t1[3] + 12*t1[4] + 8*t1[5] + 4*t1[6] +
              t2[0] + t2[1] + t2[2] + t2[3] + t2[4] + t2[5] + t2[6] + t2[7]
          ) + 528*CHAR_OFFSET;
    s1 += (uint32)(t1[0] + t1[1] + t1[2] + t1[3] + t1[4] + t1[5] + t1[6] + t1[7]) +
          32*CHAR_OFFSET;
 */
int32 get_checksum1_ssse3_32(schar *buf, int32 len, int32 i, uint32 *ps1, uint32 *ps2)
{
	if (len > 32) {
		int aligned = ((uintptr_t)buf & 15) == 0;

		__m128i ss1 = _mm_cvtsi32_si128((int)*ps1);
		__m128i ss2 = _mm_cvtsi32_si128((int)*ps2);

		const __m128i mul_t1 = _mm_setr_epi16(28, 24, 20, 16, 12, 8, 4, 0);
		const __m128i mul_one = _mm_set1_epi8(1);
		const __m128i mul_const = _mm_set1_epi32(4 + (3 << 8) + (2 << 16) + (1 << 24));

		for (; i < (len-32); i+=32) {
			// Load ... 2*[int8*16]
			__m128i in8_1, in8_2;
			if (!aligned) {
				// Synonymous with _mm_loadu_si128 on all but a handful of old CPUs
				in8_1 = _mm_lddqu_si128((const __m128i *)&buf[i]);
				in8_2 = _mm_lddqu_si128((const __m128i *)&buf[i + 16]);
			} else {
				in8_1 = _mm_load_si128((const __m128i *)&buf[i]);
				in8_2 = _mm_load_si128((const __m128i *)&buf[i + 16]);
			}

			// (1*buf[i] + 1*buf[i+1]), (1*buf[i+2], 1*buf[i+3]), ... 2*[int16*8]
			// Fastest, even though multiply by 1
			__m128i add16_1 = _mm_maddubs_epi16(mul_one, in8_1);
			__m128i add16_2 = _mm_maddubs_epi16(mul_one, in8_2);

			// (4*buf[i] + 3*buf[i+1]), (2*buf[i+2], buf[i+3]), ... 2*[int16*8]
			__m128i mul_add16_1 = _mm_maddubs_epi16(mul_const, in8_1);
			__m128i mul_add16_2 = _mm_maddubs_epi16(mul_const, in8_2);

			// s2 += 32*s1
			ss2 = _mm_add_epi32(ss2, _mm_slli_epi32(ss1, 5));

			// [sum(t1[0]..t1[7]), X, X, X] [int32*4]; faster than multiple _mm_hadds_epi16
			// Shifting left, then shifting right again and shuffling (rather than just
			// shifting right as with mul32 below) to cheaply end up with the correct sign
			// extension as we go from int16 to int32.
			__m128i sum_add32 = _mm_add_epi16(add16_1, add16_2);
			sum_add32 = _mm_add_epi16(sum_add32, _mm_slli_si128(sum_add32, 2));
			sum_add32 = _mm_add_epi16(sum_add32, _mm_slli_si128(sum_add32, 4));
			sum_add32 = _mm_add_epi16(sum_add32, _mm_slli_si128(sum_add32, 8));
			sum_add32 = _mm_srai_epi32(sum_add32, 16);
			sum_add32 = _mm_shuffle_epi32(sum_add32, 3);

			// [sum(t2[0]..t2[7]), X, X, X] [int32*4]; faster than multiple _mm_hadds_epi16
			__m128i sum_mul_add32 = _mm_add_epi16(mul_add16_1, mul_add16_2);
			sum_mul_add32 = _mm_add_epi16(sum_mul_add32, _mm_slli_si128(sum_mul_add32, 2));
			sum_mul_add32 = _mm_add_epi16(sum_mul_add32, _mm_slli_si128(sum_mul_add32, 4));
			sum_mul_add32 = _mm_add_epi16(sum_mul_add32, _mm_slli_si128(sum_mul_add32, 8));
			sum_mul_add32 = _mm_srai_epi32(sum_mul_add32, 16);
			sum_mul_add32 = _mm_shuffle_epi32(sum_mul_add32, 3);

			// s1 += t1[0] + t1[1] + t1[2] + t1[3] + t1[4] + t1[5] + t1[6] + t1[7]
			ss1 = _mm_add_epi32(ss1, sum_add32);

			// s2 += t2[0] + t2[1] + t2[2] + t2[3] + t2[4] + t2[5] + t2[6] + t2[7]
			ss2 = _mm_add_epi32(ss2, sum_mul_add32);

			// [t1[0] + t1[1], t1[2] + t1[3] ...] [int16*8]
			// We could've combined this with generating sum_add32 above and
			// save an instruction but benchmarking shows that as being slower
			__m128i add16 = _mm_hadds_epi16(add16_1, add16_2);

			// [t1[0], t1[1], ...] -> [t1[0]*28 + t1[1]*24, ...] [int32*4]
			__m128i mul32 = _mm_madd_epi16(add16, mul_t1);

			// [sum(mul32), X, X, X] [int32*4]; faster than multiple _mm_hadd_epi32
			mul32 = _mm_add_epi32(mul32, _mm_srli_si128(mul32, 4));
			mul32 = _mm_add_epi32(mul32, _mm_srli_si128(mul32, 8));

			// s2 += 28*t1[0] + 24*t1[1] + 20*t1[2] + 16*t1[3] + 12*t1[4] + 8*t1[5] + 4*t1[6]
			ss2 = _mm_add_epi32(ss2, mul32);

#if CHAR_OFFSET != 0
			// s1 += 32*CHAR_OFFSET
			ss1 = _mm_add_epi32(ss1, _mm_set1_epi32(32 * CHAR_OFFSET));

			// s2 += 528*CHAR_OFFSET
			ss2 = _mm_add_epi32(ss2, _mm_set1_epi32(528 * CHAR_OFFSET));
#endif
		}

		*ps1 = (uint32)_mm_cvtsi128_si32(ss1);
		*ps2 = (uint32)_mm_cvtsi128_si32(ss2);
	}
	return i;
}

/*
  Same as SSSE3 version, but using macros defined above to emulate SSSE3 calls that are not available with SSE2.
 */
int32 get_checksum1_sse2_32(schar *buf, int32 len, int32 i, uint32 *ps1, uint32 *ps2)
{
	if (len > 32) {
		int aligned = ((uintptr_t)buf & 15) == 0;

		__m128i ss1 = _mm_cvtsi32_si128((int)*ps1);
		__m128i ss2 = _mm_cvtsi32_si128((int)*ps2);

		const __m128i mul_t1 = _mm_setr_epi16(28, 24, 20, 16, 12, 8, 4, 0);
		const __m128i mul_one = _mm_set1_epi8(1);
		const __m128i mul_const = _mm_set1_epi32(4 + (3 << 8) + (2 << 16) + (1 << 24));

		for (; i < (len-32); i+=32) {
			// Load ... 2*[int8*16]
			__m128i in8_1, in8_2;
			if (!aligned) {
				in8_1 = _mm_loadu_si128((const __m128i *)&buf[i]);
				in8_2 = _mm_loadu_si128((const __m128i *)&buf[i + 16]);
			} else {
				in8_1 = _mm_load_si128((const __m128i *)&buf[i]);
				in8_2 = _mm_load_si128((const __m128i *)&buf[i + 16]);
			}

			// (1*buf[i] + 1*buf[i+1]), (1*buf[i+2], 1*buf[i+3]), ... 2*[int16*8]
			// Fastest, even though multiply by 1
			__m128i add16_1 = SSE2_MADDUBS_EPI16(mul_one, in8_1);
			__m128i add16_2 = SSE2_MADDUBS_EPI16(mul_one, in8_2);

			// (4*buf[i] + 3*buf[i+1]), (2*buf[i+2], buf[i+3]), ... 2*[int16*8]
			__m128i mul_add16_1 = SSE2_MADDUBS_EPI16(mul_const, in8_1);
			__m128i mul_add16_2 = SSE2_MADDUBS_EPI16(mul_const, in8_2);

			// s2 += 32*s1
			ss2 = _mm_add_epi32(ss2, _mm_slli_epi32(ss1, 5));

			// [sum(t1[0]..t1[7]), X, X, X] [int32*4]; faster than multiple _mm_hadds_epi16
			// Shifting left, then shifting right again and shuffling (rather than just
			// shifting right as with mul32 below) to cheaply end up with the correct sign
			// extension as we go from int16 to int32.
			__m128i sum_add32 = _mm_add_epi16(add16_1, add16_2);
			sum_add32 = _mm_add_epi16(sum_add32, _mm_slli_si128(sum_add32, 2));
			sum_add32 = _mm_add_epi16(sum_add32, _mm_slli_si128(sum_add32, 4));
			sum_add32 = _mm_add_epi16(sum_add32, _mm_slli_si128(sum_add32, 8));
			sum_add32 = _mm_srai_epi32(sum_add32, 16);
			sum_add32 = _mm_shuffle_epi32(sum_add32, 3);

			// [sum(t2[0]..t2[7]), X, X, X] [int32*4]; faster than multiple _mm_hadds_epi16
			__m128i sum_mul_add32 = _mm_add_epi16(mul_add16_1, mul_add16_2);
			sum_mul_add32 = _mm_add_epi16(sum_mul_add32, _mm_slli_si128(sum_mul_add32, 2));
			sum_mul_add32 = _mm_add_epi16(sum_mul_add32, _mm_slli_si128(sum_mul_add32, 4));
			sum_mul_add32 = _mm_add_epi16(sum_mul_add32, _mm_slli_si128(sum_mul_add32, 8));
			sum_mul_add32 = _mm_srai_epi32(sum_mul_add32, 16);
			sum_mul_add32 = _mm_shuffle_epi32(sum_mul_add32, 3);

			// s1 += t1[0] + t1[1] + t1[2] + t1[3] + t1[4] + t1[5] + t1[6] + t1[7]
			ss1 = _mm_add_epi32(ss1, sum_add32);

			// s2 += t2[0] + t2[1] + t2[2] + t2[3] + t2[4] + t2[5] + t2[6] + t2[7]
			ss2 = _mm_add_epi32(ss2, sum_mul_add32);

			// [t1[0] + t1[1], t1[2] + t1[3] ...] [int16*8]
			// We could've combined this with generating sum_add32 above and
			// save an instruction but benchmarking shows that as being slower
			__m128i add16 = SSE2_HADDS_EPI16(add16_1, add16_2);

			// [t1[0], t1[1], ...] -> [t1[0]*28 + t1[1]*24, ...] [int32*4]
			__m128i mul32 = _mm_madd_epi16(add16, mul_t1);

			// [sum(mul32), X, X, X] [int32*4]; faster than multiple _mm_hadd_epi32
			mul32 = _mm_add_epi32(mul32, _mm_srli_si128(mul32, 4));
			mul32 = _mm_add_epi32(mul32, _mm_srli_si128(mul32, 8));

			// s2 += 28*t1[0] + 24*t1[1] + 20*t1[2] + 16*t1[3] + 12*t1[4] + 8*t1[5] + 4*t1[6]
			ss2 = _mm_add_epi32(ss2, mul32);

#if CHAR_OFFSET != 0
			// s1 += 32*CHAR_OFFSET
			ss1 = _mm_add_epi32(ss1, _mm_set1_epi32(32 * CHAR_OFFSET));

			// s2 += 528*CHAR_OFFSET
			ss2 = _mm_add_epi32(ss2, _mm_set1_epi32(528 * CHAR_OFFSET));
#endif
		}

		*ps1 = (uint32)_mm_cvtsi128_si32(ss1);
		*ps2 = (uint32)_mm_cvtsi128_si32(ss2);
	}
	return i;
}

int32 get_checksum1_default_1(schar *buf, int32 len, int32 i, uint32 *ps1, uint32 *ps2)
{
	uint32 s1 = *ps1;
	uint32 s2 = *ps2;
	for (; i < (len-4); i+=4) {
		s2 += 4*(s1 + buf[i]) + 3*buf[i+1] + 2*buf[i+2] + buf[i+3] + 10*CHAR_OFFSET;
		s1 += (buf[i+0] + buf[i+1] + buf[i+2] + buf[i+3] + 4*CHAR_OFFSET);
	}
	for (; i < len; i++) {
		s1 += (buf[i]+CHAR_OFFSET); s2 += s1;
	}
	*ps1 = s1;
	*ps2 = s2;
	return i;
}

uint32 get_checksum1(char *buf1, int32 len)
{
	schar *buf = (schar *)buf1;
	int level = win32_simd_level();
	int32 i = 0;
	uint32 s1 = 0;
	uint32 s2 = 0;

	// multiples of 64 bytes using AVX2 (if available)
	if (level >= WIN32_SIMD_AVX2)
		i = get_checksum1_avx2_64(buf, len, i, &s1, &s2);

	// multiples of 32 bytes using SSSE3, or SSE2 (if available)
	if (level >= WIN32_SIMD_SSSE3)
		i = get_checksum1_ssse3_32(buf, len, i, &s1, &s2);
	else if (level >= WIN32_SIMD_SSE2)
		i = get_checksum1_sse2_32(buf, len, i, &s1, &s2);

	// whatever is left (updates s1/s2; the returned offset is unused here)
	get_checksum1_default_1(buf, len, i, &s1, &s2);

	return (s1 & 0xffff) + (s2 << 16);
}

#endif /* USE_ROLL_SIMD */

#ifdef TEST_WIN32CHECKSUM /* { */
/*
 * The t_win32checksum helper.
 *
 * Every vector routine must produce the same (s1, s2) as the plain C loop
 * for any length and either alignment, and none may read past the end of
 * the buffer -- the last is checked by placing each buffer flush against a
 * PAGE_NOACCESS guard page, the Windows equivalent of the mprotect() test
 * in simd-checksum-x86_64.cpp, which is how a 64-byte over-read in the
 * AVX2 assembly was once found.  The weak checksum is compared across the
 * wire, so a wrong answer here would not crash anything: it would silently
 * turn every block into a mismatch and every delta into a full copy.
 *
 * With USE_XXH_DISPATCH, the AVX2 build of XXH3 must also produce the same
 * digests as the baseline one (checksum.c uses whichever win32xxh.c picks).
 *
 * Exit status 0 on success, 1 on any mismatch.  Prints the CPU level the
 * dispatcher chose and a throughput figure per routine.
 */

#include <windows.h>
#include "win32cpu.h"
#ifdef USE_XXH_DISPATCH
#define WIN32XXH_NO_REPLACE
#include "win32xxh.h"
#endif

#ifdef USE_ROLL_SIMD

typedef int32 (*part_fn)(schar *buf, int32 len, int32 i, uint32 *ps1, uint32 *ps2);

static uint32 via(part_fn fn, char *buf, int32 len)
{
	uint32 s1 = 0, s2 = 0;
	int32 i = fn ? fn((schar *)buf, len, 0, &s1, &s2) : 0;
	get_checksum1_default_1((schar *)buf, len, i, &s1, &s2);
	return (s1 & 0xffff) + (s2 << 16);
}

static const struct { const char *name; part_fn fn; int level; } routines[] = {
	{ "SSE2",  get_checksum1_sse2_32,  WIN32_SIMD_SSE2 },
	{ "SSSE3", get_checksum1_ssse3_32, WIN32_SIMD_SSSE3 },
	{ "AVX2",  get_checksum1_avx2_64,  WIN32_SIMD_AVX2 },
};
#define NUM_ROUTINES (int)(sizeof routines / sizeof routines[0])

#endif /* USE_ROLL_SIMD */

static void fill(char *buf, int32 len)
{
	int32 i;
	for (i = 0; i < len; i++)
		buf[i] = (char)((i + (i % 3) + (i % 11)) % 256);
}

static double seconds_since(const LARGE_INTEGER *t0)
{
	LARGE_INTEGER freq, t1;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t1);
	return (double)(t1.QuadPart - t0->QuadPart) / (double)freq.QuadPart;
}

int main(UNUSED(int argc), UNUSED(char *argv[]))
{
	int level = win32_simd_level();
	int failures = 0;
	const int32 blen = 1024 * 1024;
	const int rounds = 256;
	char *bbuf, *bb;

	printf("cpu level %d (%s)\n", level,
	       level >= WIN32_SIMD_AVX2 ? "AVX2" : level == WIN32_SIMD_SSSE3 ? "SSSE3"
	       : level == WIN32_SIMD_SSE2 ? "SSE2" : "scalar");

	bbuf = malloc(blen + 64);
	if (!bbuf) {
		fprintf(stderr, "malloc failed\n");
		return 1;
	}
	bb = bbuf + (64 - ((uintptr_t)bbuf % 64));
	fill(bb, blen);

#ifdef USE_ROLL_SIMD
	{
		static const int32 sizes[] = { 1, 4, 31, 32, 33, 63, 64, 65, 127, 128, 129, 256, 700, 1024, 4096, 65536, 131072 };
		int num_sizes = sizeof sizes / sizeof sizes[0];
		int r, s, b;
		SYSTEM_INFO si;
		char *region;
		size_t pagesz;

		/* Routines above the CPU's level cannot be run here; say so rather
		 * than let a pass on an old CPU read as coverage of the AVX2 path. */
		for (r = 0; r < NUM_ROUTINES; r++)
			if (routines[r].level > level)
				printf("%s: NOT exercised, this CPU lacks it\n", routines[r].name);

		for (b = 0; b < 2; b++) {
			char *buf = bb + b;   /* 64-byte aligned, then one byte off */
			for (s = 0; s < num_sizes; s++) {
				int32 len = sizes[s];
				uint32 ref = via(NULL, buf, len);
				for (r = 0; r < NUM_ROUTINES; r++) {
					uint32 got;
					if (routines[r].level > level)
						continue;
					got = via(routines[r].fn, buf, len);
					if (got != ref) {
						printf("FAIL %-9s size=%6d: %s=%08x ref=%08x\n",
						       b ? "unaligned" : "aligned", len, routines[r].name, got, ref);
						failures++;
					}
				}
				if (get_checksum1(buf, len) != ref) {
					printf("FAIL %-9s size=%6d: get_checksum1=%08x ref=%08x\n",
					       b ? "unaligned" : "aligned", len, get_checksum1(buf, len), ref);
					failures++;
				}
			}
		}

		/* Guard page: 4 pages, the last one unreadable.  Buffers end flush
		 * against it, stepping the length by one so every remainder mod 64
		 * and both alignments are covered.  A read past the end faults
		 * right here. */
		GetSystemInfo(&si);
		pagesz = si.dwPageSize;
		region = VirtualAlloc(NULL, pagesz * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (!region) {
			printf("FAIL guard-page: VirtualAlloc failed\n");
			failures++;
		} else {
			DWORD old;
			if (!VirtualProtect(region + pagesz * 3, pagesz, PAGE_NOACCESS, &old)) {
				printf("FAIL guard-page: VirtualProtect failed\n");
				failures++;
			} else {
				int32 len;
				int guard_failures = 0;
				for (len = 1; len <= 4096; len++) {
					char *buf = region + pagesz * 3 - len;   /* last byte abuts the guard */
					uint32 ref;
					fill(buf, len);
					ref = via(NULL, buf, len);
					for (r = 0; r < NUM_ROUTINES; r++) {
						if (routines[r].level > level)
							continue;
						if (via(routines[r].fn, buf, len) != ref) {
							printf("FAIL guard-page size=%5d: %s mismatch\n", len, routines[r].name);
							guard_failures++;
						}
					}
					if (get_checksum1(buf, len) != ref) {
						printf("FAIL guard-page size=%5d: get_checksum1 mismatch\n", len);
						guard_failures++;
					}
					if (guard_failures > 4)
						break;
				}
				failures += guard_failures;
				if (!guard_failures)
					printf("guard-page: no over-read, lengths 1..4096, both alignments\n");
			}
			VirtualFree(region, 0, MEM_RELEASE);
		}

		/* Throughput, so a build log shows what the dispatch is worth. */
		for (r = -1; r < NUM_ROUTINES; r++) {
			const char *name = r < 0 ? "scalar" : routines[r].name;
			part_fn fn = r < 0 ? NULL : routines[r].fn;
			volatile uint32 sink = 0;
			LARGE_INTEGER t0;
			double secs;
			int i;
			if (r >= 0 && routines[r].level > level)
				continue;
			QueryPerformanceCounter(&t0);
			for (i = 0; i < rounds; i++)
				sink += via(fn, bb, blen);
			secs = seconds_since(&t0);
			printf("get_checksum1 %-6s: %6.0f MB/s\n", name, secs > 0 ? rounds / secs : 0.0);
		}
	}
#else
	printf("built without USE_ROLL_SIMD: get_checksum1 is the scalar one\n");
#endif /* USE_ROLL_SIMD */

#ifdef USE_XXH_DISPATCH
	if (level >= WIN32_SIMD_AVX2) {
		XXH128_hash_t plain = XXH3_128bits_withSeed(bb, blen, 42);
		XXH128_hash_t avx2 = rsync_avx2_XXH3_128bits_withSeed(bb, blen, 42);
		int which;
		if (plain.low64 != avx2.low64 || plain.high64 != avx2.high64) {
			printf("FAIL xxh128: AVX2 digest differs from the baseline one\n");
			failures++;
		}
		if (XXH3_64bits_withSeed(bb, blen, 42) != rsync_avx2_XXH3_64bits_withSeed(bb, blen, 42)) {
			printf("FAIL xxh3: AVX2 digest differs from the baseline one\n");
			failures++;
		}
		for (which = 0; which < 2; which++) {
			volatile XXH64_hash_t sink = 0;
			LARGE_INTEGER t0;
			double secs;
			int i;
			QueryPerformanceCounter(&t0);
			for (i = 0; i < rounds; i++)
				sink += which ? rsync_avx2_XXH3_128bits_withSeed(bb, blen, (XXH64_hash_t)i).low64
					      : XXH3_128bits_withSeed(bb, blen, (XXH64_hash_t)i).low64;
			secs = seconds_since(&t0);
			printf("xxh128 %-8s: %6.0f MB/s\n", which ? "AVX2" : "baseline",
			       secs > 0 ? rounds / secs : 0.0);
		}
	} else
		printf("xxh128 AVX2 build: NOT exercised, this CPU lacks AVX2\n");
#else
	printf("built without USE_XXH_DISPATCH: XXH3 is the baseline build\n");
#endif /* USE_XXH_DISPATCH */

	free(bbuf);
	if (failures) {
		printf("%d checksum mismatches!\n", failures);
		return 1;
	}
	printf("All win32 checksum tests passed.\n");
	return 0;
}

#endif /* } TEST_WIN32CHECKSUM */
