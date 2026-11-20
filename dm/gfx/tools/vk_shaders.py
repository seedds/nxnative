#!/usr/bin/env python3
# Vulkan games' SPIR-V shaders → one Metal library + a resource table for the host's Vulkan layer (dm/gfx/vk/hle_vk.m). Hades ships its 74 shaders as SPIR-V files
# (RomFS Content/EffectsForgeSwitch/*.vert / *.frag); the game hands the same words to vkCreateShaderModule, so a shader is found again by the hash of its SPIR-V.
# Per shader:
#   · key = sha256(SPIR-V words)[:16] (hex); the Metal function is vk_<key>
#   · its descriptor bindings are renumbered per kind (uniform / storage buffers → [[buffer(n)]], images → [[texture(n)]], samplers → [[sampler(n)]], n from 0 in binding order)
#     by rewriting the Binding decorations in a copy of the SPIR-V, then translated with spirv-cross --msl-decoration-binding: the slots stay below Metal's limits whatever the
#     game's set / binding numbers are (Hades uses set 1 bindings up to 49), and the table records which (set, binding) each slot came from
#   · vertex shaders get --flip-vert-y: Vulkan's clip space has y pointing down, Metal's up, and the viewports stay as the game gives them
#   · vertex buffers take the highest Metal buffer slots not used by the vertex shader's resource bindings
#   · only resources the translated entry point takes are listed (a declared but unused one has no Metal slot to bind)
#   · a push-constant block gets the buffer slot after the descriptor buffers (spirv-cross puts it at buffer(0), where the first uniform buffer goes), listed as p:0.0=slot;
#     the host sets the pushed bytes there (Super Mario Galaxy: 54 of its 56 built-in shaders have one)
#   · compute shaders are translated as kernels; their local size is stored as comp:x,y,z, since Metal takes the threadgroup size at dispatch
#     (Re:ZERO skins its characters and builds its light clusters in compute); specialization-constant local sizes are skipped
#   · stages other than vertex, fragment and compute are skipped and counted: Metal has no geometry shaders (Super Mario Galaxy has 2)
#   · descriptor sets move to argument buffers only when combined per-stage bindings exceed Metal's limits; the table gives the Metal ID of each array's first element
#   · a fragment shader with only outputs 0 and 1 of the same type gets a variant vk_<key>_d with [[color(0), index(1)]]: Super Mario Galaxy's GX shaders write
#     the second blend source at location 1 (no Index decoration), which NVIDIA's hardware feeds to dual-source blending; the host picks the variant for pipelines
#     whose blend factors read source 1
# Super Mario Galaxy generates its GX shaders at run time; they come from a capture folder (the Mac host's DM_VK_CAPTURE), read like a RomFS: every input folder is
# searched for SPIR-V files by their magic
# Output: <out>/vk/vk_shaders.metallib and <out>/vk/vk_shaders.tsv; --ios <dir> also writes <dir>/vk/ with an iPhone library (same table)
# Table rows: key <TAB> stage (vert / frag / comp:x,y,z) <TAB> resources, space-separated kind:set.binding=slot[*count]; lower-case u/b/t/s/c/p bind discrete buffers, textures, samplers and push constants;
#   upper-case U/B/T/S are argument-buffer members, with @id after the slot and count for the first element's Metal ID;
#   o maps a dynamic buffer's set.binding to an offset index, and q gives that offset buffer's Metal slot
#   a RenderDragon vertex shader with an unsigned bone-index variant vk_<key>_u adds the input location in column 5
# Usage: vk_shaders.py <SPIR-V dir | NSO>... <out dir> [--ios <dir>]
import concurrent.futures as cf, hashlib, json, os, re, struct, subprocess, sys, tempfile
from typing import NoReturn
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../tools'))
from romfs import os, open
import nso
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dmsh_chain import SPIRV_CROSS, MAC_MIN_FLAG   # the in-repo spirv-cross build the NVN chain uses too (spirv_cross_bin)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../ios/tools')); from ios_metal import IOS_FLAGS   # the iPhone's deployment target and math mode

MAGIC = 0x07230203
SPIRV_MAGIC = struct.pack('<I', MAGIC)
MATERIAL_MAGIC = struct.pack('<Q', 0x0A11DA1A)
BGFX_SHADER = re.compile(rb'[VF]SH\x05|CSH\x03')
OP_DECORATE, OP_MEMBER_DECORATE, OP_TYPE_ARRAY, OP_TYPE_RTARRAY, OP_TYPE_STRUCT = 71, 72, 28, 29, 30
DEC_ARRAY_STRIDE, DEC_BINDING, DEC_SET = 6, 33, 34
KINDS = {'ubos': 'u', 'ssbos': 'b', 'separate_images': 't', 'images': 't', 'separate_samplers': 's', 'textures': 'c'}
SLOT_CLASS = {'u': 'buffer', 'b': 'buffer', 't': 'texture', 's': 'sampler', 'c': 'texture', 'p': 'buffer'}
# Metal Feature Set Tables, buffers 0–30, textures 0–30 (Apple2/3 minimum), samplers 0–15: https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf
METAL_BINDING_LIMITS = {'buffer': 31, 'texture': 31, 'sampler': 16}
# GLSL's FMin / FMax / FClamp, which spirv-cross writes as fast::min / max / clamp, leave a NaN operand's result to the GPU; the Switch's GPU returns the other operand, as Metal's
# precise:: functions (IEEE fmin / fmax) do on every GPU. Super Mario Galaxy 2's GX lighting normalizes light directions the game leaves at (0, 0, 0) (the key and fill lights of
# its characters) and takes max(0, dot(ldir, NaN)) as the spot term: 0 on the Switch and on the M4 Pro, so the attenuation is 1; on an A20 Pro (iPhone) the half of Mario
# that the fill light (from the camera's left) faces came out white (GitHub issue 72)
PRECISE = re.compile(r'\bfast::(min|max|clamp)\(')

def red(msg: str) -> NoReturn: sys.exit('[RED] ' + msg)

def whole_module(data, start, end):
    if end > len(data) or end - start < 24 or (end - start) % 4: return False
    if data[start:start + 4] != SPIRV_MAGIC or struct.unpack_from('<I', data, start + 16)[0]: return False
    pos = start + 20
    while pos < end:
        words = struct.unpack_from('<I', data, pos)[0] >> 16
        if not words: return False
        pos += words * 4
    return pos == end


def bgfx_modules(data):
    """Read the size-delimited SPIR-V payload of a bgfx v5 graphics or v3 compute shader."""
    for match in BGFX_SHADER.finditer(data):
        pos = match.end() + 4   # shader I/O hash
        if pos + 2 > len(data): break
        uniforms = struct.unpack_from('<H', data, pos)[0]; pos += 2
        for _ in range(uniforms):
            if pos >= len(data): break
            pos += 7 + data[pos]   # name length, name, type, element count, register index and count
        if pos + 4 > len(data): continue
        size = struct.unpack_from('<I', data, pos)[0]
        start, end = pos + 4, pos + 4 + size
        if whole_module(data, start, end): yield start, bytes(data[start:end])


def shader_inputs(root):
    if os.path.isfile(root):   # explicit ExeFS input for built-in shaders
        with open(root, 'rb') as f:
            if f.read(4) == b'NSO0': yield root, 'nso'
        return
    for d, _, fs in os.walk(root):
        for f in sorted(fs):
            p = os.path.join(d, f)
            with open(p, 'rb') as fh: head = fh.read(8)
            if head.startswith(SPIRV_MAGIC): yield p, 'spirv'
            elif head == MATERIAL_MAGIC: yield p, 'material'


def embedded_modules(path, kind):
    with open(path, 'rb') as f: data = f.read()
    if kind == 'nso': data = nso.load(data, path).image
    yield from bgfx_modules(data)


def spirv_files(root):
    for path, kind in shader_inputs(root):
        if kind == 'spirv' or next(embedded_modules(path, kind), None) is not None: yield path


def spirv_sources(roots):
    seen = set()
    for root in roots:
        if not os.path.exists(root): red(f'{root}: no such file or folder')
        for path, kind in shader_inputs(root):
            name = os.path.basename(path) if kind == 'nso' else '/' + os.path.relpath(path, root)
            if kind == 'spirv':
                with open(path, 'rb') as f: modules = [(None, f.read())]
            else:
                modules = embedded_modules(path, kind)
            for offset, data in modules:
                key = hashlib.sha256(data).hexdigest()[:16]   # of the game's words, which the host hashes
                if key in seen: continue
                seen.add(key)
                yield key, name if offset is None else f'{name}@{offset:#x}', data

def decorations(words):
    """variable id → {'set': n, 'binding': n, 'binding_at': word index of the binding value}"""
    out, i = {}, 5
    while i < len(words):
        op, n = words[i] & 0xffff, words[i] >> 16
        if n == 0: red('SPIR-V instruction length 0')
        if op == OP_DECORATE and n >= 4:
            v = out.setdefault(words[i + 1], {})
            if words[i + 2] == DEC_BINDING: v['binding'], v['binding_at'] = words[i + 3], i + 3
            elif words[i + 2] == DEC_SET: v['set'] = words[i + 3]
        i += n
    return out

def fix_strides(words):
    """Super Mario Galaxy's generated shaders put ArrayStride on the struct member (OpMemberDecorate) instead of on the array type: NVIDIA's driver accepts it, spirv-cross
    stops with "Struct member does not have ArrayStride set". Copy each such stride onto its array type, inserted after the last of those member decorations"""
    arrays, have, member, members, at, i = set(), set(), [], {}, None, 5
    while i < len(words):
        op, n = words[i] & 0xffff, words[i] >> 16
        if n == 0: red('SPIR-V instruction length 0')
        if op == OP_DECORATE and n >= 4 and words[i + 2] == DEC_ARRAY_STRIDE: have.add(words[i + 1])
        elif op == OP_MEMBER_DECORATE and n >= 5 and words[i + 3] == DEC_ARRAY_STRIDE: member.append((words[i + 1], words[i + 2], words[i + 4])); at = i + n
        elif op in (OP_TYPE_ARRAY, OP_TYPE_RTARRAY): arrays.add(words[i + 1])
        elif op == OP_TYPE_STRUCT: members[words[i + 1]] = words[i + 2:i + n]
        i += n
    add = {}
    for st, m, stride in member:
        t = members.get(st, [])[m] if m < len(members.get(st, [])) else None
        if t in arrays and t not in have: add.setdefault(t, stride)
    if not add: return words
    return words[:at] + [w for t, s in add.items() for w in ((4 << 16) | OP_DECORATE, t, DEC_ARRAY_STRIDE, s)] + words[at:]

def unsigned_bone_indices(src):
    """RenderDragon's scalar a_indices as an unsigned fetch, widened to int before the shader uses it."""
    attribute = re.search(r'\bint(\s+a_indices\s*\[\[attribute\((\d+)\)\]\];)', src)
    stage_in = re.search(r'\b(\w+)\s*\[\[stage_in\]\]', src)
    if not attribute or not stage_in: return None
    value = re.escape(stage_in[1]) + r'\.a_indices\b'
    src = src[:attribute.start()] + 'uint' + src[attribute.start() + 3:]
    variant, uses = re.subn(r'\b' + value, lambda m: f'int({m[0]})', src)
    if not uses: return None
    return attribute[2], variant


def entry_args(src, key, path):
    """Find the translated entry point with its argument list in group 1."""
    m = re.search(r'\bvk_' + key + r'\((.*?)\)\s*\{', src, re.S)
    if not m: red(f'{path}: entry point vk_{key} not found in the translated MSL')
    return m

def one(source, tmp):
    key, path, data = source
    if len(data) % 4: red(f'{path}: SPIR-V length {len(data)} is not a multiple of 4')
    words = fix_strides(list(struct.unpack(f'<{len(data) // 4}I', data)))
    src_spv = os.path.join(tmp, key + '.in.spv')
    with open(src_spv, 'wb') as f: f.write(struct.pack(f'<{len(words)}I', *words))
    refl = json.loads(subprocess.run([SPIRV_CROSS, src_spv, '--reflect'], capture_output=True, text=True, check=True).stdout)
    eps = refl.get('entryPoints', [])
    if len(eps) != 1: red(f'{path}: entry points {eps} are not exactly one')
    stage = eps[0]['mode']
    if stage not in ('vert', 'frag', 'comp'): return stage   # skipped (main counts them)
    field = stage
    if stage == 'comp':
        wg = eps[0].get('workgroup_size') or []
        if len(wg) != 3 or any(eps[0].get('workgroup_size_is_spec_constant_id') or []): return 'comp with a local size from specialization constants'
        field = 'comp:' + ','.join(str(n) for n in wg)
    dec = decorations(words)
    by_pair, count = {}, {}
    for k, kind in KINDS.items():
        for r in refl.get(k, []):
            pair = (r.get('set', 0), r['binding']); by_pair[pair] = kind
            dims = r.get('array') or [1]
            if len(dims) != 1 or not dims[0]: red(f'{path}: array dimensions {dims} of resource {r.get("name")} are not implemented')
            count[pair] = dims[0]   # arrays occupy consecutive slots in a descriptor set
    pcs = refl.get('push_constants', [])
    if len(pcs) > 1: red(f'{path}: {len(pcs)} push constant blocks')
    # Keep sets discrete until their combined per-stage bindings exceed Apple's Metal limits; moving a set costs a buffer slot.
    set_counts = {}
    dynamic_sets = {pair[0] for pair, kind in by_pair.items() if kind in ('u', 'b')}
    for pair, kind in by_pair.items():
        per_set = set_counts.setdefault(pair[0], {'buffer': 0, 'texture': 0, 'sampler': 0})
        n = count[pair]
        if kind in ('u', 'b'): per_set['buffer'] += n
        if kind in ('t', 'c'): per_set['texture'] += n
        if kind in ('s', 'c'): per_set['sampler'] += n
    argsets = []
    def binding_totals():
        totals = {'buffer': int(bool(pcs)), 'texture': 0, 'sampler': 0}
        for pair, kind in by_pair.items():
            if pair[0] in argsets: continue
            n = count[pair]
            if kind in ('u', 'b'): totals['buffer'] += n
            if kind in ('t', 'c'): totals['texture'] += n
            if kind in ('s', 'c'): totals['sampler'] += n
        totals['buffer'] += len(argsets) + int(any(s in dynamic_sets for s in argsets))
        return totals
    while True:
        totals = binding_totals(); over = {cls for cls, limit in METAL_BINDING_LIMITS.items() if totals[cls] > limit}
        if not over: break
        choices = []
        for s, counts in set_counts.items():
            if s in argsets: continue
            gains = {cls: counts[cls] - (1 if cls == 'buffer' else 0) for cls in over}
            if 'buffer' in over and s in dynamic_sets and not any(t in dynamic_sets for t in argsets):
                gains['buffer'] -= 1   # the first argument-buffer UBO/SSBO set also needs the dynamic-offset table
            score = sum(max(0, n) for n in gains.values())
            if score: choices.append((score, s))
        if not choices: red(f'{path}: resource slots exceeded {totals}')
        argsets.append(max(choices, key=lambda item: (item[0], -item[1]))[1])
    argsets.sort()
    # Vulkan descriptor arrays consume one binding. In a Metal argument buffer each element needs its own ID.
    arg_ids, next_id = {}, dict.fromkeys(argsets, 0)
    for pair in sorted(count):
        s, _ = pair
        if s not in next_id: continue
        arg_ids[pair] = next_id[s]
        next_id[s] += count[pair]
    slots, nxt, res = {}, {'buffer': 0, 'texture': 0, 'sampler': 0}, []
    for vid, d in sorted(dec.items(), key=lambda t: (t[1].get('set', 0), t[1].get('binding', 0))):
        if 'binding_at' not in d: continue
        pair = (d.get('set', 0), d['binding'])
        kind = by_pair.get(pair)
        if not kind: continue   # declared but unused: spirv-cross drops it
        if pair in arg_ids:
            words[d['binding_at']] = arg_ids[pair]
            continue
        if pair in slots: n = slots[pair]
        else:
            cls, k = SLOT_CLASS[kind], count[pair]; n = nxt[cls]; nxt[cls] += k
            if kind == 'c': nxt['sampler'] = max(nxt['sampler'], n + k)
            slots[pair] = n; res.append(f'{kind}:{pair[0]}.{pair[1]}={n}' + (f'*{k}' if k > 1 else ''))
        words[d['binding_at']] = n
    pc_slot = nxt['buffer'] if pcs else None
    if pcs: nxt['buffer'] += 1
    ab_slot = {}
    for s in argsets: ab_slot[s] = nxt['buffer']; nxt['buffer'] += 1
    dyn = [p for p in sorted(count) if p[0] in argsets and by_pair[p] in ('u', 'b')]
    dyn_slot = None
    if dyn: dyn_slot = nxt['buffer']; nxt['buffer'] += 1
    if any(nxt[cls] > limit for cls, limit in METAL_BINDING_LIMITS.items()): red(f'{path}: resource slots exceeded {nxt}')
    spv = os.path.join(tmp, key + '.spv'); msl = os.path.join(tmp, key + '.metal')
    with open(spv, 'wb') as f: f.write(struct.pack(f'<{len(words)}I', *words))
    # --msl-texture-1d-as-2d: a 1D image is a one-row 2D texture in the host (hle_vk.m vkCreateImage), since Metal's 1D texture cannot be drawn into, mipmapped or depth; spirv-cross samples it at
    # the row's centre (float2(x, 0.5)) and reads it at (x, 0)
    # the entry point keeps the name it has in the SPIR-V (HLSL-compiled shaders name theirs PSMain or ps_*, not main); the host finds the function by key, not pName.
    # Texel buffers are native texture_buffer<T>, as the host binds them (bview_tex: MTLTextureTypeTextureBuffer); spirv-cross's default emulates them as a 2D texture
    # 4096 texels wide, which reads garbage from the bound buffer texture (Metal validation: "incorrect type of texture", Re:ZERO's ClusteredLight kernel)
    cmd = [SPIRV_CROSS, spv, '--msl', '--msl-version', '20300', '--msl-decoration-binding', '--msl-texture-1d-as-2d', '--msl-texture-buffer-native', '--rename-entry-point', eps[0]['name'], f'vk_{key}',
           stage, '--output', msl]
    if argsets:
        cmd += ['--msl-argument-buffers', '--msl-argument-buffer-tier', '1']
        for s in range(8):
            if s not in argsets: cmd += ['--msl-discrete-descriptor-set', str(s)]
        for s, b in dyn: cmd += ['--msl-dynamic-buffer', str(s), str(arg_ids[s, b])]
    if stage == 'vert': cmd.append('--flip-vert-y')
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode: red(f'{path}: spirv-cross failed: {r.stderr.strip()[:300]}')
    # The reflection lists every declared resource; the Metal entry point only takes the ones the shader uses (Hades' ShadowMap.frag declares a buffer it never reads,
    # which the game's own pipeline layout leaves out): keep the rows whose slot is an argument of the entry point, so the host only binds, and only reports, real ones
    with open(msl) as f: src = f.read()
    m = entry_args(src, key, path)
    if pcs:   # the push-constant argument is `constant <block type>& name [[buffer(0)]]`: move it to its own slot
        # SPIRV-Cross sanitizes DXC names such as type.PushConstant.RootConstants on output.
        tname = re.sub(r'[^A-Za-z0-9_]', '_', refl['types'][pcs[0]['type']]['name'])
        a = re.search(r'constant\s+' + re.escape(tname) + r'&\s+\w+\s+\[\[buffer\((\d+)\)\]\]', m.group(1))
        if a:
            s0, s1 = m.start(1) + a.start(1), m.start(1) + a.end(1)
            src = src[:s0] + str(pc_slot) + src[s1:]
            m = entry_args(src, key, path)
            res.append(f'p:0.0={pc_slot}')
    if argsets:
        def param(pat, slot):   # move an entry-point argument `<name> [[buffer(n)]]` to its assigned slot
            nonlocal src, m
            a = re.search(pat + r'\s+\[\[buffer\((\d+)\)\]\]', m.group(1))
            if not a: return False
            s0, s1 = m.start(1) + a.start(1), m.start(1) + a.end(1)
            src = src[:s0] + str(slot) + src[s1:]; m = entry_args(src, key, path)
            return True
        names = {}   # (set, member name) → argument-buffer ID
        for s, body in re.findall(r'struct spvDescriptorSetBuffer(\d+)\s*\{(.*?)\};', src, re.S):
            for name, i in re.findall(r'(\w+)\s+\[\[id\((\d+)\)\]\]', body): names[(int(s), name)] = int(i)
        by_arg_id = {(s, i): b for (s, b), i in arg_ids.items()}
        for s in argsets:
            if not param(rf'spvDescriptorSet{s}', ab_slot[s]): continue   # the shader uses nothing of the set
            ids = {i for (t, _), i in names.items() if t == s}
            for p, i in arg_ids.items():
                if p[0] != s or i not in ids: continue
                if by_pair[p] == 'c': red(f'{path}: combined image sampler {p} in an argument buffer not implemented')
                array = f'*{count[p]}' if count[p] > 1 else ''
                res.append(f'{by_pair[p].upper()}:{s}.{p[1]}={ab_slot[s]}{array}@{i}')
        if param(r'spvDynamicOffsets', dyn_slot):
            res.append(f'q:0.0={dyn_slot}')
            for s, name, i in sorted(set(re.findall(r'spvDescriptorSet(\d+)\.(\w+)\s*\+\s*spvDynamicOffsets\[(\d+)\]', src))):
                res.append(f'o:{s}.{by_arg_id[int(s), names[int(s), name]]}={i}')
    used = {(c, int(n)) for c, n in re.findall(r'\[\[(buffer|texture|sampler)\((\d+)\)\]\]', m.group(1))}
    res = [x for x in res if x[0] not in SLOT_CLASS or (SLOT_CLASS[x[0]], int(x.rsplit('=', 1)[1].split('*')[0])) in used]
    src = PRECISE.sub(r'precise::\1(', src)
    with open(msl, 'w') as f: f.write(src)
    msls = [msl]
    bone_index = ''
    if stage == 'frag':
        # Metal requires two outputs of the same type for the dual-source variant.
        outs = {int(i): t for t, i in re.findall(r'(\w+)\s+\w+\s+\[\[color\((\d+)\)\]\]', src)}
        if outs.keys() == {0, 1} and outs[0] == outs[1]:
            msl_d = os.path.join(tmp, key + '_d.metal'); msls.append(msl_d)
            with open(msl_d, 'w') as f: f.write(src.replace(f'vk_{key}', f'vk_{key}_d').replace('[[color(1)]]', '[[color(0), index(1)]]'))
    if stage == 'vert' and '.material.bin@' in path and (unsigned := unsigned_bone_indices(src)):
        bone_index, variant = unsigned
        msl_u = os.path.join(tmp, key + '_u.metal'); msls.append(msl_u)
        open(msl_u, 'w').write(variant.replace(f'vk_{key}', f'vk_{key}_u'))
    return key, field, res, msls, path, bone_index

def metallib(msls, out, sdk, extra):
    os.makedirs(out, exist_ok=True)
    airs = []
    def comp(m):
        a = m[:-6] + f'.{sdk}.air'
        # -fwrapv: signed integer overflow wraps, as SPIR-V's integer arithmetic does; in MSL it is undefined and the compiler folds around it (dmsh_chain.py MAC_METAL)
        r = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-c', '-fpreserve-invariance', '-fwrapv', *extra, m, '-o', a], capture_output=True, text=True)
        if r.returncode: red(f'{m}: metal compile failed: {r.stderr.strip()[:400]}')
        return a
    with cf.ThreadPoolExecutor(os.cpu_count()) as ex: airs = list(ex.map(comp, msls))
    lib = os.path.join(out, 'vk_shaders.metallib')
    # relative to their common folder: thousands of absolute paths under a long work folder pass ARG_MAX (1 MB on macOS) and xcrun fails with E2BIG
    base = os.path.commonpath([os.path.dirname(x) for x in airs])
    subprocess.run(['xcrun', '-sdk', sdk, 'metallib', *[os.path.relpath(x, base) for x in airs], '-o', os.path.abspath(lib)], check=True, cwd=base)
    return lib

def main():
    a = sys.argv[1:]
    ios = None
    if '--ios' in a: i = a.index('--ios'); ios = a[i + 1]; del a[i:i + 2]
    if len(a) < 2: sys.exit('usage: vk_shaders.py <SPIR-V dir | NSO>... <out dir> [--ios <dir>]')
    roots, out = a[:-1], a[-1]
    files = list(spirv_sources(roots))
    if not files: red(f'no SPIR-V files in {" ".join(roots)}')
    with tempfile.TemporaryDirectory() as tmp:
        with cf.ThreadPoolExecutor(os.cpu_count()) as ex: rows = list(ex.map(lambda src: one(src, tmp), files))
        seen, skipped = {}, {}
        for row in rows:
            if isinstance(row, str): skipped[row] = skipped.get(row, 0) + 1; continue
            key, stage, res, msl, src, info = row
            seen[key] = (stage, res, msl, src, info)
        if skipped: print('[RED] skipped unsupported shader stages: ' + ', '.join(f'{k} {v}' for k, v in sorted(skipped.items())))
        msls = [m for v in seen.values() for m in v[2]]
        tsv = ['# Vulkan shaders (dm/gfx/tools/vk_shaders.py): key = sha256(SPIR-V)[:16], Metal function vk_<key>; stage vert/frag/comp:x,y,z; resource kinds u/b/t/s/c/p and argument-buffer U/B/T/S/o/q',
               '# spirv-cross ' + ' '.join((lambda r: r.stdout + r.stderr)(subprocess.run([SPIRV_CROSS, '--revision'], capture_output=True, text=True)).split())]
        tsv += [f'{k}\t{stage}\t{" ".join(res)}\t{src}' + (f'\t{info}' if info else '')
                for k, (stage, res, _, src, info) in sorted(seen.items())]
        dirs = [(os.path.join(out, 'vk'), 'macosx', [MAC_MIN_FLAG])]   # the Mac host's minimum macOS, as the NVN chain's libraries (dmsh_chain.py MAC_MIN)
        # The iPhone library is compiled with -fmetal-math-mode=relaxed, as the NVN chain's (dm/ios/tools/ios_metal.py): the default fast mode lets the GPU compiler assume there is
        # no NaN / Inf, which would let it drop the NaN handling of the precise:: functions above
        if ios: dirs.append((os.path.join(ios, 'vk'), 'iphoneos', list(IOS_FLAGS)))
        for d, sdk, extra in dirs:
            lib = metallib(msls, d, sdk, extra)
            open(os.path.join(d, 'vk_shaders.tsv'), 'w').write('\n'.join(tsv) + '\n')
            print(f'{sdk}: {len(seen)} shaders → {lib}')

if __name__ == '__main__':
    try: main()
    except nso.Error as e: red(str(e))
