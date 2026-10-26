"""Include-graph audit between modules (top-level dirs; engine/<sub> split).

Reference: docs/ARCHITECTURE-LAYERS.md. Each module has an intended rank
(lower = lower layer); an include from a lower-ranked module into a
higher-ranked one is an UPWARD edge, and any strongly connected component
of the module graph is a CYCLE. Both are layer violations.

    python tools/scripts/layers_audit.py                  # the full report
    python tools/scripts/layers_audit.py --baseline FILE  # report + verdict:
        exit 1 on any violation missing from FILE, or any FILE line whose
        violation no longer exists (a resolved cycle removes its line)
    python tools/scripts/layers_audit.py --write-baseline FILE  # regenerate

Baseline format: one violation per line, "A -> B" (upward edge) or
"A <-> B [<-> C ...]" (cycle, members sorted); everything after '#' is a
comment — write the causing include there, so the line documents itself.
"""
import argparse
import collections
import os
import re
import sys

TOP = ['engine', 'data', 'world', 'gameplay', 'script', 'quest', 'game', 'tools', 'tests']
INC_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.M)

# Intended rank (lower = lower layer). The measured stack is
# data -> gameplay -> world -> script/quest -> game (CLAUDE.md §4): the
# spawner wires GAS components, so world sits ABOVE gameplay.
RANK = {
    'engine/core': 0, 'engine/platform': 0, 'engine/reflect': 1, 'engine/ecs': 2,
    'engine/rhi': 2, 'engine/assets': 2, 'engine/terrain': 2, 'engine/nav': 2,
    'engine/dungeon': 2,
    'engine/anim': 3, 'engine/physics': 3, 'engine/audio': 3, 'engine/fx': 3,
    'engine/render': 4, 'engine/ui': 4, 'engine/(root)': 5,
    'data': 2, 'gameplay': 5, 'world': 6, 'script': 7, 'quest': 7,
    'game/scenes': 9, 'game/ui': 9, 'game/(root)': 9, 'game': 9, 'tools': 10, 'tests': 11,
}


def rank_of(module):
    if module in RANK:
        return RANK[module]
    if module.startswith('game/'):
        return 9
    return 5


def module_of(rel):
    parts = rel.replace('\\', '/').split('/')
    if parts[0] == 'engine' and len(parts) > 2:
        return 'engine/' + parts[1]
    if parts[0] == 'engine':
        return 'engine/(root)'
    if parts[0] in ('game',) and len(parts) > 2:
        return 'game/' + parts[1]
    return parts[0]


def scan(root):
    """Returns (edges Counter[(a, b)], examples {(a, b): [str]}, nfiles, loc)."""
    files = []
    for top in TOP:
        for dp, _dn, fn in os.walk(os.path.join(root, top)):
            if 'shaders' in dp or 'data\\base' in dp or 'data/base' in dp:
                continue
            for f in fn:
                if f.endswith(('.hpp', '.cpp', '.h', '.inl')):
                    files.append(os.path.relpath(os.path.join(dp, f), root).replace('\\', '/'))
    known_prefixes = tuple(t + '/' for t in TOP)
    edges = collections.Counter()
    examples = collections.defaultdict(list)
    loc = collections.Counter()
    nfiles = collections.Counter()
    for rel in files:
        mod = module_of(rel)
        nfiles[mod] += 1
        try:
            src = open(os.path.join(root, rel), encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        loc[mod] += src.count('\n')
        for inc in INC_RE.findall(src):
            inc = inc.replace('\\', '/')
            if not inc.startswith(known_prefixes):
                continue
            tgt = module_of(inc)
            if tgt == mod:
                continue
            edges[(mod, tgt)] += 1
            if len(examples[(mod, tgt)]) < 3:
                examples[(mod, tgt)].append('%s -> %s' % (rel, inc))
    return edges, examples, nfiles, loc


def strongly_connected(edges):
    graph = collections.defaultdict(set)
    for (a, b) in edges:
        graph[a].add(b)
    index = {}
    low = {}
    stack = []
    on = set()
    sccs = []
    counter = [0]

    def strong(v):
        index[v] = low[v] = counter[0]
        counter[0] += 1
        stack.append(v)
        on.add(v)
        for w in graph[v]:
            if w not in index:
                strong(w)
                low[v] = min(low[v], low[w])
            elif w in on:
                low[v] = min(low[v], index[w])
        if low[v] == index[v]:
            comp = []
            while True:
                w = stack.pop()
                on.discard(w)
                comp.append(w)
                if w == v:
                    break
            if len(comp) > 1:
                sccs.append(sorted(comp))

    sys.setrecursionlimit(10000)
    for v in list(graph):
        if v not in index:
            strong(v)
    return sccs


def violations(edges, sccs):
    """The set of baseline keys: 'A -> B' for upward edges, 'A <-> B...' for cycles."""
    keys = set()
    for (a, b) in edges:
        if rank_of(a) < rank_of(b):
            keys.add('%s -> %s' % (a, b))
    for comp in sccs:
        keys.add(' <-> '.join(comp))
    return keys


def report(edges, examples, nfiles, loc, sccs):
    mods = sorted(set(nfiles) | {m for e in edges for m in e})
    print('== modules (files, lines)')
    for m in mods:
        print('  %-16s %4d files %7d lines  rank %s' % (m, nfiles[m], loc[m], RANK.get(m, '?')))
    print('\n== edges (module -> module : #includes), upward or same-rank-cycle marked')
    for (a, b), n in sorted(edges.items(), key=lambda kv: (kv[0][0], -kv[1])):
        flag = ''
        if rank_of(a) < rank_of(b):
            flag = '  <-- UPWARD'
        elif edges.get((b, a)):
            flag = '  <-- BIDIRECTIONAL (%d back)' % edges[(b, a)]
        print('  %-16s -> %-16s %4d%s' % (a, b, n, flag))
    print('\n== upward edges with examples')
    for (a, b), n in sorted(edges.items()):
        if rank_of(a) < rank_of(b):
            print('  %s -> %s (%d)' % (a, b, n))
            for e in examples[(a, b)]:
                print('      ' + e)
    print('\n== bidirectional pairs with examples')
    seen = set()
    for (a, b), n in sorted(edges.items()):
        if (b, a) in edges and (b, a) not in seen:
            seen.add((a, b))
            print('  %s <-> %s  (%d / %d)' % (a, b, n, edges[(b, a)]))
            for e in examples[(a, b)][:2] + examples[(b, a)][:2]:
                print('      ' + e)
    print('\n== strongly connected components (cycles)')
    for c in sccs:
        print('  ' + ' <-> '.join(c))


def read_baseline(path):
    keys = set()
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.split('#', 1)[0].strip()
            if line:
                keys.add(' '.join(line.split()))
    return keys


def write_baseline(path, keys, edges, examples):
    lines = [
        '# Tolerated layer violations (docs/ARCHITECTURE-LAYERS.md §5).',
        '# One per line: "A -> B" = upward include, "A <-> B" = cycle.',
        '# A violation missing here fails the layers_audit test; a line whose',
        '# violation is resolved fails it too — remove the line with the fix.',
        '',
    ]
    for key in sorted(keys):
        if ' -> ' in key and ' <-> ' not in key:
            a, b = key.split(' -> ')
            why = '; '.join(examples[(a, b)][:2])
            lines.append('%s  # %d include(s): %s' % (key, edges[(a, b)], why))
        else:
            lines.append(key)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', default=os.path.dirname(os.path.dirname(here)),
                        help='repository root (default: two levels above this script)')
    parser.add_argument('--baseline', help='tolerated violations; exit 1 on any difference')
    parser.add_argument('--write-baseline', metavar='FILE',
                        help='write the current violations as a baseline and exit')
    parser.add_argument('--quiet', action='store_true', help='verdict only, no report')
    args = parser.parse_args()

    edges, examples, nfiles, loc = scan(args.root)
    sccs = strongly_connected(edges)
    found = violations(edges, sccs)

    if not args.quiet:
        report(edges, examples, nfiles, loc, sccs)

    if args.write_baseline:
        write_baseline(args.write_baseline, found, edges, examples)
        print('\nbaseline written: %s (%d line(s))' % (args.write_baseline, len(found)))
        return 0

    if not args.baseline:
        return 0

    tolerated = read_baseline(args.baseline)
    new = sorted(found - tolerated)
    stale = sorted(tolerated - found)
    print('\n== layer verdict against %s' % args.baseline)
    for key in new:
        print('  NEW violation (not in baseline): %s' % key)
        if ' <-> ' not in key:
            a, b = key.split(' -> ')
            for e in examples[(a, b)]:
                print('      ' + e)
    for key in stale:
        print('  STALE baseline line (violation resolved — remove it): %s' % key)
    if new or stale:
        print('  FAIL: %d new, %d stale' % (len(new), len(stale)))
        return 1
    print('  OK: %d tolerated violation(s), nothing new' % len(found))
    return 0


if __name__ == '__main__':
    sys.exit(main())
