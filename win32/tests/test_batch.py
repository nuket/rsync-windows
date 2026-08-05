#!/usr/bin/env python3
"""--write-batch and --only-write-batch, and the refusal of --read-batch.

Writing a batch works and is worth covering here: the batch file is a
byte-for-byte record of the wire format, so it is exactly the sort of thing
that newline translation would silently corrupt, and this port sets its
stdio to binary mode explicitly to prevent that.

Replaying one does not work.  A batch has no sender, so the generator's
index stream loops back to the receiver through a pipe; how much of it
recv_files() consumes depends on incremental recursion, which this port
forces off while receiving because the receiver is a thread sharing the
generator's heap.  The two halves then wait on each other.  rsync refuses
--read-batch up front rather than hanging (win32/win32args.c), and this
pins that refusal.

What makes --write-batch useful anyway is that the result is portable: if
RSYNC_WIN_TEST_HOST is set, the batch written here is replayed by the rsync
on that host and the result compared against the source.

Copyright (C) 2026 Max Vilimpoc, rsync CMake/Windows port.
Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
"""

import hashlib
import os
import shutil
import subprocess

from wintest import (SCRATCH, check, compare_trees, ok, pseudo_random, rsync,
                     write)

os.chdir(SCRATCH)

src = SCRATCH / 'from'
write(src / 'text.txt', 'line one\nline two\r\nline three\n')
write(src / 'binary.bin', pseudo_random(150000, seed=17))
write(src / 'sub' / 'nested.txt', 'nested\n')

# --- --write-batch transfers and records at the same time -----------------
dst = SCRATCH / 'to'
rsync('-rt', '--write-batch=BATCH', f'{src}/', f'{dst}/')
compare_trees(src, dst, '--write-batch transfer')

batch = SCRATCH / 'BATCH'
check(batch.is_file(), '--write-batch did not create the batch file')
check(batch.stat().st_size > 0, 'the batch file is empty')
# The companion script records the flags the batch was made with.
check((SCRATCH / 'BATCH.sh').is_file(),
      '--write-batch did not write the companion BATCH.sh')

# The recorded stream has to be at least as large as the data it carries;
# if stdio had been translating newlines the size would not add up.
check(batch.stat().st_size > 150000,
      f'the batch file is too small to hold the data: {batch.stat().st_size}')

# --- --only-write-batch records without transferring ----------------------
notdst = SCRATCH / 'never-created'
rsync('-rt', '--only-write-batch=BATCH2', f'{src}/', f'{notdst}/')
check(not notdst.exists(),
      '--only-write-batch created the destination it was told not to touch')
check((SCRATCH / 'BATCH2').is_file(),
      '--only-write-batch did not write the batch file')

# --- --read-batch is refused, clearly, and before it does anything --------
target = SCRATCH / 'replayed'
proc = rsync('-rt', '--read-batch=BATCH', f'{target}/', expect=1)
combined = proc.stdout + proc.stderr
check('--read-batch is not supported' in combined,
      f'--read-batch did not explain itself:\n{combined}')
check(not target.exists(),
      '--read-batch created a destination before refusing')

# --- the batch replays on a peer that can fork ----------------------------
host = os.environ.get('RSYNC_WIN_TEST_HOST')
if not host or not shutil.which('ssh'):
    ok('--write-batch and --only-write-batch correct; --read-batch refused '
       '(set RSYNC_WIN_TEST_HOST to also check the batch replays elsewhere)')


def ssh(command, expect=0):
    proc = subprocess.run(
        ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10',
         host, command],
        capture_output=True, text=True, encoding='utf-8', errors='replace')
    if expect is not None and proc.returncode != expect:
        ok('--write-batch and --only-write-batch correct; --read-batch '
           f'refused (ssh to {host} unavailable: {proc.stderr.strip()})')
    return proc.stdout


remote = os.environ.get('RSYNC_WIN_TEST_DIR', '/tmp/rsync-win-test') + '-batch'
ssh('true')
ssh(f'rm -rf {remote} && mkdir -p {remote}')

# Send just the batch, then let the peer replay it into an empty tree.
rsync('-t', str(batch), str(SCRATCH / 'BATCH.sh'), f'{host}:{remote}/')
ssh(f'cd {remote} && rsync -rt --read-batch=BATCH out/')

# Compare the peer's result against the source, by digest.
listing = ssh(f'cd {remote}/out && find . -type f | sort | '
              'xargs md5sum | sed "s|  \\./|  |"')
remote_digests = {}
for line in listing.splitlines():
    digest, _, name = line.partition('  ')
    if name:
        remote_digests[name.strip()] = digest.strip()

local_digests = {}
for dirpath, _dirnames, filenames in os.walk(src):
    for name in filenames:
        full = os.path.join(dirpath, name)
        rel = os.path.relpath(full, src).replace('\\', '/')
        local_digests[rel] = hashlib.md5(open(full, 'rb').read()).hexdigest()

check(remote_digests == local_digests,
      'the batch replayed on the peer did not reproduce the source:\n'
      f'  local : {sorted(local_digests.items())}\n'
      f'  remote: {sorted(remote_digests.items())}')

ok('--write-batch round-trips, replays correctly on a forking rsync, and '
   '--read-batch is refused with an explanation')
