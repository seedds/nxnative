#!/usr/bin/env python3
# Internal-resolution facts of the B shader library (the reflection fields the host needs to fill support_buffer.render_scale per draw).
# The translator (Ryujinx.Graphics.Shader, dmsh_xlate) already emits Ryujinx's render-scale code: FragCoord.xy / render_scale[0], and helper calls that scale texel-fetch
# coordinates (TexelFetchScale) or unscale texture sizes (TextureSizeUnscale) by render_scale[1 + i], with i a constant per texture binding (TexturePass.cs InsertTexelFetchScale).
# facts() (called by dmsh_reflect.reflect) copies into the reflection, per texture or image binding, that index (sidx) and Ryujinx's TextureUsageFlags (sfl), and per shader uses_frag_coord (frag_coord).
# The index rule (TexturePass.cs): texture k of the translator's texture list has index k; image k has (textures that are not arrays) + k; arrays have none.
# The host fills render_scale per draw from these (nvn_mtl.m rs_block); dmsh_build.py writes frag_coord itself. check() (called by dmsh_build.py) verifies the facts against the MSL itself: every helper call's constant index equals the sidx of the texture it touches, a shader that divides FragCoord has frag_coord,
# and a shader that reads render_scale sees it at byte 192 with a 16-byte stride (where the host writes it, nvn_mtl.m support_buf).
# Usage: dmsh_rscale.py <b-V library dir> [key...]   checks every <key>_b.json / <key>_b.metal pair (default all); prints mismatches, exits 1 on any
import os, re, sys, json

SUPPORT_RENDER_SCALE_OFF, SUPPORT_RENDER_SCALE_STRIDE, SUPPORT_RENDER_SCALE_FIELD = 192, 16, 5   # SupportBuffer.cs field 5: render_scale[73]
MAX_SIDX = 71   # render_scale[1 + sidx] must stay inside its 73 entries (nvn_refl.h NVN_RS_MAXIDX)
NAME = re.compile(r'[a-z]+_([ti])\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)')   # Ryujinx resource names: <stage>_<t|i>[type]_<cb<slot>|tcb>_<handle hex>[_format]


def res_entries(res):
    """The translator's textures and images (res.json) in binding order, each as (is image, cbuf slot, handle, render_scale index or None, flags)"""
    tex, img = res.get('textures', []), res.get('images', [])
    plain = sum(1 for t in tex if t.get('array', 1) <= 1)
    out = [(t['binding'], False, t, k) for k, t in enumerate(tex)] + [(t['binding'], True, t, plain + k) for k, t in enumerate(img)]
    return [(isimg, t.get('cbuf_slot', -1), t.get('handle'), k if t.get('array', 1) <= 1 and k <= MAX_SIDX else None, int(t.get('flags', 0)))
            for _, isimg, t, k in sorted(out, key=lambda e: e[0])]


def name_key(msl_name):
    """(is image, cbuf slot, handle) of a texture or image member of the argument buffer, None for anything else (samplers, buffers, the image-atomic pointer)"""
    if msl_name.endswith(('Smplr', '_atomic')): return None
    m = NAME.match(msl_name)
    return (m.group(1) == 'i', int(m.group(2)) if m.group(2) else -1, int(m.group(3), 16)) if m else None


def facts(binds, res):
    """binds (dmsh_build.py reflect, in argument-buffer order) with sidx / sfl added to each texture or image member, matched to the translator's list by name in binding order
    (spirv-cross keeps the translator's binding order; a member the translator did not list stays without facts). Returns new dicts; binds is not changed"""
    pending, out = res_entries(res), []
    for b in binds:
        key, e = name_key(b.get('msl_name', '')), dict(b)
        hit = next((i for i, r in enumerate(pending) if key and r[:3] == key), None)
        if hit is not None:
            _, _, _, sidx, fl = pending[hit]; pending = pending[hit + 1:]
            if sidx is not None: e['sidx'] = sidx
            if fl: e['sfl'] = fl
        out.append(e)
    return out


def _struct_layout(body):
    """Byte offset and array stride of each member of an MSL constant struct made of 32-bit scalars and 4-vectors (the support buffer's types)"""
    out, off = [], 0
    for ty, n in re.findall(r'^\s*(\w+)\s+_m\d+(?:\[(\d+)\])?;', body, re.M):
        size = 16 if ty[-1] == '4' else 4 if ty in ('uint', 'int', 'float') else None
        if size is None: return None
        off = (off + size - 1) // size * size
        out.append((off, size)); off += size * (int(n) if n else 1)
    return out


def _helpers(msl):
    """Scale helper functions (value, texture index[, axis], support buffer[, FragCoord]) that read render_scale at a computed index: name → 'fetch' (TexelFetchScale) or
    'size' (TextureSizeUnscale, the one that takes the scale's absolute value; HelperFunctionManager.cs)"""
    out = {}
    for m in re.finditer(r'\nint (_\d+)\(thread const int& _\d+, thread const int& _\d+[^)]*constant \w+& support_buffer[^)]*\)\n\{(.*?)\n\}', msl, re.S):
        name, body = m.groups()
        if re.search(r'support_buffer\._m%d\[_\d+\]' % SUPPORT_RENDER_SCALE_FIELD, body): out[name] = 'size' if 'abs(' in body else 'fetch'
    return out


def _const(msl, var, depth=8):
    """The integer constant a variable holds (int _N = 3; or a chain of copies), else None"""
    if re.fullmatch(r'-?\d+', var): return int(var)
    m = re.search(r'\bint %s = (-?\d+|_\d+);' % var, msl)
    return None if not m or depth == 0 else _const(msl, m.group(1), depth - 1)


def _copies(msl, var):
    """var and every variable that is a plain copy of it (int _b = _a;), transitively"""
    seen, todo = {var}, [var]
    while todo:
        for m in re.finditer(r'\bint (_\d+) = %s;' % todo.pop(), msl):
            if m.group(1) not in seen: seen.add(m.group(1)); todo.append(m.group(1))
    return seen


def _source(msl, var, depth=8):
    """The texture whose size a variable was read from (int _N = int2(...NAME.get_width()...).x, possibly through copies), else None"""
    m = re.search(r'\bint %s = (.*?);\n' % var, msl)
    if not m or depth == 0: return None
    t = re.search(r'spvDescriptorSet0\.(\w+)\.get_(?:width|height)\(', m.group(1))
    return t.group(1) if t else _source(msl, m.group(1), depth - 1) if re.fullmatch(r'_\d+', m.group(1)) else None


def _user(msl, var):
    """The texture or image whose read / write / atomic takes var (or a copy of it) as a coordinate, else None (none or several)"""
    names = _copies(msl, var); found = set()
    for ln in msl.splitlines():
        if not any(re.search(r'\b%s\b' % v, ln) for v in names) or re.search(r'\bint (%s) = ' % '|'.join(names), ln): continue
        for t in re.findall(r'spvDescriptorSet0\.(\w+?)(?:_atomic)?(?:\.(?:read|write)\(|\[)', ln): found.add(t)
    return found.pop() if len(found) == 1 else None


def check(msl, refl):
    """Problems (strings) between a shader's MSL and its reflection's internal-resolution facts; [] when they agree"""
    bad, binds = [], {b.get('msl_name'): b for b in refl.get('bindings', [])}
    reads = re.findall(r'support_buffer\)?\._m%d\[(\w+)\]' % SUPPORT_RENDER_SCALE_FIELD, msl)
    if reads:
        st = re.search(r'constant (\w+)\* support_buffer \[\[id', msl)
        body = re.search(r'struct %s\n\{(.*?)\n\};' % st.group(1), msl, re.S) if st else None
        lay = _struct_layout(body.group(1)) if body else None
        if not lay or len(lay) <= SUPPORT_RENDER_SCALE_FIELD or lay[SUPPORT_RENDER_SCALE_FIELD] != (SUPPORT_RENDER_SCALE_OFF, SUPPORT_RENDER_SCALE_STRIDE):
            bad.append(f'render_scale not at byte {SUPPORT_RENDER_SCALE_OFF} with stride {SUPPORT_RENDER_SCALE_STRIDE}: {lay and len(lay) > SUPPORT_RENDER_SCALE_FIELD and lay[SUPPORT_RENDER_SCALE_FIELD]}')
    if '0' in reads and not refl.get('frag_coord'): bad.append('divides FragCoord by render_scale[0] but frag_coord is not set')
    helpers = _helpers(msl)
    for m in re.finditer(r'\bint (_\d+) = (_\d+)\((_\d+), (\w+)[,)]', msl):
        res, fn, arg, idx = m.groups()
        if fn not in helpers: continue
        k = _const(msl, idx); tex = _source(msl, arg) if helpers[fn] == 'size' else _user(msl, res)
        if k is None or tex is None: bad.append(f'{fn}({arg}, {idx}): unresolved (index {k}, texture {tex})'); continue
        got = binds.get(tex, {}).get('sidx')
        if got != k: bad.append(f'{tex}: helper {fn} uses render_scale[1 + {k}], reflection sidx {got}')
    return bad


def main(argv):
    lib, keys = argv[0], argv[1:]
    keys = keys or sorted(f[:-7] for f in os.listdir(lib) if f.endswith('_b.json'))
    n = calls = with_idx = 0; bad = {}
    for k in keys:
        mp = os.path.join(lib, k + '_b.metal')
        if not os.path.exists(mp): continue
        msl = open(mp).read(); refl = json.load(open(os.path.join(lib, k + '_b.json')))
        hs = _helpers(msl); n += 1; calls += sum(1 for m in re.finditer(r'\bint _\d+ = (_\d+)\(', msl) if m.group(1) in hs)
        with_idx += any('sidx' in b for b in refl.get('bindings', []))
        p = check(msl, refl)
        if p: bad[k] = p
    for k, p in bad.items():
        for x in p: print(f'[RED] {k}: {x}')
    print(f'rscale check: {n} shaders, {calls} scale helper calls, {with_idx} shaders with a render_scale index, {len(bad)} with mismatches')
    return 1 if bad else 0


if __name__ == '__main__':
    if len(sys.argv) < 2: sys.exit('usage: dmsh_rscale.py <b-V library dir> [key...]')
    sys.exit(main(sys.argv[1:]))
