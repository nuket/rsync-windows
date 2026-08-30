/*
 * win32cnggcm.c -- AES-GCM for the SSH transport through Windows CNG.
 *
 * Part of the rsync for Windows port (github.com/nuket/rsync-windows).
 * Compiled into Win32-OpenSSH's libssh beside cipher.c, which reaches it
 * through patch 0006 in win32/openssh/patches.
 *
 * Copyright (c) 2026 Max Vilimpoc.  ISC, as cipher.c.
 *
 * The libcrypto.dll Windows ships -- LibreSSL, which this build links, and
 * the only one rsync's hardening lets ssh.exe load -- runs AES-128-GCM at
 * about 1.6GB/s in SSH's 32KB packets on an i5-8350U; CNG's AES-GCM runs
 * it at 2.2GB/s, the difference being LibreSSL's lack of the stitched
 * AES-NI+PCLMUL path.  On a 20Gbit link the cipher is half of the client's
 * single thread, so this is the biggest single-thread lever there is.
 *
 * Only the bulk cipher moves: key exchange, signatures, digests and
 * chacha20-poly1305 stay with libcrypto.  The framing is exactly RFC 5647
 * as OpenSSH does it with EVP: a 12-byte IV of 4 fixed bytes and an 8-byte
 * big-endian invocation counter, used for a packet and then incremented;
 * the 4-byte length as additional data; a 16-byte tag after the
 * ciphertext.  SSH_AESGCM_BACKEND=libcrypto in the environment forces the
 * EVP path, for comparison and as a fallback.  BCrypt's GCM has been in
 * Windows since Vista, so this narrows nothing.
 */

#include "includes.h"

#include <windows.h>
#include <bcrypt.h>
#include <string.h>
#include <stdlib.h>

#include "ssherr.h"
#include "win32cnggcm.h"
#include "win32isalgcm.h"

#ifndef STATUS_AUTH_TAG_MISMATCH
#define STATUS_AUTH_TAG_MISMATCH ((NTSTATUS)0xC000A002L)
#endif
#define CNG_GCM_IVLEN 12

struct cng_gcm {
	BCRYPT_ALG_HANDLE alg;
	BCRYPT_KEY_HANDLE key;
	struct isal_gcm *isal;   /* set instead of alg/key when ISA-L is doing it */
	u_char iv[CNG_GCM_IVLEN];
};

/*
 * Which of the three does the bulk cipher.  SSH_AESGCM_BACKEND picks:
 * "libcrypto" keeps EVP (cng_gcm_wanted() says no and cipher.c never gets
 * here), "cng" forces CNG, and unset means ISA-L where there is any -- the
 * assembly has to have been built in and the processor has to have AVX2,
 * AES-NI and PCLMULQDQ, and failing either of those this quietly becomes
 * CNG.
 *
 * ISA-L is the default because of what a long transfer does to a 15W
 * laptop rather than because of its peak: held at line rate for three
 * minutes from a cold start, CNG went from 1436 to 1276 MB/s (-11.2%) and
 * again from 1342 to 1255 (-6.4%), while ISA-L went 1460 -> 1444 (-1.1%)
 * and 1434 -> 1419 (-1.0%).  Both ended at the same clock -- around 130% of
 * base, down from 145-168 -- so the throttling is identical and what
 * differs is how much throughput it costs.
 */
static int
isal_wanted(void)
{
	static int wanted = -1;

	if (wanted < 0) {
		const char *e = getenv("SSH_AESGCM_BACKEND");
		wanted = e == NULL || e[0] == 0 || strcasecmp(e, "isal") == 0;
	}
	return wanted;
}

int
cng_gcm_wanted(void)
{
	static int wanted = -1;

	if (wanted < 0) {
		const char *e = getenv("SSH_AESGCM_BACKEND");
		wanted = !(e != NULL && strcasecmp(e, "libcrypto") == 0);
	}
	return wanted;
}

void
cng_gcm_free(struct cng_gcm *g)
{
	if (g == NULL)
		return;
	isal_gcm_free(g->isal);
	if (g->key)
		BCryptDestroyKey(g->key);
	if (g->alg)
		BCryptCloseAlgorithmProvider(g->alg, 0);
	freezero(g, sizeof(*g));
}

struct cng_gcm *
cng_gcm_new(const u_char *key, u_int keylen, const u_char *iv, u_int ivlen)
{
	struct cng_gcm *g;

	if (ivlen != CNG_GCM_IVLEN || (g = calloc(1, sizeof(*g))) == NULL)
		return NULL;
	if (isal_wanted() && (g->isal = isal_gcm_new(key, keylen)) != NULL) {
		memcpy(g->iv, iv, CNG_GCM_IVLEN);
		return g;
	}
	if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&g->alg,
	    BCRYPT_AES_ALGORITHM, NULL, 0)) ||
	    !BCRYPT_SUCCESS(BCryptSetProperty(g->alg, BCRYPT_CHAINING_MODE,
	    (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0)) ||
	    !BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(g->alg, &g->key, NULL, 0,
	    (PUCHAR)key, keylen, 0))) {
		cng_gcm_free(g);
		return NULL;
	}
	memcpy(g->iv, iv, CNG_GCM_IVLEN);
	return g;
}

/* the invocation counter: the last 8 bytes, big-endian, as ctr64_inc() */
static void
cng_gcm_iv_inc(u_char *iv)
{
	int i;

	for (i = CNG_GCM_IVLEN - 1; i >= CNG_GCM_IVLEN - 8; i--)
		if (++iv[i] != 0)
			break;
}

int
cng_gcm_crypt(struct cng_gcm *g, int encrypt, u_char *dest, const u_char *src,
    u_int len, u_int aadlen, u_int authlen)
{
	BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
	u_char nonce[CNG_GCM_IVLEN], tag[16];
	ULONG done = 0;
	NTSTATUS st;

	if (authlen > sizeof(tag))
		return SSH_ERR_INVALID_ARGUMENT;
	if (g->isal != NULL) {
		int r = isal_gcm_crypt(g->isal, encrypt, dest, src, len,
		    aadlen, authlen, g->iv);
		cng_gcm_iv_inc(g->iv);   /* one IV per packet, either way */
		return r;
	}
	BCRYPT_INIT_AUTH_MODE_INFO(info);
	memcpy(nonce, g->iv, sizeof(nonce));
	info.pbNonce = nonce;
	info.cbNonce = sizeof(nonce);
	info.pbAuthData = (PUCHAR)src;
	info.cbAuthData = aadlen;
	info.pbTag = tag;
	info.cbTag = authlen;
	if (!encrypt)
		memcpy(tag, src + aadlen + len, authlen);

	if (encrypt)
		st = BCryptEncrypt(g->key, (PUCHAR)src + aadlen, len, &info,
		    NULL, 0, dest + aadlen, len, &done, 0);
	else
		st = BCryptDecrypt(g->key, (PUCHAR)src + aadlen, len, &info,
		    NULL, 0, dest + aadlen, len, &done, 0);
	/* one IV per packet, whatever became of it */
	cng_gcm_iv_inc(g->iv);
	explicit_bzero(nonce, sizeof(nonce));
	if (st == STATUS_AUTH_TAG_MISMATCH)
		return SSH_ERR_MAC_INVALID;
	if (!BCRYPT_SUCCESS(st) || done != len)
		return SSH_ERR_LIBCRYPTO_ERROR;
	if (aadlen)
		memcpy(dest, src, aadlen);
	if (encrypt)
		memcpy(dest + aadlen + len, tag, authlen);
	return 0;
}

/* as EVP_CTRL_GCM_IV_GEN: hand out the current IV, then step it */
int
cng_gcm_get_iv(struct cng_gcm *g, u_char *iv, size_t len)
{
	if (len != CNG_GCM_IVLEN)
		return SSH_ERR_INVALID_ARGUMENT;
	memcpy(iv, g->iv, len);
	cng_gcm_iv_inc(g->iv);
	return 0;
}

int
cng_gcm_set_iv(struct cng_gcm *g, const u_char *iv, size_t len)
{
	if (len != CNG_GCM_IVLEN)
		return SSH_ERR_INVALID_ARGUMENT;
	memcpy(g->iv, iv, len);
	return 0;
}
