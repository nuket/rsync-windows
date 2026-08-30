"""Raw TCP throughput, no protocol: one side sinks, the other sources.

  tcp.py serve <port> [streams]   accept `streams` connections; each connection:
                                  first byte 'R' -> we RECEIVE until the peer closes,
                                  'S' -> we SEND `n` bytes (8-byte length follows).
  tcp.py send <host> <port> <bytes> [streams] [bind]   push bytes to a serving peer
  tcp.py recv <host> <port> <bytes> [streams] [bind]   pull bytes from a serving peer

Prints MB/s (10^6) for the aggregate.  4 MB socket writes, 4 MB recv buffers,
one thread per stream.  Works on Python 3.6+ on either OS."""
import socket, struct, sys, threading, time

CHUNK = 4 << 20
ZEROS = bytes(CHUNK)

def set_bufs(s):
    for opt in (socket.SO_SNDBUF, socket.SO_RCVBUF):
        try:
            s.setsockopt(socket.SOL_SOCKET, opt, 8 << 20)
        except OSError:
            pass

def pump_in(conn):
    total = 0
    buf = bytearray(CHUNK)
    view = memoryview(buf)
    while True:
        n = conn.recv_into(view, CHUNK)
        if not n:
            break
        total += n
    return total

def pump_out(conn, nbytes):
    sent = 0
    while sent < nbytes:
        k = min(CHUNK, nbytes - sent)
        conn.sendall(ZEROS[:k] if k != CHUNK else ZEROS)
        sent += k
    return sent

def serve(port, streams):
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("", port))
    ls.listen(streams)
    conns = [ls.accept()[0] for _ in range(streams)]
    ls.close()
    def one(c, out):
        set_bufs(c)
        hdr = c.recv(9)
        if hdr[0:1] == b"R":
            out.append(pump_in(c))
        else:
            n = struct.unpack("!Q", hdr[1:9])[0]
            out.append(pump_out(c, n))
        c.close()
    out = []
    ts = [threading.Thread(target=one, args=(c, out)) for c in conns]
    t0 = time.time()
    for t in ts: t.start()
    for t in ts: t.join()
    el = time.time() - t0
    print(f"serve: {sum(out) / 1e6 / el:.0f} MB/s over {el:.2f}s, {streams} stream(s)")

def client(mode, host, port, nbytes, streams, bind):
    per = nbytes // streams
    def one(out):
        s = socket.socket()
        set_bufs(s)
        if bind:
            s.bind((bind, 0))
        s.settimeout(5)          # a blocked port fails in 5 s, not 21
        s.connect((host, port))
        s.settimeout(None)
        if mode == "send":
            s.sendall(b"R" + bytes(8))
            out.append(pump_out(s, per))
            s.shutdown(socket.SHUT_WR)
            s.recv(1)
        else:
            s.sendall(b"S" + struct.pack("!Q", per))
            out.append(pump_in(s))
        s.close()
    out = []
    ts = [threading.Thread(target=one, args=(out,)) for _ in range(streams)]
    t0 = time.time()
    for t in ts: t.start()
    for t in ts: t.join()
    el = time.time() - t0
    print(f"{mode}: {sum(out) / 1e6 / el:.0f} MB/s over {el:.2f}s, {streams} stream(s)")

if __name__ == "__main__":
    a = sys.argv[1:]
    if a[0] == "serve":
        serve(int(a[1]), int(a[2]) if len(a) > 2 else 1)
    else:
        client(a[0], a[1], int(a[2]), int(a[3]), int(a[4]) if len(a) > 4 else 1,
               a[5] if len(a) > 5 else None)
