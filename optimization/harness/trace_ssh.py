import subprocess, sys, time, collections, re

ssh = sys.argv[1]
infile = sys.argv[2]
out = sys.argv[3]

cmd = [ssh, '-vvv', '-o', 'BatchMode=yes', '-b', '192.168.178.86', '-c', 'aes128-gcm@openssh.com',
       'max@192.168.178.150', 'dd of=/dev/null bs=1M']
t0 = time.perf_counter()
with open(infile, 'rb') as f:
    p = subprocess.Popen(cmd, stdin=f, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    lines = []
    for raw in p.stderr:
        lines.append((time.perf_counter() - t0, raw.decode('utf-8', 'replace').rstrip()))
    p.wait()
t1 = time.perf_counter() - t0

with open(out, 'w', encoding='utf-8') as w:
    for t, l in lines:
        w.write('%9.4f %s\n' % (t, l))

# summary: what kinds of lines dominate during the bulk phase, and packet timing
kinds = collections.Counter()
data_t = []
for t, l in lines:
    m = re.match(r'debug\d: (send packet: type \d+|receive packet: type \d+|channel \d+: [a-z_ ]+|[^:,(]+)', l)
    k = m.group(1).strip() if m else l[:40]
    kinds[k] += 1
    if 'send packet: type 94' in l:
        data_t.append(t)
print('total %.2fs, %d lines' % (t1, len(lines)))
print('data packets sent (type 94): %d' % len(data_t))
if len(data_t) > 10:
    gaps = [b - a for a, b in zip(data_t, data_t[1:])]
    gaps.sort()
    n = len(gaps)
    print('gap between data packets: median %.3f ms, p90 %.3f ms, p99 %.3f ms, max %.3f ms, mean %.3f ms'
          % (gaps[n//2]*1e3, gaps[int(n*0.9)]*1e3, gaps[int(n*0.99)]*1e3, gaps[-1]*1e3, sum(gaps)/n*1e3))
print('top line kinds:')
for k, c in kinds.most_common(25):
    print('  %7d  %s' % (c, k))
