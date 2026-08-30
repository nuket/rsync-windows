"""Where the wall clock goes, from an xperf dumper CSV.

  python waits.py push2-dump.csv

For every thread of rsync.exe / ssh.exe: how long it ran, how long it was
blocked and on what, how often it was woken and by whom, and -- the number
that matters on a latency-bound pipeline -- how long it sat *ready* before a
CPU actually picked it up.
"""
import sys, csv, collections

PROCS = ('rsync.exe', 'ssh.exe')

def pname(field):
    # "rsync.exe ( 1234)" -> ("rsync.exe", 1234)
    f = field.strip()
    if '(' not in f:
        return f, None
    n, _, p = f.rpartition('(')
    return n.strip(), p.strip().rstrip(')').strip()

def main(path):
    run_start = {}          # tid -> ts it began running
    ran = collections.Counter()          # tid -> running us
    blocked = collections.Counter()      # tid -> blocked us
    blocks = collections.Counter()       # tid -> count of block episodes
    off_at = {}             # tid -> (ts, waitreason) when switched out blocked
    reason_us = collections.defaultdict(collections.Counter)   # tid -> reason -> us
    ready_at = {}           # tid -> ts it was made ready
    ready_delay = collections.defaultdict(list)   # tid -> [us ready->running]
    waker = collections.defaultdict(collections.Counter)       # tid -> waker label
    tid_name = {}
    first_ts = None
    last_ts = None

    with open(path, newline='', errors='replace') as fh:
        for row in csv.reader(fh):
            if not row:
                continue
            kind = row[0].strip()
            if kind == 'CSwitch':
                try:
                    ts = int(row[1])
                except (ValueError, IndexError):
                    continue
                if first_ts is None:
                    first_ts = ts
                last_ts = ts
                nproc, _ = pname(row[2]); ntid = row[3].strip()
                oproc, _ = pname(row[8]); otid = row[9].strip()
                ostate = row[12].strip() if len(row) > 12 else ''
                reason = row[13].strip() if len(row) > 13 else ''

                if nproc in PROCS:
                    tid_name[ntid] = nproc
                    run_start[ntid] = ts
                    if ntid in off_at:
                        t0, r = off_at.pop(ntid)
                        blocked[ntid] += ts - t0
                        blocks[ntid] += 1
                        reason_us[ntid][r] += ts - t0
                    if ntid in ready_at:
                        ready_delay[ntid].append(ts - ready_at.pop(ntid))
                if oproc in PROCS:
                    tid_name[otid] = oproc
                    if otid in run_start:
                        ran[otid] += ts - run_start.pop(otid)
                    if ostate in ('Waiting', 'Standby', 'Terminated'):
                        off_at[otid] = (ts, reason or ostate)
            elif kind == 'ReadyThread':
                try:
                    ts = int(row[1])
                except (ValueError, IndexError):
                    continue
                wproc, _ = pname(row[2]); wtid = row[3].strip()
                rproc, _ = pname(row[4]); rtid = row[5].strip()
                if rproc in PROCS:
                    ready_at.setdefault(rtid, ts)
                    waker[rtid][f'{wproc}/{wtid}'] += 1

    span = (last_ts - first_ts) / 1e6 if first_ts else 0
    print(f'trace span {span:.3f} s\n')
    interesting = sorted(tid_name, key=lambda t: -(ran[t] + blocked[t]))
    for tid in interesting:
        tot = ran[tid] + blocked[tid]
        if tot < 200_000:          # ignore threads alive for < 0.2 s of the run
            continue
        d = ready_delay[tid]
        d_sorted = sorted(d)
        med = d_sorted[len(d)//2] if d else 0
        p95 = d_sorted[int(len(d)*0.95)] if d else 0
        print(f'--- {tid_name[tid]} tid {tid} ---')
        print(f'    running {ran[tid]/1e6:7.3f} s   blocked {blocked[tid]/1e6:7.3f} s'
              f'   ({100*ran[tid]/tot:4.1f}% busy over {tot/1e6:.2f} s)')
        print(f'    block episodes {blocks[tid]:7d}   mean block '
              f'{blocked[tid]/max(blocks[tid],1):8.1f} us')
        if d:
            print(f'    ready->running: n={len(d)} median {med} us  p95 {p95} us'
                  f'  total {sum(d)/1e6:.3f} s')
        top_r = reason_us[tid].most_common(3)
        if top_r:
            print('    blocked on: ' + ', '.join(
                f'{r} {v/1e6:.3f}s' for r, v in top_r))
        top_w = waker[tid].most_common(3)
        if top_w:
            print('    woken by:   ' + ', '.join(f'{w} x{c}' for w, c in top_w))
        print()

main(sys.argv[1])
