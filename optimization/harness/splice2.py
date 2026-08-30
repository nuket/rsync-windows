import io
path = r'C:\Users\Claude\devsrc\rsync-windows\openssh\contrib\win32\win32compat\termio.c'
src = io.open(path, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in src else '\n'
lines = src.split(nl)
start = next(i for i, l in enumerate(lines) if 'A pipe or a file on a sync fd is bulk data' in l) - 1
assert lines[start].strip() == '/*', lines[start]
end = next(i for i, l in enumerate(lines) if l.startswith('BOOL isFirstTime = TRUE;'))
new = io.open(r'C:\Users\Claude\AppData\Local\Temp\claude\C--Users-Claude-devsrc-rsync-windows\66c00488-88a8-48c3-be31-6abc940b8590\scratchpad\new_pumps.c', encoding='utf-8').read().rstrip('\n').split('\n')
lines[start:end] = new + ['']
io.open(path, 'w', encoding='utf-8', newline='').write(nl.join(lines))
print('replaced lines', start + 1, 'to', end, 'with', len(new), 'lines; eol', repr(nl))
