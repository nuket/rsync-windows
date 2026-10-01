#!/usr/bin/env python3
"""Transfers at protocols 29 and 30 finish, and a pull's summary comes last.

A local copy pushes and a stand-in remote shell pulls, so no ssh is needed.
"""

import sys

from wintest import (FROM, RSYNC, SCRATCH, check, compare_trees,
                     make_test_tree, ok, rsync, write)

TIMEOUT = 60

make_test_tree(FROM)

# rsync runs it as `shell.py <host> rsync --server ...`.
shell = write(SCRATCH / 'shell.py',
              'import subprocess, sys\n'
              f'sys.exit(subprocess.run([{str(RSYNC)!r}, *sys.argv[3:]],\n'
              f'                        cwd={str(SCRATCH)!r},\n'
              '                        stdin=sys.stdin, stdout=sys.stdout)'
              '.returncode)\n')
rsh = f'"{sys.executable}" "{shell}"'

for protocol in (29, 30):
    push = SCRATCH / f'push-{protocol}'
    write(push / 'stale.txt', 'not in the source\n')
    rsync(f'--protocol={protocol}', '-rt', '--delete-after', f'{FROM}/',
          f'{push}/', timeout=TIMEOUT)
    compare_trees(FROM, push, f'push at protocol {protocol}')

    pull = SCRATCH / f'pull-{protocol}'
    write(pull / 'stale.txt', 'not in the source\n')
    proc = rsync(f'--protocol={protocol}', '-rtv', '--delete-after', '-e',
                 rsh, f'localhost:{FROM.name}/', f'{pull}/', timeout=TIMEOUT)
    compare_trees(FROM, pull, f'pull at protocol {protocol}')
    deleted, summary = (proc.stdout.find('deleting stale.txt'),
                        proc.stdout.find('\nsent '))
    check(0 <= deleted < summary,
          f'the pull at protocol {protocol} printed its summary before the '
          f'generator was done:\n{proc.stdout}')

ok('push and pull at protocols 29 and 30 both finished')
