#!/usr/bin/env python3
# NVN2 compute shaders: the bindings of the reflection dmsh_build.py wrote, from the layout of their c0. NVN1 has fixed tables (c0 and c2, dmsh_abi.h); an NVN2 compute program packs what it uses into c0, and its
# control section gives the counts (+0xb1 uniforms, +0xb2 storage buffers, +0xb3 textures; +0xba total bytes): 0x10 reserved, uniforms and storage buffers 0x10 bytes each (address, size), textures and images 8
# bytes each, images to the end. Checked on all of Mario Kart World's compute programs: every offset the translator found lands on an entry, and the totals agree.
# A step after dmsh_build.py, as it changes no generated code. Usage: nx2_reflect.py <library dir> <layout table written by nx2_split.py> <directory of the translator's <key>.res.json>
import glob, json, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import dmsh_abi as abi

lib, table, resdir = sys.argv[1:4]
layout = {c[0]: tuple(int(x) for x in c[1:5]) for c in (l.split() for l in open(table) if l.strip() and not l.startswith('#'))}
stats = dict(shaders=0, sbuf=0, ubo_ptr=0, tex=0, img=0, unmapped=0)
for key, (nu, ns, nt, total) in sorted(layout.items()):
    p = os.path.join(lib, key + '_b.json'); res = os.path.join(resdir, key + '.res.json')
    if not os.path.exists(p) or not os.path.exists(res): continue
    j = json.load(open(p)); stats['shaders'] += 1
    ubase, sbase = 0x10, 0x10 + 0x10 * nu
    tbase = sbase + 0x10 * ns; ibase = tbase + 8 * nt
    offs = [b['cb_offset'] * 4 if b['cb_slot'] == 0 else None for b in sorted(json.load(open(res))['sbufs'], key=lambda b: b['binding'])]
    for e in j.get('bindings', []):
        nm = e.get('msl_name', '')
        if e.get('kind') == abi.KIND_SBUF:
            off = offs[e['index']] if offs and e['index'] < len(offs) else None
            # Stopgap: the translator traced this one's address to c1 + 0, which an NVN2 compute program's c1 (empty in all of them) cannot hold. Open: what the program reads there. An unused slot lets the
            # program load; without it the host refuses the whole program
            if off is None: e['reg'] = 15; stats['unmapped'] += 1; continue
            if ubase <= off < sbase: e.update(kind=abi.KIND_UBO_PTR, reg=(off - ubase) // 0x10); stats['ubo_ptr'] += 1
            elif sbase <= off < tbase: e['reg'] = (off - sbase) // 0x10; stats['sbuf'] += 1
            else: stats['unmapped'] += 1
            continue
        m = re.match(r'[a-z]+_([ti])\w*?_(?:cb\d+|tcb)_([0-9A-F]+?)_?(Smplr(_w)?)?$', nm)
        if not m: continue
        byte = (int(m.group(2), 16) & 0x3fff) * 4
        if tbase <= byte < ibase: slot, is_img = (byte - tbase) // 8, False
        elif ibase <= byte < total: slot, is_img = (byte - ibase) // 8, True
        else: stats['unmapped'] += 1; continue
        if m.group(3):
            e.update(kind=abi.KIND_SAMP_TEX, reg=slot)
        elif is_img or m.group(1) == 'i':
            e.update(kind=abi.KIND_IMG, reg=slot); stats['img'] += 1
        else:
            e.update(kind=abi.KIND_TBUF if 'texture_buffer' in e.get('msl_type', '') else abi.KIND_TEX, reg=slot); stats['tex'] += 1
    with open(p, 'w') as f: json.dump(j, f, ensure_ascii=False, indent=1)
print(stats)
