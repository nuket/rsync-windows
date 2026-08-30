/*
 * AES-128-GCM: Intel ISA-L crypto against Windows CNG, the way SSH uses it --
 * one complete GCM message per packet, fresh IV, 16-byte tag, 4 bytes of AAD
 * for the packet length.
 *
 * Rounds are interleaved because this laptop throttles; running one library
 * to completion before the other would measure the order, not the code.
 */
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "isa-l_crypto/aes_gcm.h"

#define ROUNDS 3

static double now(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

int main(void)
{
    static const unsigned char key[16] = "0123456789abcdef";
    static const unsigned char iv[12] = "0123456789ab";
    unsigned char aad[4] = { 0, 0, 0x80, 0 };
    size_t sizes[2] = { 32u << 10, 1u << 20 };
    const unsigned long long total = 512ull << 20;

    /* ISA-L wants these aligned; 64 is safe for every implementation. */
    struct isal_gcm_key_data *gkey =
        (struct isal_gcm_key_data *)_aligned_malloc(sizeof(*gkey), 64);
    struct isal_gcm_context_data *gctx =
        (struct isal_gcm_context_data *)_aligned_malloc(sizeof(*gctx), 64);

    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE ckey = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0) != 0 ||
        BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0 ||
        BCryptGenerateSymmetricKey(alg, &ckey, NULL, 0, (PUCHAR)key, sizeof key, 0) != 0) {
        fprintf(stderr, "CNG setup failed\n");
        return 1;
    }
    isal_aes_gcm_pre_128(key, gkey);

    for (int s = 0; s < 2; s++) {
        size_t chunk = sizes[s];
        unsigned long long n = total / chunk;
        unsigned char *in = (unsigned char *)_aligned_malloc(chunk, 64);
        unsigned char *out = (unsigned char *)_aligned_malloc(chunk + 16, 64);
        unsigned char tag[16];
        double best_isal = 0, best_cng = 0;

        for (size_t i = 0; i < chunk; i++)
            in[i] = (unsigned char)i;

        printf("\n%zu KB messages, %llu MB per round, %d rounds interleaved:\n",
               chunk >> 10, total >> 20, ROUNDS);

        for (int r = 0; r < ROUNDS; r++) {
            double t0, el, mbps;

            t0 = now();
            for (unsigned long long i = 0; i < n; i++)
                isal_aes_gcm_enc_128(gkey, gctx, out, in, chunk,
                                     (uint8_t *)iv, aad, sizeof aad, tag, sizeof tag);
            el = now() - t0;
            mbps = (double)total / el / 1e6;
            if (mbps > best_isal) best_isal = mbps;
            printf("   round %d  %-22s %7.0f MB/s\n", r + 1, "ISA-L crypto", mbps);

            {
                BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
                unsigned char nonce[12];
                ULONG done = 0;
                memcpy(nonce, iv, sizeof nonce);
                BCRYPT_INIT_AUTH_MODE_INFO(info);
                info.pbNonce = nonce;
                info.cbNonce = sizeof nonce;
                info.pbAuthData = aad;
                info.cbAuthData = sizeof aad;
                info.pbTag = tag;
                info.cbTag = sizeof tag;

                t0 = now();
                for (unsigned long long i = 0; i < n; i++)
                    BCryptEncrypt(ckey, in, (ULONG)chunk, &info, NULL, 0,
                                  out, (ULONG)chunk, &done, 0);
                el = now() - t0;
                mbps = (double)total / el / 1e6;
                if (mbps > best_cng) best_cng = mbps;
                printf("   round %d  %-22s %7.0f MB/s\n", r + 1, "CNG (bcrypt)", mbps);
            }
        }
        printf("   -- best of %d: ISA-L %.0f MB/s, CNG %.0f MB/s -> ISA-L is %+.0f%%\n",
               ROUNDS, best_isal, best_cng, 100.0 * (best_isal / best_cng - 1.0));
        _aligned_free(in);
        _aligned_free(out);
    }
    return 0;
}
