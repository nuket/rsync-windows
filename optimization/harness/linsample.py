"""Sample Linux CPU, temperature and interface bytes once a second.

  linsample.py <seconds> <dev> <outfile>

CSV: t,cpu_pct,temp_c,rx_MBps
"""
import os, sys, time

secs, dev, out = float(sys.argv[1]), sys.argv[2], sys.argv[3]
rxp = '/sys/class/net/%s/statistics/rx_bytes' % dev


def cpu():
    with open('/proc/stat') as fh:
        p = fh.readline().split()[1:]
    v = [int(x) for x in p]
    idle = v[3] + v[4]
    return sum(v), idle


def temp():
    for z in sorted(os.listdir('/sys/class/thermal')):
        if z.startswith('thermal_zone'):
            try:
                with open('/sys/class/thermal/%s/temp' % z) as fh:
                    return int(fh.read()) / 1000.0
            except (IOError, OSError):
                pass
    return 0.0


def rx():
    with open(rxp) as fh:
        return int(fh.read())


t0 = time.time()
pt, pi = cpu()
pr = rx()
rows = []
while time.time() - t0 < secs:
    time.sleep(1.0)
    nt, ni = cpu()
    nr = rx()
    dt, di = nt - pt, ni - pi
    rows.append((time.time() - t0,
                 100.0 * (dt - di) / dt if dt else 0.0,
                 temp(),
                 (nr - pr) / 1e6))
    pt, pi, pr = nt, ni, nr

with open(out, 'w') as fh:
    fh.write('t,cpu_pct,temp_c,rx_MBps\n')
    for r in rows:
        fh.write('%.2f,%.2f,%.1f,%.1f\n' % r)
print('wrote %d rows' % len(rows))
