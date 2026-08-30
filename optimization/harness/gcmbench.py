"""AES-128-GCM throughput of C:\\Windows\\System32\\libcrypto.dll (what ssh.exe
uses) via its EVP API, one core, 32KB packets like SSH and 1MB chunks."""
import ctypes, ctypes.wintypes as W, time, os

lc = ctypes.WinDLL(r"C:\Windows\System32\libcrypto.dll")
lc.EVP_aes_128_gcm.restype = ctypes.c_void_p
lc.EVP_CIPHER_CTX_new.restype = ctypes.c_void_p
lc.EVP_EncryptInit_ex.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
lc.EVP_EncryptUpdate.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int), ctypes.c_void_p, ctypes.c_int]
lc.EVP_EncryptFinal_ex.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
lc.EVP_CIPHER_CTX_ctrl.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
EVP_CTRL_GCM_GET_TAG = 0x10

key = b"k" * 16
iv = b"i" * 12
ctx = lc.EVP_CIPHER_CTX_new()
assert lc.EVP_EncryptInit_ex(ctx, lc.EVP_aes_128_gcm(), None, key, iv) == 1

def bench(chunk, total=1 << 30):
    src = ctypes.create_string_buffer(os.urandom(chunk), chunk)
    dst = ctypes.create_string_buffer(chunk + 16)
    outl = ctypes.c_int()
    tag = ctypes.create_string_buffer(16)
    n = total // chunk
    t0 = time.perf_counter()
    for i in range(n):
        # a fresh GCM message per packet, as SSH does (new IV, tag at the end)
        lc.EVP_EncryptInit_ex(ctx, None, None, None, iv)
        lc.EVP_EncryptUpdate(ctx, dst, ctypes.byref(outl), src, chunk)
        lc.EVP_EncryptFinal_ex(ctx, dst, ctypes.byref(outl))
        lc.EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag)
    el = time.perf_counter() - t0
    return total / el / 1e6

for chunk in (32 * 1024, 1 << 20):
    print(f"libcrypto EVP aes-128-gcm, {chunk // 1024} KB messages: {bench(chunk):.0f} MB/s")
