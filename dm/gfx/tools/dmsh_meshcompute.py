"""NVN2 task and mesh shaders as compute, for GPUs without mesh-shading hardware (Apple M1 and M2; Apple9, M3 on, has it).

On those GPUs a render pass with any mesh draw costs 0.14-0.24 ms more, and the mesh stage runs about 20 % slower per meshlet than the same code as a kernel. This tool appends
to a translated <key>_b.metal (dmsh_meshstage.py output)

  main0_k / main0_k_z   the mesh wrapper as a kernel: one threadgroup a meshlet slot, vertices to
                        dm_kv[slot * MAXV + n], index triples to dm_ki[slot * MAXP * 3 + n]
                        (absolute vertex numbers; degenerate past the primitive count)
  main0_kv              pass-through vertex function: dm_kv[vertex_id], every user output a float4
                        (Metal links a vertex output to a fragment input only when their types are
                        equal), the locations no output writes 0
  main0_tk              (task shaders) the object wrapper as a kernel: payload to dm_pl[tg],
                        launch count to dm_cnt[tg]
  dm_kinfo_<maxv>_<maxp>_<record bytes>   an empty kernel the host reads the limits from

Buffers (compute table): 24 dm_moff (task path: exclusive offsets of the launch counts, n + 1),
25 dm_cnt, 26 dm_pl, 27 dm_kv, 28 dm_ki, 29 dm_draw {count, first, ntask}. Vertex function: 27 dm_kv.
Not taken: per-primitive outputs, non-triangle topology (those draws stay native).

The host (nvn_mtl.m, mk_*; DM_NVN_MESH_COMPUTE=1) loads <key>_bk.metallib from the DM_MSL_DIR overlay. Build them for a game with

  python3 dm/gfx/tools/dmsh_meshcompute.py build <app>/Contents/Resources/dm/shader_map.tsv <shader cache image or folder> <out folder>

which writes <out>/<key>_bk.metallib for every mesh and task shader of the map; point DM_MSL_DIR at a folder holding
<out> as b-<chain>/ (and the b-<chain>.img of the app beside it).
"""
import re
import sys

KV, KI, DRAW, PL, CNT, MOFF = 27, 28, 29, 26, 25, 24


def _func(s, head):
    i = s.find(head)
    if i < 0: return None, None
    j = s.find('\n{', i); depth = 0; k = j + 1
    while True:
        c = s[k]
        if c == '{': depth += 1
        elif c == '}':
            depth -= 1
            if not depth: return i, k + 1
        k += 1


def mesh_kernels(s):
    m = re.search(r'metal::mesh<dm_vertex_t, (\w+), (\d+), (\d+), metal::topology::(\w+)>', s)
    if not m: return None, 'no mesh type'
    if m.group(1) != 'void': return None, 'per-primitive outputs'
    if m.group(4) != 'triangle': return None, 'topology ' + m.group(4)
    maxv, maxp = int(m.group(2)), int(m.group(3))
    out = [f'\n// ---- mesh as compute (genk.py): MAXV {maxv} MAXP {maxp}\n']
    task = None
    for name, kname in (('main0', 'main0_k'), ('main0_z', 'main0_k_z')):
        a, b = _func(s, f'[[mesh]] void {name}(')
        if a is None: continue
        f = s[a:b]
        sig_end = f.find(')\n{')
        sig, body = f[:sig_end], f[sig_end:]
        task = 'dm_payload_t& dm_payload' in sig
        sig = sig.replace(f'[[mesh]] void {name}(', f'kernel void {kname}(')
        sig = re.sub(r',\s*dm_mesh_t dm_out', '', sig)
        sig = re.sub(r',\s*const object_data dm_(?:payload|block)_t& dm_(?:payload|block) \[\[payload\]\]', '', sig)
        if task:
            sig = sig.replace('uint dm_tg [[threadgroup_position_in_grid]]', 'uint dm_gtg [[threadgroup_position_in_grid]]')
        sig += (f', device dm_kvrec_t* dm_kv [[buffer({KV})]], device uint* dm_ki [[buffer({KI})]], constant uint* dm_draw [[buffer({DRAW})]]')
        if task: sig += f', const device dm_payload_t* dm_pl [[buffer({PL})]], const device uint* dm_moff [[buffer({MOFF})]]'
        if task:
            pre = ('\n    uint dm_lo = 0u, dm_hi = dm_draw[2];\n'
                   '    while (dm_hi - dm_lo > 1u) { uint md = (dm_lo + dm_hi) >> 1; if (dm_moff[md] <= dm_gtg) dm_lo = md; else dm_hi = md; }\n'
                   '    uint dm_tg = dm_gtg - dm_moff[dm_lo]; uint dm_slot = dm_gtg;\n'
                   '    const device dm_payload_t& dm_payload = dm_pl[dm_lo];\n')
        else:
            pre = '\n    uint dm_slot = dm_tg;\n'
            body = body.replace('dm_block.first + dm_tg', 'dm_draw[1] + dm_tg')
        body = body.replace(')\n{', ')\n{' + pre, 1)
        body = re.sub(r'\n\s*if \(dm_lid == 0u\) dm_out\.set_primitive_count\(np\);', '', body)
        body = body.replace('dm_out.set_vertex(n, v);', 'dm_kv[dm_slot * %du + n] = dm_kv_pack(v);' % maxv)
        # index loop: run over every index slot, degenerate past the primitive count
        lm = re.search(r'for \(uint n = dm_lid; n < np \* 3u; n \+= (\d+)u\)', body)
        if not lm: return None, 'index loop'
        body = body.replace(lm.group(0), 'for (uint n = dm_lid; n < %du; n += %su)' % (maxp * 3, lm.group(1)))
        im = re.search(r'dm_out\.set_index\(n, (.*)\);', body)
        if not im: return None, 'set_index'
        body = body.replace(im.group(0), 'dm_ki[dm_slot * %du + n] = dm_slot * %du + (n < np * 3u ? uint(%s) : 0u);' % (maxp * 3, maxv, im.group(1)))
        if 'dm_out' in body or 'dm_block' in body: return None, 'leftover mesh output use'
        out.append(sig + body + '\n')
    if len(out) < 2: return None, 'no mesh entry'
    # record type and packer from dm_vertex_t
    vm = re.search(r'struct dm_vertex_t \{(.*?)\};', s, re.S)
    fields = re.findall(r'^\s*(\w+) (\w+) \[\[[^\]]*\]\];', vm.group(1), re.M)
    rec = ['struct dm_kvrec_t {']
    for t, n in fields:
        pt = t if t in ('float', 'int', 'uint', 'half', 'short', 'ushort') else 'packed_' + t
        rec.append(f'    {pt} {n};')
    rec.append('};')
    pack = ['static inline dm_kvrec_t dm_kv_pack(thread const dm_vertex_t& v) { dm_kvrec_t r;'] + [f'    r.{n} = v.{n};' for _, n in fields] + ['    return r; }']
    # Metal links a vertex output to a fragment input only when their types are equal, where a mesh output may be narrower (Metal accepted float3 into float4 for mesh
    # pipelines). The game's fragment shaders take every input as float4 (SPIR-V Cross), so each user output is a float4 here, its missing components 0
    attrs = dict(re.findall(r'^\s*\w+ (\w+) (\[\[[^\]]*\]\]);', vm.group(1), re.M))
    out_t = ['struct dm_kvout_t {'] + [f'    {"float4" if n != "position" else t} {n} {attrs[n]};' for t, n in fields] + ['};']
    have = {int(x) for x in re.findall(r'user\(locn(\d+)\)', vm.group(1))}
    pad = [i for i in range(16) if i not in have]   # a fragment input no output feeds fails the link: those are 0
    out_t = out_t[:-1] + [f'    float4 dm_z{i} [[user(locn{i})]];' for i in pad] + ['};']
    def widen(t, n):
        d = int(re.fullmatch(r'\w+?([234]?)', t).group(1) or 1)
        return f'float4(float{d if d > 1 else ""}(r.{n})' + ', 0.0' * (4 - d) + ')' if d < 4 else f'float4(r.{n})'
    vs = out_t + [f'vertex dm_kvout_t main0_kv(uint dm_vid [[vertex_id]], const device dm_kvrec_t* dm_kv [[buffer({KV})]])', '{',
          '    dm_kvrec_t r = dm_kv[dm_vid]; dm_kvout_t v;'] + [f'    v.{n} = {widen(t, n)};' for t, n in fields] + [f'    v.dm_z{i} = float4(0.0);' for i in pad] + ['    return v;', '}']
    size = {'float': 4, 'int': 4, 'uint': 4}
    rb = 0
    for t, _ in fields:
        m2 = re.fullmatch(r'(float|int|uint)([234]?)', t)
        if not m2: return None, 'vertex field type ' + t
        rb += 4 * int(m2.group(2) or 1)
    # the host reads the limits from this entry's name: dm_kinfo_<max vertices>_<max primitives>_<record bytes>
    info = f'kernel void dm_kinfo_{maxv}_{maxp}_{rb}() {{}}\n'
    return '\n'.join(rec + pack) + '\n' + ''.join(out) + '\n'.join(vs) + '\n' + info, f'{"task" if task else "notask"} {maxv} {maxp} rec {rb} bytes'


def task_kernel(s):
    a, b = _func(s, '[[object]] void main0(')
    if a is None: return None, 'no object entry'
    f = s[a:b]
    f = f.replace('[[object]] void main0(', 'kernel void main0_tk(', 1)
    f = re.sub(r',\s*object_data dm_payload_t& dm_payload \[\[payload\]\], mesh_grid_properties dm_grid', '', f)
    f = f.replace('constant uint* dm_draw [[buffer(29)]])', f'constant uint* dm_draw [[buffer({DRAW})]], device dm_payload_t* dm_pl [[buffer({PL})]], device uint* dm_cnt [[buffer({CNT})]])')
    f = f.replace('dm_payload.w[', 'dm_pl[dm_tg].w[')
    gm = re.search(r'dm_grid\.set_threadgroups_per_grid\(uint3\((.*), 1u, 1u\)\);', f)
    if not gm: return None, 'grid'
    f = f.replace(gm.group(0), f'dm_cnt[dm_tg] = {gm.group(1)};')
    if 'dm_grid' in f or 'dm_payload.' in f: return None, 'leftover'
    return '\n// ---- task as compute (genk.py)\n' + f + '\n', 'task'



def _compile(job):
    import subprocess, os
    k, text, out = job
    air = os.path.join(out, k + '.air')
    p = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance', '-fwrapv', '-fbracket-depth=1024', '-x', 'metal', '-', '-o', air],
                       input=text.encode(), capture_output=True)
    if p.returncode == 0:
        p = subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', air, '-o', os.path.join(out, k + '.metallib')], capture_output=True)
    if os.path.exists(air): os.remove(air)
    return k, p.returncode, p.stderr.decode()[-400:]


def build(shader_map, lib, out):
    import csv, os, pathlib, collections
    from concurrent.futures import ProcessPoolExecutor
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'tools'))
    import shader_lib
    v = pathlib.Path(lib) if os.path.isdir(lib) else shader_lib.view(lib)
    rows = [r for r in csv.reader(open(shader_map), delimiter='\t') if len(r) > 3 and r[2] in ('mesh', 'task')]   # the map names task shaders' stage task
    keys = sorted({r[3] for r in rows})
    os.makedirs(out, exist_ok=True)
    jobs, notes = [], collections.Counter()
    for k in keys:
        try: s = (v / f'{k}_b.metal').read_text()
        except Exception: notes['no source'] += 1; continue
        is_task = '[[object]] void main0(' in s and '[[mesh]] void main0(' not in s
        t, note = (task_kernel if is_task else mesh_kernels)(s)
        notes[note.split(' rec ')[0] if t else 'skip: ' + note] += 1
        if t: jobs.append((k + '_bk', s + t, out))
    failed = 0
    with ProcessPoolExecutor() as ex:
        for k, rc, err in ex.map(_compile, jobs):
            if rc: failed += 1; print('FAILED', k, err, file=sys.stderr)
    for n, c in sorted(notes.items()): print(f'{c:5d}  {n}')
    print(f'compiled {len(jobs) - failed}, failed {failed}')
    return 1 if failed else 0


if __name__ == '__main__':
    import sys
    if len(sys.argv) == 5 and sys.argv[1] == 'build': sys.exit(build(*sys.argv[2:]))
    if len(sys.argv) != 4 or sys.argv[2] not in ('mesh', 'task'):
        sys.exit('usage: dmsh_meshcompute.py build <shader_map.tsv> <shader cache image or folder> <out>\n       dmsh_meshcompute.py <key>_b.metal mesh|task <out.metal>')
    s = open(sys.argv[1]).read()
    t, note = (mesh_kernels if sys.argv[2] == 'mesh' else task_kernel)(s)
    print(note, file=sys.stderr)
    if t: open(sys.argv[3], 'w').write(s + t)
