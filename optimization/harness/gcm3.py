"""AES-128-GCM throughput, three implementations, the way SSH uses it:
a fresh GCM message per packet with a new IV and a 16-byte tag.

  LibreSSL  System32\\libcrypto.dll   -- what ssh.exe used to use
  OpenSSL 3 Python's libcrypto-3.dll  -- the modern stitched AES-NI path
  CNG       bcrypt.dll                -- what ssh.exe uses now

Rounds are interleaved, because this laptop throttles and running one
implementation to completion before the next would just measure the order.
"""
import ctypes
import ctypes.wintypes as W
import os
import sys
import time

LIBRESSL = r"C:\Windows\System32\libcrypto.dll"
OPENSSL3 = os.path.join(os.path.dirname(sys.executable), "DLLs", "libcrypto-3.dll")

KEY = b"k" * 16
IV = b"i" * 12
EVP_CTRL_GCM_GET_TAG = 0x10


class EvpGcm:
    def __init__(self, path, name):
        self.name = name
        lc = ctypes.CDLL(path)
        lc.EVP_aes_128_gcm.restype = ctypes.c_void_p
        lc.EVP_CIPHER_CTX_new.restype = ctypes.c_void_p
        lc.EVP_EncryptInit_ex.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                          ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
        lc.EVP_EncryptUpdate.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                         ctypes.POINTER(ctypes.c_int), ctypes.c_void_p, ctypes.c_int]
        lc.EVP_EncryptFinal_ex.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                           ctypes.POINTER(ctypes.c_int)]
        lc.EVP_CIPHER_CTX_ctrl.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                           ctypes.c_int, ctypes.c_void_p]
        self.lc = lc
        self.ctx = lc.EVP_CIPHER_CTX_new()
        if lc.EVP_EncryptInit_ex(self.ctx, lc.EVP_aes_128_gcm(), None, KEY, IV) != 1:
            raise RuntimeError("EVP_EncryptInit_ex failed for " + name)

    def run(self, src, dst, n, chunk):
        lc, ctx = self.lc, self.ctx
        outl = ctypes.c_int()
        tag = ctypes.create_string_buffer(16)
        for _ in range(n):
            lc.EVP_EncryptInit_ex(ctx, None, None, None, IV)
            lc.EVP_EncryptUpdate(ctx, dst, ctypes.byref(outl), src, chunk)
            lc.EVP_EncryptFinal_ex(ctx, dst, ctypes.byref(outl))
            lc.EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag)


class BCRYPT_AUTH_INFO(ctypes.Structure):
    _fields_ = [("cbSize", ctypes.c_ulong), ("dwInfoVersion", ctypes.c_ulong),
                ("pbNonce", ctypes.c_void_p), ("cbNonce", ctypes.c_ulong),
                ("pbAuthData", ctypes.c_void_p), ("cbAuthData", ctypes.c_ulong),
                ("pbTag", ctypes.c_void_p), ("cbTag", ctypes.c_ulong),
                ("pbMacContext", ctypes.c_void_p), ("cbMacContext", ctypes.c_ulong),
                ("cbAAD", ctypes.c_ulong), ("cbData", ctypes.c_ulonglong),
                ("dwFlags", ctypes.c_ulong)]


class CngGcm:
    name = "CNG (bcrypt)"

    def __init__(self):
        b = ctypes.WinDLL("bcrypt.dll")
        self.b = b
        alg = ctypes.c_void_p()
        if b.BCryptOpenAlgorithmProvider(ctypes.byref(alg), "AES", None, 0) != 0:
            raise RuntimeError("BCryptOpenAlgorithmProvider failed")
        mode = "ChainingModeGCM\0".encode("utf-16-le")
        if b.BCryptSetProperty(alg, "ChainingMode", mode, len(mode), 0) != 0:
            raise RuntimeError("BCryptSetProperty failed")
        key = ctypes.c_void_p()
        if b.BCryptGenerateSymmetricKey(alg, ctypes.byref(key), None, 0,
                                        KEY, len(KEY), 0) != 0:
            raise RuntimeError("BCryptGenerateSymmetricKey failed")
        self.key = key

    def run(self, src, dst, n, chunk):
        b = self.b
        nonce = ctypes.create_string_buffer(IV, 12)
        tag = ctypes.create_string_buffer(16)
        info = BCRYPT_AUTH_INFO()
        info.cbSize = ctypes.sizeof(info)
        info.dwInfoVersion = 1
        info.pbNonce = ctypes.cast(nonce, ctypes.c_void_p)
        info.cbNonce = 12
        info.pbTag = ctypes.cast(tag, ctypes.c_void_p)
        info.cbTag = 16
        done = ctypes.c_ulong()
        for _ in range(n):
            rc = b.BCryptEncrypt(self.key, src, chunk, ctypes.byref(info), None, 0,
                                 dst, chunk, ctypes.byref(done), 0)
            if rc != 0:
                raise RuntimeError("BCryptEncrypt failed 0x%08x" % (rc & 0xffffffff))


def main():
    impls = []
    for path, name in ((LIBRESSL, "LibreSSL 3.8.2 (System32)"),
                       (OPENSSL3, "OpenSSL 3 (Python)")):
        try:
            impls.append(EvpGcm(path, name))
        except Exception as e:
            print("skipping %s: %s" % (name, e))
    impls.append(CngGcm())

    for chunk in (32 * 1024, 1 << 20):
        total = 512 << 20
        n = total // chunk
        src = ctypes.create_string_buffer(os.urandom(chunk), chunk)
        dst = ctypes.create_string_buffer(chunk + 16)
        best = {}
        print("\n%d KB messages, %d MB per round, 3 rounds interleaved:"
              % (chunk // 1024, total >> 20))
        for rnd in range(3):
            for im in impls:
                t0 = time.perf_counter()
                im.run(src, dst, n, chunk)
                el = time.perf_counter() - t0
                mbps = total / el / 1e6
                best[im.name] = max(best.get(im.name, 0), mbps)
                print("   round %d  %-28s %7.0f MB/s" % (rnd + 1, im.name, mbps))
        print("   -- best of three --")
        for name, v in sorted(best.items(), key=lambda kv: -kv[1]):
            print("   %-28s %7.0f MB/s" % (name, v))


main()
