# The bit of ISA-L that AES-GCM needs

Six files, taken verbatim from Intel's
[isa-l_crypto](https://github.com/intel/isa-l_crypto) 2.26.1 (commit
`f22c49a`), which is BSD-3-Clause — see `LICENSE`, which is Intel's own,
unmodified. Nothing here is edited; updating means copying the same six
files again.

| file | what it is |
| --- | --- |
| `gcm128_avx_gen4.asm` | two lines: `%define GCM128_MODE`, then include the body |
| `gcm256_avx_gen4.asm` | the same for 256-bit keys |
| `gcm_avx_gen4.asm` | the body — AES-GCM for AVX2 with AES-NI and PCLMULQDQ |
| `gcm_defines.asm` | the field layout the body and the caller must agree on |
| `reg_sizes.asm`, `clear_regs.inc` | NASM helpers the body includes |

That is the whole dependency closure of `gcm128_avx_gen4.asm` and its 256-bit
twin: 154 KB, no C, no build system, no library.

## What was deliberately left out

* **The multibinary dispatcher** (`gcm_multibinary.asm` and the SSE, AVX and
  AVX-512 variants it chooses between). It would have meant four
  implementations instead of one, and the choice it makes at run time we can
  make ourselves — `win32isalgcm.c` asks CPUID for AVX2, AES-NI and
  PCLMULQDQ, and if the answer is no it says so and the caller stays with
  CNG. Every x86-64 part since Haswell (2013) answers yes.
* **The non-temporal variants** (`*_nt.asm`). They exist to avoid polluting
  cache with data that will not be read again, which is not this workload:
  the ciphertext is handed straight to the socket pump.
* **`gcm_pre.c` and `aes_keyexp*.asm`**, whose whole job is the AES key
  schedule before `precomp`. That is thirty lines of AES-NI in
  `win32isalgcm.c` instead, and it keeps the take to assembly with no C and
  no second header to track.

## Building it

NASM, and only for these two objects:

    nasm -f win64 -I isal/ isal/gcm128_avx_gen4.asm -o gcm128_avx_gen4.obj
    nasm -f win64 -I isal/ isal/gcm256_avx_gen4.asm -o gcm256_avx_gen4.obj

`build-openssh.ps1` does that when it can find NASM and links the results in;
when it cannot, `ssh.exe` is built without them and AES-GCM stays on CNG. So
NASM is a nice-to-have, not a build requirement.
