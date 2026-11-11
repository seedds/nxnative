"""NVN2 task and mesh shaders as Metal object and mesh functions.

The Switch 2's Ampere has them; a shader says so in its control section (+0x56: 6 task, 7 mesh), while its SPH is of the vertex or domain kind. The translator reads one as a compute
shader with its SPH taken off and its stage memory in shared memory (job stage_memory, InstEmitAmpereMem.cs AIsberd, AStageOut). This wraps that kernel into the Metal stage function:
the wrapper fills the stage inputs, calls the kernel's body and hands the stage outputs to Metal.

Stage memory, in shared memory (memory()), all of it zero when the body starts:
  word 0        the workgroup's number (ISBERD mode 0x78): a task's is the draw's first task + its threadgroup, a mesh shader's with no task its block's first + its threadgroup

A mesh shader with no task also gets an object function, main0_o: Metal launches at most METAL_MAX_GRID threadgroups in a mesh grid, a limit the object grid does not have, and a draw's
workgroups went past it (Ocarina of Time's foliage: up to 1,056 a draw, PRAGMATA's 6,623; past the limit the GPU read out of bounds). Each of its one-thread threadgroups launches a
block of up to METAL_MAX_GRID mesh threadgroups and hands them the block's first workgroup in its payload, so a draw is one Metal draw of ceil(count / METAL_MAX_GRID) object
threadgroups (nvn_geometry.inc mtk_encode). The task and object functions read the draw's {count, first} at buffer MESH_DRAW_SLOT, NVN's DrawMeshTasksIndirect record
  INPUT         the payload a mesh shader's task wrote (ISBERD mode 0x38). Its word 0 is the mesh workgroup's number among its task's launches, which the hardware puts there
  outputs       a task's payload (0xb8): its word 1 is the number of mesh workgroups to launch, its records start at +0x10
                or a mesh shader's attributes (0xf8): all blocks of 32 vertices (the components the SPH's OMAP enables, 0x80 bytes each, a word per vertex), followed by all blocks of 32
                primitives (the components the per-primitive SPH's OMAP enables, 0x80 bytes each, a word per primitive)
  primitives    a mesh shader's primitive bytes: the count at +3 (0x88), index bytes from +4 (0x80, control +0x5f per primitive: 1 point, 2 line, 3 triangle)
"""
import re
import struct
import dmsh_abi as abi

WORKGROUP, INPUT = 0, 0x10
BLOCK = 32   # vertices and primitives of an attribute block
TOPOLOGY = {1: 'point', 2: 'line', 3: 'triangle'}
METAL_MAX_VERTICES, METAL_MAX_PRIMITIVES = 256, 512   # a Metal mesh's limits
METAL_MAX_GRID = abi.MESH_GRID   # mesh threadgroups an object threadgroup may launch
METAL_MAX_SHARED = 0x8000   # threadgroup memory of a Metal function


def is_stage(ctl):
    """'ts', 'ms' or None, by the control section's stage byte."""
    return {6: 'ts', 7: 'ms'}.get(ctl[0x56]) if len(ctl) > 0x56 else None


def sph_outputs(sph):
    """The components an SPH's OMAP enables, in the order of the attribute area: [('psiz', 0)] for the point size (byte 0x35, bit 3), [(None, c)] for the position's (byte 0x35,
    bits 4..7), then [(n, c)] for generic attribute n's (bytes 0x36 on, a nibble an attribute, bit c its component)."""
    out = ([('psiz', 0)] if sph[0x35] >> 3 & 1 else []) + [(None, c) for c in range(4) if sph[0x35] >> (4 + c) & 1]
    for n in range(32):
        out += [(n, c) for c in range(4) if sph[0x36 + n // 2] >> (4 * (n % 2) + c) & 1]
    return out


def info(ctl, area):
    """A task or mesh shader's facts from its control section and its whole code area (the SPH, the instructions, a mesh shader's second SPH): +0x58 its workgroup's threads, +0x5a its
    payload's bytes, +0x5e whether a mesh shader has a task, +0x5f a primitive's vertices, +0x60 / +0x62 a mesh shader's most vertices and primitives, +0x64 the offset of its per-primitive
    SPH where +0x68 bit 7 is set."""
    kind = is_stage(ctl)
    threads, payload = struct.unpack_from('<2H', ctl, 0x58)
    out = {'kind': kind, 'threads': threads, 'payload': payload}
    if kind == 'ms':
        max_v, max_p = struct.unpack_from('<2H', ctl, 0x60)
        at = struct.unpack_from('<I', ctl, 0x64)[0] if ctl[0x68] & 0x80 else 0
        prim = sph_outputs(area[at:at + 0x80]) if at else []
        out.update(task=bool(ctl[0x5e]), verts=ctl[0x5f] or 3, max_v=max_v, max_p=max_p, vertex=sph_outputs(area[:0x80]), primitive=[c for c in prim if isinstance(c[0], int)])
    return out


def memory(i):
    """(the stage memory's offsets for the translator: the workgroup's number, the input, the outputs, the primitive bytes; the shared memory's bytes)."""
    out = INPUT + (i['payload'] + 15 & ~15)
    if i['kind'] == 'ts':
        return [WORKGROUP, INPUT, out, out + i['payload']], out + (i['payload'] + 3 & ~3)
    # The guest's attribute stores: all vertex blocks of 32 vertices first, then all primitive blocks of 32 primitives, each block its own SPH's components (Ocarina of Time's
    # ee4ef51a4fddc28e, 128 vertices of 6 components and 64 primitives of 4, writes vertex v at (v / 32) * 768 + (v % 32) * 4 and primitive p at 3072 + (p / 32) * 512 + (p % 32) * 4)
    v_blocks = (max(i['max_v'], 1) + BLOCK - 1) // BLOCK
    p_blocks = (max(i['max_p'], 1) + BLOCK - 1) // BLOCK
    prim = out + max(v_blocks * len(i['vertex']) + p_blocks * len(i['primitive']), 1) * BLOCK * 4
    # The guest writes the index bytes of primitive min(count - 1, lane + 32k) in each of its primitive blocks, and a meshlet count of 0 wraps count - 1, so every lane of every primitive
    # block writes: the bytes are reserved for whole blocks of 32 primitives, a primitive's vertices each (a mesh with max_p 126 wrote past 4 + 3 * 126, into the next workgroup's memory)
    return [WORKGROUP, INPUT, out, prim], prim + (4 + i.get('verts', 3) * p_blocks * BLOCK + 3 & ~3)


def own_shared(code):
    """Whether the instructions use shared memory of their own (LDS, STS, ATOMS: Ampere opcodes as DecoderAmpere.cs marks them). It would lie over the stage memory, and the control
    section does not give its size."""
    return any(struct.unpack_from('<H', code, o)[0] & 0xfff in (0x984, 0x388, 0x988, 0x38c, 0x98c, 0xf8c) for o in range(0, len(code) - 15, 16))


# The kernel, its parameters, the thread variables spirv-cross declares ahead of the threadgroup memory (a private variable's initializer: GlobalToStorage.TryGenerateRawStorageOp's last entry,
# uint _N = 0u;), and its stage memory, the first threadgroup array
_KERNEL = re.compile(r'kernel\s+void\s+main0\s*\(((?:[^()]|\([^()]*\))*)\)\s*\{\s*((?:(?!threadgroup\b)[^\n{};]+;\n\s*)*)threadgroup\s+spvUnsafeArray<uint,\s*(\d+)>\s+(\w+);\n')
_NAME = re.compile(r'(\w+)\s*(?:\[\[.*\]\])?$')
_THREAD = re.compile(r'(uint3?)\s+(\w+)\s*\[\[(thread_position_in_threadgroup|threadgroup_position_in_grid)\]\]$')


def _params(text):
    """Split a parameter list at its top-level commas."""
    out, depth, cur = [], 0, ''
    for ch in text:
        if ch == ',' and not depth: out.append(cur.strip()); cur = ''; continue
        depth += ch in '([<'; depth -= ch in ')]>'; cur += ch
    return out + [cur.strip()] if cur.strip() else out


def _name(param):
    """A kernel parameter's name."""
    m = _NAME.search(param)
    if not m: raise ValueError(f'no name in the kernel parameter {param}')
    return m[1]


def _members(comps):
    """{n: [the component's index in comps, or None, for x, y, z, w]} per attribute (None for the position)."""
    return {n: [comps.index((n, c)) if (n, c) in comps else None for c in range(4)] for n in dict.fromkeys(n for n, _ in comps)}


def pixel_reads(sph):
    """{generic attribute n: the mask of its components (bit c) a pixel shader reads} from its SPH's input map: byte 0x18 + n, two bits a component (0 unused, 1 constant,
    2 perspective, 3 linear; Ryujinx ShaderHeader ImapTypes), the same place in a Maxwell and an Ampere pixel SPH."""
    return {n: sum(1 << c for c in range(4) if sph[0x18 + n] >> (2 * c) & 3) for n in range(32) if sph[0x18 + n]}


def _width(written, read):
    """The components an output needs: those the shader writes, and a w its pixel shaders read (read: component mask, None = not known, every component) that it does not write,
    which must hold the hardware's 1. A Metal fragment input wider than the output it links to reads 0 in the components the output lacks, the hardware's default for x, y and z."""
    hi = max((c + 1 for c in range(4) if written[c] is not None), default=0)
    return max(hi, 4 if (0xf if read is None else read) & 8 and written[3] is None else 0, 1)


def _value(width, base):
    """The output's value of width components: base[c] is the word's expression, or None for a component the shader does not write (0, 0, 0, 1 as a vertex output starts)."""
    parts = [f'as_type<float>({base[k]})' if base[k] else ('1.0' if k == 3 else '0.0') for k in range(width)]
    return parts[0] if width == 1 else f'float{width}(' + ', '.join(parts) + ')'


def _type(width):
    return 'float' if width == 1 else f'float{width}'


def to_stage(msl, i, reads=None):
    """The translated kernel as the object function main0 (kind 'ts'), or the mesh functions main0 and main0_z (kind 'ms': the two depth conventions, as dmsh_entry.py vs_depth gives a
    vertex shader, near = -w and near = 0). reads: {user(locnN) location: component mask} of what a mesh shader's paired pixel shaders read (pixel_reads), None when not known.
    A location its SPHs do not enable becomes a vertex output of (0,0,0,1), what the hardware gives a pixel shader input no stage wrote (dmsh_build.py vs_pad does the same for a
    vertex shader), since Metal links no pixel input the mesh stage does not write; each output is declared as wide as those readers can see (_width)."""
    kind = i['kind']
    m = _KERNEL.search(msl)
    if not m: raise ValueError(f'{kind}: no kernel main0 starting with its stage memory')
    params, local, words, mem = _params(m[1]), m[2].strip(), int(m[3]), m[4]
    names = [_name(p) for p in params]
    plain = [re.sub(r'\s*\[\[.*\]\]$', '', p) for p in params]
    # The kernel's body, called with the stage memory the wrapper owns: its early returns (a thread with nothing to write) come back to the wrapper's copy of the outputs. Its thread variables
    # stay in it: one call a thread, as the kernel ran
    src = (msl[:m.start()] + f'static inline __attribute__((always_inline)) void dm_stage_body({", ".join(plain + [f"threadgroup spvUnsafeArray<uint, {words}>& {mem}"])})\n{{\n' +
           (f'    {local}\n' if local else '') + msl[m.end():])
    call = f'dm_stage_body({", ".join(names + [mem])});'
    # The thread's and the threadgroup's numbers: the kernel's parameters where it has them (an attribute may appear once in a function), else parameters of the wrapper's own
    own = {t[3]: t[2] + ('.x' if t[1] == 'uint3' else '') for t in map(_THREAD.match, params) if t}
    lid, tg = own.get('thread_position_in_threadgroup', 'dm_lid'), own.get('threadgroup_position_in_grid', 'dm_tg')
    extra = [f'uint {v} [[{a}]]' for a, v in (('thread_position_in_threadgroup', 'dm_lid'), ('threadgroup_position_in_grid', 'dm_tg')) if a not in own]
    lay, _ = memory(i)
    thr, pay = i['threads'], i['payload'] + 3 >> 2
    wg, inp, out, prim = lay[0] >> 2, lay[1] >> 2, lay[2] >> 2, lay[3]
    start = (f'    threadgroup spvUnsafeArray<uint, {words}> {mem};\n'
             f'    for (uint i = {lid}; i < {words}u; i += {thr}u) {mem}[i] = 0u;\n'
             '    threadgroup_barrier(mem_flags::mem_threadgroup);\n')
    head = f'struct dm_payload_t {{ uint w[{max(pay, 1)}]; }};\n'
    sig = ', '.join(params + extra)
    draw = f'constant uint* dm_draw [[buffer({abi.MESH_DRAW_SLOT})]]'   # the draw's {count, first}, which the host binds (nvn_geometry.inc mtk_encode)
    if kind == 'ts':
        return src + '\n' + head + (
            f'[[object]] void main0({sig}, object_data dm_payload_t& dm_payload [[payload]], mesh_grid_properties dm_grid, {draw})\n{{\n{start}'
            f'    if ({lid} == 0u) {mem}[{wg}] = dm_draw[1] + {tg};\n'
            f'    threadgroup_barrier(mem_flags::mem_threadgroup);\n    {call}\n    threadgroup_barrier(mem_flags::mem_threadgroup);\n'
            f'    for (uint i = {lid}; i < {pay}u; i += {thr}u) dm_payload.w[i] = {mem}[{out}u + i];\n'
            f'    if ({lid} == 0u) dm_grid.set_threadgroups_per_grid(uint3(min({mem}[{out + 1}], {METAL_MAX_GRID}u), 1u, 1u));\n}}\n')
    vertex, primitive = i['vertex'], i['primitive']
    max_v, max_p = i['max_v'], i['max_p']
    verts = i.get('verts', 3)
    if verts not in TOPOLOGY: raise ValueError(f'ms: primitive vertex count {verts} is not 1, 2 or 3')
    if max_v > METAL_MAX_VERTICES or max_p > METAL_MAX_PRIMITIVES: raise ValueError(f'ms: {max_v} vertices and {max_p} primitives exceed a Metal mesh')
    v_blocks = max(max(max_v, 1) + BLOCK - 1, 0) // BLOCK
    v_stride, p_stride = max(len(vertex), 1) * BLOCK, max(len(primitive), 1) * BLOCK   # words of a vertex or primitive attribute block
    p_out = out + v_blocks * len(vertex) * BLOCK   # per-primitive blocks follow all vertex blocks
    word = lambda comps: [f'{mem}[b + {comps[k] * BLOCK}u]' if comps[k] is not None else None for k in range(4)]
    vm, pm = _members(vertex), _members(primitive)
    # Each output as wide as its pixel shaders can see (_width): what Metal counts against a mesh's 32 KB is the declared outputs, and the float4 of every attribute the SPH names
    # put Ocarina of Time's foliage mesh 626fbf48041cb82b (160 vertices, 26 components written) at 35,840 bytes. A location they read that no SPH enables is padded (0,0,0,1)
    read = (lambda n: reads.get(n, 0)) if reads is not None else (lambda n: None)
    miss = sorted(set(reads or ()) - {n for n in vm if isinstance(n, int)} - set(pm))
    vw = {n: _width(c, read(n)) for n, c in vm.items() if isinstance(n, int)}
    pw = {n: _width(c, read(n)) for n, c in pm.items()}
    mw = {n: _width([None] * 4, read(n)) for n in miss}
    # The position is invariant, as a vertex shader's: a game may draw the same geometry with two mesh shaders and test the second for depth equality
    # (Ocarina of Time draws its foliage with a mesh depth pre-pass and a mesh colour pass with depth EQUAL)
    head += ('struct dm_vertex_t {\n    float4 position [[position, invariant]];\n' +
             ('    float point_size [[point_size]];\n' if 'psiz' in vm else '') +
             ''.join(f'    {_type(w)} a{n} [[user(locn{n})]];\n' for n, w in vw.items()) +
             ''.join(f'    {_type(w)} dm_pad{n} [[user(locn{n})]];\n' for n, w in mw.items()) + '};\n' +
             ('struct dm_prim_t {\n' + ''.join(f'    {_type(w)} p{n} [[user(locn{n})]];\n' for n, w in pw.items()) + '};\n' if pm else '') +
             f'using dm_mesh_t = metal::mesh<dm_vertex_t, {"dm_prim_t" if pm else "void"}, {max_v}, {max_p}, metal::topology::{TOPOLOGY[verts]}>;\n')
    v_block = lambda v: f'{out}u + ({v} / {BLOCK}u) * {v_stride}u + {v} % {BLOCK}u'
    p_block = lambda p: f'{p_out}u + ({p} / {BLOCK}u) * {p_stride}u + {p} % {BLOCK}u'
    sets = ''.join(f'        v.point_size = as_type<float>({word(c)[0]});\n' if n == 'psiz' else
                   f'        v.position = {_value(4, word(c))};\n' if n is None else
                   f'        v.a{n} = {_value(vw[n], word(c))};\n' for n, c in vm.items()) + \
        ''.join(f'        v.dm_pad{n} = {_value(w, [None] * 4)};\n' for n, w in mw.items())
    psets = ''.join(f'        p.p{n} = {_value(pw[n], word(c))};\n' for n, c in pm.items())
    if None not in vm: raise ValueError('ms: its SPH enables no position component')

    def entry(name, gl):
        # With a task the payload is the input, its word 0 the workgroup's number among the task's launches, as the hardware leaves it (+0 is not a word the task writes: it starts at +4)
        fill = (f'    for (uint i = {lid}; i < {pay}u; i += {thr}u) {mem}[{inp}u + i] = dm_payload.w[i];\n'
                '    threadgroup_barrier(mem_flags::mem_threadgroup);\n'
                f'    if ({lid} == 0u) {{ {mem}[{wg}] = {tg}; {mem}[{inp}] = {tg}; }}\n') if i['task'] else f'    if ({lid} == 0u) {mem}[{wg}] = dm_block.first + {tg};\n'
        return (f'[[mesh]] void {name}({sig}, dm_mesh_t dm_out' + (', const object_data dm_payload_t& dm_payload [[payload]]' if i['task'] else ', const object_data dm_block_t& dm_block [[payload]]') +
                f')\n{{\n{start}{fill}'
                f'    threadgroup_barrier(mem_flags::mem_threadgroup);\n    {call}\n    threadgroup_barrier(mem_flags::mem_threadgroup);\n'
                f'    uint np = min({mem}[{prim >> 2}] >> 24, {max_p}u);\n'
                f'    if ({lid} == 0u) dm_out.set_primitive_count(np);\n'
                f'    for (uint n = {lid}; n < {max_v}u; n += {thr}u)\n    {{\n        uint b = {v_block("n")};\n        dm_vertex_t v;\n{sets}' +
                ('        v.position.z = fma(v.position.z, 0.5, v.position.w * 0.5);\n' if gl else '') +
                '        dm_out.set_vertex(n, v);\n    }\n' +
                (f'    for (uint n = {lid}; n < np; n += {thr}u)\n    {{\n        uint b = {p_block("n")};\n        dm_prim_t p;\n{psets}        dm_out.set_primitive(n, p);\n    }}\n' if pm else '') +
                f'    for (uint n = {lid}; n < np * {verts}u; n += {thr}u)\n    {{\n        uint at = {prim + 4}u + n;\n        dm_out.set_index(n, ({mem}[at >> 2] >> ((at & 3u) * 8u)) & 0xffu);\n    }}\n}}\n')
    if i['task']: return src + '\n' + head + entry('main0', True) + '\n' + entry('main0_z', False)
    # The object function of a mesh shader with no task: one thread a block of METAL_MAX_GRID workgroups
    head += 'struct dm_block_t { uint first; };\n'
    obj = (f'[[object]] void main0_o(object_data dm_block_t& dm_block [[payload]], mesh_grid_properties dm_grid, uint dm_otg [[threadgroup_position_in_grid]], {draw})\n{{\n'
           f'    uint b = dm_otg * {METAL_MAX_GRID}u;\n    dm_block.first = dm_draw[1] + b;\n'
           f'    dm_grid.set_threadgroups_per_grid(uint3(min(dm_draw[0] - b, {METAL_MAX_GRID}u), 1u, 1u));\n}}\n')
    return src + '\n' + head + entry('main0', True) + '\n' + entry('main0_z', False) + '\n' + obj
