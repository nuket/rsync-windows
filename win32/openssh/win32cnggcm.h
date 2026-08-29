/*
 * win32cnggcm.h -- AES-GCM for the SSH transport through Windows CNG.
 * See win32cnggcm.c.  cipher.c reaches it through patch 0006 in
 * win32/openssh/patches.
 *
 * Part of the rsync for Windows port (github.com/nuket/rsync-windows).
 * Copyright (c) 2026 Max Vilimpoc.  ISC, as cipher.c.
 */
#ifndef WIN32CNGGCM_H
#define WIN32CNGGCM_H

#include <sys/types.h>

struct cng_gcm;

/* FALSE when SSH_AESGCM_BACKEND=libcrypto is in the environment */
int cng_gcm_wanted(void);
/* NULL if CNG will not take the key (the caller falls back to EVP) */
struct cng_gcm *cng_gcm_new(const u_char *key, u_int keylen,
    const u_char *iv, u_int ivlen);
void cng_gcm_free(struct cng_gcm *);
/* one SSH packet: aadlen bytes of length in the clear, len of payload,
 * authlen of tag; the IV steps after each call.  SSH_ERR_* on failure. */
int cng_gcm_crypt(struct cng_gcm *, int encrypt, u_char *dest,
    const u_char *src, u_int len, u_int aadlen, u_int authlen);
/* as EVP_CTRL_GCM_IV_GEN / EVP_CTRL_GCM_SET_IV_FIXED with a full IV */
int cng_gcm_get_iv(struct cng_gcm *, u_char *iv, size_t len);
int cng_gcm_set_iv(struct cng_gcm *, const u_char *iv, size_t len);

#endif /* WIN32CNGGCM_H */
