#!/usr/bin/env python3
# NVN2 compute programs: the translator's <key>.res.json, which nx2_reflect.py reads for the offsets of their storage buffers. Runs after dmsh_build.py, with c1 passed as dmsh_build.py passes it: a program with a
# jump table does not decode without it. Usage: nx2_res.py <shader map> <package dir> <layout table written by nx2_split.py> <output dir>; DOTNET_ROOT as for dmsh_build.py
import json, os, re, struct, subprocess, sys

smap, pkg, table, out = sys.argv[1:5]
XL = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dmsh_xlate_bin', 'dmsh_xlate')
loc = {}
for l in open(smap):
    c = l.rstrip('\n').split('\t')
    if len(c) > 4 and c[0].endswith('.bin'): loc[c[3]] = (c[0], int(c[1]))
os.makedirs(out, exist_ok=True)
jobs = []
for key in (l.split()[0] for l in open(table) if l.strip() and not l.startswith('#')):
    if key not in loc: continue
    name, idx = loc[key]
    d = open(os.path.join(pkg, name), 'rb').read()
    r = [m.start() for m in re.finditer(b'BTCDBTCD', d)]
    rec = d[r[idx]:(r[idx + 1] if idx + 1 < len(r) else len(d))]
    ci = rec.find(bytes.fromhex('34127698'))
    c1o, c1t = struct.unpack_from('<2I', rec, ci + 0x700)
    cp = os.path.join(out, key + '.bin'); open(cp, 'wb').write(rec[0x38:ci][0x100:])
    jobs.append({'key': key, 'stage': 'cs', 'code': cp, 'tex': {}, 'img': {}, 'attr': {}, 'fout': {}, 'c1': rec[0x38 + c1o:0x38 + c1t].hex(), 'debug': True, 'reduced': True})
jp = os.path.join(out, 'job.json'); json.dump(jobs, open(jp, 'w'))
sys.exit(subprocess.run([XL, jp, out]).returncode)
