/*
 * XXH3 with an AVX2 build chosen at runtime -- see win32xxh.c.
 *
 * checksum.c includes this after <xxhash.h> when USE_XXH_DISPATCH is set;
 * the macros send its XXH3 calls through the wrappers, which pick the AVX2
 * or the baseline build once from CPUID.  Every call for one hash goes to
 * the same build, so a state created by one is never fed to the other.
 *
 * Copyright (C) 2026 Max Vilimpoc, rsync CMake/Windows port.
 * Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
 */
#ifndef WIN32XXH_H
#define WIN32XXH_H

#include <xxhash.h>

XXH3_state_t *win32_xxh3_createState(void);
XXH_errorcode win32_xxh3_64bits_reset(XXH3_state_t *state);
XXH_errorcode win32_xxh3_64bits_update(XXH3_state_t *state, const void *input, size_t len);
XXH64_hash_t win32_xxh3_64bits_digest(const XXH3_state_t *state);
XXH64_hash_t win32_xxh3_64bits_withSeed(const void *input, size_t len, XXH64_hash_t seed);
XXH_errorcode win32_xxh3_128bits_reset(XXH3_state_t *state);
XXH_errorcode win32_xxh3_128bits_update(XXH3_state_t *state, const void *input, size_t len);
XXH128_hash_t win32_xxh3_128bits_digest(const XXH3_state_t *state);
XXH128_hash_t win32_xxh3_128bits_withSeed(const void *input, size_t len, XXH64_hash_t seed);

/* The AVX2 build's own entry points, for the test helper to compare
 * against the baseline ones.  XXH_NAMESPACE gives them this prefix. */
XXH64_hash_t rsync_avx2_XXH3_64bits_withSeed(const void *input, size_t len, XXH64_hash_t seed);
XXH128_hash_t rsync_avx2_XXH3_128bits_withSeed(const void *input, size_t len, XXH64_hash_t seed);

#ifndef WIN32XXH_NO_REPLACE
#define XXH3_createState	win32_xxh3_createState
#define XXH3_64bits_reset	win32_xxh3_64bits_reset
#define XXH3_64bits_update	win32_xxh3_64bits_update
#define XXH3_64bits_digest	win32_xxh3_64bits_digest
#define XXH3_64bits_withSeed	win32_xxh3_64bits_withSeed
#define XXH3_128bits_reset	win32_xxh3_128bits_reset
#define XXH3_128bits_update	win32_xxh3_128bits_update
#define XXH3_128bits_digest	win32_xxh3_128bits_digest
#define XXH3_128bits_withSeed	win32_xxh3_128bits_withSeed
#endif

#endif
