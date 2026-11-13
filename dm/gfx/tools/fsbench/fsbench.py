#!/usr/bin/env python3
"""fsbench driver: scenes from a game's pass dump, variant compiles with the chain's Metal flags, runs of fsbench.

  fsbench.py tool                                  build the fsbench binary (beside this file, in $FSBENCH_BIN or the work folder given by --bin)
  fsbench.py fetch <img> <key> <dir>               the shader's _b.metal / _b.json / _b.metallib from a build's shader image (shader_lib.py cat)
  fsbench.py compile <in.metal> <out.metallib>     one source through the chain's compile (dmsh_build.MAC_METAL flags, -fbracket-depth=1024)
  fsbench.py scene --log game.log --frame F --pass P --key K --json K_b.json --msl K_b.metal --cb DIR [--draw N] [--fill rich|zero] --out scene.json
  fsbench.py cscene --log game.log --frame F --key K --json K_b.json --msl K_b.metal [--cb DIR] [--draw N] [--fill rich|zero] --out scene.json   a compute dispatch
  fsbench.py variants <base.metal> <dir> [name...] write the named source variants of a shader (VARIANTS below) beside each other

Game files stay in the game's work folder: the scene, its buffers and every output go where --out / the run's out dir say.
"""
import argparse, glob, json, os, re, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..'))
METAL = ['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance', '-fwrapv', '-fbracket-depth=1024']   # dmsh_build.MAC_METAL + METAL_DEPTH

def compile_msl(src, lib, extra=()):
    air = lib + '.air'
    subprocess.run([*METAL, *extra, src, '-o', air], check=True)
    subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', air, '-o', lib], check=True)
    os.unlink(air)

# NVN wrap modes (CLAMP 0, REPEAT 1, MIRROR_CLAMP 2, MIRROR_CLAMP_TO_EDGE 3, MIRROR_CLAMP_TO_BORDER 4, CLAMP_TO_BORDER 5, MIRRORED_REPEAT 6, CLAMP_TO_EDGE 7) → fsbench's
# (repeat 0, mirrored repeat 1, clamp to edge 2, clamp to border 3, mirror clamp to edge 4)
WRAP = {0: 2, 1: 0, 2: 4, 3: 4, 4: 4, 5: 3, 6: 1, 7: 2}
# NVN min filter: 0 nearest, 1 linear, 2 nearest-mip-nearest, 3 linear-mip-nearest, 4 nearest-mip-linear, 5 linear-mip-linear
def sampler(m):
    mn = int(m['min']); return {**({'cmp': int(m['cmpf']) - 1} if m.get('cmpf') and int(m.get('cmpe', 0)) else {}),   # NVN compare (enabled/func, NEVER 1 … ALWAYS 8) → MTLCompareFunction (Never 0 … Always 7)
                               'min': mn in (1, 3, 5), 'mag': int(m['mag']) == 1, 'mip': 0 if mn < 2 else 1 if mn < 4 else 2,
                               'wrap': [WRAP.get(int(x), 0) for x in m['wrap'].split(',')], 'aniso': int(m['aniso']),
                               'lodmin': float(m['lod'].split('..')[0]), 'lodmax': float(m['lod'].split('..')[1])}
TARGET = {2: '2d', 3: '2da', 5: 'cube', 7: '3d', 1: '2d'}   # MTLTextureType numbering (the dump's target=); the shader's declaration wins (scene)

def draw_lines(log, frame, pas, key, nth):
    """The [pass] B lines of the nth draw (0-based) of shader key in that pass: those between its last 'K <pass> st1 key=<key>' and the draw's 'd' line"""
    L = [l.rstrip('\n') for l in open(log, errors='replace') if l.startswith(f'[pass] F{frame} ')]
    ds = [i for i, l in enumerate(L) if re.match(rf'\[pass\] F{frame} d {pas} .*fs={key[:16]}', l)]
    if nth >= len(ds): sys.exit(f'{len(ds)} draws of {key} in pass {pas} of frame {frame}')
    d = ds[nth]; k = max(i for i in range(d) if re.match(rf'\[pass\] F{frame} K {pas} st1 key={key[:16]}', L[i]))
    return L[k:d + 1], L

def scene(a):
    j = json.load(open(a.json)); lines, all_lines = draw_lines(a.log, a.frame, a.pas, a.key, a.draw)
    cb, tex, samp, sb = {}, {}, {}, {}
    for l in lines:
        m = re.search(r' st1 cb reg(\d+) addr=0x([0-9a-f]+) size=(\d+)', l)
        if m: cb[int(m[1])] = (m[2], int(m[3]))
        m = re.search(r' st1 tex reg(\d+) .*fmt=(\d+) (\d+)x(\d+)x(\d+) view_lv=(\d+)\+(\d+) .*array=(\d+) target=(\d+)', l)
        if m: tex[int(m[1])] = {'fmt': int(m[2]), 'w': int(m[3]), 'h': int(m[4]), 'd': max(int(m[5]), int(m[8])), 'mips': int(m[7]), 'type': TARGET.get(int(m[9]), '2d')}
        m = re.search(r' st1 samp reg(\d+) sid=\d+ mag=(\d+) min=(\d+) .*wrap=([\d,]+) border=\S+ lod=(\S+) bias=\S+ aniso=(\d+)(?: cmp=(\d+)/(\d+))?', l)
        if m: samp[int(m[1])] = sampler({'mag': m[2], 'min': m[3], 'wrap': m[4], 'lod': m[5], 'aniso': m[6], 'cmpe': m[7], 'cmpf': m[8]})
        m = re.search(r' st1 sbufp reg(\d+) \w+ addr=0x[0-9a-f]+ size=(\d+)', l)
        if m: sb[int(m[1])] = int(m[2])
    att = [re.search(r' c(\d+) .*fmt=(\d+) (\d+)x(\d+)', l) for l in all_lines if re.match(rf'\[pass\] F{a.frame} P {a.pas} c\d+ ', l)]
    att = sorted((int(m[1]), int(m[2]), int(m[3]), int(m[4])) for m in att if m)
    d = os.path.dirname(os.path.abspath(a.out)); bufs = os.path.join(d, 'buf'); os.makedirs(bufs, exist_ok=True)
    def wfile(name, data):
        p = os.path.join(bufs, name); open(p, 'wb').write(data); return p
    rich = a.fill == 'rich'; msl_src = open(a.msl).read()
    args = []
    for b in j['bindings']:
        i, k = b['arg_id'], b['kind']
        if k == 'support':   # Ryujinx's support buffer: _m5[i].x (byte 192 + 16 i) is each target's render scale, 1
            s = bytearray(4096)
            for n in range(73): struct.pack_into('<f', s, 192 + 16 * n, 1.0)
            args.append({'id': i, 'kind': 'buffer', 'file': wfile('support.bin', bytes(s)), 'size': 4096})
        elif k == 'cbuf':
            addr, size = cb[b['reg']]
            f = os.path.join(a.cb, f'F{a.frame}_p{a.pas}_st1_cb{b["reg"]}_{addr}.bin')
            if not os.path.exists(f):   # a shader whose draws were not dumped: the same slot's block of a dumped draw of the pass, the same size first (approximate: the scene builder prints which block it used)
                alt = sorted(glob.glob(os.path.join(a.cb, f'F{a.frame}_p{a.pas}_st1_cb{b["reg"]}_*.bin')), key=lambda x: os.path.getsize(x) != size) or \
                      sorted(glob.glob(os.path.join(a.cb, f'F{a.frame}_p*_st1_cb{b["reg"]}_*.bin')), key=lambda x: os.path.getsize(x) != size)
                if not alt: sys.exit(f'no uniform dump {f}')
                print(f'cb reg {b["reg"]}: {os.path.basename(f)} not dumped, using {os.path.basename(alt[0])}', file=sys.stderr); f = alt[0]
            args.append({'id': i, 'kind': 'buffer', 'file': f, 'size': 65536})
        elif k == 'c1': args.append({'id': i, 'kind': 'buffer', 'hex': j['c1'], 'size': 65536})
        elif k == 'sbuf':   # not dumped: synthetic, by the game's binding slot. OoT's main pass: reg3 material texture handles, reg2 light
            # vectors, reg0 a probe grid of record indices, reg1 the 96-byte probe records. rich: every optional texture handle set, records of the full path
            r = b['reg']; n = max(sb.get(r, 1 << 16) // 4, 4); w = bytearray(4 * n)
            if rich:
                if r == 3: [struct.pack_into('<I', w, 4 * x, (1 << 20) | (1 + x % 7)) for x in range(n)]   # sampler 1, texture 1..7
                elif r == 2: [struct.pack_into('<f', w, 4 * x, 0.577) for x in range(n)]
                elif r == 0:   # light-cluster grid of {count, list offset} pairs; the index list (from word c3[149].x) is in the same buffer. Each pair is
                    # {L, (x/2) % 512}, so a word read as a list entry is L or < 512, a valid record index. count drives the per-pixel light loop
                    # (min(count, c6[113].x = 1000) + 1 passes): a grid of cell indices ran it 1000 times a pixel (170 ms a pass)
                    nrec = max(1, sb.get(1, 96) // 96)
                    for x in range(n): struct.pack_into('<I', w, 4 * x, a.lights if x % 2 == 0 else (x // 2) % min(512, nrec))
                elif r == 1:
                    for x in range(n): struct.pack_into('<f', w, 4 * x, 0.125 + (x % 5) * 0.05)
                    for q in range(n // 24): struct.pack_into('<I', w, 96 * q + 16, 1)                         # record type 1 (not 5): the full path
            args.append({'id': i, 'kind': 'buffer', 'file': wfile(f'sb{r}.bin', bytes(w)), 'size': len(w)})
        elif k == 'tex':
            if b['reg'] not in tex: sys.exit(f'no texture bound at reg {b["reg"]} in the dump')
            t = dict(tex[b['reg']]); d = re.search(r'(\w+)<[^>]*> \w+ \[\[id\(%d\)\]\]' % i, msl_src)   # the type the shader declares (the dump's target is Metal's numbering: 3 is a 2D array, not a cube)
            if d: t['type'] = {'texture2d': '2d', 'depth2d': '2d', 'texture2d_array': '2da', 'depth2d_array': '2da', 'texturecube': 'cube', 'depthcube': 'cube', 'texture3d': '3d'}.get(d[1], t['type'])
            args.append({'id': i, 'kind': 'tex', **t})
        elif k == 'samp_tex': args.append({'id': i, 'kind': 'samp', **samp.get(b['reg'], {'min': 1, 'mag': 1, 'mip': 2, 'wrap': [0, 0, 0], 'aniso': 1})})
        elif k == 'tex_pool':
            arr = 'array' in b['elem']
            args.append({'id': i, 'kind': 'texpool', 'n': 16, 'tex': {'type': '2da' if arr else '2d', 'fmt': 190 if not arr else 70, 'w': 1024 if not arr else 512, 'h': 1024 if not arr else 512, 'd': 4, 'mips': 11}})
        elif k == 'samp_pool': args.append({'id': i, 'kind': 'samppool', 'n': 16, 'samp': {'min': 1, 'mag': 1, 'mip': 2, 'wrap': [0, 0, 0], 'aniso': 1}})
        elif k == 'border': args.append({'id': i, 'kind': 'buffer', 'file': wfile('border.bin', bytes(16 + 32 * 16)), 'size': 528})   # mask 0: Metal's own border colours
        else: print(f'binding {b["msl_name"]}: kind {k} left unset', file=sys.stderr)
    sc = {'key': a.key, 'frame': a.frame, 'pass': a.pas, 'draw': a.draw, 'fill': a.fill, 'lights': a.lights, 'w': att[0][2] if att else 1440, 'h': att[0][3] if att else 810,
          'color': [x[1] for x in att] or [92], 'msl': os.path.abspath(a.msl), 'fn': 'main0', 'args': args}
    json.dump(sc, open(a.out, 'w'), indent=1); print(f'{a.out}: {sc["w"]}x{sc["h"]} colour {sc["color"]}, {len(args)} arguments ({len(tex)} textures, {len(cb)} uniform blocks), fill {a.fill}')

def cscene(a):
    """A compute dispatch's scene from a pass dump: its K .. X lines (the nth dispatch of the key in the frame). Uniform blocks: the --cb dump when there is one (F<frame>_p<pass>_st5_cb<reg>_<addr>.bin),
    else zeros with the B line's first four floats (DM_NVN_CBDUMP saves draws' blocks; the note says which). Storage buffers: their size, rich fill random words, read-write ones compared as outputs.
    Storage images (W lines): written and compared. Textures and samplers as scene()."""
    j = json.load(open(a.json)); L = [l.rstrip('\n') for l in open(a.log, errors='replace') if l.startswith(f'[pass] F{a.frame} ')]
    xs = [i for i, l in enumerate(L) if re.match(rf'\[pass\] F{a.frame} X \d+ dispatch {a.key[:16]}', l)]
    if a.draw >= len(xs): sys.exit(f'{len(xs)} dispatches of {a.key} in frame {a.frame}')
    x = xs[a.draw]; k = max(i for i in range(x) if re.match(rf'\[pass\] F{a.frame} K \d+ st5 key={a.key[:16]}', L[i])); lines = L[k:x + 1]
    g = re.search(r'groups (\d+)x(\d+)x(\d+)', L[x]); groups = [int(v) for v in g.groups()]
    cb, tex, samp, sb, img = {}, {}, {}, {}, {}
    for l in lines:
        m = re.search(r' st5 cb reg(\d+) addr=0x([0-9a-f]+) size=(\d+) .*?f=([^ ]+)', l)
        if m: cb[int(m[1])] = (m[2], int(m[3]), m[4])
        m = re.search(r' st5 tex reg(\d+) .*fmt=(\d+) (\d+)x(\d+)x(\d+) view_lv=(\d+)\+(\d+) .*array=(\d+) target=(\d+)', l)
        if m: tex[int(m[1])] = {'fmt': int(m[2]), 'w': int(m[3]), 'h': int(m[4]), 'd': max(int(m[5]), int(m[8])), 'mips': int(m[7]), 'type': TARGET.get(int(m[9]), '2d')}
        m = re.search(r' st5 samp reg(\d+) sid=\d+ mag=(\d+) min=(\d+) .*wrap=([\d,]+) border=\S+ lod=(\S+) bias=\S+ aniso=(\d+)(?: cmp=(\d+)/(\d+))?', l)
        if m: samp[int(m[1])] = sampler({'mag': m[2], 'min': m[3], 'wrap': m[4], 'lod': m[5], 'aniso': m[6], 'cmpe': m[7], 'cmpf': m[8]})
        m = re.search(r' st5 sbufp reg(\d+) \w+ addr=0x[0-9a-f]+ size=(\d+) (r[ow])', l)
        if m: sb[int(m[1])] = (int(m[2]), m[3])
        m = re.search(r' W \d+ st5 reg(\d+) .*fmt=(\d+) (\d+)x(\d+) .*array=(\d+) target=(\d+)', l)
        if m: img[int(m[1])] = {'fmt': int(m[2]), 'w': int(m[3]), 'h': int(m[4]), 'd': int(m[5]), 'mips': 1, 'type': TARGET.get(int(m[6]), '2d')}
    d = os.path.dirname(os.path.abspath(a.out)); bufs = os.path.join(d, 'buf'); os.makedirs(bufs, exist_ok=True)
    def wfile(name, data): p = os.path.join(bufs, name); open(p, 'wb').write(data); return p
    msl_src = open(a.msl).read(); args = []; notes = []
    for b in j['bindings']:
        i, kd = b['arg_id'], b['kind']; reg = b.get('reg')
        if kd == 'support': sbuf = bytearray(4096); [struct.pack_into('<f', sbuf, 192 + 16 * n, 1.0) for n in range(73)]; args.append({'id': i, 'kind': 'buffer', 'file': wfile('support.bin', bytes(sbuf)), 'size': 4096})
        elif kd == 'cbuf' and reg in cb:
            addr, size, f4 = cb[reg]; fp = os.path.join(a.cb or '', f'F{a.frame}_p{lines[0].split()[3]}_st5_cb{reg}_{addr}.bin')
            if a.cb and os.path.exists(fp): args.append({'id': i, 'kind': 'buffer', 'file': fp, 'size': 65536})
            else:
                bb = bytearray(max(size, 16)); [struct.pack_into('<f', bb, 4 * n, float(v)) for n, v in enumerate(f4.split(',')[:4]) if v not in ('nan', 'inf', '-inf')]
                args.append({'id': i, 'kind': 'buffer', 'file': wfile(f'cb{reg}.bin', bytes(bb)), 'size': 65536}); notes.append(f'cb{reg}: not dumped, zeros + its first 4 floats')
        elif kd == 'cbuf': args.append({'id': i, 'kind': 'buffer', 'size': 65536}); notes.append(f'cb{reg}: not bound in the dump, zeros')
        elif kd == 'sbuf':
            size, rw = sb.get(reg, (65536, 'rw')); args.append({'id': i, 'kind': 'buffer', 'size': size, 'fill': 'random' if a.fill == 'rich' else 'zero', **({'out': True} if rw == 'rw' else {})})
        elif kd == 'tex' and reg in tex:
            t = dict(tex[reg]); dd = re.search(r'(\w+)<[^>]*> \w+ \[\[id\(%d\)\]\]' % i, msl_src)
            if dd: t['type'] = {'texture2d': '2d', 'depth2d': '2d', 'texture2d_array': '2da', 'depth2d_array': '2da', 'texturecube': 'cube', 'depthcube': 'cube', 'texture3d': '3d'}.get(dd[1], t['type'])
            args.append({'id': i, 'kind': 'tex', **t})
        elif kd == 'img' and reg in img: args.append({'id': i, 'kind': 'tex', **img[reg], 'write': True, 'out': True})
        elif kd == 'samp_tex': args.append({'id': i, 'kind': 'samp', **samp.get(reg, {'min': 1, 'mag': 1, 'mip': 2, 'wrap': [0, 0, 0], 'aniso': 1})})
        elif kd == 'border': args.append({'id': i, 'kind': 'buffer', 'file': wfile('border.bin', bytes(16 + 32 * 16)), 'size': 528})
        else: notes.append(f'binding {b.get("msl_name")} ({kd}, reg {reg}): not in the dump or not handled, left unbound')
    sc = {'kind': 'compute', 'key': a.key, 'frame': a.frame, 'pass': int(lines[0].split()[3]), 'draw': a.draw, 'fill': a.fill, 'groups': groups, 'tg': j.get('threadgroup_size') or [1, 1, 1],
          'msl': os.path.abspath(a.msl), 'fn': 'main0', 'args': args, 'notes': notes}
    json.dump(sc, open(a.out, 'w'), indent=1); print(f'{a.out}: groups {groups} of {sc["tg"]}, {len(args)} arguments; ' + ('; '.join(notes) or 'everything from the dump'))

# ---------- source variants ----------
def _unwrap(u, head, pick):   # head(...) → pick(its arguments), balanced parentheses
    out, pos = [], 0
    while True:
        i = u.find(head, pos)
        if i < 0: out.append(u[pos:]); return ''.join(out)
        j = i + len(head); d = 1; k = j; start = j; args = []
        while d:
            c = u[k]
            if c == '(': d += 1
            elif c == ')':
                d -= 1
                if d == 0: args.append(u[start:k])
            elif c == ',' and d == 1: args.append(u[start:k]); start = k + 1
            k += 1
        r = pick(args); out.append(u[pos:i] + (r if r is not None else u[i:k])); pos = k
def v_nomark(u):   # bindless reads without the pool's first-read mark (an atomic load, sometimes an atomic or, a pixel)
    return re.sub(r'(uint dm_pool_mark\(constant P\* p, uint i\)\n\{).*?(\n\})', r'\1\n    return i;\2', u, flags=re.S)
def v_noclamp(u):  # bindless index without min(ID, n)
    return _unwrap(u, '[min(uint(', lambda a: '[uint(' + a[0] + ')' if len(a) == 2 and a[1].strip().endswith('->n') else None)
def v_fast(u): return re.sub(r'\bprecise::(min|max|clamp)\(', r'fast::\1(', u)
def v_nobs(u):     # border emulation off: no second sample (mask 0 at run time when Metal has the border colour)
    return re.sub(r'(V dm_bs\(V r, constant dm_border_t\* b, uint i, T t, sampler w, A\.\.\. a\)\n\{\n)    if \(b->mask >> i & 1u\) r = dm_bmix\(r, t\.sample\(w, a\.\.\.\), b->k\[i\]\);\n', r'\1', u)
def v_nonan(u):    # sample coordinates without the NaN → 0 scrub
    return re.sub(r'select\((\w+), (float[234]?)\(0\.0\), isnan\(\1\)\)', r'\1', u)
def v_nanfmax(u):  # #1230's cheaper guard on 2D coordinates: fmax(c, -FLT_MAX) (NaN -> the most negative float, the same texel as 0 except through clamp-to-border); float3 (cube, 3D, array) keep the select
    return re.sub(r'select\((\w+), (float2)\(0\.0\), isnan\(\1\)\)', r'fmax(\1, \2(-FLT_MAX))', u)
def v_nanfmaxall(u):   # the same on every guard, cube directions included (timing only: #1230 found fmax on a cube direction as slow as the select on the M1)
    return re.sub(r'select\((\w+), (float[234]?)\(0\.0\), isnan\(\1\)\)', r'fmax(\1, \2(-FLT_MAX))', u)
def v_sconst(u):   # read-only storage buffers through the constant address space (preamble hoisting of uniform-address loads)
    u = re.sub(r'const device (_\w+)\* (fp_s\d+|vp_s\d+) ', r'constant \1* \2 ', u)
    return re.sub(r'\(const device (packed_uint[24])\*\)&\(\*spvDescriptorSet0\.(fp_s\d+)\)', r'(constant \1*)&(*spvDescriptorSet0.\2)', u)
def v_uidx(u):     # bindless pool indices as SIMD-group uniform (simd_broadcast_first): valid only where the index is the same for a whole draw
    return re.sub(r'min\(uint\(int\((_\d+)\)\), (spvDescriptorSet0\.\w+)->n\)', r'simd_broadcast_first(min(uint(int(\1)), \2->n))', u)
def v_guard(u):    # PR #2254's loop guard (dmsh_rewrite.loop_guard, copied): every loop also ends after 2^18 iterations of its function, one counter a function
    lines, out, bodies, used = u.split('\n'), [], [], set(); cap = '++dm_lg < 262144u'
    for i, l in enumerate(lines):
        if l == '{' and i and lines[i - 1].rstrip().endswith(')'): out.append(l); bodies.append(len(out)); continue
        g = l
        if (m := re.fullmatch(r'(\s*)\} while \((.*)\);\s*', l)): l = f'{m.group(1)}}} while (({m.group(2)}) && {cap});'
        elif (m := re.fullmatch(r'(\s*)for \(;;\)\s*', l)): l = f'{m.group(1)}for (; {cap};)'
        elif (m := re.fullmatch(r'(\s*)while \((.*)\)\s*', l)): l = f'{m.group(1)}while (({m.group(2)}) && {cap})'
        elif (m := re.fullmatch(r'(\s*)for \(([^;]*);([^;]*);([^;]*)\)\s*', l)): l = f'{m.group(1)}for ({m.group(2)}; ({m.group(3).strip() or "true"}) && {cap};{m.group(4)})'
        if l != g and bodies: used.add(bodies[-1])
        out.append(l)
    for at in sorted(used, reverse=True): out.insert(at, '    uint dm_lg = 0u;')
    return '\n'.join(out)
def v_mark1(u):    # the pool read mark kept, but once a SIMD group when every active lane reads the same ID (a material's texture, the usual case), else per lane as before
    return re.sub(r'(uint dm_pool_mark\(constant P\* p, uint i\)\n\{\n)(.*?)(\n    return i;\n\})',
                  lambda m: m[1] + '    uint f = simd_broadcast_first(i);\n    if (simd_all(i == f) && !simd_is_first()) return i;\n' + m[2] + m[3], u, flags=re.S)
VARIANTS = {'nanfmax': v_nanfmax, 'nanfmaxall': v_nanfmaxall, 'guard': v_guard, 'mark1': v_mark1, 'sconst': v_sconst, 'uidx': v_uidx, 'nomark': v_nomark, 'noclamp': v_noclamp, 'fast': v_fast, 'nobs': v_nobs, 'nonan': v_nonan}

def main():
    p = argparse.ArgumentParser(); s = p.add_subparsers(dest='cmd', required=True)
    t = s.add_parser('tool'); t.add_argument('--bin', default=os.environ.get('FSBENCH_BIN', os.path.join(HERE, 'fsbench')))
    f = s.add_parser('fetch'); f.add_argument('img'); f.add_argument('key'); f.add_argument('dir')
    c = s.add_parser('compile'); c.add_argument('src'); c.add_argument('lib'); c.add_argument('extra', nargs='*')
    sc = s.add_parser('scene')
    for k, ty in (('--log', str), ('--frame', int), ('--key', str), ('--json', str), ('--msl', str), ('--cb', str), ('--out', str)): sc.add_argument(k, type=ty, required=True)
    sc.add_argument('--pass', dest='pas', type=int, required=True); sc.add_argument('--draw', type=int, default=0); sc.add_argument('--fill', default='rich', choices=('rich', 'zero'))
    sc.add_argument('--lights', type=int, default=2)   # lights per cluster cell (OoT's main pass), the loop's trip count
    cs = s.add_parser('cscene')
    for k, ty in (('--log', str), ('--frame', int), ('--key', str), ('--json', str), ('--msl', str), ('--out', str)): cs.add_argument(k, type=ty, required=True)
    cs.add_argument('--cb', default=None); cs.add_argument('--draw', type=int, default=0); cs.add_argument('--fill', default='rich', choices=('rich', 'zero'))
    v = s.add_parser('variants'); v.add_argument('base'); v.add_argument('dir'); v.add_argument('names', nargs='*')
    a = p.parse_args()
    if a.cmd == 'tool':
        subprocess.run(['clang', '-O2', '-fobjc-arc', '-framework', 'Metal', '-framework', 'Foundation', '-framework', 'ImageIO', '-framework', 'CoreGraphics',
                        os.path.join(HERE, 'fsbench.m'), '-o', a.bin], check=True); print(a.bin)
    elif a.cmd == 'fetch':
        os.makedirs(a.dir, exist_ok=True)
        for suf in ('_b.metal', '_b.json', '_b.metallib'):
            out = subprocess.run([sys.executable, os.path.join(ROOT, 'dm', 'tools', 'shader_lib.py'), 'cat', a.img, a.key + suf], capture_output=True, check=True).stdout
            open(os.path.join(a.dir, a.key + suf), 'wb').write(out)
    elif a.cmd == 'compile': compile_msl(a.src, a.lib, a.extra)
    elif a.cmd == 'scene': scene(a)
    elif a.cmd == 'cscene': cscene(a)
    elif a.cmd == 'variants':
        src = open(a.base).read(); stem = os.path.basename(a.base).split('.')[0]
        for n in a.names or VARIANTS:
            u = src
            for part in n.split('+'): u = VARIANTS[part](u)
            out = os.path.join(a.dir, f'{stem}.{n}.metal'); open(out, 'w').write(u)
            print(out, 'unchanged' if u == src else f'{sum(1 for x, y in zip(src.splitlines(), u.splitlines()) if x != y)} lines changed')

if __name__ == '__main__': main()
