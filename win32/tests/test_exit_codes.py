#!/usr/bin/env python3
"""A push or pull whose receiver cannot replace a file exits 23, else 0.

An open Python file has no FILE_SHARE_DELETE, so the rename over it fails.
"""

import sys

from wintest import (FROM, RSYNC, SCRATCH, compare_trees, ok, rsync,
                     write)

TIMEOUT = 60

write(FROM / 'busy.txt', 'the new version\n')
write(FROM / 'free.txt', 'nothing holds this one\n')

# rsync runs it as `shell.py <host> rsync --server ...`.
shell = write(SCRATCH / 'shell.py',
              'import subprocess, sys\n'
              f'sys.exit(subprocess.run([{str(RSYNC)!r}, *sys.argv[3:]],\n'
              f'                        cwd={str(SCRATCH)!r},\n'
              '                        stdin=sys.stdin, stdout=sys.stdout)'
              '.returncode)\n')
rsh = f'"{sys.executable}" "{shell}"'

for direction in ('push', 'pull'):
    dst = SCRATCH / direction
    busy = write(dst / 'busy.txt', 'old\n')
    args = ([f'{FROM}/', f'{dst}/'] if direction == 'push'
            else ['-e', rsh, f'localhost:{FROM.name}/', f'{dst}/'])
    with open(busy, 'rb'):
        rsync('-rt', *args, expect=23, timeout=TIMEOUT)
    rsync('-rt', *args, timeout=TIMEOUT)
    compare_trees(FROM, dst, f'{direction} once nothing held the file')

ok('a push and a pull that could not replace a file both exited 23, '
   'and 0 once they could')
