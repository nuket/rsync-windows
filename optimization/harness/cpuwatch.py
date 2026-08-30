"""Sustained per-process CPU on the remote while a transfer runs.

  cpuwatch.py <seconds> <comm> [comm...]

Reports, per process name, the mean fraction of one core over the intervals
in which anything was busy, and the peak -- so "is the far end the limit"
has an answer.
"""
import os, sys, time

secs = float(sys.argv[1])
want = set(sys.argv[2:])
HZ = os.sysconf('SC_CLK_TCK')


def snap():
    out = {}
    for pid in os.listdir('/proc'):
        if not pid.isdigit():
            continue
        try:
            with open('/proc/%s/stat' % pid, 'rb') as fh:
                d = fh.read().decode('utf-8', 'replace')
            lp, rp = d.index('('), d.rindex(')')
            comm = d[lp + 1:rp]
            if comm not in want:
                continue
            rest = d[rp + 2:].split()
            out[pid] = (comm, int(rest[11]) + int(rest[12]))
        except (IOError, OSError, ValueError, IndexError):
            pass
    return out


series = {}
prev, t_prev = snap(), time.time()
end = t_prev + secs
while time.time() < end:
    time.sleep(0.2)
    cur = snap()
    now = time.time()
    dt = now - t_prev
    per = {}
    for pid, (comm, tot) in cur.items():
        if pid in prev:
            per[comm] = per.get(comm, 0.0) + (tot - prev[pid][1]) / HZ / dt
    for comm, frac in per.items():
        series.setdefault(comm, []).append(frac)
    prev, t_prev = cur, now

for comm in sorted(series):
    busy = [f for f in series[comm] if f > 0.10]
    if not busy:
        continue
    print("%-14s busy %4.1f s  mean %3.0f%% of a core  peak %3.0f%%"
          % (comm, 0.2 * len(busy), 100 * sum(busy) / len(busy), 100 * max(busy)))
