"""Linux side of the encrypt-and-send test: accept one connection, drain it,
and log what the receiving machine sees once a second.

  linsink.py <port> <seconds> <dev> <outfile>

CSV: t,rx_MBps,cpu_pct,temp_c

rx_MBps is counted from the socket (what actually arrived), not the
interface counter, so it is directly comparable with the sender's number.
"""
import os, socket, sys, time

port, secs, dev, out = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3], sys.argv[4]
HZ = os.sysconf('SC_CLK_TCK')


def cpu():
    with open('/proc/stat') as fh:
        v = [int(x) for x in fh.readline().split()[1:]]
    return sum(v), v[3] + v[4]


def temp():
    for z in sorted(os.listdir('/sys/class/thermal')):
        if z.startswith('thermal_zone'):
            try:
                with open('/sys/class/thermal/%s/temp' % z) as fh:
                    return int(fh.read()) / 1000.0
            except (IOError, OSError):
                pass
    return 0.0


srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('', port))
srv.listen(1)
srv.settimeout(secs)
print('listening on %d' % port, flush=True)

try:
    conn, addr = srv.accept()
except socket.timeout:
    print('no connection', flush=True)
    sys.exit(1)
conn.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)

buf = bytearray(4 << 20)
view = memoryview(buf)
rows = []
t0 = time.time()
mark = t0
bytes_total = 0
at_mark = 0
pt, pi = cpu()

while time.time() - t0 < secs:
    try:
        n = conn.recv_into(view, len(buf))
    except (ConnectionResetError, OSError):
        break
    if not n:
        break
    bytes_total += n
    now = time.time()
    if now - mark >= 1.0:
        nt, ni = cpu()
        dt, di = nt - pt, ni - pi
        rows.append((now - t0,
                     (bytes_total - at_mark) / (now - mark) / 1e6,
                     100.0 * (dt - di) / dt if dt else 0.0,
                     temp()))
        pt, pi = nt, ni
        mark, at_mark = now, bytes_total

with open(out, 'w') as fh:
    fh.write('t,rx_MBps,cpu_pct,temp_c\n')
    for r in rows:
        fh.write('%.2f,%.1f,%.2f,%.1f\n' % r)
print('received %.2f GB, wrote %d rows' % (bytes_total / 1e9, len(rows)), flush=True)
