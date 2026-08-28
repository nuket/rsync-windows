/*
 * The AVX2 half of the MSVC get_checksum1() port.  Compiled with /arch:AVX2
 * (see CMakeLists.txt) so the whole function is VEX-encoded and pays no
 * AVX/SSE transition penalty; win32checksum.c calls it only after
 * CPUID and XGETBV say AVX2 is usable, so nothing here ever runs on a CPU
 * that lacks it.  The arithmetic is simd-checksum-x86_64.cpp's 64-byte
 * loop unchanged.
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

#include "rsync.h"

#ifdef USE_ROLL_SIMD

#include <intrin.h>
#include "win32checksum.h"

/*
  AVX2 loop per 64 bytes:
    int16 t1[16];
    int16 t2[16];
    for (int j = 0; j < 16; j++) {
      t1[j] = buf[j*4 + i] + buf[j*4 + i+1] + buf[j*4 + i+2] + buf[j*4 + i+3];
      t2[j] = 4*buf[j*4 + i] + 3*buf[j*4 + i+1] + 2*buf[j*4 + i+2] + buf[j*4 + i+3];
    }
    s2 += 64*s1 + (uint32)(
              60*t1[0] + 56*t1[1] + 52*t1[2] + 48*t1[3] + 44*t1[4] + 40*t1[5] + 36*t1[6] + 32*t1[7] + 28*t1[8] + 24*t1[9] + 20*t1[10] + 16*t1[11] + 12*t1[12] + 8*t1[13] + 4*t1[14] +
              t2[0] + t2[1] + t2[2] + t2[3] + t2[4] + t2[5] + t2[6] + t2[7] + t2[8] + t2[9] + t2[10] + t2[11] + t2[12] + t2[13] + t2[14] + t2[15]
          ) + 2080*CHAR_OFFSET;
    s1 += (uint32)(t1[0] + t1[1] + t1[2] + t1[3] + t1[4] + t1[5] + t1[6] + t1[7] + t1[8] + t1[9] + t1[10] + t1[11] + t1[12] + t1[13] + t1[14] + t1[15]) +
          64*CHAR_OFFSET;
 */
int32 get_checksum1_avx2_64(schar *buf, int32 len, int32 i, uint32 *ps1, uint32 *ps2)
{
	if (len > 64) {
		__m128i ss1 = _mm_cvtsi32_si128((int)*ps1);
		__m128i ss2 = _mm_cvtsi32_si128((int)*ps2);

		const __m256i mul_t1 = _mm256_cvtepu8_epi16(
			_mm_setr_epi8(60, 56, 52, 48, 44, 40, 36, 32, 28, 24, 20, 16, 12, 8, 4, 0));
		const __m256i mul_const = _mm256_set1_epi32(4 | (3 << 8) | (2 << 16) | (1 << 24));
		const __m256i mul_one = _mm256_set1_epi8(1);

		for (; i < (len-64); i+=64) {
			// Load ... 4*[int8*16]
			__m128i in8_1_low = _mm_loadu_si128((const __m128i *)&buf[i]);
			__m128i in8_2_low = _mm_loadu_si128((const __m128i *)&buf[i+16]);
			__m128i in8_1_high = _mm_loadu_si128((const __m128i *)&buf[i+32]);
			__m128i in8_2_high = _mm_loadu_si128((const __m128i *)&buf[i+48]);
			__m256i in8_1 = _mm256_inserti128_si256(_mm256_castsi128_si256(in8_1_low), in8_1_high, 1);
			__m256i in8_2 = _mm256_inserti128_si256(_mm256_castsi128_si256(in8_2_low), in8_2_high, 1);

			// (1*buf[i] + 1*buf[i+1]), (1*buf[i+2], 1*buf[i+3]), ... 2*[int16*8]
			// Fastest, even though multiply by 1
			__m256i add16_1 = _mm256_maddubs_epi16(mul_one, in8_1);
			__m256i add16_2 = _mm256_maddubs_epi16(mul_one, in8_2);

			// (4*buf[i] + 3*buf[i+1]), (2*buf[i+2], buf[i+3]), ... 2*[int16*8]
			__m256i mul_add16_1 = _mm256_maddubs_epi16(mul_const, in8_1);
			__m256i mul_add16_2 = _mm256_maddubs_epi16(mul_const, in8_2);

			// s2 += 64*s1
			ss2 = _mm_add_epi32(ss2, _mm_slli_epi32(ss1, 6));

			// [sum(t1[0]..t1[7]), X, X, X] [int32*4]; faster than multiple _mm_hadds_epi16
			__m256i sum_add32 = _mm256_add_epi16(add16_1, add16_2);
			sum_add32 = _mm256_add_epi16(sum_add32, _mm256_srli_epi32(sum_add32, 16));
			sum_add32 = _mm256_add_epi16(sum_add32, _mm256_srli_si256(sum_add32, 4));
			sum_add32 = _mm256_add_epi16(sum_add32, _mm256_srli_si256(sum_add32, 8));

			// [sum(t2[0]..t2[7]), X, X, X] [int32*4]; faster than multiple _mm_hadds_epi16
			__m256i sum_mul_add32 = _mm256_add_epi16(mul_add16_1, mul_add16_2);
			sum_mul_add32 = _mm256_add_epi16(sum_mul_add32, _mm256_srli_epi32(sum_mul_add32, 16));
			sum_mul_add32 = _mm256_add_epi16(sum_mul_add32, _mm256_srli_si256(sum_mul_add32, 4));
			sum_mul_add32 = _mm256_add_epi16(sum_mul_add32, _mm256_srli_si256(sum_mul_add32, 8));

			// s1 += t1[0] + t1[1] + t1[2] + t1[3] + t1[4] + t1[5] + t1[6] + t1[7]
			__m128i sum_add32_hi = _mm256_extracti128_si256(sum_add32, 0x1);
			ss1 = _mm_add_epi32(ss1, _mm256_castsi256_si128(sum_add32));
			ss1 = _mm_add_epi32(ss1, sum_add32_hi);

			// s2 += t2[0] + t2[1] + t2[2] + t2[3] + t2[4] + t2[5] + t2[6] + t2[7]
			__m128i sum_mul_add32_hi = _mm256_extracti128_si256(sum_mul_add32, 0x1);
			ss2 = _mm_add_epi32(ss2, _mm256_castsi256_si128(sum_mul_add32));
			ss2 = _mm_add_epi32(ss2, sum_mul_add32_hi);

			// [t1[0] + t1[1], t1[2] + t1[3] ...] [int16*8]
			// We could've combined this with generating sum_add32 above and
			// save an instruction but benchmarking shows that as being slower
			__m256i add16 = _mm256_hadds_epi16(add16_1, add16_2);

			// [t1[0], t1[1], ...] -> [t1[0]*28 + t1[1]*24, ...] [int32*4]
			__m256i mul32 = _mm256_madd_epi16(add16, mul_t1);

			// [sum(mul32), X, X, X] [int32*4]; faster than multiple _mm_hadd_epi32
			mul32 = _mm256_add_epi32(mul32, _mm256_srli_si256(mul32, 4));
			mul32 = _mm256_add_epi32(mul32, _mm256_srli_si256(mul32, 8));
			// prefetch 2 cacheline ahead
			_mm_prefetch((const char *)&buf[i + 160], _MM_HINT_T0);

			// s2 += 28*t1[0] + 24*t1[1] + 20*t1[2] + 16*t1[3] + 12*t1[4] + 8*t1[5] + 4*t1[6]
			__m128i mul32_hi = _mm256_extracti128_si256(mul32, 0x1);
			ss2 = _mm_add_epi32(ss2, _mm256_castsi256_si128(mul32));
			ss2 = _mm_add_epi32(ss2, mul32_hi);

#if CHAR_OFFSET != 0
			// s1 += 64*CHAR_OFFSET
			ss1 = _mm_add_epi32(ss1, _mm_set1_epi32(64 * CHAR_OFFSET));

			// s2 += 2080*CHAR_OFFSET
			ss2 = _mm_add_epi32(ss2, _mm_set1_epi32(2080 * CHAR_OFFSET));
#endif
		}

		*ps1 = (uint32)_mm_cvtsi128_si32(ss1);
		*ps2 = (uint32)_mm_cvtsi128_si32(ss2);
	}
	return i;
}

#endif /* USE_ROLL_SIMD */
