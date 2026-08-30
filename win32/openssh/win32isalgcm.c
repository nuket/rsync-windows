/*
 * win32isalgcm.c -- AES-GCM for the SSH transport through Intel's ISA-L
 * assembly.
 *
 * Part of the rsync for Windows port (github.com/nuket/rsync-windows).
 * Compiled into Win32-OpenSSH's libssh beside win32cnggcm.c, which picks
 * between the two; cipher.c knows about neither.
 *
 * Copyright (c) 2026 Max Vilimpoc.  ISC, as cipher.c.  The assembly it
 * calls is Intel's, BSD-3-Clause, vendored unmodified in isal/ -- see
 * isal/README.md for what was taken and what was left behind.
 *
 * Why
 * ---
 * The cipher is what is left of the client's single thread once the pipes,
 * the copies and the syscalls are gone: about 45% of a core per GB/s.  CNG
 * runs AES-128-GCM at ~2.2GB/s in SSH's 32KB packets on an i5-8350U, ISA-L
 * at rather more, and on a 15W laptop the difference shows up twice --
 * once as throughput, and again as heat that would otherwise have cost
 * clock speed later in a long transfer.
 *
 * What
 * ----
 * One call per packet into gcm_avx_gen4.asm, which is AES-GCM stitched for
 * AVX2 with AES-NI and PCLMULQDQ.  The four struct fields below have to
 * match the assembly's idea of them exactly (isal/gcm_defines.asm), so
 * they are copied from ISA-L's header verbatim rather than paraphrased,
 * and a known-answer test runs once before any of it is used: if the key
 * schedule this file builds does not agree with NIST's vectors, the whole
 * backend reports itself unavailable and the caller stays on CNG.
 *
 * The AES key schedule is here rather than taken from ISA-L because taking
 * it would have meant three more assembly files and a C file for thirty
 * lines of AES-NI that FIPS-197 specifies exactly.
 */

#include "includes.h"

#include <windows.h>
#include <intrin.h>
#include <wmmintrin.h>
#include <string.h>
#include <stdlib.h>

#include "ssherr.h"
#include "win32isalgcm.h"

#ifdef RSYNC_ISAL_GCM

/* ---- the layout gcm_avx_gen4.asm expects (isa-l_crypto aes_gcm.h) ----- */

#define ISAL_GCM_BLOCK_LEN   16
#define ISAL_GCM_ENC_KEY_LEN 16
#define ISAL_GCM_KEY_SETS    15   /* the key plus 14 round keys */

/* expanded_keys first, then the shifted hash keys the assembly precomputes:
 * 8 powers, 8 Karatsuba halves, and room for the vaes variants' 2x32. */
struct isal_key_data {
	uint8_t expanded_keys[ISAL_GCM_ENC_KEY_LEN * ISAL_GCM_KEY_SETS];
	uint8_t shifted_hkeys[ISAL_GCM_ENC_KEY_LEN * (8 + 8 + (64 - 16))];
};

struct isal_context_data {
	uint8_t  aad_hash[ISAL_GCM_BLOCK_LEN];
	uint64_t aad_length;
	uint64_t in_length;
	uint8_t  partial_block_enc_key[ISAL_GCM_BLOCK_LEN];
	uint8_t  orig_IV[ISAL_GCM_BLOCK_LEN];
	uint8_t  current_counter[ISAL_GCM_BLOCK_LEN];
	uint64_t partial_block_length;
};

/* ISA-L exports these with a leading underscore on every platform. */
void _aes_gcm_precomp_128_avx_gen4(struct isal_key_data *);
void _aes_gcm_precomp_256_avx_gen4(struct isal_key_data *);
void _aes_gcm_enc_128_avx_gen4(const struct isal_key_data *,
    struct isal_context_data *, uint8_t *out, const uint8_t *in, uint64_t len,
    uint8_t *iv, const uint8_t *aad, uint64_t aad_len, uint8_t *tag,
    uint64_t tag_len);
void _aes_gcm_dec_128_avx_gen4(const struct isal_key_data *,
    struct isal_context_data *, uint8_t *out, const uint8_t *in, uint64_t len,
    uint8_t *iv, const uint8_t *aad, uint64_t aad_len, uint8_t *tag,
    uint64_t tag_len);
void _aes_gcm_enc_256_avx_gen4(const struct isal_key_data *,
    struct isal_context_data *, uint8_t *out, const uint8_t *in, uint64_t len,
    uint8_t *iv, const uint8_t *aad, uint64_t aad_len, uint8_t *tag,
    uint64_t tag_len);
void _aes_gcm_dec_256_avx_gen4(const struct isal_key_data *,
    struct isal_context_data *, uint8_t *out, const uint8_t *in, uint64_t len,
    uint8_t *iv, const uint8_t *aad, uint64_t aad_len, uint8_t *tag,
    uint64_t tag_len);

struct isal_gcm {
	__declspec(align(64)) struct isal_key_data key;
	u_int keylen;
};

/* ---- the AES key schedule (FIPS-197, the AES-NI way) ----------------- */

static __m128i
key_128_assist(__m128i k, __m128i gen)
{
	__m128i t;

	gen = _mm_shuffle_epi32(gen, 0xff);
	t = _mm_slli_si128(k, 4);
	k = _mm_xor_si128(k, t);
	t = _mm_slli_si128(t, 4);
	k = _mm_xor_si128(k, t);
	t = _mm_slli_si128(t, 4);
	k = _mm_xor_si128(k, t);
	return _mm_xor_si128(k, gen);
}

static void
aes128_key_expand(const u_char *key, __m128i *rk)
{
	__m128i k = _mm_loadu_si128((const __m128i *)key);

	rk[0] = k;
	k = key_128_assist(k, _mm_aeskeygenassist_si128(k, 0x01)); rk[1] = k;
	k = key_128_assist(k, _mm_aeskeygenassist_si128(k, 0x02)); rk[2] = k;
	k = key_128_assist(k, _mm_aeskeygenassist_si128(k, 0x04)); rk[3] = k;
	k = key_128_assist(k, _mm_aeskeygenassist_si128(k, 0x08)); rk[4] = k;
	k = key_128_assist(k, _mm_aeskeygenassist_si128(k, 0x10)); rk[5] = k;
	k = key_128_assist(k, _mm_aeskeygenassist_si128(k, 0x20)); rk[6] = k;
	k = key_128_assist(k, _mm_aeskeygenassist_si128(k, 0x40)); rk[7] = k;
	k = key_128_assist(k, _mm_aeskeygenassist_si128(k, 0x80)); rk[8] = k;
	k = key_128_assist(k, _mm_aeskeygenassist_si128(k, 0x1b)); rk[9] = k;
	k = key_128_assist(k, _mm_aeskeygenassist_si128(k, 0x36)); rk[10] = k;
}

/* the even and odd halves of the 256-bit schedule */
static __m128i
key_256_assist_1(__m128i a, __m128i gen)
{
	__m128i t;

	gen = _mm_shuffle_epi32(gen, 0xff);
	t = _mm_slli_si128(a, 4);
	a = _mm_xor_si128(a, t);
	t = _mm_slli_si128(t, 4);
	a = _mm_xor_si128(a, t);
	t = _mm_slli_si128(t, 4);
	a = _mm_xor_si128(a, t);
	return _mm_xor_si128(a, gen);
}

static __m128i
key_256_assist_2(__m128i a, __m128i b)
{
	__m128i gen = _mm_shuffle_epi32(_mm_aeskeygenassist_si128(a, 0x00), 0xaa);
	__m128i t = _mm_slli_si128(b, 4);

	b = _mm_xor_si128(b, t);
	t = _mm_slli_si128(t, 4);
	b = _mm_xor_si128(b, t);
	t = _mm_slli_si128(t, 4);
	b = _mm_xor_si128(b, t);
	return _mm_xor_si128(b, gen);
}

/*
 * Unrolled because the round constant is an immediate operand:
 * _mm_aeskeygenassist_si128() will not take a variable.  Fourteen rounds,
 * so fifteen round keys, the last of them an "even" one with no odd half.
 */
static void
aes256_key_expand(const u_char *key, __m128i *rk)
{
	__m128i a = _mm_loadu_si128((const __m128i *)key);
	__m128i b = _mm_loadu_si128((const __m128i *)(key + 16));

	rk[0] = a;
	rk[1] = b;
#define STEP(rcon, even, odd)						\
	a = key_256_assist_1(a, _mm_aeskeygenassist_si128(b, rcon));	\
	rk[even] = a;							\
	b = key_256_assist_2(a, b);					\
	rk[odd] = b;
	STEP(0x01,  2,  3)
	STEP(0x02,  4,  5)
	STEP(0x04,  6,  7)
	STEP(0x08,  8,  9)
	STEP(0x10, 10, 11)
	STEP(0x20, 12, 13)
#undef STEP
	a = key_256_assist_1(a, _mm_aeskeygenassist_si128(b, 0x40));
	rk[14] = a;
}

/* ---- is any of this usable here? ------------------------------------- */

static int
cpu_has_what_the_assembly_needs(void)
{
	int r[4];

	__cpuid(r, 0);
	if (r[0] < 7)
		return 0;
	__cpuidex(r, 1, 0);
	if (!(r[2] & (1 << 25)))        /* AES-NI */
		return 0;
	if (!(r[2] & (1 << 1)))         /* PCLMULQDQ */
		return 0;
	if (!(r[2] & (1 << 27)))        /* OSXSAVE: the OS saves YMM state */
		return 0;
	if ((_xgetbv(0) & 0x6) != 0x6)  /* ... and really does */
		return 0;
	__cpuidex(r, 7, 0);
	if (!(r[1] & (1 << 5)))         /* AVX2 */
		return 0;
	return 1;
}

/*
 * NIST's GCM test cases 2 and 14: an all-zero key, IV and block, no
 * additional data.  They exercise the key schedule this file builds, the
 * precompute in the assembly, and one encryption -- which is everything
 * that could be wrong about the way the two are glued together.
 */
static int
known_answers_agree(void)
{
	static const u_char zero[32];
	static const u_char c128[16] = {
		0x03, 0x88, 0xda, 0xce, 0x60, 0xb6, 0xa3, 0x92,
		0xf3, 0x28, 0xc2, 0xb9, 0x71, 0xb2, 0xfe, 0x78
	};
	static const u_char t128[16] = {
		0xab, 0x6e, 0x47, 0xd4, 0x2c, 0xec, 0x13, 0xbd,
		0xf5, 0x3a, 0x67, 0xb2, 0x12, 0x57, 0xbd, 0xdf
	};
	static const u_char c256[16] = {
		0xce, 0xa7, 0x40, 0x3d, 0x4d, 0x60, 0x6b, 0x6e,
		0x07, 0x4e, 0xc5, 0xd3, 0xba, 0xf3, 0x9d, 0x18
	};
	static const u_char t256[16] = {
		0xd0, 0xd1, 0xc8, 0xa7, 0x99, 0x99, 0x6b, 0xf0,
		0x26, 0x5b, 0x98, 0xb5, 0xd4, 0x8a, 0xb9, 0x19
	};
	__declspec(align(64)) struct isal_key_data key;
	__declspec(align(64)) struct isal_context_data ctx;
	u_char out[16], tag[16], iv[12];
	int ok;

	memset(iv, 0, sizeof(iv));

	memset(&key, 0, sizeof(key));
	aes128_key_expand(zero, (__m128i *)key.expanded_keys);
	_aes_gcm_precomp_128_avx_gen4(&key);
	_aes_gcm_enc_128_avx_gen4(&key, &ctx, out, zero, 16, iv, NULL, 0,
	    tag, sizeof(tag));
	ok = memcmp(out, c128, 16) == 0 && memcmp(tag, t128, 16) == 0;

	memset(&key, 0, sizeof(key));
	aes256_key_expand(zero, (__m128i *)key.expanded_keys);
	_aes_gcm_precomp_256_avx_gen4(&key);
	_aes_gcm_enc_256_avx_gen4(&key, &ctx, out, zero, 16, iv, NULL, 0,
	    tag, sizeof(tag));
	ok = ok && memcmp(out, c256, 16) == 0 && memcmp(tag, t256, 16) == 0;

	explicit_bzero(&key, sizeof(key));
	explicit_bzero(&ctx, sizeof(ctx));
	return ok;
}

int
isal_gcm_available(void)
{
	static int usable = -1;

	if (usable < 0)
		usable = cpu_has_what_the_assembly_needs() && known_answers_agree();
	return usable;
}

/* ---- the backend ----------------------------------------------------- */

struct isal_gcm *
isal_gcm_new(const u_char *key, u_int keylen)
{
	struct isal_gcm *g;

	if ((keylen != 16 && keylen != 32) || !isal_gcm_available())
		return NULL;
	if ((g = _aligned_malloc(sizeof(*g), 64)) == NULL)
		return NULL;
	memset(g, 0, sizeof(*g));
	g->keylen = keylen;
	if (keylen == 16) {
		aes128_key_expand(key, (__m128i *)g->key.expanded_keys);
		_aes_gcm_precomp_128_avx_gen4(&g->key);
	} else {
		aes256_key_expand(key, (__m128i *)g->key.expanded_keys);
		_aes_gcm_precomp_256_avx_gen4(&g->key);
	}
	return g;
}

void
isal_gcm_free(struct isal_gcm *g)
{
	if (g == NULL)
		return;
	explicit_bzero(g, sizeof(*g));
	_aligned_free(g);
}

int
isal_gcm_crypt(struct isal_gcm *g, int encrypt, u_char *dest,
    const u_char *src, u_int len, u_int aadlen, u_int authlen,
    const u_char *iv)
{
	__declspec(align(64)) struct isal_context_data ctx;
	u_char nonce[12], tag[16];
	int ok;

	if (authlen > sizeof(tag))
		return SSH_ERR_INVALID_ARGUMENT;
	memcpy(nonce, iv, sizeof(nonce));

	if (encrypt) {
		if (g->keylen == 16)
			_aes_gcm_enc_128_avx_gen4(&g->key, &ctx, dest + aadlen,
			    src + aadlen, len, nonce, src, aadlen, tag, authlen);
		else
			_aes_gcm_enc_256_avx_gen4(&g->key, &ctx, dest + aadlen,
			    src + aadlen, len, nonce, src, aadlen, tag, authlen);
		if (aadlen)
			memcpy(dest, src, aadlen);
		memcpy(dest + aadlen + len, tag, authlen);
		ok = 1;
	} else {
		if (g->keylen == 16)
			_aes_gcm_dec_128_avx_gen4(&g->key, &ctx, dest + aadlen,
			    src + aadlen, len, nonce, src, aadlen, tag, authlen);
		else
			_aes_gcm_dec_256_avx_gen4(&g->key, &ctx, dest + aadlen,
			    src + aadlen, len, nonce, src, aadlen, tag, authlen);
		/* the assembly computes the tag; comparing it is ours, and it
		 * has to be a comparison that does not leak where it differed */
		ok = timingsafe_bcmp(tag, src + aadlen + len, authlen) == 0;
		if (ok && aadlen)
			memcpy(dest, src, aadlen);
	}

	explicit_bzero(&ctx, sizeof(ctx));
	explicit_bzero(nonce, sizeof(nonce));
	explicit_bzero(tag, sizeof(tag));
	return ok ? 0 : SSH_ERR_MAC_INVALID;
}

#else /* !RSYNC_ISAL_GCM -- built without NASM, so without the assembly */

int isal_gcm_available(void) { return 0; }
struct isal_gcm *isal_gcm_new(const u_char *k, u_int l) { (void)k; (void)l; return NULL; }
void isal_gcm_free(struct isal_gcm *g) { (void)g; }
int
isal_gcm_crypt(struct isal_gcm *g, int e, u_char *d, const u_char *s,
    u_int l, u_int a, u_int t, const u_char *iv)
{
	(void)g; (void)e; (void)d; (void)s; (void)l; (void)a; (void)t; (void)iv;
	return SSH_ERR_INVALID_ARGUMENT;
}

#endif /* RSYNC_ISAL_GCM */
