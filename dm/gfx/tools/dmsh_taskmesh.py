"""NVN2 task/mesh programs as native Metal object/mesh entries.

The translator lowers ISBE accesses to three named shared-memory regions. Their
offsets are already the hardware's packed/SKEW byte offsets; only exporting the
finished vertex attributes needs the SPH mask to undo that layout. MAP has one
uint per byte, so independently written adjacent indices never race.

Layout references: NVIDIA clc797sph.h output masks and Mesa NAK's
nak_nir_lower_mesh_intrinsics.c / nak_mesh_skew_offset. NVN2 control offsets are
the SDK's stage 6/7 setup, not the reused vertex/domain stage in the SPH.
"""
import re
import struct

FIRST_SLOT = 14
_REGIONS = ('dm_isbe_input', 'dm_isbe_output', 'dm_isbe_map')
_TOPOLOGY = {1: 'point', 2: 'line', 3: 'triangle'}


def _output_attrs(header, primitive=False):
    """Enabled output components in SPH order, before SKEW packing."""
    sph = int.from_bytes(header, 'little')
    enabled = [b for lo, hi in ((400, 576), (576, 592), (632, 640))
               for b in range(lo, hi) if sph & (1 << b)]
    if any(b < (432 if primitive else 428) or b >= 560 for b in enabled):
        raise ValueError('taskmesh: unsupported system output mask')
    if not primitive and not all(b in enabled for b in range(428, 432)):
        raise ValueError('taskmesh: mesh does not export complete Position')
    return [(b - 400) * 4 for b in enabled]


def metadata(ctl, code):
    """Decode control fields and both packed vertex/primitive output masks.

    Sizes passed to the translator are uint counts, including MAP's one uint
    per byte. Payload is the complete allocation, including its 16-byte header.
    code must be the complete code area, before truncating to the instructions:
    ctl+0x64/+0x68 locate a second, geometry-passthrough SPH for primitive data.
    Unsupported SPH outputs fail explicitly rather than disappear at export.
    """
    if len(ctl) < 0x6c or len(code) < 0x80:
        raise ValueError('taskmesh: truncated NVN2 control or SPH')
    stage = {6: 'ts', 7: 'ms'}.get(ctl[0x56])
    if stage is None: raise ValueError('taskmesh: not an NVN2 task/mesh program')
    threads, payload, shared = struct.unpack_from('<3H', ctl, 0x58)
    if not 1 <= threads <= 1024 or payload < 16 or payload % 4 or shared % 4:
        raise ValueError('taskmesh: invalid thread count or memory allocation')
    if shared:
        raise ValueError('taskmesh: nonzero shared allocation requires ISBE scratch-layout support')
    out = dict(stage=stage, threads=threads, payload=payload, shared=shared,
               vertices=0, primitives=0, topology=0, attrs=[], primitive_attrs=[], primitive_base_words=0)
    if stage == 'ts':
        out['isbe_sizes'] = [payload // 4, payload // 4, 1]
        return out
    topology = ctl[0x5f]
    vertices, primitives = struct.unpack_from('<2H', ctl, 0x60)
    if topology not in _TOPOLOGY or not 1 <= vertices <= 256 or not 1 <= primitives <= 256:
        raise ValueError('taskmesh: unsupported topology or mesh output limits')
    attrs = _output_attrs(code[:0x80])
    primitive_attrs = []
    gs_offset, gs_size = struct.unpack_from('<2I', ctl, 0x64)
    if gs_offset or gs_size:
        if gs_offset < 0x80 or gs_size != 0x80 or gs_offset + gs_size > len(code):
            raise ValueError('taskmesh: missing/truncated primitive GS SPH in full code area')
        gs_header = code[gs_offset:gs_offset + gs_size]
        word0 = struct.unpack_from('<I', gs_header)[0]
        if word0 & 0x3fff != 0x1081 or not word0 & (1 << 24):
            raise ValueError('taskmesh: primitive SPH is not version-4 geometry passthrough')
        primitive_attrs = _output_attrs(gs_header, primitive=True)
    locations = {(a - 0x80) // 16 for a in attrs if a >= 0x80}
    primitive_locations = {(a - 0x80) // 16 for a in primitive_attrs}
    if locations & primitive_locations:
        raise ValueError('taskmesh: a generic vector mixes vertex and primitive components')
    if vertices * (1 + len(locations)) * 16 + primitives * (4 + len(primitive_locations) * 16) > 32768:
        raise ValueError('taskmesh: mesh output exceeds Metal 32 KiB limit')
    vertex_words = ((vertices + 31) // 32) * 32 * len(attrs)
    primitive_words = ((primitives + 31) // 32) * 32 * len(primitive_attrs)
    out.update(vertices=vertices, primitives=primitives, topology=topology, attrs=attrs,
               primitive_attrs=primitive_attrs, primitive_base_words=vertex_words,
               isbe_sizes=[payload // 4, vertex_words + primitive_words, 4 + primitives * topology])
    return out


def skew_word(vertex, component, components):
    """Packed output word for a vertex and its rank among enabled components."""
    return (vertex // 32) * components * 32 + component * 32 + vertex % 32


def map_count(words):
    """ISBEWR.MAP.32 [3] stores value bytes backwards from address 3."""
    return sum((words[i] & 255) << (8 * (3 - i)) for i in range(4))


def _params(text):
    result, start, depth = [], 0, 0
    for i, ch in enumerate(text):
        depth += ch in '<(['
        depth -= ch in '>)]'
        if ch == ',' and not depth:
            result.append(text[start:i].strip())
            start = i + 1
    return result + ([text[start:].strip()] if text[start:].strip() else [])


def _kernel(src):
    match = re.search(r'\bkernel\s+void\s+main0\s*\((.*?)\)\s*\{', src, re.S)
    if not match: raise ValueError('taskmesh: missing compute-lowered main0')
    end, depth = match.end(), 1
    while end < len(src) and depth:
        depth += (src[end] == '{') - (src[end] == '}')
        end += 1
    if depth: raise ValueError('taskmesh: unbalanced main0 body')
    if src[end:].strip(): raise ValueError('taskmesh: unexpected source after main0')
    return src[:match.start()], _params(match[1]), src[match.end():end - 1]


def _lift(src, meta):
    prefix, params, body = _kernel(src)
    # Shared arrays belong to the native entry, not the guest-body helper. An
    # early guest return must still reach the entry's barrier and mesh export.
    arrays = []
    def array(match):
        typ, name = match[1], match[2]
        arrays.append((typ, name))
        return ''
    body = re.sub(r'^\s*threadgroup\s+(spvUnsafeArray<[^;]+?>)\s+(\w+);\s*$', array, body, flags=re.M)
    if re.search(r'\bthreadgroup\s+(?!_barrier)', body):
        raise ValueError('taskmesh: unrecognized guest shared-memory declaration')
    present = {name: typ for typ, name in arrays}
    for name, count in zip(_REGIONS, meta['isbe_sizes']):
        if name in present and present[name] != f'spvUnsafeArray<uint, {count}>':
            raise ValueError('taskmesh: translated ISBE size disagrees with metadata: ' + name)
    if any(name.startswith('_') for _, name in arrays):
        raise ValueError('taskmesh: anonymous shared memory; preserve translator ISBE OpName')
    if 'dm_isbe_output' not in present:
        raise ValueError('taskmesh: translated shader has no ISBE output')
    if meta['stage'] == 'ms' and 'dm_isbe_map' not in present:
        raise ValueError('taskmesh: translated mesh has no primitive map')
    for name, count in zip(_REGIONS, meta['isbe_sizes']):
        if name not in present:
            arrays.append((f'spvUnsafeArray<uint, {count}>', name))
    resources, declarations, args = [], [], []
    builtins = {
        'thread_index_in_simdgroup': 'dm_lane',
        'thread_index_in_threadgroup': 'dm_t',
        'thread_position_in_threadgroup': 'uint3(dm_t, 0u, 0u)',
        'threadgroup_position_in_grid': 'dm_tg',
        'threadgroups_per_grid': 'dm_dims',
        'thread_position_in_grid': f'dm_tg * uint3({meta["threads"]}u, 1u, 1u) + uint3(dm_t, 0u, 0u)',
        'threads_per_grid': f'dm_dims * uint3({meta["threads"]}u, 1u, 1u)',
        'threads_per_threadgroup': f'uint3({meta["threads"]}u, 1u, 1u)',
        'simdgroup_index_in_threadgroup': 'dm_t / 32u',
        'threads_per_simdgroup': '32u',
    }
    for p in params:
        a = re.fullmatch(r'(.+?)\s+(\w+)\s*\[\[(\w+)(?:\((\d+)\))?\]\]', p)
        if not a: raise ValueError('taskmesh: unrecognized kernel parameter: ' + p)
        declarations.append(f'{a[1]} {a[2]}')
        if a[3] in ('buffer', 'texture', 'sampler'):
            if a[3] == 'buffer' and int(a[4]) == FIRST_SLOT:
                raise ValueError('taskmesh: first-workgroup buffer slot already occupied')
            resources.append(p)
            args.append(a[2])
        elif a[3] in builtins:
            args.append(builtins[a[3]])
        else: raise ValueError('taskmesh: unsupported kernel builtin: ' + a[3])
    declarations += [f'threadgroup {typ}& {name}' for typ, name in arrays]
    args += [name for _, name in arrays]
    helper = '\nstatic inline void dm_guest(' + ', '.join(declarations) + ')\n{' + body + '\n}\n'
    return prefix + helper, arrays, resources, 'dm_guest(' + ', '.join(args) + ');'


def _storage(arrays, meta, payload):
    lines = [f'    threadgroup {typ} {name};' for typ, name in arrays]
    for name, count in zip(_REGIONS, meta['isbe_sizes']):
        value = 'dm_payload.data[dm_i]' if payload and name == 'dm_isbe_input' else '0u'
        lines.append(f'    for (uint dm_i = dm_t; dm_i < {count}u; dm_i += {meta["threads"]}u) {name}[dm_i] = {value};')
    # The input index is per task for object/direct mesh, per child for mesh.
    lines += ['    threadgroup_barrier(mem_flags::mem_threadgroup);',
              '    if (dm_t == 0u) dm_isbe_input[0] = dm_group;',
              '    threadgroup_barrier(mem_flags::mem_threadgroup);']
    return '\n'.join(lines)


def _vertex_export(meta):
    attrs, lines = meta['attrs'], ['        dm_vertex dm_v = {};']
    for rank, attr in enumerate(attrs):
        name = 'position' if attr < 0x80 else f'attr{(attr - 0x80) // 16}'
        component = ((attr - 0x70) if attr < 0x80 else (attr - 0x80)) // 4 % 4
        offset = f'(dm_vtx / 32u) * {len(attrs) * 32}u + {rank * 32}u + (dm_vtx % 32u)'
        lines.append(f'        dm_v.{name}[{component}] = as_type<float>(dm_isbe_output[{offset}]);')
    return '\n'.join(lines)


def _primitive_export(meta):
    attrs = meta['primitive_attrs']
    lines = ['        dm_primitive dm_prim = {};']
    for rank, attr in enumerate(attrs):
        location, component = (attr - 0x80) // 16, (attr - 0x80) // 4 % 4
        offset = f'{meta["primitive_base_words"]}u + (dm_p / 32u) * {len(attrs) * 32}u + {rank * 32}u + (dm_p % 32u)'
        lines.append(f'        dm_prim.attr{location}[{component}] = as_type<float>(dm_isbe_output[{offset}]);')
    lines.append('        dm_mesh.set_primitive(dm_p, dm_prim);')
    return '\n'.join(lines)


def wrap(src, meta):
    """Replace the temporary compute entry with native object/mesh entries.

    main0 follows the existing GL-depth convention; main0_z preserves zero-to-one
    clip depth. Direct mesh entries omit the object payload and add API first.
    """
    prefix, arrays, resources, call = _lift(src, meta)
    payload_decl = f'\nstruct dm_task_payload {{ uint data[{meta["payload"] // 4}]; }};\n'
    common = resources + ['uint dm_t [[thread_index_in_threadgroup]]',
                          'uint dm_lane [[thread_index_in_simdgroup]]',
                          'uint3 dm_tg [[threadgroup_position_in_grid]]',
                          'uint3 dm_dims [[threadgroups_per_grid]]']
    first = f'constant uint& dm_first [[buffer({FIRST_SLOT})]]'
    if meta['stage'] == 'ts':
        params = common + [first, 'object_data dm_task_payload& dm_payload [[payload]]',
                           'metal::mesh_grid_properties dm_grid']
        return prefix + payload_decl + f'''\n[[object]] void main0({', '.join(params)})
{{
    uint dm_group = dm_first + dm_tg.x + dm_dims.x * (dm_tg.y + dm_dims.y * dm_tg.z);
{_storage(arrays, meta, False)}
    {call}
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint dm_i = dm_t; dm_i < {meta['payload'] // 4}u; dm_i += {meta['threads']}u) dm_payload.data[dm_i] = dm_isbe_output[dm_i];
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (dm_t == 0u) dm_grid.set_threadgroups_per_grid(uint3(dm_isbe_output[1], 1u, 1u));
}}
'''
    locations = sorted({(a - 0x80) // 16 for a in meta['attrs'] if a >= 0x80})
    vertex = ['struct dm_vertex {', '    float4 position [[position]];']
    vertex += [f'    float4 attr{loc} [[user(locn{loc})]];' for loc in locations]
    vertex += ['};', 'struct dm_primitive {', '    bool culled [[primitive_culled]];']
    primitive_locations = sorted({(a - 0x80) // 16 for a in meta['primitive_attrs']})
    vertex += [f'    float4 attr{loc} [[user(locn{loc})]];' for loc in primitive_locations]
    vertex += ['};']
    mesh_type = f'metal::mesh<dm_vertex, dm_primitive, {meta["vertices"]}, {meta["primitives"]}, metal::topology::{_TOPOLOGY[meta["topology"]]}>'
    result = [prefix, payload_decl, '\n'.join(vertex)]
    for direct in (False, True):
        for zero in (False, True):
            name = 'main0' + ('_direct' if direct else '') + ('_z' if zero else '')
            params = [mesh_type + ' dm_mesh'] + common
            params += [first] if direct else ['const object_data dm_task_payload& dm_payload [[payload]]']
            depth = '' if zero else '        dm_v.position.z = (dm_v.position.z + dm_v.position.w) * 0.5f;\n'
            result.append(f'''\n[[mesh]] void {name}({', '.join(params)})
{{
    uint dm_group = {'dm_first + ' if direct else ''}dm_tg.x + dm_dims.x * (dm_tg.y + dm_dims.y * dm_tg.z);
{_storage(arrays, meta, not direct)}
    {call}
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint dm_count = (dm_isbe_map[0] << 24u) | (dm_isbe_map[1] << 16u) | (dm_isbe_map[2] << 8u) | dm_isbe_map[3];
    for (uint dm_vtx = dm_t; dm_vtx < {meta['vertices']}u; dm_vtx += {meta['threads']}u) {{
{_vertex_export(meta)}
{depth}        dm_mesh.set_vertex(dm_vtx, dm_v);
    }}
    for (uint dm_p = dm_t; dm_p < dm_count; dm_p += {meta['threads']}u) {{
        for (uint dm_j = 0u; dm_j < {meta['topology']}u; ++dm_j) {{
            uint dm_idx = dm_isbe_map[4u + dm_p * {meta['topology']}u + dm_j];
            dm_mesh.set_index(dm_p * {meta['topology']}u + dm_j, uchar(dm_idx));
        }}
{_primitive_export(meta)}
    }}
    if (dm_t == 0u) dm_mesh.set_primitive_count(dm_count);
}}
''')
    return '\n'.join(result)
