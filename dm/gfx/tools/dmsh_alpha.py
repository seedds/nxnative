# Alpha test (the <entry>_a entry points of <key>_bx, dmsh_side.py): NVN's ColorStateSetAlphaTest and SetAlphaRef compare the alpha a pixel shader writes to colour 0 with a reference, before blending, and drop the fragment
# when the comparison fails (Maxwell's fixed-function alpha test, as in OpenGL's compatibility profile). Metal has none. Foliage, hair, fences and dissolve effects draw with it
# (Dark Souls Remastered, ASTRAL CHAIN, Animal Crossing's island map). Each fragment entry point (main0 and the main0_u / main0_d dmsh_build.py adds) gets a thin entry point that
# runs the shader's body, then takes the test's function and reference from a constant buffer at fragment buffer DMSH_ALPHA_BUFFER and tests colour 0's alpha.
# The function and reference are data, not part of the code: one pipeline serves every function, and a reference that changes every draw (a dissolve) keeps it. The host takes these
# entry points only for pipelines drawn with an alpha function other than ALWAYS (nvn_mtl.m at_fn), so draws without the test keep the base library as it was.
# [[early_fragment_tests]] stays: a shader that forces early depth tests has its depth written before the alpha test, on Maxwell too.
# Usage: refused(msl) → why the shader gets none, or None; entry(msl, e) → the entry point's text, e from dmsh_side.entries
import re
import dmsh_abi

HELPER = '''
struct dm_alpha_state { uint func; float ref; };   // NVN_ALPHA_FUNC_* and the reference (dmsh_alpha.py)
static inline bool dm_alpha_pass(float a, constant dm_alpha_state& s)
{
    switch (s.func)
    {
        case 1u: return false;
        case 2u: return a < s.ref;
        case 3u: return a == s.ref;
        case 4u: return a <= s.ref;
        case 5u: return a > s.ref;
        case 6u: return a != s.ref;
        case 7u: return a >= s.ref;
        default: return true;
    }
}
'''


def _colour0(msl, struct):   # (type, name, its function constant or None) of each of the output struct's colour 0 members with four components, a float or integer bits
    st = re.search(r'struct %s\s*\{(.*?)\};' % struct, msl, re.S)
    return [(ty, nm, fc or None) for ty, nm, fc in re.findall(r'^\s*(float4|half4|uint4)\s+(\w+)\s*\[\[color\(0\)(?:, index\(0\))?(?:, function_constant\((\w+)\))?\]\];', st.group(1), re.M)] if st else []


def refused(msl):
    st = re.search(r'struct main0_out\s*\{(.*?)\};', msl, re.S)
    if not st: return 'no colour output'
    if [ty for ty, _, _ in _colour0(msl, 'main0_out')] in ([], ['uint4']):   # no colour 0, or one written as an integer or with fewer than four components: the test has no alpha to read
        return 'colour 0 has no float alpha' if re.search(r'\[\[color\(0\)\]\]', st.group(1)) else 'no colour 0 output'
    if f'[[buffer({dmsh_abi.ALPHA_BUFFER})]]' in msl: return f'fragment buffer {dmsh_abi.ALPHA_BUFFER} is taken'
    return None


def entry(msl, e):
    cols = _colour0(msl, e['ret'])
    if not cols: return None
    # main0_u writes colour 0 as a float or as the integer vector of the float's bits, each member present by its function constant (dmsh_entry.py fs_uint): the test reads the float
    read = lambda ty, nm: f'as_type<float>(dm_r.{nm}.w)' if ty == 'uint4' else f'float(dm_r.{nm}.w)'
    alpha = ' '.join(f'if ({fc}) dm_a = {read(ty, nm)};' if fc else f'dm_a = {read(ty, nm)};' for ty, nm, fc in cols)
    params = ', '.join(e['params'] + [f'constant dm_alpha_state& dm_alpha [[buffer({dmsh_abi.ALPHA_BUFFER})]]'])
    return (f'{e["attr"]}fragment {e["ret"]} {e["name"]}_a({params})\n{{\n    {e["ret"]} dm_r = dm_body_{e["name"]}({e["args"]});\n'
            f'    float dm_a = 1.0f; {alpha}\n    if (!dm_alpha_pass(dm_a, dm_alpha)) discard_fragment();\n    return dm_r;\n}}\n')
