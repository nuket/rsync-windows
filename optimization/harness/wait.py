import io

p = 'win32/win32io.c'
s = io.open(p, encoding='utf-8').read()

old = """	/* Mixed (or pipe-only): poll, backing off as described above poll_backoff(). */
	for (;;) {"""

new = """	/*
	 * Pipes only, waiting to read: this is the shape rsync-over-ssh has
	 * whenever it is waiting for the peer, and the one that has to be
	 * fast.  Every fd here can be pumped, so wait on the pumps' events --
	 * a real block, woken the moment bytes arrive.
	 *
	 * A write fd in the set skips this: pipe writability is not something
	 * Windows reports, so crtfd_writable() calls them all ready and the
	 * loop below returns without waiting at all.
	 */
	if (!have_socks && n_crt_w == 0 && n_crt_r > 0) {
		HANDLE evts[MAXIMUM_WAIT_OBJECTS];
		int    evt_fd[MAXIMUM_WAIT_OBJECTS];
		int    n_evt = 0, unpumped = 0, j;

		for (j = 0; j < n_crt_r; j++) {
			struct pipe_pump *p = pump_for(crt_r[j], 1);

			if (!p || n_evt == MAXIMUM_WAIT_OBJECTS) {
				unpumped = 1;
				break;
			}
			evts[n_evt] = p->data_evt;
			evt_fd[n_evt++] = crt_r[j];
		}

		/* Anything a pump could not take (a console, a file) leaves the
		 * set unwaitable, so fall through to the polling loop. */
		if (!unpumped) {
			for (;;) {
				fd_set out_r;
				DWORD rc, ms;
				int count = 0;

				FD_ZERO(&out_r);
				for (j = 0; j < n_evt; j++) {
					if (crtfd_readable(evt_fd[j])) {
						FD_SET((SOCKET)evt_fd[j], &out_r);
						count++;
					}
				}
				if (count) {
					TRACE("select -> %d (pump)\\n", count);
					if (rfds) *rfds = out_r;
					if (wfds) FD_ZERO(wfds);
					if (efds) FD_ZERO(efds);
					return count;
				}

				if (infinite)
					ms = INFINITE;
				else {
					DWORD now = GetTickCount();
					if ((long)(now - deadline) >= 0)
						ms = 0;
					else
						ms = deadline - now;
				}

				rc = WaitForMultipleObjects((DWORD)n_evt, evts, FALSE, ms);
				if (rc == WAIT_TIMEOUT) {
					if (rfds) FD_ZERO(rfds);
					if (wfds) FD_ZERO(wfds);
					if (efds) FD_ZERO(efds);
					return 0;
				}
				if (rc == WAIT_FAILED) {
					errno = EIO;
					return -1;
				}
			}
		}
	}

	/* Mixed, or a set a pump could not cover: poll, backing off as
	 * described above poll_backoff(). */
	for (;;) {"""

assert old in s
s = s.replace(old, new, 1)

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('wait path added')
