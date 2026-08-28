/*
 * XXH3 with an AVX2 build chosen at runtime.
 *
 * xxhash.c compiled for the baseline ISA gives XXH3 its SSE2 kernels on
 * every x86 CPU.  xxHash ships a dispatcher add-on (xxh_x86dispatch.c) for
 * this, but on MSVC it is 2.5x SLOWER than the plain build: MSVC has no
 * per-function target attribute, so the add-on's AVX2 kernels are compiled
 * without /arch:AVX2 and the compiler mixes VEX-encoded AVX2 with legacy
 * SSE in the same functions, and the AVX/SSE transition penalty on Intel
 * CPUs costs more than the wider vectors gain (measured on an i5-8350U:
 * 9.5 GB/s plain, 3.7 GB/s dispatched).
 *
 * So instead CMake compiles xxhash.c a second time, whole, with /arch:AVX2,
 * XXH_VECTOR=XXH_AVX2 and XXH_NAMESPACE=rsync_avx2_, which prefixes every
 * symbol of that copy.  These wrappers pick that copy or the baseline one
 * from win32_simd_level(), once; XXH3_state_t has the same layout in both
 * (it is fixed by xxhash.h, not by the vector width), and since every call
 * of one hash goes the same way a state never crosses over.  Nothing in the
 * AVX2 copy is called on a CPU without AVX2, or on an OS that does not save
 * the YMM registers.
 *
 * Copyright (C) 2026 Max Vilimpoc, rsync CMake/Windows port.
 * Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
 */

#include "rsync.h"

#ifdef USE_XXH_DISPATCH

#define WIN32XXH_NO_REPLACE
#include "win32xxh.h"
#include "win32cpu.h"

/* The AVX2 copy's other entry points (the two one-shots are in the header). */
XXH3_state_t *rsync_avx2_XXH3_createState(void);
XXH_errorcode rsync_avx2_XXH3_64bits_reset(XXH3_state_t *state);
XXH_errorcode rsync_avx2_XXH3_64bits_update(XXH3_state_t *state, const void *input, size_t len);
XXH64_hash_t rsync_avx2_XXH3_64bits_digest(const XXH3_state_t *state);
XXH_errorcode rsync_avx2_XXH3_128bits_reset(XXH3_state_t *state);
XXH_errorcode rsync_avx2_XXH3_128bits_update(XXH3_state_t *state, const void *input, size_t len);
XXH128_hash_t rsync_avx2_XXH3_128bits_digest(const XXH3_state_t *state);

static int use_avx2(void)
{
	static int decided = -1;
	if (decided < 0)
		decided = win32_simd_level() >= WIN32_SIMD_AVX2;
	return decided;
}

XXH3_state_t *win32_xxh3_createState(void)
{
	return use_avx2() ? rsync_avx2_XXH3_createState() : XXH3_createState();
}

XXH_errorcode win32_xxh3_64bits_reset(XXH3_state_t *state)
{
	return use_avx2() ? rsync_avx2_XXH3_64bits_reset(state) : XXH3_64bits_reset(state);
}

XXH_errorcode win32_xxh3_64bits_update(XXH3_state_t *state, const void *input, size_t len)
{
	return use_avx2() ? rsync_avx2_XXH3_64bits_update(state, input, len)
			  : XXH3_64bits_update(state, input, len);
}

XXH64_hash_t win32_xxh3_64bits_digest(const XXH3_state_t *state)
{
	return use_avx2() ? rsync_avx2_XXH3_64bits_digest(state) : XXH3_64bits_digest(state);
}

XXH64_hash_t win32_xxh3_64bits_withSeed(const void *input, size_t len, XXH64_hash_t seed)
{
	return use_avx2() ? rsync_avx2_XXH3_64bits_withSeed(input, len, seed)
			  : XXH3_64bits_withSeed(input, len, seed);
}

XXH_errorcode win32_xxh3_128bits_reset(XXH3_state_t *state)
{
	return use_avx2() ? rsync_avx2_XXH3_128bits_reset(state) : XXH3_128bits_reset(state);
}

XXH_errorcode win32_xxh3_128bits_update(XXH3_state_t *state, const void *input, size_t len)
{
	return use_avx2() ? rsync_avx2_XXH3_128bits_update(state, input, len)
			  : XXH3_128bits_update(state, input, len);
}

XXH128_hash_t win32_xxh3_128bits_digest(const XXH3_state_t *state)
{
	return use_avx2() ? rsync_avx2_XXH3_128bits_digest(state) : XXH3_128bits_digest(state);
}

XXH128_hash_t win32_xxh3_128bits_withSeed(const void *input, size_t len, XXH64_hash_t seed)
{
	return use_avx2() ? rsync_avx2_XXH3_128bits_withSeed(input, len, seed)
			  : XXH3_128bits_withSeed(input, len, seed);
}

#endif /* USE_XXH_DISPATCH */
