# Guard-band instrumentation for a translated mesh or task shader (<key>_b.metal): the threadgroup stage array of main0 grows by GUARD words, filled with a marker after the
# shader's own zeroing; after dm_stage_body, thread 0 counts the guard words that changed and the first one, into dm_flag (buffer 1). Shader validation does not check static
# threadgroup arrays (a deliberate one-past-the-end write was not reported), so this is how a stage-memory overflow is seen.
# usage: mesh_guard.py <in.metal> <out.metal>
import re, sys
GUARD = 64
src = open(sys.argv[1]).read()
m = re.search(r'\[\[(mesh|object)\]\] void main0\((.*?)\)\n\{\n    threadgroup spvUnsafeArray<uint, (\d+)> (_\d+);\n', src, re.S)
if not m: sys.exit('no main0 with a threadgroup stage array')
kind, params, n, arr = m.group(1), m.group(2), int(m.group(3)), m.group(4)
src = src.replace(f'spvUnsafeArray<uint, {n}>', f'spvUnsafeArray<uint, {n + GUARD}>')
start = src.index(f'void main0(')
body0 = src.index('\n{\n', start)
end = src.index('\n}\n', body0)
body = src[body0:end]
zero = f'for (uint i = dm_lid; i < {n}u; i += 32u) {arr}[i] = 0u;'
if zero not in body: sys.exit('no zeroing loop')
body = body.replace(zero, zero + f'\n    for (uint i = dm_lid; i < {GUARD}u; i += 32u) {arr}[{n}u + i] = 0xDEADBEEFu;', 1)
call = re.search(r'\n    dm_stage_body\([^;]*\);\n', body)
if not call: sys.exit('no dm_stage_body call')
check = (f'    threadgroup_barrier(mem_flags::mem_threadgroup);\n'
         f'    if (dm_lid == 0u) {{ uint c = 0u, f = 0xffffffffu; for (uint g = 0u; g < {GUARD}u; g++) if ({arr}[{n}u + g] != 0xDEADBEEFu) {{ c++; f = min(f, g); }}\n'
         f'        atomic_store_explicit(&dm_flag[0], c, memory_order_relaxed); atomic_store_explicit(&dm_flag[1], f, memory_order_relaxed); atomic_store_explicit(&dm_flag[2], {n}u, memory_order_relaxed); }}\n')
body = body[:call.end()] + check + body[call.end():]
src = src[:body0] + body + src[end:]
src = src.replace('void main0(' + params + ')', 'void main0(' + params + ', device atomic_uint* dm_flag [[buffer(1)]])', 1)
open(sys.argv[2], 'w').write(src)
print(kind, n, arr)
