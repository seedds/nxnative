"""NVN2 task and mesh shaders as Metal object and mesh functions.

The Switch 2's Ampere has them; a shader says so in its control section (+0x56: 6 task, 7 mesh), while its SPH is of the vertex or domain kind. The translator reads one as a compute
shader with its SPH taken off and its stage memory in shared memory (job stage_memory, InstEmitAmpereMem.cs AIsberd, AStageOut). This wraps that kernel into the Metal stage function:
the wrapper fills the stage inputs, calls the kernel's body and hands the stage outputs to Metal.

Stage memory, in shared memory (memory()), all of it zero when the body starts:
  word 0        the workgroup's number (ISBERD mode 0x78): a task's is the draw's first task + its threadgroup, a mesh shader's with no task the draw's first + its threadgroup
  INPUT         the payload a mesh shader's task wrote (ISBERD mode 0x38). Its word 0 is the mesh workgroup's number among its task's launches, which the hardware puts there
  outputs       a task's payload (0xb8): its word 1 is the number of mesh workgroups to launch, its records start at +0x10
                or a mesh shader's attributes (0xf8): blocks of 32 vertices and 32 primitives, each component 0x80 bytes (a word per vertex or primitive), the vertex components the
                SPH's OMAP enables first, then the per-primitive SPH's
  primitives    a mesh shader's primitive bytes: the count at +3 (0x88), three index bytes a triangle from +4 (0x80)
"""
import re
import struct
import dmsh_abi as abi

WORKGROUP, INPUT = 0, 0x10
BLOCK = 32   # vertices and primitives of an attribute block
METAL_MAX_VERTICES, METAL_MAX_PRIMITIVES = 256, 512   # a Metal mesh's limits
METAL_MAX_GRID = 1024   # mesh threadgroups an object threadgroup may launch
METAL_MAX_SHARED = 0x8000   # threadgroup memory of a Metal function


def is_stage(ctl):
    """'ts', 'ms' or None, by the control section's stage byte."""
    return {6: 'ts', 7: 'ms'}.get(ctl[0x56]) if len(ctl) > 0x56 else None


def sph_outputs(sph):
    """The components an SPH's OMAP enables, in the order of the attribute area: [(None, c)] for the position's (byte 0x35, bits 4..7), then [(n, c)] for generic attribute n's
    (bytes 0x36 on, a nibble an attribute, bit c its component)."""
    out = [(None, c) for c in range(4) if sph[0x35] >> (4 + c) & 1]
    for n in range(32):
        out += [(n, c) for c in range(4) if sph[0x36 + n // 2] >> (4 * (n % 2) + c) & 1]
    return out


def info(ctl, area):
    """A task or mesh shader's facts from its control section and its whole code area (the SPH, the instructions, a mesh shader's second SPH): +0x58 its workgroup's threads, +0x5a its
    payload's bytes, +0x5e whether a mesh shader has a task, +0x60 / +0x62 a mesh shader's most vertices and primitives, +0x64 the offset of its per-primitive SPH where +0x68 bit 7 is set."""
    kind = is_stage(ctl)
    threads, payload = struct.unpack_from('<2H', ctl, 0x58)
    out = {'kind': kind, 'threads': threads, 'payload': payload}
    if kind == 'ms':
        max_v, max_p = struct.unpack_from('<2H', ctl, 0x60)
        at = struct.unpack_from('<I', ctl, 0x64)[0] if ctl[0x68] & 0x80 else 0
        prim = sph_outputs(area[at:at + 0x80]) if at else []
        out.update(task=bool(ctl[0x5e]), max_v=max_v, max_p=max_p, vertex=sph_outputs(area[:0x80]), primitive=[c for c in prim if c[0] is not None])
    return out


def memory(i):
    """(the stage memory's offsets for the translator: the workgroup's number, the input, the outputs, the primitive bytes; the shared memory's bytes)."""
    out = INPUT + (i['payload'] + 15 & ~15)
    if i['kind'] == 'ts':
        return [WORKGROUP, INPUT, out, out + i['payload']], out + (i['payload'] + 3 & ~3)
    blocks = max(max(i['max_v'], i['max_p'], 1) + BLOCK - 1, 0) // BLOCK
    prim = out + blocks * max(len(i['vertex']) + len(i['primitive']), 1) * BLOCK * 4
    return [WORKGROUP, INPUT, out, prim], prim + (4 + 3 * max(i['max_p'], 1) + 3 & ~3)


def own_shared(code):
    """Whether the instructions use shared memory of their own (LDS, STS, ATOMS: Ampere opcodes as DecoderAmpere.cs marks them). It would lie over the stage memory, and the control
    section does not give its size."""
    return any(struct.unpack_from('<H', code, o)[0] & 0xfff in (0x984, 0x388, 0x988, 0x38c, 0x98c, 0xf8c) for o in range(0, len(code) - 15, 16))


_KERNEL = re.compile(r'kernel\s+void\s+main0\s*\(((?:[^()]|\([^()]*\))*)\)\s*\{\s*threadgroup\s+spvUnsafeArray<uint,\s*(\d+)>\s+(\w+);\n')
_NAME = re.compile(r'(\w+)\s*(?:\[\[.*\]\])?$')
_THREAD = re.compile(r'(uint3?)\s+(\w+)\s*\[\[(thread_position_in_threadgroup|threadgroup_position_in_grid)\]\]$')


def _params(text):
    """Split a parameter list at its top-level commas."""
    out, depth, cur = [], 0, ''
    for ch in text:
        if ch == ',' and not depth: out.append(cur.strip()); cur = ''; continue
        depth += ch in '([<'; depth -= ch in ')]>'; cur += ch
    return out + [cur.strip()] if cur.strip() else out


def _component(c, base):
    """A vector of 4 from the attribute area's components: base[c] is the word's expression, or None for a component the shader does not write (0, 0, 0, 1 as a vertex output starts)."""
    return 'float4(' + ', '.join(f'as_type<float>({base[k]})' if base[k] else ('1.0' if k == 3 else '0.0') for k in range(4)) + ')' if c else ''


def _members(comps):
    """{n: [the component's index in comps, or None, for x, y, z, w]} per attribute (None for the position)."""
    return {n: [comps.index((n, c)) if (n, c) in comps else None for c in range(4)] for n in dict.fromkeys(n for n, _ in comps)}


def to_stage(msl, i):
    """The translated kernel as the object function main0 (kind 'ts'), or the mesh functions main0 and main0_z (kind 'ms': the two depth conventions, as dmsh_build.py vs_depth gives a
    vertex shader, near = -w and near = 0)."""
    kind = i['kind']
    m = _KERNEL.search(msl)
    if not m: raise ValueError(f'{kind}: no kernel main0 starting with its stage memory')
    params, words, mem = _params(m[1]), int(m[2]), m[3]
    names = [_NAME.search(p)[1] for p in params]
    plain = [re.sub(r'\s*\[\[.*\]\]$', '', p) for p in params]
    # The kernel's body, called with the stage memory the wrapper owns: its early returns (a thread with nothing to write) come back to the wrapper's copy of the outputs
    src = msl[:m.start()] + f'static inline __attribute__((always_inline)) void dm_stage_body({", ".join(plain + [f"threadgroup spvUnsafeArray<uint, {words}>& {mem}"])})\n{{\n' + msl[m.end():]
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
    first = f'constant uint* dm_first [[buffer({abi.MESH_FIRST_SLOT})]]'   # the draw's first task or mesh workgroup, which the host binds (nvn_geometry.inc mtk_encode)
    if kind == 'ts':
        return src + '\n' + head + (
            f'[[object]] void main0({sig}, object_data dm_payload_t& dm_payload [[payload]], mesh_grid_properties dm_grid, {first})\n{{\n{start}'
            f'    if ({lid} == 0u) {mem}[{wg}] = dm_first[0] + {tg};\n'
            f'    threadgroup_barrier(mem_flags::mem_threadgroup);\n    {call}\n    threadgroup_barrier(mem_flags::mem_threadgroup);\n'
            f'    for (uint i = {lid}; i < {pay}u; i += {thr}u) dm_payload.w[i] = {mem}[{out}u + i];\n'
            f'    if ({lid} == 0u) dm_grid.set_threadgroups_per_grid(uint3(min({mem}[{out + 1}], {METAL_MAX_GRID}u), 1u, 1u));\n}}\n')
    vertex, primitive = i['vertex'], i['primitive']
    max_v, max_p = i['max_v'], i['max_p']
    if max_v > METAL_MAX_VERTICES or max_p > METAL_MAX_PRIMITIVES: raise ValueError(f'ms: {max_v} vertices and {max_p} primitives exceed a Metal mesh')
    stride = max(len(vertex) + len(primitive), 1) * BLOCK   # words of an attribute block
    word = lambda comps, n: [f'{mem}[b + {comps[k] * BLOCK}u]' if comps[k] is not None else None for k in range(4)]
    vm, pm = _members(vertex), {n: [c + len(vertex) if c is not None else None for c in v] for n, v in _members(primitive).items()}
    head += ('struct dm_vertex_t {\n    float4 position [[position]];\n' + ''.join(f'    float4 a{n} [[user(locn{n})]];\n' for n in vm if n is not None) + '};\n' +
             ('struct dm_prim_t {\n' + ''.join(f'    float4 p{n} [[user(locn{n})]];\n' for n in pm) + '};\n' if pm else '') +
             f'using dm_mesh_t = metal::mesh<dm_vertex_t, {"dm_prim_t" if pm else "void"}, {max_v}, {max_p}, metal::topology::triangle>;\n')
    block = lambda v: f'{out}u + ({v} / {BLOCK}u) * {stride}u + {v} % {BLOCK}u'
    sets = ''.join(f'        v.{"position" if n is None else f"a{n}"} = {_component(True, word(c, n))};\n' for n, c in vm.items())
    psets = ''.join(f'        p.p{n} = {_component(True, word(c, n))};\n' for n, c in pm.items())
    if None not in vm: raise ValueError('ms: its SPH enables no position component')

    def entry(name, gl):
        # With a task the payload is the input, its word 0 the workgroup's number among the task's launches, as the hardware leaves it (+0 is not a word the task writes: it starts at +4)
        fill = (f'    for (uint i = {lid}; i < {pay}u; i += {thr}u) {mem}[{inp}u + i] = dm_payload.w[i];\n'
                '    threadgroup_barrier(mem_flags::mem_threadgroup);\n'
                f'    if ({lid} == 0u) {{ {mem}[{wg}] = {tg}; {mem}[{inp}] = {tg}; }}\n') if i['task'] else f'    if ({lid} == 0u) {mem}[{wg}] = dm_first[0] + {tg};\n'
        return (f'[[mesh]] void {name}({sig}, dm_mesh_t dm_out' + (', const object_data dm_payload_t& dm_payload [[payload]]' if i['task'] else f', {first}') + f')\n{{\n{start}{fill}'
                f'    threadgroup_barrier(mem_flags::mem_threadgroup);\n    {call}\n    threadgroup_barrier(mem_flags::mem_threadgroup);\n'
                f'    uint np = min({mem}[{prim >> 2}] >> 24, {max_p}u);\n'
                f'    if ({lid} == 0u) dm_out.set_primitive_count(np);\n'
                f'    for (uint n = {lid}; n < {max_v}u; n += {thr}u)\n    {{\n        uint b = {block("n")};\n        dm_vertex_t v;\n{sets}' +
                ('        v.position.z = fma(v.position.z, 0.5, v.position.w * 0.5);\n' if gl else '') +
                '        dm_out.set_vertex(n, v);\n    }\n' +
                (f'    for (uint n = {lid}; n < np; n += {thr}u)\n    {{\n        uint b = {block("n")};\n        dm_prim_t p;\n{psets}        dm_out.set_primitive(n, p);\n    }}\n' if pm else '') +
                f'    for (uint n = {lid}; n < np * 3u; n += {thr}u)\n    {{\n        uint at = {prim + 4}u + n;\n        dm_out.set_index(n, ({mem}[at >> 2] >> ((at & 3u) * 8u)) & 0xffu);\n    }}\n}}\n')
    return src + '\n' + head + entry('main0', True) + '\n' + entry('main0_z', False)
