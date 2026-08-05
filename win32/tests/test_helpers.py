#!/usr/bin/env python3
"""The C test helpers that are pure path/string arithmetic.

CMake builds these on Windows but nothing was driving them, which left
t_secure_relpath -- a security-relevant path validator -- compiled and
untested.  Both run unmodified here: neither touches anything Unix-specific.

t_chmod_secure is deliberately not driven.  Its whole subject is the
symlink-TOCTOU primitive behind CVE-2026-29518: every case either builds a
parent symlink that escapes the tree or checks that a legitimate one still
resolves.  A build without symlink support cannot set any of that up, so
running it would report failures that say nothing about this port.

Copyright (C) 2026 Max Vilimpoc, rsync CMake/Windows port.
Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
"""

from wintest import SCRATCH, check, helper, ok

# --- t_secure_relpath ------------------------------------------------------
# secure_relative_open()'s front-door check must reject every spelling of a
# path that climbs out of the receiver's confinement, with -1/EINVAL.  On
# Windows there is no AT_FDCWD, so syscall.c takes the portable fallback --
# which is precisely the path that has no kernel-side RESOLVE_BENEATH to
# catch what the front door misses.  So this matters more here, not less.
relpath_dir = SCRATCH / 'relpath'
relpath_dir.mkdir(parents=True, exist_ok=True)

proc = helper('t_secure_relpath', relpath_dir)
report = proc.stdout + proc.stderr   # it writes its case log to stderr
check('[relpath=..' in report,
      f"t_secure_relpath did not report the bare '..' case:\n{report}")
check('FAIL' not in report,
      f"t_secure_relpath reported a failure:\n{report}")

# --- trimslash -------------------------------------------------------------
# The same inputs and expected output as testsuite/trimslash_test.py.
TRIM_INPUTS = [
    "/usr/local/bin",
    "/usr/local/bin/",
    "/usr/local/bin///",
    "//a//",
    "////",
    "/Users/Weird Macintosh Name/// Ooh, translucent plastic/",
]
TRIM_EXPECTED = """\
/usr/local/bin
/usr/local/bin
/usr/local/bin
//a
/
/Users/Weird Macintosh Name/// Ooh, translucent plastic
"""

proc = helper('trimslash', *TRIM_INPUTS)
got = proc.stdout.replace('\r\n', '\n')
check(got == TRIM_EXPECTED,
      "trimslash output did not match:\n"
      f"--- expected ---\n{TRIM_EXPECTED}--- got ---\n{got}")

ok('t_secure_relpath and trimslash behave as upstream expects')
