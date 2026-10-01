"""Include-graph audit between modules (top-level dirs; engine/<sub> split)."""
import os, re, collections, sys
ROOT = r'C:\Rufflerim\meadows'
TOP = ['engine', 'data', 'world', 'gameplay', 'script', 'quest', 'game', 'tools', 'tests']
inc_re = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.M)

def module_of(rel):
    parts = rel.replace('\\', '/').split('/')
    if parts[0] == 'engine' and len(parts) > 2:
        return 'engine/' + parts[1]
    if parts[0] == 'engine':
        return 'engine/(root)'
    if parts[0] in ('game',) and len(parts) > 2:
        return 'game/' + parts[1]
    return parts[0]

files = []
for top in TOP:
    for dp, dn, fn in os.walk(os.path.join(ROOT, top)):
        if 'shaders' in dp or 'data\\base' in dp or 'data/base' in dp:
            continue
        for f in fn:
            if f.endswith(('.hpp', '.cpp', '.h', '.inl')):
                files.append(os.path.relpath(os.path.join(dp, f), ROOT).replace('\\', '/'))
known_prefixes = tuple(t + '/' for t in TOP)
edges = collections.Counter()
examples = collections.defaultdict(list)
loc = collections.Counter()
nfiles = collections.Counter()
for rel in files:
    mod = module_of(rel)
    nfiles[mod] += 1
    try:
        src = open(os.path.join(ROOT, rel), encoding='utf-8', errors='replace').read()
    except OSError:
        continue
    loc[mod] += src.count('\n')
    for inc in inc_re.findall(src):
        inc = inc.replace('\\', '/')
        if not inc.startswith(known_prefixes):
            continue
        tgt = module_of(inc)
        if tgt == mod:
            continue
        edges[(mod, tgt)] += 1
        if len(examples[(mod, tgt)]) < 3:
            examples[(mod, tgt)].append('%s -> %s' % (rel, inc))

mods = sorted(set(nfiles) | {m for e in edges for m in e})
# intended rank (lower = lower layer)
rank = {
    'engine/core': 0, 'engine/platform': 0, 'engine/reflect': 1, 'engine/ecs': 2,
    'engine/rhi': 2, 'engine/assets': 2, 'engine/terrain': 2, 'engine/nav': 2,
    'engine/anim': 3, 'engine/physics': 3, 'engine/audio': 3, 'engine/fx': 3,
    'engine/render': 4, 'engine/ui': 4, 'engine/(root)': 5,
    'data': 2, 'world': 5, 'gameplay': 6, 'script': 7, 'quest': 7,
    'game/scenes': 9, 'game/ui': 9, 'game/(root)': 9, 'game': 9, 'tools': 10, 'tests': 11,
}
def r(m):
    if m in rank: return rank[m]
    if m.startswith('game/'): return 9
    return 5
print('== modules (files, lines)')
for m in mods:
    print('  %-16s %4d files %7d lines  rank %s' % (m, nfiles[m], loc[m], rank.get(m, '?')))
print('\n== edges (module -> module : #includes), upward or same-rank-cycle marked')
for (a, b), n in sorted(edges.items(), key=lambda kv: (kv[0][0], -kv[1])):
    flag = ''
    if r(a) < r(b): flag = '  <-- UPWARD'
    elif edges.get((b, a)): flag = '  <-- BIDIRECTIONAL (%d back)' % edges[(b, a)]
    print('  %-16s -> %-16s %4d%s' % (a, b, n, flag))
print('\n== upward edges with examples')
for (a, b), n in sorted(edges.items()):
    if r(a) < r(b):
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
# cycles (Tarjan SCC)
graph = collections.defaultdict(set)
for (a, b) in edges:
    graph[a].add(b)
index = {}; low = {}; stack = []; on = set(); sccs = []; counter = [0]
def strong(v):
    index[v] = low[v] = counter[0]; counter[0] += 1; stack.append(v); on.add(v)
    for w in graph[v]:
        if w not in index:
            strong(w); low[v] = min(low[v], low[w])
        elif w in on:
            low[v] = min(low[v], index[w])
    if low[v] == index[v]:
        comp = []
        while True:
            w = stack.pop(); on.discard(w); comp.append(w)
            if w == v: break
        if len(comp) > 1: sccs.append(sorted(comp))
sys.setrecursionlimit(10000)
for v in list(graph):
    if v not in index: strong(v)
print('\n== strongly connected components (cycles)')
for c in sccs:
    print('  ' + ' <-> '.join(c))
