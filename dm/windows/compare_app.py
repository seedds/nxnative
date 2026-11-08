#!/usr/bin/env python3
# compare_app.py <app_hashes.tsv> <reference .app>: each file of the app made on Windows against the real Mac app
import hashlib, os, sys
ref = os.path.dirname(os.path.abspath(sys.argv[2])); name = os.path.basename(sys.argv[2])
win = {}
for l in open(sys.argv[1]):
    x = l.rstrip('\n').split('\t')
    if len(x) == 3: win[x[2]] = (x[0], int(x[1]))
def sha(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    return h.hexdigest()
have = {}
for r, _, fs in os.walk(sys.argv[2]):
    for n in fs: q = os.path.join(r, n); have[os.path.relpath(q, ref)] = q
same = [k for k in have if k in win and win[k][0] == sha(have[k])]
print(f'real app files {len(have)}; byte-identical {len(same)}; different {sum(k in win and k not in same for k in have)}; missing {sum(k not in win for k in have)}; only on Windows {sum(k not in have for k in win)}')
for k in sorted(have):
    if k not in win: print('  MISSING', k.replace(name + '/', '')[:90])
    elif k not in same: print('  DIFFERS', k.replace(name + '/', '')[:90], win[k][1], os.path.getsize(have[k]))
for k in sorted(set(win) - set(have)): print('  ONLY WINDOWS', k.replace(name + '/', '')[:90])
