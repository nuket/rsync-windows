"""Delta-transfer edge cases: run each case through the installed rsync.exe and
the new build, check the reconstructed file is byte-exact, and compare the
literal byte counts (the new sender may send a little more literal data when
an aligned probe skips a shifted match; it must never send a wrong file)."""
import os, sys, subprocess, random, re, shutil, time

OLD = r"C:\Tools\rsync\rsync.exe"
NEW = sys.argv[1]
WORK = os.path.join(os.path.dirname(os.path.abspath(__file__)), "edge")
rnd = random.Random(1234)

def rb(n):
    return rnd.randbytes(n)

def vmlike(base, period, wlen, start=1000):
    b = bytearray(base)
    off = start
    while off + wlen < len(b):
        b[off:off + wlen] = rb(wlen)
        off += period
    return bytes(b)

def cases():
    M = 1024 * 1024
    r8 = rb(8 * M)
    yield "empty-new", r8[:100000], b"", []
    yield "empty-basis", b"", r8[:100000], []
    yield "tiny-1byte", r8[:100], r8[:50] + b"X" + r8[51:100], []
    yield "sub-block-B700", r8[:5000], r8[:2000] + b"YY" + r8[2002:5000], ["-B", "700"]
    yield "insert-mid", r8, r8[:4 * M] + rb(1000) + r8[4 * M:], []
    yield "delete-mid", r8, r8[:4 * M] + r8[4 * M + 5000:], []
    yield "vmlike-4k-per-64k", r8, vmlike(r8, 65536, 4096), []
    yield "vmlike-sparse", r8, vmlike(r8, 1 * M, 512), []
    yield "append-1M", r8, r8 + rb(M), []
    yield "truncate", r8, r8[:5 * M + 12345], []
    z = bytes(8 * M)
    zb = bytearray(z); zb[3 * M + 7] = 1; zb[6 * M + 100:6 * M + 200] = rb(100)
    yield "zeros-few-bytes", z, bytes(zb), []
    yield "zeros-vs-random", z, r8, []
    yield "all-different", r8, rb(8 * M), []
    yield "inplace-vmlike", r8, vmlike(r8, 65536, 4096), ["--inplace"]
    yield "inplace-insert", r8, r8[:4 * M] + rb(1000) + r8[4 * M:], ["--inplace"]
    yield "append-mode", r8, r8 + rb(M), ["--append"]
    yield "append-verify", r8, r8 + rb(M), ["--append-verify"]
    yield "B700-vmlike", r8, vmlike(r8, 65536, 4096), ["-B", "700"]
    yield "B128k-vmlike", r8, vmlike(r8, 65536, 4096), ["-B", "131072"]
    # Shifted copy inside the probe span: blocks moved 1000 bytes forward
    # inside a region that is otherwise unchanged around them.
    mv = bytearray(r8); mv[2 * M + 1000:2 * M + 1000 + 300000] = r8[2 * M:2 * M + 300000]
    yield "shifted-copy", r8, bytes(mv), []
    yield "md5-vmlike", r8, vmlike(r8, 65536, 4096), ["--checksum-choice=md5"]
    yield "last-partial-block", r8[:7 * M + 777], r8[:7 * M + 700] + rb(77), []
    # --protocol=30 hangs locally with the installed release too (pre-existing).
    yield "seed-fixed", r8, vmlike(r8, 65536, 4096), ["--checksum-seed=42"]

def run(exe, name, basis, new, opts):
    src = os.path.join(WORK, "src"); dst = os.path.join(WORK, "dst")
    for d in (src, dst):
        shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
    with open(os.path.join(src, "f"), "wb") as fh: fh.write(new)
    with open(os.path.join(dst, "f"), "wb") as fh: fh.write(basis)
    os.utime(os.path.join(dst, "f"), (1000000000, 1000000000))
    t = time.time()
    try:
        p = subprocess.run([exe, "-t", "--no-whole-file", "--stats"] + opts + ["src/f", "dst/"],
                           cwd=WORK, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        subprocess.run(["taskkill", "/F", "/IM", os.path.basename(exe)], capture_output=True)
        return 999, False, -1, -1, time.time() - t, "TIMEOUT"
    t = time.time() - t
    got = open(os.path.join(dst, "f"), "rb").read()
    lit = re.search(r"Literal data: ([\d,]+)", p.stdout)
    mat = re.search(r"Matched data: ([\d,]+)", p.stdout)
    lit = int(lit.group(1).replace(",", "")) if lit else -1
    mat = int(mat.group(1).replace(",", "")) if mat else -1
    return p.returncode, got == new, lit, mat, t, (p.stderr or "").strip()[:200]

os.makedirs(WORK, exist_ok=True)
bad = 0
print(f"{'case':22} {'old lit':>11} {'new lit':>11} {'old mat':>11} {'new mat':>11}  ok  time old/new")
for name, basis, new, opts in cases():
    ro = run(OLD, name, basis, new, opts)
    rn = run(NEW, name, basis, new, opts)
    ok = ro[0] == 0 and rn[0] == 0 and ro[1] and rn[1]
    if not ok:
        bad += 1
    flag = "OK " if ok else "BAD"
    print(f"{name:22} {ro[2]:11,} {rn[2]:11,} {ro[3]:11,} {rn[3]:11,}  {flag} {ro[4]:.2f}/{rn[4]:.2f}"
          + (f"  rc={ro[0]}/{rn[0]} exact={ro[1]}/{rn[1]} {ro[5]} {rn[5]}" if not ok else ""))
print("FAILURES:", bad)
sys.exit(1 if bad else 0)
