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
#   · triangle pass-through geometry stages that only route an instance to a viewport are folded into the vertex stage; other geometry stages are skipped and counted
#   · descriptor sets move to argument buffers only when combined per-stage bindings exceed Metal's limits; the table gives the Metal ID of each array's first element
#   · a fragment shader with only outputs 0 and 1 of the same type gets a variant vk_<key>_d with [[color(0), index(1)]]: Super Mario Galaxy's GX shaders write
#     the second blend source at location 1 (no Index decoration), which NVIDIA's hardware feeds to dual-source blending; the host picks the variant for pipelines
#     whose blend factors read source 1
# Super Mario Galaxy generates its GX shaders at run time; they come from a capture folder (the Mac host's DM_VK_CAPTURE), read like a RomFS: every input folder is
# searched for SPIR-V files by their magic
# Output: <out>/vk/vk_shaders.metallib and <out>/vk/vk_shaders.tsv; --ios <dir> also writes <dir>/vk/ with an iPhone library (same table)
# Table rows: key <TAB> stage (vert / frag / comp:x,y,z / geomvp:location) <TAB> resources, space-separated kind:set.binding=slot[*count]; lower-case u/b/t/s/c/p bind discrete buffers, textures, samplers and push constants;
#   upper-case U/B/T/S are argument-buffer members, with @id after the slot and count for the first element's Metal ID;
#   o maps a dynamic buffer's set.binding to an offset index, and q gives that offset buffer's Metal slot
# RenderDragon material bone-index inputs are repaired in SPIR-V before translation; keys still identify the game's original words.
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
DEC_BUILTIN, DEC_LOCATION = 11, 30
POSITION, INSTANCE_INDEX, VIEWPORT_INDEX = 0, 43, 10
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

def unsigned_bone_indices(words):
    """Repair RenderDragon's R16_UINT bone input, preserving signed values/arithmetic after each load.

    Its material compiler declares scalar a_indices as int. The engine's bone layout supplies an unsigned
    short (the binary trace and the other layout paths are recorded in Minecraft's game note).
    """
    instructions, names, ints, pointers, variables, models = [], set(), {}, {}, {}, []
    locations, interface = set(), set()
    i = 5
    while i < len(words):
        n, op = words[i] >> 16, words[i] & 0xffff
        if not n: red('SPIR-V instruction length 0')
        ins = words[i:i + n]; instructions.append(ins)
        if op == 5 and struct.pack(f'<{n - 2}I', *ins[2:]).split(b'\0', 1)[0] == b'a_indices': names.add(ins[1])   # OpName
        elif op == 15:   # OpEntryPoint: interfaces follow the nul-terminated entry name
            models.append(ins[1])
            end = next(j for j in range(3, n) if b'\0' in struct.pack('<I', ins[j]))
            interface.update(ins[end + 1:])
        elif op == 21: ints[ins[1]] = tuple(ins[2:])   # OpTypeInt
        elif op == 32: pointers[ins[1]] = tuple(ins[2:])   # OpTypePointer
        elif op == 59: variables[ins[2]] = (ins[1], ins[3])   # OpVariable
        elif op == OP_DECORATE and n >= 4 and ins[2] == 30: locations.add(ins[1])   # Location
        i += n
    if models != [0]: return words   # vertex entry point only
    targets = {v for v in names & locations & interface if v in variables and variables[v][1] == 1
               and pointers.get(variables[v][0], ())[:1] == (1,)
               and ints.get(pointers[variables[v][0]][1]) == (32, 1)}
    if not targets: return words
    # Only direct loads occur in this asset contract. Reject a changed pointer-use shape before changing its type.
    in_function = False
    for ins in instructions:
        op = ins[0] & 0xffff
        if op == 54: in_function = True   # OpFunction
        if op in (8, 317): continue   # OpLine / OpNoLine contain debug literals, not input-pointer uses
        if in_function and targets.intersection(ins[1:]) and not (op == 61 and ins[3] in targets):
            red('RenderDragon a_indices input has a use other than OpLoad')
    targets &= {ins[3] for ins in instructions if ins[0] & 0xffff == 61}   # ignore declared but unread inputs
    if not targets: return words
    bound = words[3]
    uint = next((t for t, shape in ints.items() if shape == (32, 0)), None)
    if uint is None: uint, bound = bound, bound + 1
    pointer = next((t for t, shape in pointers.items() if shape == (1, uint)), None)
    if pointer is None: pointer, bound = bound, bound + 1
    # Put the unsigned scalar/pointer declarations before the first integer type. Moving existing definitions
    # earlier also handles a module whose uint type was originally declared after its signed input variable.
    out, inserted = list(words[:5]), False
    for ins in instructions:
        op = ins[0] & 0xffff
        if op == 21 and not inserted:
            out += [(4 << 16) | 21, uint, 32, 0, (4 << 16) | 32, pointer, 1, uint]
            inserted = True
        if op in (21, 32) and ins[1] in (uint, pointer): continue
        if op == 59 and ins[2] in targets: ins = [ins[0], pointer, *ins[2:]]
        if op == 61 and ins[3] in targets:   # OpLoad, then OpBitcast under the original signed result ID
            out += [ins[0], uint, bound, *ins[3:], (4 << 16) | 124, ins[1], ins[2], bound]
            bound += 1
        else: out += ins
    out[3] = bound
    return out


def entry_args(src, key, path):
    """Find the translated entry point with its argument list in group 1."""
    m = re.search(r'\bvk_' + key + r'\((.*?)\)\s*\{', src, re.S)
    if not m: red(f'{path}: entry point vk_{key} not found in the translated MSL')
    return m

def spv_ops(words):
    i = 5
    while i < len(words):
        op, n = words[i] & 0xffff, words[i] >> 16
        if not 0 < n <= len(words) - i: red('malformed SPIR-V instruction length')
        yield op, tuple(words[i + 1:i + n])
        i += n

def entry_interface(entry):
    # The null-terminated entry-point name occupies whole words between the function and interface IDs.
    return entry[next(i for i in range(2, len(entry)) if 0 in entry[i].to_bytes(4, 'little')) + 1:]

def geometry_viewport(words):
    """Prove that the geometry stage emits the three input vertices in order, forwarding every output.

    Treat each input element as a distinct symbolic value and run only the SPIR-V operations needed for
    copying values and traversing a constant-bound loop. A data-dependent branch or any other operation
    defeats the proof. The result is the location the geometry shader copies to ViewportIndex.
    """
    ops = list(spv_ops(words))
    entry, = (a for op, a in ops if op == 15)   # OpEntryPoint; one() checked there is exactly one
    func = entry[1]
    modes = {a[1:] for op, a in ops if op == 16 and a[0] == func}
    # Triangles, OutputTriangleStrip, OutputVertices 3, Invocations 1; no other execution modes.
    if modes - {(0, 1)} != {(22,), (29,), (26, 3)}: return None
    if [a[1] for op, a in ops if op == 54] != [func]: return None   # one OpFunction
    if any(op == 17 and a not in ((1,), (2,), (57,)) for op, a in ops): return None   # Shader, Geometry, MultiViewport
    if any(op in (10, 331, 332) for op, _ in ops): return None   # extensions, ExecutionModeId, DecorateId
    dec = {}
    for op, a in ops:
        if op == OP_DECORATE:
            if len(a) != 3 or a[1] not in (DEC_BUILTIN, DEC_LOCATION) or a[0] in dec: return None
            dec[a[0]] = a[1:]
    ints = {a[0] for op, a in ops if op == 21 and a[1] == 32}   # OpTypeInt
    uints = {a[0] for op, a in ops if op == 21 and a[1:] == (32, 0)}
    scalars = ints | {a[0] for op, a in ops if op == 22 and a[1] == 32}
    varyings = scalars | {a[0] for op, a in ops if op == 23 and a[1] in scalars}
    vals = {a[1]: a[2] for op, a in ops if op == 43 and a[0] in ints}
    ptr_types = {a[0]: a[1:] for op, a in ops if op == 32}   # OpTypePointer
    arrays = {a[0]: a[1] for op, a in ops if op == 28 and vals.get(a[2]) == 3}
    variables = {a[1]: a[0] for op, a in ops if op == 59 and a[2] != 7}   # OpVariable outside Function
    inputs, outputs, memory = {}, {}, {}
    if set(variables) != set(entry_interface(entry)): return None
    for var, ty in variables.items():
        if var not in dec: return None
        storage, ty = ptr_types[ty]
        if storage == 1 and ty in arrays and arrays[ty] in varyings:
            if dec[var] in inputs: return None
            inputs[dec[var]] = arrays[ty], var
            memory[var] = tuple(object() for _ in range(3))
        elif storage == 3 and ty in varyings:
            if dec[var] in outputs: return None
            outputs[dec[var]] = ty, var
        else: return None
    position, viewport = (DEC_BUILTIN, POSITION), (DEC_BUILTIN, VIEWPORT_INDEX)
    if position not in outputs or outputs.get(viewport, (None,))[0] not in uints: return None
    forwarded = {}
    for d, (ty, var) in outputs.items():
        if d == viewport: continue
        if d != position and d[0] != DEC_LOCATION: return None
        if d not in inputs or inputs[d][0] != ty: return None
        forwarded[var] = memory[inputs[d][1]]
    routes = {d[1]: memory[var] for d, (ty, var) in inputs.items() if d[0] == DEC_LOCATION and d[1] < 32 and d not in outputs and ty in uints}
    if not routes: return None
    target = outputs[viewport][1]

    body = ops[next(i for i, (op, _) in enumerate(ops) if op == 54) + 1:]
    labels = {a[0]: i for i, (op, a) in enumerate(body) if op == 248}
    pointers = {v: (v, ()) for v in variables}
    locals_ = set()
    route, emitted, ended = None, 0, False
    pc, block, prev = 0, None, None
    try:
        for _ in range(256):   # a proof never runs indefinitely on a foreign shader
            op, a = body[pc]
            pc += 1
            if op == 248:   # OpLabel; resolve consecutive OpPhi in parallel
                prev, block = block, a[0]
                phis = {}
                while body[pc][0] == 245:
                    phi = body[pc][1]
                    phis[phi[1]] = vals[dict(zip(phi[3::2], phi[2::2]))[prev]]
                    pc += 1
                vals.update(phis)
            elif op == 59:
                if len(a) != 3 or a[2] != 7: return None
                locals_.add(a[1])
                pointers[a[1]] = a[1], ()
            elif op == 65:   # OpAccessChain
                base, indices = pointers[a[2]]
                pointers[a[1]] = base, indices + tuple(vals[i] for i in a[3:])
            elif op == 61:   # OpLoad
                if len(a) != 3: return None
                base, indices = pointers[a[2]]
                value = memory[base]
                for i in indices: value = value[i]
                vals[a[1]] = value
            elif op == 62:   # OpStore: whole locals and outputs only
                if len(a) != 2 or (a[0] not in locals_ and a[0] != target and a[0] not in forwarded): return None
                memory[a[0]] = vals[a[1]]
            elif op == 80:   # OpCompositeConstruct
                vals[a[1]] = tuple(vals[i] for i in a[2:])
            elif op == 81:   # OpCompositeExtract
                value = vals[a[2]]
                for i in a[3:]: value = value[i]
                vals[a[1]] = value
            elif op == 83:   # OpCopyObject
                vals[a[1]] = vals[a[2]]
            elif op == 128 and a[0] in ints:   # OpIAdd (32-bit)
                vals[a[1]] = (vals[a[2]] + vals[a[3]]) & 0xffffffff
            elif op == 177:   # OpSLessThan: flip the sign bit to compare as signed
                vals[a[1]] = (vals[a[2]] ^ 0x80000000) < (vals[a[3]] ^ 0x80000000)
            elif op == 218:   # OpEmitVertex; SPIR-V makes the outputs undefined after each emit
                if ended or emitted == 3 or any(memory.pop(var) is not values[emitted] for var, values in forwarded.items()): return None
                value = memory.pop(target)
                match = next((loc for loc, values in routes.items() if value is values[emitted]), None)
                if match is None or (emitted and route != match): return None
                route, emitted = match, emitted + 1
            elif op == 219:   # OpEndPrimitive; otherwise return ends the final strip
                if emitted != 3 or ended: return None
                ended = True
            elif op in (246, 247):   # structured control-flow declarations
                continue
            elif op == 249:   # OpBranch
                pc = labels[a[0]]
            elif op == 250 and isinstance(vals[a[0]], bool):   # OpBranchConditional
                pc = labels[a[1] if vals[a[0]] else a[2]]
            elif op == 253:   # OpReturn
                return route if emitted == 3 else None
            else: return None
    except (IndexError, KeyError, TypeError):
        return None
    return None

def instance_locations(words):
    """Metal requires the same viewport index at every vertex of a primitive."""
    ops = list(spv_ops(words))
    entry, = (a for op, a in ops if op == 15)
    # Restrict the proof to ordinary logical pointers and a single function. Outputs can only be
    # written via their variable or an access chain, and the scalar locations below have no members.
    if sum(op == 54 for op, _ in ops) != 1 or any(op in (10, 67, 70) or op == 14 and a[0] != 0 or
                                                 op == 17 and a[0] in (4441, 4442) for op, a in ops):
        return []
    interface = set(entry_interface(entry))
    builtin = {a[0] for op, a in ops if op == OP_DECORATE and a[1:] == (DEC_BUILTIN, INSTANCE_INDEX) and a[0] in interface}
    values = {a[1] for op, a in ops if op == 61 and a[2] in builtin}   # OpLoad InstanceIndex
    if not values: return []
    outputs = {a[1] for op, a in ops if op == 59 and a[2] == 3 and a[1] in interface}
    stores = {v: set() for v in outputs}
    for op, a in ops:
        if op == 62 and a[0] in stores: stores[a[0]].add(a[1])
        # A derived pointer or a copy into a candidate output would need a separate proof.
        elif op in (65, 66, 83) and a[2] in stores: outputs.discard(a[2])
        elif op in (63, 64) and a[0] in stores: outputs.discard(a[0])
    locations = {}
    for op, a in ops:
        if op == OP_DECORATE and len(a) == 3 and a[1] == DEC_LOCATION and a[0] in stores:
            locations.setdefault(a[2], []).append(a[0])
    return sorted(loc for loc, members in locations.items() if len(members) == 1 and
                  members[0] in outputs and stores[members[0]] and stores[members[0]] <= values)

def viewport_variant(src, key, location):
    out = re.search(rf'struct vk_{key}_out\s*\{{(.*?)\}};', src, re.S)
    if not out: return None
    # Removing the geometry stage must not expose vertex builtins that the stage did not forward.
    if any(a != 'position' and not a.startswith('user(') for a in re.findall(r'\[\[([^\]]+)\]\]', out[1])):
        return None
    member = re.search(rf'\buint\s+\w+\s+(\[\[user\(locn{location}\)\]\]);', out[1])
    if not member: return None
    i, j = out.start(1) + member.start(1), out.start(1) + member.end(1)
    return (src[:i] + '[[viewport_array_index]]' + src[j:]).replace(f'vk_{key}', f'vk_{key}_vp{location}')

def one(source, tmp):
    key, path, data = source
    if len(data) % 4: red(f'{path}: SPIR-V length {len(data)} is not a multiple of 4')
    words = fix_strides(list(struct.unpack(f'<{len(data) // 4}I', data)))
    if '.material.bin@' in path: words = unsigned_bone_indices(words)
    src_spv = os.path.join(tmp, key + '.in.spv')
    with open(src_spv, 'wb') as f: f.write(struct.pack(f'<{len(words)}I', *words))
    refl = json.loads(subprocess.run([SPIRV_CROSS, src_spv, '--reflect'], capture_output=True, text=True, check=True).stdout)
    eps = refl.get('entryPoints', [])
    if len(eps) != 1: red(f'{path}: entry points {eps} are not exactly one')
    stage = eps[0]['mode']
    if stage == 'geom':
        location = geometry_viewport(words)
        if location is not None: return key, f'geomvp:{location}', [], [], path
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
    if stage == 'frag':
        # Metal requires two outputs of the same type for the dual-source variant.
        outs = {int(i): t for t, i in re.findall(r'(\w+)\s+\w+\s+\[\[color\((\d+)\)\]\]', src)}
        if outs.keys() == {0, 1} and outs[0] == outs[1]:
            msl_d = os.path.join(tmp, key + '_d.metal'); msls.append(msl_d)
            with open(msl_d, 'w') as f: f.write(src.replace(f'vk_{key}', f'vk_{key}_d').replace('[[color(1)]]', '[[color(0), index(1)]]'))
    elif stage == 'vert':
        for location in instance_locations(words):
            variant = viewport_variant(src, key, location)
            if variant is None: continue
            msl_vp = os.path.join(tmp, f'{key}_vp{location}.metal'); msls.append(msl_vp)
            with open(msl_vp, 'w') as f: f.write(variant)
    return key, field, res, msls, path

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
            key, stage, res, msl, src = row
            seen[key] = (stage, res, msl, src)
        if skipped: print('[RED] skipped unsupported shader stages: ' + ', '.join(f'{k} {v}' for k, v in sorted(skipped.items())))
        viewport_suffixes = {f'_vp{v[0].split(":")[1]}.metal' for v in seen.values() if v[0].startswith('geomvp:')}
        msls = [m for v in seen.values() for m in v[2] if '_vp' not in os.path.basename(m) or any(m.endswith(s) for s in viewport_suffixes)]
        tsv = ['# Vulkan shaders (dm/gfx/tools/vk_shaders.py): key = sha256(SPIR-V)[:16], Metal function vk_<key>; stage vert/frag/comp:x,y,z/geomvp:location; resource kinds u/b/t/s/c/p and argument-buffer U/B/T/S/o/q',
               '# spirv-cross ' + ' '.join((lambda r: r.stdout + r.stderr)(subprocess.run([SPIRV_CROSS, '--revision'], capture_output=True, text=True)).split())]
        tsv += [f'{k}\t{stage}\t{" ".join(res)}\t{src}' for k, (stage, res, _, src) in sorted(seen.items())]
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
