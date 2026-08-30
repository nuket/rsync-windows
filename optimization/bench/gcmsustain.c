/*
 * AES-128-GCM, sustained: run one library flat out on one thread and print
 * its throughput every second, so we can see whether it holds up as the
 * chip heats or decays.  Same work in every case -- one complete GCM
 * message per 32 KB packet, fresh IV, 4 bytes of AAD, 16-byte tag, exactly
 * as SSH uses it.
 *
 *   gcmsustain <isal|cng|openssl3|libressl> <seconds> [host port]
 *
 * With a host and port it also *sends* every encrypted packet over raw TCP,
 * which is as close to "ssh built with this cipher" as we can get without
 * building four ssh.exes: same per-packet GCM work, same 32 KB packets, same
 * link.  Throughput is then whichever binds first, the cipher or the wire.
 *
 * Prints "t,mbps" once a second on stdout, counting payload bytes (the tag
 * is on the wire but not counted), so the number is comparable with and
 * without the network.  Temperature, clock and machine CPU are sampled
 * alongside by the PowerShell driver; the Linux end logs itself.
 *
 * The two EVP libraries export the same symbol names, so both are loaded
 * dynamically and resolved per handle rather than linked.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "isa-l_crypto/aes_gcm.h"

#define CHUNK (32u << 10)

static double now(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

/* ---- EVP, resolved out of whichever DLL we were pointed at ------------- */
typedef void *(*fn_cipher)(void);
typedef void *(*fn_ctx_new)(void);
typedef int (*fn_init)(void *, void *, void *, const unsigned char *, const unsigned char *);
typedef int (*fn_update)(void *, unsigned char *, int *, const unsigned char *, int);
typedef int (*fn_final)(void *, unsigned char *, int *);
typedef int (*fn_ctrl)(void *, int, int, void *);

struct evp {
    fn_cipher aes_128_gcm;
    fn_ctx_new ctx_new;
    fn_init init;
    fn_update update;
    fn_final final;
    fn_ctrl ctrl;
    void *ctx;
};

static int evp_load(struct evp *e, const char *dll)
{
    HMODULE h = LoadLibraryA(dll);
    if (!h) {
        fprintf(stderr, "LoadLibrary(%s) failed: %lu\n", dll, GetLastError());
        return 0;
    }
    e->aes_128_gcm = (fn_cipher)GetProcAddress(h, "EVP_aes_128_gcm");
    e->ctx_new = (fn_ctx_new)GetProcAddress(h, "EVP_CIPHER_CTX_new");
    e->init = (fn_init)GetProcAddress(h, "EVP_EncryptInit_ex");
    e->update = (fn_update)GetProcAddress(h, "EVP_EncryptUpdate");
    e->final = (fn_final)GetProcAddress(h, "EVP_EncryptFinal_ex");
    e->ctrl = (fn_ctrl)GetProcAddress(h, "EVP_CIPHER_CTX_ctrl");
    if (!e->aes_128_gcm || !e->ctx_new || !e->init || !e->update || !e->final || !e->ctrl) {
        fprintf(stderr, "missing EVP symbols in %s\n", dll);
        return 0;
    }
    e->ctx = e->ctx_new();
    return e->ctx != NULL;
}

int main(int argc, char **argv)
{
    static const unsigned char key[16] = "0123456789abcdef";
    static const unsigned char iv[12] = "0123456789ab";
    unsigned char aad[4] = { 0, 0, 0x80, 0 };
    unsigned char tag[16];
    const char *which = argc > 1 ? argv[1] : "cng";
    double secs = argc > 2 ? atof(argv[2]) : 120.0;
    const char *host = argc > 4 ? argv[3] : NULL;
    const char *port = argc > 4 ? argv[4] : NULL;
    SOCKET sock = INVALID_SOCKET;

    if (host) {
        WSADATA wsa;
        struct addrinfo hints, *ai = NULL;
        int one = 1, sndbuf = 8 << 20;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, port, &hints, &ai) != 0) {
            fprintf(stderr, "getaddrinfo(%s:%s) failed\n", host, port);
            return 1;
        }
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock == INVALID_SOCKET || connect(sock, ai->ai_addr, (int)ai->ai_addrlen) != 0) {
            fprintf(stderr, "connect to %s:%s failed: %d\n", host, port, WSAGetLastError());
            return 1;
        }
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char *)&sndbuf, sizeof sndbuf);
        freeaddrinfo(ai);
    }

    unsigned char *in = (unsigned char *)_aligned_malloc(CHUNK, 64);
    unsigned char *out = (unsigned char *)_aligned_malloc(CHUNK + 16, 64);
    for (unsigned i = 0; i < CHUNK; i++)
        in[i] = (unsigned char)i;

    struct isal_gcm_key_data *gkey = NULL;
    struct isal_gcm_context_data *gctx = NULL;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE ckey = NULL;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    unsigned char nonce[12];
    struct evp evp;
    int is_isal = 0, is_cng = 0, is_evp = 0;

    if (!strcmp(which, "isal")) {
        is_isal = 1;
        gkey = (struct isal_gcm_key_data *)_aligned_malloc(sizeof(*gkey), 64);
        gctx = (struct isal_gcm_context_data *)_aligned_malloc(sizeof(*gctx), 64);
        isal_aes_gcm_pre_128(key, gkey);
    } else if (!strcmp(which, "cng")) {
        is_cng = 1;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0) != 0 ||
            BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                              sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0 ||
            BCryptGenerateSymmetricKey(alg, &ckey, NULL, 0, (PUCHAR)key, 16, 0) != 0) {
            fprintf(stderr, "CNG setup failed\n");
            return 1;
        }
        memcpy(nonce, iv, sizeof nonce);
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = nonce;
        info.cbNonce = sizeof nonce;
        info.pbAuthData = aad;
        info.cbAuthData = sizeof aad;
        info.pbTag = tag;
        info.cbTag = sizeof tag;
    } else {
        /* full paths: the two export the same names, and neither is on PATH */
        const char *dll = !strcmp(which, "openssl3")
            ? "C:\\Users\\Claude\\AppData\\Local\\Programs\\Python\\Python313\\DLLs\\libcrypto-3.dll"
            : "C:\\Windows\\System32\\libcrypto.dll";
        is_evp = 1;
        if (!evp_load(&evp, dll))
            return 1;
        if (evp.init(evp.ctx, evp.aes_128_gcm(), NULL, key, iv) != 1) {
            fprintf(stderr, "EVP init failed\n");
            return 1;
        }
    }

    double t0 = now(), mark = t0;
    unsigned long long bytes = 0, at_mark = 0;
    /* no setvbuf here: MSVC rejects a zero size through the invalid
     * parameter handler, which __fastfails as 0xC0000409.  Flush instead. */

    for (;;) {
        for (int i = 0; i < 64; i++) {          /* a few before checking the clock */
            if (is_isal) {
                isal_aes_gcm_enc_128(gkey, gctx, out, in, CHUNK,
                                     (uint8_t *)iv, aad, sizeof aad, tag, sizeof tag);
            } else if (is_cng) {
                ULONG done = 0;
                BCryptEncrypt(ckey, in, CHUNK, &info, NULL, 0, out, CHUNK, &done, 0);
            } else {
                int outl = 0;
                evp.init(evp.ctx, NULL, NULL, NULL, iv);
                evp.update(evp.ctx, out, &outl, in, CHUNK);
                evp.final(evp.ctx, out, &outl);
                evp.ctrl(evp.ctx, 0x10 /* EVP_CTRL_GCM_GET_TAG */, 16, tag);
            }
            if (sock != INVALID_SOCKET) {
                /* the tag rides along on the wire, as it does in SSH */
                int left = (int)CHUNK + 16, off = 0;
                while (left > 0) {
                    int k = send(sock, (const char *)out + off, left, 0);
                    if (k <= 0) {
                        fprintf(stderr, "send failed: %d\n", WSAGetLastError());
                        return 1;
                    }
                    off += k;
                    left -= k;
                }
            }
            bytes += CHUNK;
        }
        double t = now();
        if (t - mark >= 1.0) {
            printf("%.2f,%.0f\n", t - t0, (double)(bytes - at_mark) / (t - mark) / 1e6);
            fflush(stdout);
            mark = t;
            at_mark = bytes;
        }
        if (t - t0 >= secs)
            break;
    }
    return 0;
}
