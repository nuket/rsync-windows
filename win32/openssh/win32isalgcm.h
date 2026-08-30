/*
 * win32isalgcm.h -- AES-GCM for the SSH transport through Intel's ISA-L
 * assembly.  See win32isalgcm.c.  win32cnggcm.c picks between this and CNG;
 * cipher.c knows about neither.
 *
 * Part of the rsync for Windows port (github.com/nuket/rsync-windows).
 * Copyright (c) 2026 Max Vilimpoc.  ISC, as cipher.c.
 */
#ifndef WIN32ISALGCM_H
#define WIN32ISALGCM_H

#include <sys/types.h>

struct isal_gcm;

/*
 * TRUE if this build has the ISA-L objects linked in and this processor has
 * what they need (AVX2, AES-NI, PCLMULQDQ).  Everything else here may only
 * be called when it is.
 */
int isal_gcm_available(void);

/* keylen is 16 or 32; anything else, or no memory, gives NULL. */
struct isal_gcm *isal_gcm_new(const u_char *key, u_int keylen);
void isal_gcm_free(struct isal_gcm *);

/*
 * One SSH packet, with the same shape cipher.c uses everywhere else: aadlen
 * bytes of length in the clear, len bytes of payload, authlen bytes of tag
 * after it.  `iv` is the 12 bytes for this packet, which the caller steps.
 * Returns 0, SSH_ERR_MAC_INVALID on a bad tag, or SSH_ERR_INVALID_ARGUMENT.
 */
int isal_gcm_crypt(struct isal_gcm *, int encrypt, u_char *dest,
    const u_char *src, u_int len, u_int aadlen, u_int authlen,
    const u_char *iv);

#endif /* WIN32ISALGCM_H */
