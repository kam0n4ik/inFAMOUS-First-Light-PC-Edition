"""The smoke pipeline from a command trace (SSPC_DIAG_CMD_TRACE; docs/PHASE4_SMOKE_PIPELINE.md, SSPC-020).

Usage: py -I tools/smoke_trace.py <cmd_trace.txt> [--vs 2edac4000] [--ps 2edad2700] [--depth 6]
Finds the draws whose VS/PS (PGM address, footer hash or content hash) match, then walks back through the frame: for
each guest range a command reads, the latest earlier command that writes an overlapping range (and, for that writer,
its own inputs, to --depth levels). Prints per frame the smoke draws and the chain of producers with their identities,
queues, ordinals, groups and the ranges that link them; and per program how often it ran in the traced frames.
"""
import re
import sys
from collections import Counter, defaultdict

LINE = re.compile(r'^f(\d+) #(\d+) ([DXS]) q(\d+) seq (\d+) @([0-9a-f]+) b(\d+) gb(\d+)(.*)$')


def parse(path):
    cmds = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for raw in f:
            if raw.startswith('#'):
                continue
            m = LINE.match(raw.rstrip('\n'))
            if not m:
                continue
            frame, ordinal, kind, q, seq, packet, batch, gb, rest = m.groups()
            parts = rest.split(' | ')
            head = parts[0]
            c = {'frame': int(frame), 'ord': int(ordinal), 'kind': kind, 'q': int(q), 'seq': int(seq),
                 'packet': packet, 'batch': int(batch), 'gb': int(gb), 'head': head.strip(), 'reads': [], 'writes': [],
                 'tex': [], 'idx': len(cmds)}
            mm = re.search(r'(vs|cs) ([0-9a-f]+)/([0-9a-f]+)/([0-9a-f]+)', head)
            if mm:
                c['s0'] = (mm.group(2), mm.group(3), mm.group(4))
            mm = re.search(r' ps ([0-9a-f]+)/([0-9a-f]+)/([0-9a-f]+)', head)
            if mm:
                c['s1'] = (mm.group(1), mm.group(2), mm.group(3))
            for p in parts[1:]:
                rm = re.match(r'R(\d)(W?) ([0-9a-f]+)\+([0-9a-f]+)', p)
                if rm:
                    a, s = int(rm.group(3), 16), int(rm.group(4), 16)
                    if a == 0:
                        a = 1 << 48  # GDS: a range of its own
                    (c['writes'] if rm.group(2) else c['reads']).append((a, a + s, int(rm.group(1))))
                    continue
                tm = re.match(r'T ([0-9a-f]+):(\d+)( S)?', p)
                if tm:
                    c['tex'].append((int(tm.group(1), 16), int(tm.group(2)), bool(tm.group(3))))
            # a draw's colour targets are written; textures are read
            for rt in re.findall(r' rt\d ([0-9a-f]+):(\d+)', head):
                a = int(rt[0], 16)
                c['writes'].append((a, a + 1, 9))
            cmds.append(c)
    return cmds


def matches(c, key, value):
    s = c.get(key)
    return s is not None and value in s


def ident(c):
    if c['kind'] == 'X':
        return f"cs {c['s0'][0]} (footer {c['s0'][1]})"
    if c['kind'] == 'D':
        return f"draw vs {c['s0'][0]} ps {c.get('s1', ('0',))[0]}"
    return 'skipped'


def overlap(a, b):
    return a[0] < b[1] and b[0] < a[1]


def producers(cmds, c, depth, seen, out, level=1):
    if depth == 0:
        return
    inputs = list(c['reads']) + [(t[0], t[0] + 1, 8) for t in c['tex']]
    for r in inputs:
        # the latest earlier command (any queue, the trace's recording order) writing an overlapping range
        for j in range(c['idx'] - 1, -1, -1):
            w = cmds[j]
            if w['kind'] == 'S':
                continue
            hit = next((x for x in w['writes'] if overlap(x, r)), None)
            if hit is None:
                continue
            key = (w['idx'], r[0])
            if key not in seen:
                seen.add(key)
                out.append((level, w, r, hit))
                if w['idx'] not in {x[1]['idx'] for x in out[:-1]}:
                    producers(cmds, w, depth - 1, seen, out, level + 1)
            break


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    path = argv[1]
    vs, ps, depth = '2edac4000', '2edad2700', 6
    i = 2
    while i < len(argv):
        if argv[i] == '--vs':
            vs = argv[i + 1]
        elif argv[i] == '--ps':
            ps = argv[i + 1]
        elif argv[i] == '--depth':
            depth = int(argv[i + 1])
        i += 2
    cmds = parse(path)
    frames = sorted({c['frame'] for c in cmds})
    print(f'{path}: {len(cmds)} commands, frames {frames}')
    kinds = Counter((c['frame'], c['kind']) for c in cmds)
    for fr in frames:
        print(f'  frame {fr}: draws {kinds[(fr, "D")]} dispatches {kinds[(fr, "X")]} not executed {kinds[(fr, "S")]}')
    skipped = [c for c in cmds if c['kind'] == 'S']
    for c in skipped[:20]:
        print(f'  not executed f{c["frame"]} #{c["ord"]}: {c["head"]}')
    smoke = [c for c in cmds if c['kind'] == 'D' and (matches(c, 's0', vs) or matches(c, 's1', ps))]
    print(f'smoke draws (vs {vs} / ps {ps}): {len(smoke)}')
    for c in smoke:
        print(f'\n== f{c["frame"]} #{c["ord"]} q{c["q"]} b{c["batch"]} gb{c["gb"]} {c["head"]}')
        for r in c['reads']:
            print(f'   reads  R{r[2]} {r[0]:#x}+{r[1] - r[0]:#x}')
        for t in c['tex']:
            print(f'   tex    {t[0]:#x} fmt {t[1]}{" storage" if t[2] else ""}')
        out = []
        producers(cmds, c, depth, set(), out)
        for level, w, r, hit in out:
            print(f'   {"  " * level}<- f{w["frame"]} #{w["ord"]} q{w["q"]} b{w["batch"]} gb{w["gb"]} {ident(w)} '
                  f'{w["head"].split(" groups ")[-1] if w["kind"] == "X" else ""} writes {hit[0]:#x}+{hit[1] - hit[0]:#x} '
                  f'(read {r[0]:#x}+{r[1] - r[0]:#x} as R{r[2]})')
    # every program writing a range the smoke reads, per frame
    if smoke:
        reads = [r for c in smoke for r in c['reads']]
        writers = defaultdict(lambda: Counter())
        for c in cmds:
            if c['kind'] != 'X':
                continue
            for w in c['writes']:
                if any(overlap(w, r) for r in reads):
                    writers[c['s0'][0]][c['frame']] += 1
                    break
        print('\ncompute programs writing what the smoke draws read (dispatches per frame):')
        for k, v in sorted(writers.items(), key=lambda kv: -sum(kv[1].values())):
            print(f'  cs {k}: ' + ', '.join(f'f{fr} {n}' for fr, n in sorted(v.items())))


if __name__ == '__main__':
    main(sys.argv)
