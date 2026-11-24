# Logic operations (the <entry>_l entry points of <key>_bx, dmsh_side.py): NVN's ColorStateSetLogicOp combines the bits a pixel shader writes with the bits already in the target (CLEAR, AND, ..., OR, ..., SET), in place of
# blending; as in OpenGL and Vulkan it applies to integer and normalized targets. Metal has none. Xenoblade Chronicles 2 builds its per-pixel light probe lists with OR: each probe's box draws its index
# into the next free byte of an RG32Uint target and every other fragment writes 0, so without the operation every fragment replaced the list and the lighting showed hard-edged dark boxes.
# Skyward Sword HD's Dof_FogToAlpha ORs (0, 0, 0, fog) into the frame's RGBA8Unorm target to put fog in its alpha: as a plain write it blacked out the colours, a brown cast over the distance.
# main0 of a shader without the typed-output entry point main0_u (dmsh_entry.py fs_uint, which holds the same operation under its function constant DMSH_TYPED_LOGIC_FC: parts) gets a
# thin entry point that runs the shader's body, reads each colour output's attachment (framebuffer fetch, [[color(n)]]) and
# combines it with the output by the operation and per-target channel masks at fragment buffer DMSH_LOGIC_BUFFER. A float output goes through its target's stored bits:
# rounded to them as the target would store it, combined, and normalized again; a mask of zero (a floating-point or sRGB target, which the operation does not apply to) passes it through.
# An output whose target the pass lacks has no attachment to read, and Metal refuses a pipeline that reads one: each read depends on the function constant DMSH_LOGIC_FC + n. The operation
# is data, not part of the code: one pipeline serves every operation. The host takes these entry points only for pipelines drawn with an operation other than COPY (nvn_mtl.m lo_fn), so other
# draws keep the base library and read no attachment. Dual-source blending (main0_u's DMSH_TYPED_DUAL_FC) takes none: a logic operation takes the place of blending.
# Usage: refused(msl) → why the shader gets none, or None; entry(msl, e) → (the entry point's text, the colour outputs it reads), or None, e from dmsh_side.entries; constants(targets) → the function constants' declarations
import re
import dmsh_abi

HELPER = '''
struct dm_logic_state { uint op; uint pad0, pad1, pad2; uint4 mask[8]; };   // NVN_LOGIC_OP_* and each colour target's channel masks (dmsh_logic.py)
static inline uint4 dm_logic(uint4 s, uint4 d, constant dm_logic_state& l, uint n)
{
    uint4 r;
    switch (l.op)
    {
        case 0u: r = uint4(0u); break;
        case 1u: r = s & d; break;
        case 2u: r = s & ~d; break;
        case 4u: r = ~s & d; break;
        case 5u: r = d; break;
        case 6u: r = s ^ d; break;
        case 7u: r = s | d; break;
        case 8u: r = ~(s | d); break;
        case 9u: r = ~(s ^ d); break;
        case 10u: r = ~d; break;
        case 11u: r = s | ~d; break;
        case 12u: r = ~s; break;
        case 13u: r = ~s | d; break;
        case 14u: r = ~(s & d); break;
        case 15u: r = uint4(~0u); break;
        default: r = s; break;
    }
    return r & l.mask[n];
}
static inline float4 dm_logic_norm(float4 s, float4 d, constant dm_logic_state& l, uint n)
{
    if (all(l.mask[n] == uint4(0u))) return s;   // not a normalized target: the operation does not apply
    float4 scale = float4(l.mask[n]);
    return float4(dm_logic(uint4(floor(saturate(s) * scale + 0.5f)), uint4(floor(saturate(d) * scale + 0.5f)), l, n)) / scale;
}
'''


def refused(msl):
    if f'[[buffer({dmsh_abi.LOGIC_BUFFER})]]' in msl: return f'fragment buffer {dmsh_abi.LOGIC_BUFFER} is taken'
    if re.search(r'\[\[color\(\d+\)\]\]\s*[,)]', msl): return 'an entry point already reads an attachment'
    return None


def constants(targets):
    return ''.join(f'constant bool dm_lfb{n} [[function_constant({dmsh_abi.LOGIC_FC + n})]];\n' for n in sorted(targets))


def parts(members, var, gate=None):
    """The logic operation on the colour outputs of an output struct's members (its text between the braces), held in var: (the constants each read depends on, the fetch parameters,
    the statements, the colour outputs read), or None when it has no colour output. gate: a bool constant every read depends on too (main0_u's DMSH_TYPED_LOGIC_FC, dmsh_entry.fs_uint)"""
    outs = [(ty, nm, int(n), fc) for ty, nm, n, fc in re.findall(r'^\s*((?:uint|float)[234]?)\s+(\w+)\s*\[\[color\((\d+)\)(?:, function_constant\((\w+)\))?\]\];', members, re.M)]
    if not outs: return None
    # main0_u holds a float, an unsigned and a signed member on each colour, each present by its function constant (dmsh_entry.py fs_uint): the float and unsigned ones read their
    # attachment as their own type, only when both the attachment and the member are there; a signed one has no bits to combine (as lop_key says) and passes through
    scalar = lambda ty: 'uint' if ty.startswith('uint') else 'float'
    on = lambda n, fc: f'dm_lfb{n}_{fc}' if fc else f'dm_lfb{n}_on' if gate else f'dm_lfb{n}'
    consts = ''.join(f'constant bool {on(n, fc)} = dm_lfb{n}{f" && {fc}" if fc else ""}{f" && {gate}" if gate else ""};\n' for _, _, n, fc in outs if fc or gate)
    fetch = [f'{scalar(ty)}4 {on(n, fc).replace("dm_lfb", "dm_fb")} [[color({n}), function_constant({on(n, fc)})]]' for ty, _, n, fc in outs]
    apply = []
    for ty, nm, n, fc in outs:
        s = scalar(ty); width = int(ty[len(s):] or 1); zero = '0u' if s == 'uint' else '0.0f'
        pad = f', {zero}' * (4 - width); sw = '.' + 'xyzw'[:width] if width < 4 else ''
        apply.append(f'if ({on(n, fc)}) {var}.{nm} = {ty}({"dm_logic" if s == "uint" else "dm_logic_norm"}({s}4({var}.{nm}{pad}), {on(n, fc).replace("dm_lfb", "dm_fb")}, dm_lop, {n}u){sw});')
    return consts, fetch, apply, {n for _, _, n, _ in outs}


def entry(msl, e):   # main0's, in a shader without main0_u (whose own logic operation is in it, under DMSH_TYPED_LOGIC_FC)
    if e['name'] != 'main0': return None
    st = re.search(r'struct %s\s*\{(.*?)\};' % e['ret'], msl, re.S)
    p = parts(st.group(1), 'dm_r') if st else None
    if not p: return None
    consts, fetch, apply, targets = p
    params = ', '.join(e['params'] + fetch + [f'constant dm_logic_state& dm_lop [[buffer({dmsh_abi.LOGIC_BUFFER})]]'])
    return (f'{consts}{e["attr"]}fragment {e["ret"]} {e["name"]}_l({params})\n{{\n    {e["ret"]} dm_r = dm_body_{e["name"]}({e["args"]});\n    {" ".join(apply)}\n    return dm_r;\n}}\n',
            targets)
