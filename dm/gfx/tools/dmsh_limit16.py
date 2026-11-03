# Float16 output limit (<key>_bs): a value beyond the 16-bit float range written to a float16 colour target is stored as the format's largest finite value on the M1 Pro, and as
# infinity on the A19 Pro. A shader that later reads the target then multiplies infinity by zero and gets NaN (Super Mario Odyssey's sky writes a far-distance sentinel to its R16F
# G-buffer target, and the deferred shader's min(NaN, 2048.0) is a white line on the horizon). This variant is the pixel shader with every colour output written straight from a
# constant buffer limited to +-65504, so both GPUs store the same value. The format is not in the shader and a 32-bit float target must keep the value, so it is built offline into
# a library of its own beside <key>_b, with the same argument buffer and entry point names, and the host loads it only for pipelines with such an output on a float16 target
# (nvn_mtl.m lim_key). Outputs the shader computes are not limited: every shader's float16 pipelines would then leave the base library, and no computed overflow has been found.
# Usage: variant(msl) → (new MSL, the entry point names) or (None, reason); colors(msl) → mask of the colour outputs limited
import re

MAX = '65504.0f'
HELPER = f'static inline float dm_limit16(float v) {{ return precise::clamp(v, -{MAX}, {MAX}); }}   // dmsh_limit16.py\n'
ENTRY = re.compile(r'^[^\n]*?\bfragment\s+\w+\s+(\w+)\s*\(', re.M)
UNIFORM = r'\(\*spvDescriptorSet0\.\w+\)\._m0\[\d+\]\.[xyzw]'
ALIAS = re.compile(rf'^\s*float (_\d+) = ({UNIFORM});$', re.M)   # the SSA form of a read that SPIR-V Cross did not inline
WRITE = re.compile(rf'^(\s*out\.(\w+)\.[xyzw] = )({UNIFORM}|_\d+);$', re.M)

def outputs(msl):   # {member: colour index} of the float colour outputs
    st = re.search(r'struct main0_out\s*\{(.*?)\};', msl, re.S)
    return {m.group(2): int(m.group(3)) for m in re.finditer(r'^\s*(float4|float3|float2|float)\s+(\w+)\s*\[\[color\((\d+)\)\]\];', st.group(1), re.M)} if st else {}

def written(msl):   # the writes to limit, as matches of WRITE
    members, alias = outputs(msl), {m.group(1) for m in ALIAS.finditer(msl)}
    return [m for m in WRITE.finditer(msl) if m.group(2) in members and (m.group(3)[0] != '_' or m.group(3) in alias)]

def colors(msl):
    members = outputs(msl)
    mask = 0
    for m in written(msl): mask |= 1 << members[m.group(2)]
    return mask

def variant(msl):
    writes = written(msl)
    if not writes: return None, 'no colour output written from a constant buffer'
    entries = [m.group(1) for m in ENTRY.finditer(msl)]
    if not entries or entries[0] != 'main0': return None, 'no fragment entry point main0'
    src, at = [], 0
    for m in writes: src += [msl[at:m.start()], f'{m.group(1)}dm_limit16({m.group(3)});']; at = m.end()
    src = ''.join(src) + msl[at:]
    first = ENTRY.search(src)
    if not first: return None, 'no fragment entry point after the limit'
    return src[:first.start()] + HELPER + src[first.start():], entries
