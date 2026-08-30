"""Summarise an `xperf -a stack -butterfly` HTML report.

  python bfly.py push2-stack-rsync.txt [--top 25] [--callers SYMBOL]

Prints the exclusive-hit table (where the CPU actually was) and, with
--callers, the butterfly entry for one function so a leaf frame can be
attributed to whoever called it.
"""
import re, sys, html, argparse

def tables(doc):
    """Yield (anchor_id, heading, [rows]) for each table in the report."""
    out = []
    for m in re.finditer(r"<a id='(\w+)'><h2>(.*?)</h2></a>(.*?)</table>", doc, re.S):
        anchor, head, body = m.group(1), m.group(2), m.group(3)
        rows = []
        for tr in re.findall(r'<tr[^>]*>(.*?)</tr>', body, re.S):
            cells = [html.unescape(re.sub(r'<[^>]+>', '', c)).strip()
                     for c in re.findall(r'<t[dh][^>]*>(.*?)</t[dh]>', tr, re.S)]
            if cells:
                rows.append(cells)
        out.append((anchor, head, rows))
    return out

def show(rows, top, title):
    if not rows:
        return
    hdr, data = rows[0], rows[1:]
    print(f"\n=== {title} ===")
    for r in data[:top]:
        print('  ' + '  '.join(f'{c:>10}' if i else f'{c:<62}'
                               for i, c in enumerate(r[:4])))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--callers')
    a = ap.parse_args()
    doc = open(a.path, encoding='utf-8', errors='replace').read()
    ts = tables(doc)
    by_anchor = {anchor: (head, rows) for anchor, head, rows in ts}

    for anchor in ('TblME', 'TblSE', 'TblSI'):
        if anchor in by_anchor:
            head, rows = by_anchor[anchor]
            show(rows, a.top, head)

    if a.callers:
        # the butterfly table is emitted as a run of <table> groups; pull the
        # raw text around the symbol instead of trying to re-nest it
        pat = re.compile(r'<tbody.*?</tbody>', re.S)
        blocks = pat.findall(doc)
        want = a.callers.lower()
        print(f"\n=== butterfly blocks containing {a.callers!r} ===")
        for b in blocks:
            txt = re.sub(r'<[^>]+>', '|', b)
            txt = html.unescape(txt)
            if want in txt.lower():
                lines = [l.strip(' |') for l in txt.split('\n') if l.strip(' |')]
                for l in lines[:60]:
                    print('  ' + re.sub(r'\|+', ' | ', l))
                print('  ' + '-' * 60)

main()
