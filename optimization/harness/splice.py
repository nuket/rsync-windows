import io
src = r'C:\Users\Claude\devsrc\rsync-windows\match.c'
sp = r'C:\Users\Claude\AppData\Local\Temp\claude\C--Users-Claude-devsrc-rsync-windows\66c00488-88a8-48c3-be31-6abc940b8590\scratchpad'
lines = io.open(src, encoding='utf-8', newline='').read().split('\n')
assert lines[68].startswith('static uint32 tablesize;'), lines[68]
assert lines[109] == '}', repr(lines[109])
assert lines[161].startswith('static void hash_search('), lines[161]
assert lines[376] == '}', repr(lines[376])
table = io.open(sp + r'\new_table.c', encoding='utf-8').read().rstrip('\n').split('\n')
search = io.open(sp + r'\new_search.c', encoding='utf-8').read().rstrip('\n').split('\n')
lines[161:377] = search
lines[68:110] = table
io.open(src, 'w', encoding='utf-8', newline='').write('\n'.join(lines))
print('spliced', len(lines), 'lines')
