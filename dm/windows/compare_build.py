#!/usr/bin/env python3
# compare_build.py <folder holding build/ and results/, or hashes.tsv> <reference build folder>: every file a build made against the real Mac build of the same game
import collections, hashlib, os, sys
a, R = sys.argv[1:3]
h = a if a.endswith('.tsv') else os.path.join(a, 'results/hashes.tsv')
win = {}
for l in open(h):
    x = l.rstrip('\n').split('\t')
    if len(x) == 3: win[x[2]] = (x[0], int(x[1]))
def sha(p):
    d = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): d.update(b)
    return d.hexdigest()
ref = {}
for r, _, fs in os.walk(R):
    for n in fs:
        q = os.path.join(r, n); rel = os.path.relpath(q, R)
        if not rel.startswith(('logs/', 'tools/', 'pso/', 'app/')) and n not in ('keys.txt', 'prod.keys'): ref[rel] = q
same, diff, gone = collections.Counter(), collections.defaultdict(list), collections.defaultdict(list)
top = lambda r: '/'.join(r.split('/')[:2])
for rel, q in sorted(ref.items()):
    if rel not in win: gone[top(rel)].append(rel); continue
    if os.path.getsize(q) == win[rel][1] and sha(q) == win[rel][0]: same[top(rel)] += 1
    else: diff[top(rel)].append(rel)
extra = collections.defaultdict(list)
for rel in win:
    if rel not in ref and not rel.startswith(('logs/', 'tools/')): extra[top(rel)].append(rel)
print(f'files in the Mac build {len(ref)}; byte-identical {sum(same.values())}; different {sum(map(len, diff.values()))}; missing {sum(map(len, gone.values()))}; only in this build {sum(map(len, extra.values()))}')
for k in sorted(set(same) | set(diff) | set(gone)):
    print(f'  {k}: same {same[k]}, different {len(diff[k])}, missing {len(gone[k])}')
    for n in (diff[k] + gone[k])[:6]: print('      ', 'DIFF' if n in diff[k] else 'MISS', n[:100])
for k, v in extra.items(): print('  only here', k, len(v), v[0][:90])
