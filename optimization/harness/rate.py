"""Per-interval receive rate on an interface, to see whether a transfer runs
steady or stalls.

  rate.py <dev> <seconds> <interval>
"""
import sys, time

dev, secs, iv = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
path = '/sys/class/net/%s/statistics/rx_bytes' % dev


def rd():
    with open(path) as fh:
        return int(fh.read())


prev, t0 = rd(), time.time()
end = t0 + secs
vals = []
while time.time() < end:
    time.sleep(iv)
    now = time.time()
    cur = rd()
    vals.append((now - t0, (cur - prev) / (iv * 1e6)))
    prev = cur

busy = [v for _, v in vals if v > 50]
if busy:
    print("intervals busy: %d, mean %.0f MB/s, min %.0f, max %.0f"
          % (len(busy), sum(busy) / len(busy), min(busy), max(busy)))
print("timeline MB/s: " + " ".join("%.0f" % v for _, v in vals))
