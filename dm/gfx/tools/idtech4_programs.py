#!/usr/bin/env python3
# id Tech 4's BFG Edition renderer (DOOM 3 on the Switch) makes its GLSL at run time: idRenderProgManager converts a render program of its .resources containers
# (renderprogs/<name>.vertex / .pixel, HLSL-like source) to GLSL 1.50 and compiles it the first time the renderer or a material uses it, so a capture run that only plays the
# game gets the programs it happened to draw with (DOOM 3: 62 of 82 stages by the first level). This finds the renderer's own finders in main and lists every render program,
# so the Mac host's capture run can have the game convert each one (hle_gl.m, DM_GL_RENDERPROGS): the GLSL is then the game's own text, which the host finds by its hash.
# Found from the renderer's code, not from addresses (any build of the engine):
#   · the shader loader: the one function whose code takes the address of its GLSL cache path "renderprogs\glsl\%s" (adrp + add);
#   · the per-stage loaders: its two callers, told apart by the stage they pass (mov w1, #GL_VERTEX_SHADER 0x8b31 or #GL_FRAGMENT_SHADER 0x8b30);
#   · the finders, FindVertexShader / FindFragmentShader (this, name): the callers of one per-stage loader that do not call the other (the renderer's init and reload call both);
#   · the manager: what the callers of a finder load into x0 from main's data (adrp x0 + ldr x0, [x0, #off]: the material parser's renderProgManager), the slot's offset.
# Usage: idtech4_programs.py <main NSO> <RomFS image or folder> <job file>   (exit 1 when main has no such renderer)
# Job file: "renderprogs <find vertex> <find fragment> <manager slot>" (hex offsets in main), then "v <name>" / "f <name>" per render program, the name as the container lists it
import bisect, collections, os as _os, re, struct, sys
sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '../../tools'))
from romfs import os, open   # noqa: E402  RomFS images read in place

CACHE_PATH = b'renderprogs\\glsl\\%s\0'
GL_VERTEX_SHADER, GL_FRAGMENT_SHADER = 0x8b31, 0x8b30


def func_starts(img):
    """function starts from the module's .eh_frame_hdr (through MOD0; udata4 / sdata4 entries relative to the table, as every Switch toolchain writes it)"""
    m0 = struct.unpack_from('<I', img, 4)[0]
    if img[m0:m0 + 4] != b'MOD0': return []
    ehs = m0 + struct.unpack_from('<i', img, m0 + 16)[0]
    n = struct.unpack_from('<i', img, ehs + 8)[0]
    return sorted(ehs + struct.unpack_from('<i', img, ehs + 12 + 8 * i)[0] for i in range(n))


def scan(img, text):
    """per instruction of the text segment: bl targets, string references (adrp + add into one register) and pointer loads into x0 (adrp x0 + ldr x0, [x0, #off])"""
    bls, adds, loads = [], [], {}
    page = {}
    for o in range(text[0], text[0] + text[1], 4):
        w = struct.unpack_from('<I', img, o)[0]
        if (w & 0xFC000000) == 0x94000000:
            imm = w & 0x3FFFFFF
            bls.append((o, o + (imm - (1 << 26) if imm & (1 << 25) else imm) * 4))
        elif (w & 0x9F000000) == 0x90000000:
            imm = ((w >> 29) & 3) | (((w >> 5) & 0x7FFFF) << 2)
            page[w & 31] = ((o & ~0xFFF) + ((imm - (1 << 21) if imm & (1 << 20) else imm) << 12), o)
        elif (w & 0xFFC00000) == 0x91000000:   # add xd, xn, #imm
            rn = (w >> 5) & 31
            if rn in page and o - page[rn][1] <= 16: adds.append((o, page[rn][0] + ((w >> 10) & 0xFFF)))
        elif (w & 0xFFC003FF) == 0xF9400000 and 0 in page and o - page[0][1] <= 8:   # ldr x0, [x0, #imm]
            loads[o] = page[0][0] + ((w >> 10) & 0xFFF) * 8
    return bls, adds, loads


def find(img, text):
    """{'find_vertex', 'find_fragment', 'manager_slot'} (offsets in main), or None when main has no BFG render program manager"""
    at = img.find(b'\0' + CACHE_PATH)
    if at < 0: return None
    at += 1
    starts = func_starts(img)
    if not starts: return None
    fn = lambda o: starts[bisect.bisect_right(starts, o) - 1]
    bls, adds, loads = scan(img, text)
    loader = {fn(o) for o, t in adds if t == at}
    if len(loader) != 1: return None
    loader = loader.pop()
    per_stage = {}
    for o, t in bls:
        if t != loader: continue
        for back in range(4, 40, 4):   # the stage, set shortly before the call
            w = struct.unpack_from('<I', img, o - back)[0]
            if (w & 0xFFE0001F) == 0x52800001: per_stage[(w >> 5) & 0xFFFF] = fn(o); break
    if set(per_stage) != {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER}: return None
    calls = collections.defaultdict(set)   # function → per-stage loaders it calls
    for o, t in bls:
        for st, f in per_stage.items():
            if t == f: calls[fn(o)].add(st)
    finders = {}
    for st in per_stage:
        only = [f for f, s in calls.items() if s == {st}]
        if len(only) != 1: return None
        finders[st] = only[0]
    slots = collections.Counter(loads[o - 4] for o, t in bls if t in finders.values() and (o - 4) in loads)
    if not slots: return None
    return {'find_vertex': finders[GL_VERTEX_SHADER], 'find_fragment': finders[GL_FRAGMENT_SHADER], 'manager_slot': slots.most_common(1)[0][0]}


def container_names(path):
    """the names of an id .resources container's entries (big-endian table: magic 0xD000000D, table offset and length; per entry a little-endian name length, the name,
    and big-endian offset and length)"""
    with open(path, 'rb') as f:
        magic, toff, tlen = struct.unpack('>III', f.read(12))
        if magic != 0xD000000D: return []
        f.seek(toff); t = f.read(tlen)
    n, p, out = struct.unpack_from('>I', t, 0)[0], 4, []
    for _ in range(n):
        ln = struct.unpack_from('<I', t, p)[0]; out.append(t[p + 4:p + 4 + ln].decode('latin1')); p += 4 + ln + 8
    return out


def programs(romfs):
    """(vertex names, fragment names): the render programs of every container under the RomFS's base/ (renderprogs/<name>.vertex / .pixel)"""
    base = os.path.join(romfs, 'base')
    vs, fs = set(), set()
    for fn in sorted(os.listdir(base)) if os.path.isdir(base) else []:
        if not fn.endswith('.resources'): continue
        for name in container_names(os.path.join(base, fn)):
            m = re.fullmatch(r'renderprogs/([^/]+)\.(vertex|pixel)', name)
            if m: (vs if m.group(2) == 'vertex' else fs).add(m.group(1))
    return sorted(vs), sorted(fs)


def job(main_path, romfs):
    """the job file's text for a main with the renderer and a RomFS with render programs, else None"""
    sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '../../tools'))
    import nso
    with open(main_path, 'rb') as f: m = nso.load(f.read(), main_path)
    found = find(bytes(m.image), m.segments[0])
    if not found: return None
    vs, fs = programs(romfs)
    if not vs: return None
    return ''.join([f'renderprogs {found["find_vertex"]:#x} {found["find_fragment"]:#x} {found["manager_slot"]:#x}\n'] + [f'v {n}\n' for n in vs] + [f'f {n}\n' for n in fs])


if __name__ == '__main__':
    if len(sys.argv) != 4: sys.exit('usage: idtech4_programs.py <main NSO> <RomFS> <job file>')
    text = job(sys.argv[1], sys.argv[2])
    if text is None: print('no id Tech 4 render program manager in main'); sys.exit(1)
    with open(sys.argv[3], 'w') as f: f.write(text)
    print(text.splitlines()[0], f'({sum(1 for l in text.splitlines() if l[:2] == "v ")} vertex, {sum(1 for l in text.splitlines() if l[:2] == "f ")} fragment)')
