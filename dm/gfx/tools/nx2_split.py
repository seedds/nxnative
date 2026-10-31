#!/usr/bin/env python3
# Switch 2 (NVN2) shaders → the package, map and pair tables dmsh_build.py reads. The stage of shader_split.py for Ounce RomFS files, which it cannot read: an NVN2 control section is 0xd8 bytes (+0x40 c1's start,
# +0x44 the code area's length, +0x58 a compute thread group) and its code area is a 0x80-byte header (the 0x50 SPH first) + SM86 instructions + c1, with no 0x30 header. Every program goes through nx2_lower (Eglantine's SM86 → Maxwell lowering) and is
# written as an NVN1-shaped record: the keys stay those of the game's originals (the host's keys_hash), so nothing after this stage needs to know.
# Usage: nx2_split.py <RomFS dir> <main image or -> <package dir> <map out> <facts out> <pairs out> <rejects out>    Environment: NX2_LOWER = the nx2_lower executable
# Open: compute programs (header size 0, thread group not read from the 0xd8 section yet), BAHS (agl), and programs nx2_lower rejects: their keys are listed in the rejects table and not in the map
import collections, hashlib, json, os, struct, subprocess, sys, tempfile
import zstandard

rom, main_img, pkg, mapout, factsout, pairsout, rejout = sys.argv[1:8]
LOWER = os.environ.get('NX2_LOWER') or os.path.join(os.path.dirname(os.path.abspath(__file__)), 'nx2_lower/nx2_lower')
STAGES = ['vs', 'hs', 'ds', 'gs', 'ps', 'cs']
STAGE_NAME = {'vs': 'vertex', 'hs': 'hull', 'ds': 'domain', 'gs': 'geometry', 'ps': 'pixel', 'cs': 'compute'}
CTL_LEN, GFX_HEADER = 0xd8, 0x80
dz = zstandard.ZstdDecompressor()
cs_layout = {}   # compute programs' c0 layout: nx2_reflect.py

def bnsh_at(b, base, label, progs, pairs, stat):
    if b[base + 0x60:base + 0x64] != b'grsc': stat['no grsc'] += 1; return
    nvar, pvar = struct.unpack_from('<IQ', b, base + 0x7c)
    for v in range(nvar):
        prog = struct.unpack_from('<Q', b, base + pvar + 0x40 * v + 0x10)[0]
        if not prog: continue
        keys = {}
        for si, st in enumerate(STAGES):
            sc = struct.unpack_from('<Q', b, base + prog + 8 + 8 * si)[0]
            if not sc: continue
            pctl, pcode, ncode, nctl = struct.unpack_from('<QQII', b, base + sc + 8)
            ctl, code = b[base + pctl:base + pctl + nctl], b[base + pcode:base + pcode + ncode]
            c1o, total = struct.unpack_from('<2I', ctl, 0x40) if nctl == CTL_LEN else (0, 0)
            if nctl != CTL_LEN or total != ncode or c1o > ncode: stat[f'unexpected section ({st})'] += 1; continue
            key = hashlib.sha256(ctl + code).hexdigest()[:16]
            keys[st] = key
            local = struct.unpack_from('<3I', ctl, 0x58) if st == 'cs' else None
            if st == 'cs': cs_layout[key] = (ctl[0xb1], ctl[0xb2], ctl[0xb3], struct.unpack_from('<I', ctl, 0xb8)[0] >> 16)
            progs.setdefault(key, (label, st, code, c1o, local))
        if 'vs' in keys and 'ps' in keys: pairs.add((keys['vs'], keys['ps']))

def scan(b, label, progs, pairs, stat):
    at = 0
    while (at := b.find(b'BNSH\0\0\0\0', at)) >= 0:
        size = struct.unpack_from('<I', b, at + 0x1c)[0]
        stat['BNSH'] += 1
        bnsh_at(b, at, f'{label}#BNSH@{at:#x}', progs, pairs, stat)
        at += max(size, 8)

progs, pairs, stat = {}, set(), collections.Counter()
for root, _, files in os.walk(rom):
    for f in sorted(files):
        if f.startswith('.') or 'Nin_Ounce_NVN2' not in f or f.endswith('.json') or any(x in f for x in ('.bntx', '.bphsh', '.bphsc')): continue
        p = os.path.join(root, f)
        b = open(p, 'rb').read()
        if f.endswith('.zs'): b = dz.stream_reader(b).read(1 << 31)
        stat['files'] += 1
        scan(b, os.path.relpath(p, rom), progs, pairs, stat)
if main_img != '-': scan(open(main_img, 'rb').read(), 'main', progs, pairs, stat)
# Shaders in no game file: the game fills their control sections in at run time (a blank hash at +0x20). A run with DM_NVN_SHDUMP saves them as <key>.ctl and <key>.code; a directory in NX2_DUMPS adds them.
# The stage comes from the SPH (word 0 bits 10..13: 1 vertex, 5 pixel), and a code area with no SPH there is compute
SPH_STAGE = {1: 'vs', 2: 'hs', 3: 'ds', 4: 'gs', 5: 'ps'}
for dump in filter(None, os.environ.get('NX2_DUMPS', '').split(':')):
    for f in sorted(os.listdir(dump)):
        if not f.endswith('.ctl') or not os.path.exists(os.path.join(dump, f[:-4] + '.code')): continue
        ctl = open(os.path.join(dump, f), 'rb').read(CTL_LEN); code = open(os.path.join(dump, f[:-4] + '.code'), 'rb').read()
        c1o, total = struct.unpack_from('<2I', ctl, 0x40)
        key = f[:-4]   # the key the host computed when the game bound it, which is what it looks up: the game may rewrite the section or the code after that, so hashing the saved bytes can give another
        if total != len(code) or c1o > total: stat['dump with an unexpected section'] += 1; continue
        w0 = struct.unpack_from('<I', code, 0)[0]
        st = SPH_STAGE.get(w0 >> 10 & 15) if w0 & 0x1f in (1, 2) and w0 >> 5 & 0x1f == 4 else 'cs'
        if not st: stat['dump with an unknown stage'] += 1; continue
        progs.setdefault(key, ('dump/' + f[:-4], st, code, c1o, struct.unpack_from('<3I', ctl, 0x58) if st == 'cs' else None))
        if st == 'cs': cs_layout[key] = (ctl[0xb1], ctl[0xb2], ctl[0xb3], struct.unpack_from('<I', ctl, 0xb8)[0] >> 16)
        stat['dumps'] += 1
print('scanned', dict(stat), 'unique programs', len(progs), 'pairs', len(pairs))

order = sorted(progs)
with tempfile.TemporaryDirectory() as td:
    i, o = os.path.join(td, 'in'), os.path.join(td, 'out')
    with open(i, 'wb') as f:
        for n, k in enumerate(order):
            _, st, code, c1o, _ = progs[k]
            f.write(struct.pack('<III', n, 0 if st == 'cs' else GFX_HEADER, c1o) + code[:c1o])
    subprocess.run([LOWER, i, o], check=True)
    d = open(o, 'rb').read()

os.makedirs(pkg, exist_ok=True)
for f in os.listdir(pkg):
    if f.endswith('.bin'): os.unlink(os.path.join(pkg, f))
recs, rows, rej, why, facts, texbuf = collections.defaultdict(list), [], [], collections.Counter(), {}, collections.Counter()
brx = collections.Counter()
# A jump table in c1 holds native addresses (16 bytes an instruction), which is where a computed branch goes on the console. Lowered, the instructions are elsewhere: left as they were, the targets are wrong blocks or none,
# and a program that reaches one spins for good, the GPU at 100% and the game waiting for it. Each entry becomes its instruction's address in the layout the branches assume (+0x50, then two instructions, a scheduling word, three...)
def jump_tables(c1, native, tables):
    done = set()
    for base in tables:
        for o in range(base, len(c1) - 3, 4):
            if o in done: continue
            v = struct.unpack_from('<I', c1, o)[0]
            if not v or v % 16 or v // 16 >= len(native): break
            i = native[v // 16]
            struct.pack_into('<I', c1, o, 0x50 + i * 8 + (i + 1) // 3 * 8); done.add(o); brx['table entries'] += 1
    return bytes(c1)
at = 0
while at < len(d):
    n, nlow, nwhy, tb, nmap, ntab, nbrx = struct.unpack_from('<7I', d, at); at += 28
    low = d[at:at + nlow]; at += nlow
    reason = d[at:at + nwhy].decode(); at += nwhy
    native = struct.unpack_from(f'<{nmap}I', d, at); at += nmap * 4
    tables = struct.unpack_from(f'<{ntab}I', d, at); at += ntab * 4
    key = order[n]; label, st, code, c1o, local = progs[key]
    if nlow: texbuf['none' if tb == 0xffffffff else tb] += 1
    if not nlow: rej.append((key, st, reason, label)); why[reason] += 1; continue
    # The lowering gives instructions only: Maxwell has a scheduling word at every 32-byte boundary, and its branches assume the pattern of a first instruction at +0x50 counted from the SPH (Eden's
    # InitializeNativeCode lays it out so): two instructions, a scheduling word, three, a scheduling word. Ryujinx's decoder counts from the first word after the SPH, where it skips a scheduling word, so a
    # scheduling word and a NOP move the first instruction to +0x10 there, the same place in the pattern. Laid out as Eden's, every shader "translated" with real instructions read as scheduling words
    words = [0, 0x50b0000000000f00]
    addr = 0x10
    for k in range(0, nlow, 8):
        if addr % 32 == 0: words.append(0); addr += 8
        words.append(struct.unpack_from('<Q', low, k)[0]); addr += 8
    # The lowering drops compiler padding (@!PT), so a branch to it lands past the last instruction, where the translator's decoder reads off the end: an EXIT there gives such a branch somewhere to land
    if addr % 32 == 0: words.append(0); addr += 8
    words.append(0xe30000000007000f)
    low = struct.pack(f'<{len(words)}Q', *words) + bytes(8)
    # NVN1-shaped: 0x30 header with the stage's magic, the SPH, the lowered instructions, c1; a control section whose only meaningful fields say where c1 is
    body = bytes.fromhex('79563412') + bytes(0xfc) + low if st == 'cs' else bytes.fromhex('78563412') + bytes(0x2c) + code[:0x50] + low
    c1 = jump_tables(bytearray(code[c1o:]), native, tables)
    if nbrx: brx['programs'] += 1; brx['branches'] += nbrx; brx['branches with no table found'] += nbrx - ntab
    ctl = bytearray(0x880); ctl[:4] = bytes.fromhex('34127698'); struct.pack_into('<2I', ctl, 0x700, len(body), len(body) + len(c1))
    body += c1
    if st == 'cs': facts[key] = {'local': list(local), 'shared': 0}
    group = label.split('/')[0].split('.')[0].split('#')[0].lower()
    name = f'{st}_{group}.bin'
    rec = bytearray(b'BTCDBTCD' + bytes(0x30)); struct.pack_into('<2I', rec, 0x28, len(ctl), len(body)); rec += body + ctl
    rows.append((name, len(recs[name]), STAGE_NAME[st], key, label)); recs[name].append(bytes(rec))
print('computed branches', dict(brx))
for name, rr in recs.items():
    with open(os.path.join(pkg, name), 'wb') as f: f.write(b''.join(rr))
with open(mapout, 'w') as f:
    f.write('# kind: evidence  Switch 2 shaders (BNSH) lowered to Maxwell by nx2_split.py\npackage\tindex\tstage\tdm_ctl_sha16\tsource\n')
    for r in rows: f.write('\t'.join(map(str, r)) + '\n')
good = {r[3] for r in rows}
with open(factsout, 'w') as f:
    f.write('# kind: evidence  Switch 2 compute thread groups (control +0x58). Open: shared memory (0 here), and no texture facts yet: those queries are answered with defaults\n')
    for k in sorted(facts): f.write(k + '\t' + json.dumps(facts[k], sort_keys=True, separators=(',', ':')) + '\n')
print('texture handle buffer of the lowered programs', texbuf.most_common())
with open(pairsout, 'w') as f:
    f.write('# kind: evidence  (vertex key, pixel key) of every BNSH variation\n')
    for a, b in sorted(pairs):
        if a in good and b in good: f.write(f'{a} {b}\n')
with open(rejout, 'w') as f:
    for r in rej: f.write('\t'.join(r) + '\n')
with open(os.path.join(pkg, 'cs_layout.tsv'), 'w') as f:
    for k in sorted(cs_layout):
        if k in good: f.write(k + '\t' + '\t'.join(map(str, cs_layout[k])) + '\n')
print('lowered', len(rows), 'rejected', len(rej), why.most_common(12))
print('by stage', collections.Counter(r[2] for r in rows).most_common())
print('packages', {k: len(v) for k, v in recs.items()})
