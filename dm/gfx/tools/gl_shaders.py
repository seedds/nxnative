#!/usr/bin/env python3
# OpenGL games' GLSL shaders → one Metal library + a table for the host's GL layer (dm/gfx/gl/hle_gl.m). Digimon Story Cyber Sleuth: Complete Edition renders with
# NVIDIA's desktop GL through EGL and compiles GLSL ES 3.20 source at run time (glShaderSource / glCompileShader); the host finds each shader again by the hash of its source.
# Sources, all searched in every input path:
#   · MVGL archives (*.mvgl, dm/gfx/tools/mvgl.py): every .shad file, named <material>_vp / <material>_fp (1,682 files, 615 distinct sources in Digimon);
#   · an NSO (a file that starts with NSO0, the game's main): every NUL-terminated string that starts with '#version' (its built-in post effects, movie and shadow shaders);
#   · capture folders (the Mac host's DM_GL_CAPTURE): <key>.vert / <key>.frag, what the game really compiled, for sources it puts together at run time.
#   · loose source files: *.vert / *.frag, and *.glsl, whose name says nothing of its stage (Mega Man Legacy Collection's shaders/nx/*.glsl, read and compiled as they are).
#     libGDX's *.vs / *.fs are read where the directory has a complete source of each stage (Slay the Spire).
# Per shader:
#   · key = sha256(the source text up to its first NUL)[:16] (hex), as the host hashes the strings glShaderSource gets; the Metal function is gl_<key>_v or gl_<key>_f;
#   · the stage: a capture's extension, an archive file's _vp / _fp; a string in main or a .glsl file says nothing of its stage, so it is translated as each stage that compiles (one of
#     Digimon's built-in fragment shaders names gl_Position in a #define);
#   · GLSL → SPIR-V with glslang (-V -R: relaxed Vulkan rules, which gather the loose uniforms into one block, gl_DefaultUniformBlock; --aml / --amb number the inputs,
#     outputs and bindings), SPIR-V → MSL with spirv-cross (one Tier 2 argument buffer per stage; vertex shaders --fixup-clipspace: GL's z from −w to w → 0 to w);
#   · varyings are matched by name, as GL links them: --aml numbers each stage's outputs in its own declaration order, so [[user(locnN)]] becomes [[user(<variable name>)]];
#   · the host renders in GL's orientation (row 0 at the bottom, the NVN layer's lower-left origin), so nothing is flipped here.
# Output: <out>/gl/gl_shaders.metallib and <out>/gl/gl_shaders.tsv; --ios <dir> also writes <dir>/gl/ with an iPhone library (same table)
# Table rows: key <TAB> stage (vert / frag) <TAB> fields, space-separated (a key can have a row per stage):
#   a:<name>=<slot>:<type>                       a vertex input, [[attribute(slot)]]
#   u:<name>=<offset>:<type>:<count>:<stride>    a member of the default uniform block (count 0 = not an array)
#   r:<slot>:<count>                             the argument buffer's Metal buffer slot (-1 when absent), and number of argument IDs
#   b:<id>:<size>                                the default uniform block: its argument ID and size in bytes
#   n:<name>=<id>:<size>                         a named uniform block (DOOM 3's matrices_ubo): its argument ID and size in bytes
#   t:<name>=<texture id>:<sampler id>:<type>[:<unit>[:<count>]]  a combined sampler (-1 for absent sampler ID or implicit unit; count is for arrays)
#   o:<name>=<location>                          a fragment output, [[color(location)]]
# <TAB> where it came from
# Usage: gl_shaders.py <input path>... <out dir> [--ios <dir>]   (input paths: the RomFS, the main NSO, capture folders)
#        gl_shaders.py --capture-shader <vert|frag> <source file> <out dir>   (one Mac shader for the capture host)
import concurrent.futures as cf, hashlib, json, os, re, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../tools'))
from romfs import os, open
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mvgl
from dmsh_chain import SPIRV_CROSS, MAC_MIN_FLAG   # the in-repo spirv-cross build the NVN chain uses too (spirv_cross_bin)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../ios/tools')); from ios_metal import IOS_FLAGS   # the iPhone's deployment target and math mode

VEC_TYPES = {'float', 'vec2', 'vec3', 'vec4', 'int', 'ivec2', 'ivec3', 'ivec4', 'uint', 'uvec2', 'uvec3', 'uvec4', 'bool', 'mat2', 'mat3', 'mat4'}
TABLE_ABI = '# GL shader ABI 3'   # sampler arrays extend the argument-buffer table

def red(msg): sys.exit('[RED] ' + msg)

def text_of(b):
    b = bytes(b); z = b.find(b'\0')
    return b[:z] if z >= 0 else b

# A whole shader defines its entry point. DOOM 3's main holds two '#version 150' strings that are not shaders but the prefix its renderer puts before the GLSL it
# generates from its render programs at run time (helper functions, no main): those are left to the capture (dm/games/profile_guess.py GL_ENTRY, the same rule)
GL_ENTRY = re.compile(rb'\bvoid\s+main\s*\(')

def nso_strings(path):
    """'#version <number>' strings of an NSO's read-only segment (dm/tools/nso.py) that are whole shaders (GL_ENTRY); a main that links glslang (Balatro's LÖVE) also has its
    messages that start with '#version:'. nso.Error (a ValueError) on a damaged file"""
    import nso
    with open(path, 'rb') as f: m = nso.load(f.read(), path)
    (mo, sz) = m.segments[1]; img = bytes(m.image[mo:mo + sz])
    texts = (text_of(img[m.start():m.start() + 0x10000]) for m in re.finditer(rb'(?:^|(?<=\0))#version [0-9]', img))
    return [t for t in texts if GL_ENTRY.search(t)]


PAIR_STAGES = {'.vs': 'vert', '.fs': 'frag'}
GL_COMMENTS = re.compile(rb'/\*.*?\*/|//[^\r\n]*', re.DOTALL)
GL_MAIN_DEF = re.compile(rb'\bvoid\s+main\s*\(\s*(?:void\s*)?\)\s*\{')


def is_standalone_glsl(source):
    """True when source is text GLSL defining a main() entry point (not a header/snippet or binary)."""
    return bool(source) and not re.search(rb'[\x00-\x08\x0e-\x1f\x7f]', source) and bool(GL_MAIN_DEF.search(GL_COMMENTS.sub(b' ', source)))


def paired_sources(folder, names):
    """Find standalone vertex and fragment sources in one directory. The packer uses the same evidence before skipping capture."""
    candidates = {name: PAIR_STAGES[ext] for name in names if (ext := os.path.splitext(name)[1].lower()) in PAIR_STAGES}
    if len(set(candidates.values())) != 2: return {}   # LÖVE's .fs files, for example, are assembled at run time
    found = {}
    for name, stage in candidates.items():
        with open(os.path.join(folder, name), 'rb') as f: source = text_of(f.read())
        if is_standalone_glsl(source):
            found[name] = (source, stage)
    return found if len({stage for _, stage in found.values()}) == 2 else {}


def sources(roots):
    """(source bytes, stage or None, where) for every shader in the input paths"""
    for r in roots:
        if os.path.isfile(r):
            with open(r, 'rb') as f: head = f.read(4)
            if head == b'NSO0':
                for k, s in enumerate(nso_strings(r)): yield s, None, f'{os.path.basename(r)}#{k}'
            continue
        for d, _, fs in os.walk(r):
            paired = paired_sources(d, fs)
            for fn in sorted(fs):
                p = os.path.join(d, fn)
                low = fn.lower()
                if low.endswith('.mvgl'):
                    try: a = mvgl.Archive(p)
                    except ValueError: continue   # the sound archives are CRI AFS2 under the same extension
                    for nm in sorted(a.files):
                        if not nm.endswith('.shad'): continue
                        st = 'vert' if nm.endswith('_vp.shad') else 'frag' if nm.endswith('_fp.shad') else None
                        yield text_of(a.read(nm)), st, f'/{os.path.relpath(p, r)}:{nm}'
                elif low.endswith(('.vert', '.frag', '.glsl')):
                    with open(p, 'rb') as f: source = text_of(f.read())
                    ext = low.rsplit('.', 1)[1]
                    if ext == 'glsl' and not is_standalone_glsl(source): continue
                    yield source, None if ext == 'glsl' else ext, '/' + os.path.relpath(p, r)
                elif fn in paired:
                    source, stage = paired[fn]
                    yield source, stage, '/' + os.path.relpath(p, r)

# NVIDIA's GLSL compiler accepts more than the GLSL ES 3.20 specification, and glslang does not; these fix-ups run on the preprocessed text (glslang -E, so macros are expanded):
#   · the GLSL ES 1.0 texture functions (89 of Digimon's fragment shaders call textureCube; with -V glslang reads textureCube as Vulkan's texture type, so a macro cannot rename it);
#   · a redeclaration of gl_Position as a plain output (the built-in sprite shader of main declares `out float4 vPosition;` with vPosition defined as gl_Position);
#   · a texel offset that is not a constant (the built-in FXAA shader's textureOffset, FF7's textureLodOffset): Metal's sample offsets are constants too, so non-constant calls are
#     routed to overloaded helper functions that apply P + offset / the level's size (variable_offsets);
#   · built-ins redeclared as plain inputs and outputs, which DOOM 3's renderer writes into every GLSL 1.50 shader it generates (`out vec4 gl_Position;`, `in vec4 gl_FragCoord;`,
#     `out float gl_FragDepth;`): the declarations go, the built-ins stay; `out vec4 gl_FragColor;` becomes a plain output of the shader's own (Vulkan GLSL has no gl_FragColor);
#   · the compatibility colour declared as a plain varying, `out vec4 gl_FrontColor;` in the vertex shader and `in vec4 gl_Color;` in the fragment shader (DOOM 3): NVIDIA links the two
#     as GL's fixed pipeline does, so both become one varying, dm_Color, which the host matches by name;
#   · `sampler` as a name (DOOM 3's helpers take `sampler2D sampler`): a reserved word in Vulkan GLSL, an ordinary name in OpenGL's;
#   · a function body closed with `};` (Mega Man Legacy Collection's shaders/nx/*.glsl end main that way): GLSL ES has no empty declaration at file scope, so the `;` goes.
#     A struct's or a block's `};` ends its declaration and stays: only a `{` opened right after a `)` at file scope is a function body
OLD_TEX = [(r'\btexture2DLod\s*\(', 'textureLod('), (r'\btextureCubeLod\s*\(', 'textureLod('), (r'\btexture3DLod\s*\(', 'textureLod('),
           (r'\btexture2DProj\s*\(', 'textureProj('), (r'\btexture3DProj\s*\(', 'textureProj('),
           (r'\btexture2D\s*\(', 'texture('), (r'\btexture3D\s*\(', 'texture('), (r'\btextureCube\s*\(', 'texture(')]

def call_args(t, i):
    """the arguments of the call whose '(' is at t[i], and the index after its ')'"""
    depth, args, start = 0, [], i + 1
    for k in range(i, len(t)):
        c = t[k]
        if c == '(': depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0: args.append(t[start:k]); return args, k + 1
        elif c == ',' and depth == 1: args.append(t[start:k]); start = k + 1
    raise ValueError('unbalanced parentheses')

BUILTIN_DECL = r'^[ \t]*(?:invariant\s+)?(?:in|out)\s+(?:highp\s+|mediump\s+|lowp\s+)?(?:vec4|float)\s+gl_(?:Position|FragCoord|FragDepth|PointSize)\s*;'

def own_name(t, decl, name, new, out=None):
    """t with a built-in the shader declares as a plain varying (decl matches the declaration) renamed to a variable of its own; out: the declaration to put in its place"""
    if not re.search(decl, t, flags=re.M): return t
    t = re.sub(decl, out or '', t, flags=re.M)
    return re.sub(rf'\b{name}\b', new, t)

def function_semicolons(t):
    """t without the `;` right after the `}` that closes a function body at file scope"""
    out, depth, body, i, k = [], 0, False, 0, 0
    while k < len(t):
        c = t[k]
        if c == '{':
            if depth == 0: body = t[:k].rstrip().endswith(')')
            depth += 1
        elif c == '}' and depth:
            depth -= 1
            if depth == 0 and body:
                m = re.match(r'\s*;', t[k + 1:])
                if m: out.append(t[i:k + 1]); i = k + 1 + m.end(); k = i; continue
        k += 1
    return ''.join(out) + t[i:]

def rename_identifier(t, name, base, renamed, keep_calls=False):
    """Rename an identifier into a free name, recording only replacements made in this translation."""
    pattern = r'\b' + re.escape(name) + r'\b' + (r'(?!\s*\()' if keep_calls else '')
    if not re.search(pattern, t): return t
    used = set(re.findall(r'\b\w+\b', t))
    new, i = base, 0
    while new in used: i += 1; new = f'{base}_{i}'
    renamed[new] = name
    return re.sub(pattern, new, t)

def fixups(t, stage, renamed=None):
    if renamed is None: renamed = {}
    t = function_semicolons(t)
    for a, b in OLD_TEX: t = re.sub(a, b, t)
    t = re.sub(BUILTIN_DECL, '', t, flags=re.M)
    t = rename_identifier(t, 'sampler', 'dm_sampler', renamed)
    if stage == 'vert': t = own_name(t, r'^[ \t]*out\s+vec4\s+gl_FrontColor\s*;', 'gl_FrontColor', 'dm_Color', 'out vec4 dm_Color;')
    else:
        t = own_name(t, r'^[ \t]*in\s+vec4\s+gl_Color\s*;', 'gl_Color', 'dm_Color', 'in vec4 dm_Color;')
        t = own_name(t, r'^[ \t]*out\s+vec4\s+gl_FragColor\s*;', 'gl_FragColor', 'dm_FragColor', 'out vec4 dm_FragColor;')
    return variable_offsets(t, stage)

# All GLSL texture functions taking a texel offset: {name: {arg_count: offset_arg_index}}.
# texelFetchOffset takes 4 args normally (s, P, lod, offset) and 3 on desktop sampler2DRect (s, P, offset).
OFFSET_ARGS = {
    'textureOffset': {3: 2, 4: 2},
    'textureLodOffset': {4: 3},
    'textureGradOffset': {5: 4},
    'texelFetchOffset': {3: 2, 4: 3},
    'textureProjOffset': {3: 2, 4: 2},
    'textureProjLodOffset': {4: 3},
    'textureProjGradOffset': {5: 4},
}
OFFSET_RE = re.compile(r'\b(' + '|'.join(OFFSET_ARGS) + r')\s*\(')
CONST_DECL = re.compile(r'\bconst\s+(?:(?:highp|mediump|lowp)\s+)?\w+\s+([^;{}]+);')
NON_CONST_DECL = re.compile(r'\b(?:(?:highp|mediump|lowp)\s+)?(?:[iu]?vec[234]|int|uint|float|bool)\s+([^;{}()]+)')

def split_top(s):
    """Split s on top-level commas (outside parentheses and brackets)."""
    depth, out, start = 0, [], 0
    for k, c in enumerate(s):
        if c in '([': depth += 1
        elif c in ')]' and depth: depth -= 1
        elif c == ',' and depth == 0: out.append(s[start:k]); start = k + 1
    out.append(s[start:])
    return out

def const_offset(o, consts):
    """Whether a texel offset is a constant expression: no name in it but a vector constructor or a compile-time const name (a literal's suffix, as in 1u, is no name)."""
    return not any(n not in consts for n in re.findall(r'\b[A-Za-z_]\w*', re.sub(r'\b[iu]?vec[234]\b', '', o)))

def const_names(decls):
    """Names declared `const` with compile-time constant initializers and not also declared non-const or as a parameter anywhere in decls."""
    candidates, spans = {}, []
    for m in CONST_DECL.finditer(decls):
        items = split_top(m[1])
        parsed = []
        for item in items:
            dm = re.match(r'\s*([A-Za-z_]\w*)\s*=(.+)$', item, re.S)
            if dm: parsed.append((dm[1], dm[2]))
        if parsed and len(parsed) == len(items):
            spans.append(m.span())
            for name, init in parsed: candidates.setdefault(name, []).append(init)
    if not candidates: return set()
    # Strip matched const-variable statements and exclude any candidate name also declared non-const or as a const parameter.
    rest, pos = [], 0
    for a, b in spans: rest.append(decls[pos:a]); pos = b
    rest.append(decls[pos:])
    shadowed = set()
    for m in NON_CONST_DECL.finditer(''.join(rest)):
        for item in split_top(m[1]):
            dm = re.match(r'\s*([A-Za-z_]\w*)', item)
            if dm and dm[1] in candidates: shadowed.add(dm[1])
    consts, changed = set(), True
    while changed:
        changed = False
        for name, inits in candidates.items():
            if name in consts or name in shadowed: continue
            if all(const_offset(expr, consts) for expr in inits):
                consts.add(name); changed = True
    return consts

def offset_helpers(used, stage, desktop, prefix):
    """Generate overloaded GLSL helper functions for the offset families in `used`. Letting glslang resolve overloads covers every sampler declaration shape (arrays, structs,
    comma lists, parameters), projective vec3/vec4 variants, desktop 1D/Rect samplers, and single-evaluation of P/lod/offset without regex type inference."""
    lines = []
    def add(sig, body): lines.append(f'{sig} {{ return {body}; }}')
    tri = (('', 'vec4'), ('i', 'ivec4'), ('u', 'uvec4'))
    # (sampler, ret, p, off, grad, fetch_p, coord(sz), fetch_expr, projs, has_lod, has_bias, lod_one)
    specs = []
    for sp, rt in tri:
        specs += [
            (f'{sp}sampler2D', rt, 'vec2', 'ivec2', 'vec2', 'ivec2',
             lambda sz: f'p + vec2(o) / vec2({sz})', 'p + o',
             (('vec3', lambda sz: f'p.xy / p.z + vec2(o) / vec2({sz})'),
              ('vec4', lambda sz: f'p.xy / p.w + vec2(o) / vec2({sz})')), True, True, 'ivec2(1)'),
            (f'{sp}sampler2DArray', rt, 'vec3', 'ivec2', 'vec2', 'ivec3',
             lambda sz: f'vec3(p.xy + vec2(o) / vec2({sz}.xy), p.z)', 'p + ivec3(o, 0)', (), True, True, 'ivec3(1)'),
            (f'{sp}sampler3D', rt, 'vec3', 'ivec3', 'vec3', 'ivec3',
             lambda sz: f'p + vec3(o) / vec3({sz})', 'p + o',
             (('vec4', lambda sz: f'p.xyz / p.w + vec3(o) / vec3({sz})'),), True, True, 'ivec3(1)'),
        ]
    specs += [
        ('sampler2DShadow', 'float', 'vec3', 'ivec2', 'vec2', None,
         lambda sz: f'vec3(p.xy + vec2(o) / vec2({sz}), p.z)', None,
         (('vec4', lambda sz: f'vec3(p.xy / p.w + vec2(o) / vec2({sz}), p.z / p.w)'),), True, True, 'ivec2(1)'),
        ('sampler2DArrayShadow', 'float', 'vec4', 'ivec2', 'vec2', None,
         lambda sz: f'vec4(p.xy + vec2(o) / vec2({sz}.xy), p.zw)', None, (), False, False, 'ivec3(1)'),
    ]
    if desktop:
        for sp, rt in tri:
            specs += [
                (f'{sp}sampler1D', rt, 'float', 'int', 'float', 'int',
                 lambda sz: f'p + float(o) / float({sz})', 'p + o',
                 (('vec2', lambda sz: f'p.x / p.y + float(o) / float({sz})'),
                  ('vec4', lambda sz: f'p.x / p.w + float(o) / float({sz})')), True, True, '1'),
                (f'{sp}sampler1DArray', rt, 'vec2', 'int', 'float', 'ivec2',
                 lambda sz: f'vec2(p.x + float(o) / float({sz}.x), p.y)', 'p + ivec2(o, 0)', (), True, True, 'ivec2(1)'),
                (f'{sp}sampler2DRect', rt, 'vec2', 'ivec2', 'vec2', 'ivec2',
                 lambda _sz: 'p + vec2(o)', 'p + o',
                 (('vec3', lambda _sz: 'p.xy / p.z + vec2(o)'),
                  ('vec4', lambda _sz: 'p.xy / p.w + vec2(o)')), False, False, None),
            ]
        specs += [
            ('sampler1DShadow', 'float', 'vec3', 'int', 'float', None,
             lambda sz: f'vec3(p.x + float(o) / float({sz}), p.yz)', None,
             (('vec4', lambda sz: f'vec3(p.x / p.w + float(o) / float({sz}), p.y / p.w, p.z / p.w)'),), True, True, '1'),
            ('sampler1DArrayShadow', 'float', 'vec3', 'int', 'float', None,
             lambda sz: f'vec3(p.x + float(o) / float({sz}.x), p.yz)', None, (), True, True, 'ivec2(1)'),
            ('sampler2DRectShadow', 'float', 'vec3', 'ivec2', 'vec2', None,
             lambda _sz: 'vec3(p.xy + vec2(o), p.z)', None,
             (('vec4', lambda _sz: 'vec3(p.xy / p.w + vec2(o), p.z / p.w)'),), False, False, None),
        ]
    for sam, rt, pt, ot, gt, ft, coord, fexpr, projs, has_lod, has_bias, lod_one in specs:
        sz0 = 'textureSize(s, 0)'
        szl = f'max(textureSize(s, max(int(l), 0)), {lod_one})' if lod_one else sz0
        if 'textureOffset' in used:
            add(f'highp {rt} {prefix}textureOffset(highp {sam} s, highp {pt} p, highp {ot} o)', f'texture(s, {coord(sz0)})')
            if stage == 'frag' and has_bias:
                add(f'highp {rt} {prefix}textureOffset(highp {sam} s, highp {pt} p, highp {ot} o, highp float b)', f'texture(s, {coord(sz0)}, b)')
        if 'textureLodOffset' in used and has_lod:
            add(f'highp {rt} {prefix}textureLodOffset(highp {sam} s, highp {pt} p, highp float l, highp {ot} o)', f'textureLod(s, {coord(szl)}, l)')
        if 'textureGradOffset' in used:
            add(f'highp {rt} {prefix}textureGradOffset(highp {sam} s, highp {pt} p, highp {gt} dx, highp {gt} dy, highp {ot} o)', f'textureGrad(s, {coord(sz0)}, dx, dy)')
        if 'texelFetchOffset' in used and ft is not None:
            if '2DRect' in sam:
                add(f'highp {rt} {prefix}texelFetchOffset(highp {sam} s, highp {ft} p, highp {ot} o)', f'texelFetch(s, {fexpr})')
            else:
                add(f'highp {rt} {prefix}texelFetchOffset(highp {sam} s, highp {ft} p, highp int l, highp {ot} o)', f'texelFetch(s, {fexpr}, l)')
        for ppt, pcoord in projs:
            if 'textureProjOffset' in used:
                add(f'highp {rt} {prefix}textureProjOffset(highp {sam} s, highp {ppt} p, highp {ot} o)', f'texture(s, {pcoord(sz0)})')
                if stage == 'frag' and has_bias:
                    add(f'highp {rt} {prefix}textureProjOffset(highp {sam} s, highp {ppt} p, highp {ot} o, highp float b)', f'texture(s, {pcoord(sz0)}, b)')
            if 'textureProjLodOffset' in used and has_lod:
                add(f'highp {rt} {prefix}textureProjLodOffset(highp {sam} s, highp {ppt} p, highp float l, highp {ot} o)', f'textureLod(s, {pcoord(szl)}, l)')
            if 'textureProjGradOffset' in used:
                add(f'highp {rt} {prefix}textureProjGradOffset(highp {sam} s, highp {ppt} p, highp {gt} dx, highp {gt} dy, highp {ot} o)', f'textureGrad(s, {pcoord(sz0)}, dx, dy)')
    return '\n'.join(lines)

def variable_offsets(t, stage='frag'):
    """Lower non-constant texel offsets, which NVIDIA's compiler takes and GLSL/glslang reject ("'texel offset' : argument must be compile-time constant"), by routing each
    such call to an injected overloaded helper. Compile-time constant offsets stay untouched so the sampler applies them at each mip level natively. Replacing only the call
    name lets one flat scan handle nested offset calls automatically."""
    if not OFFSET_RE.search(t): return t
    consts = const_names(t)
    prefix = 'dm_var_'
    while prefix in t: prefix += 'x_'
    out, used, i = [], set(), 0
    for m in OFFSET_RE.finditer(t):
        fn = m[1]
        args, _ = call_args(t, m.end() - 1)
        at = OFFSET_ARGS[fn].get(len(args))
        if at is None or const_offset(args[at], consts): continue
        used.add(fn)
        out.append(t[i:m.start()] + prefix + fn)
        i = m.start() + len(fn)
    if not used: return t
    rewritten = ''.join(out) + t[i:]
    ver = re.search(r'^\s*#\s*version\s+(\d+)([^\r\n]*)', rewritten, re.M)
    desktop = bool(ver and not re.match(r'[ \t]+es\b', ver[2]) and int(ver[1]) not in (100, 300, 310, 320))
    helpers = offset_helpers(used, stage, desktop, prefix)
    lines = rewritten.splitlines()
    at = 0
    while at < len(lines) and (not lines[at].strip() or lines[at].lstrip().startswith(('#version', '#extension', '#line', '#pragma'))):
        at += 1
    return '\n'.join((*lines[:at], helpers, *lines[at:])) + ('\n' if rewritten.endswith('\n') else '')

def glsl_type(t, refl):
    return refl['types'][t]['name'] if t in refl.get('types', {}) else t

def active_default_uniforms(log):
    """Read the active names in glslang -q's default block. The SPIRV-Cross layout includes declared but inactive members."""
    _, uniform_header, data = log.partition('Uniform reflection:\n')
    uniforms, block_header, blocks = data.partition('\nUniform block reflection:\n')
    if not uniform_header or not block_header: return None
    block = re.search(r'^gl_DefaultUniformBlock: [^\n]*\bindex (\d+),', blocks.split('\n\n', 1)[0], re.M)
    if not block: return set()
    return {name for name, index in re.findall(r'^(\S+): [^\n]*\bindex (-?\d+),', uniforms, re.M) if index == block[1]}

def uni_members(refl, ms, prefix, base, f, bools, active):
    """Table rows for default block members. A struct's members recurse with their dotted name, which is the name the game passes to GL (u_LightEnv.m_dirLights_dir[0]); the
    reflection gives a nested member's offset relative to its parent, so the parent's offset is added"""
    for mb in ms:
        arr = mb.get('array', [])
        name = prefix + mb['name']
        if len(arr) > 1: return f'uniform {name}: arrays of arrays'
        if 'members' in refl['types'].get(mb['type'], {}):
            if arr: return f'uniform {name}: an array of structs is not implemented'
            e = uni_members(refl, refl['types'][mb['type']]['members'], name + '.', base + mb['offset'], f, bools, active)
            if e: return e
            continue
        if name not in active: continue
        ty = glsl_type(mb['type'], refl)
        if ty == 'uint' and mb['name'] in bools: ty = 'bool'
        if ty not in VEC_TYPES: return f'uniform {name}: type {ty} (structs are not implemented)'
        f.append(f'u:{name}={base + mb["offset"]}:{ty}:{arr[0] if arr else 0}:{mb.get("array_stride", 0)}')

# GLSL ES 1.00 (#version 100 or unversioned) and legacy desktop GLSL (#version 110 / 120 / 130): `attribute`, `varying`, `texture2D`/`texture3D`, `gl_FragColor`,
# precision statements and lowp / mediump / highp qualifiers. ES 1.00 promotes to `#version 310 es`; legacy desktop GLSL promotes to `#version 450 core`, preserving
# desktop implicit int-to-float conversions and sampler types without ES precision qualifiers (such as `sampler3D`). Precision statements/qualifiers and texture
# functions are rewritten after `glslangValidator -E` so comments are stripped and macros (such as `#define LOWPREC lowp`) are expanded first.
def promote_legacy(t, stage, renamed=None, desktop=False):
    if renamed is None: renamed = {}
    t = re.sub(r'\battribute\b', 'in', t)
    t = re.sub(r'\bvarying\b', 'out' if stage == 'vert' else 'in', t)
    if stage == 'frag' and 'gl_FragColor' in t:
        t = 'out vec4 gm_FragColor;\n' + t.replace('gl_FragColor', 'gm_FragColor')
    t = rename_identifier(t, 'sample', 'gm_sample', renamed)   # `sample` became a reserved word after ES 1.00 (GameMaker's sprite shaders use it as a variable)
    # `texture` became a built-in function after ES 1.00, and a name declared `texture` redefines it (Fate/hollow ataraxia REMASTERED: `uniform sampler2D texture;`); a call stays
    t = rename_identifier(t, 'texture', 'dm_texture', renamed, keep_calls=True)
    return ('#version 450 core\n' if desktop else '#version 310 es\n') + t

def strip_legacy_precision(t, desktop=False):
    """Strip legacy precision statements and qualifiers after preprocessing (comments gone, macros expanded), then set ES 3.10 defaults."""
    t = re.sub(r'\bprecision\s+(?:lowp|mediump|highp)\s+\w+\s*;', '', t)
    t = re.sub(r'\b(?:lowp|mediump|highp)\b[ \t]*', '', t)
    if desktop: return t
    lines = t.splitlines()
    at = 1
    while at < len(lines) and (not lines[at].strip() or lines[at].lstrip().startswith(('#extension', '#line'))):
        at += 1
    return '\n'.join((*lines[:at], 'precision highp float;', 'precision highp int;', *lines[at:])) + ('\n' if t.endswith('\n') else '')

def es100(t, stage, renamed=None):
    return promote_legacy(t, stage, renamed, desktop=False)

def legacy_desktop_io(t, stage):
    """Lower the compatibility varyings used by programmable GLSL 1.10/1.20/130 stages after expanding preprocessor macros."""
    direction = 'out' if stage == 'vert' else 'in'
    color = 'gl_FrontColor' if stage == 'vert' else 'gl_Color'
    secondary = 'gl_FrontSecondaryColor' if stage == 'vert' else 'gl_SecondaryColor'
    decl = []
    for old, name, ty in ((color, 'dm_LegacyColor', 'vec4'), (secondary, 'dm_LegacySecondaryColor', 'vec4'),
                          ('gl_FogFragCoord', 'dm_LegacyFog', 'float')):
        t, n = re.subn(rf'\b{old}\b', name, t)
        if n: decl.append(f'{direction} {ty} {name};')
    arrays = [('gl_TexCoord', 'dm_LegacyTexCoord', f'{direction} vec4')]
    if stage == 'frag': arrays.append(('gl_FragData', 'dm_LegacyFragData', 'out vec4'))
    for old, name, qualifier in arrays:
        pattern = rf'\b{old}\s*\[\s*(0|[1-9][0-9]*)\s*\]'
        for i in sorted(set(re.findall(pattern, t)), key=int):
            layout = f'layout(location = {i}) ' if old == 'gl_FragData' else ''
            decl.append(f'{layout}{qualifier} {name}{i};')
        t = re.sub(pattern, lambda m, prefix=name: prefix + m[1], t)
    if not decl: return t
    lines = t.splitlines()
    at = 1
    while at < len(lines) and (not lines[at].strip() or lines[at].lstrip().startswith(('#extension', '#line'))):
        at += 1
    return '\n'.join((*lines[:at], *decl, *lines[at:])) + ('\n' if t.endswith('\n') else '')

def explicit_sampler_bindings(text):
    """Initial texture units from explicit GLSL layout(... binding = N ...) sampler declarations in preprocessed text."""
    out = {}
    pat = (r'(?:\blayout\s*\(([^)]*)\)\s*uniform\b|\buniform\s+layout\s*\(([^)]*)\))'
           r'\s+(?:(?:lowp|mediump|highp)\s+)?[iu]?sampler\w+\s+([^;]+);')
    for m in re.finditer(pat, text):
        qual = m.group(1) if m.group(1) is not None else m.group(2)
        b = re.search(r'\bbinding\s*=\s*(0[xX][0-9a-fA-F]+|\d+)\b', qual)
        if not b: continue
        unit = int(b.group(1), 0)
        for decl in m.group(3).split(','):
            nm = re.match(r'\s*([A-Za-z_]\w*)', decl)
            if nm: out[nm.group(1)] = unit
    return out

# The game's names that the rewrite renames (es100, fixups): the table rows give them back as the game wrote them, since the host finds attributes, uniforms and samplers by the
# game's name (glGetAttribLocation, glGetUniformLocation); the key is the hash of the game's own source
def game_names(f, renamed):
    """table fields with the renamed names in their name part (a:, o:, u:, t:, n: up to the '=') given back"""
    def back(x):
        k, sep, rest = x.partition('=')
        return re.sub(r'\b\w+\b', lambda m: renamed.get(m.group(0), m.group(0)), k) + sep + rest
    return [back(x) for x in f]

def argument_names(spv):
    """Give bound SPIR-V variables collision-free names, so MSL reflection can identify even an unnamed default block. Keep the original GL names in the table.
    Descriptor sets are Vulkan's namespace, not GL's: all resources go into one argument buffer, whose IDs spirv-cross allocates independently of the Binding decorations."""
    data = open(spv, 'rb').read(); words = list(struct.unpack(f'<{len(data) // 4}I', data))
    ins, names, bound, pointers, variables, i = [], {}, set(), {}, {}, 5
    while i < len(words):
        n, op = words[i] >> 16, words[i] & 0xffff
        if not n: raise ValueError('SPIR-V instruction length 0')
        w = words[i:i + n]; ins.append(w)
        if op == 5: names[w[1]] = struct.pack(f'<{n - 2}I', *w[2:]).split(b'\0')[0].decode()
        elif op == 71 and w[2] == 33: bound.add(w[1])   # OpDecorate Binding
        elif op == 32: pointers[w[1]] = w[3]   # OpTypePointer
        elif op == 59: variables[w[2]] = (w[1], w[3])   # OpVariable: pointer type, storage class
        i += n
    prefix = 'dm_gl_resource_'
    while any(prefix in s for s in names.values()): prefix += 'x'
    renamed = {v: prefix + str(v) for v in bound}
    new_names = []
    for v, name in sorted(renamed.items()):
        b = name.encode() + b'\0'; b += b'\0' * (-len(b) % 4)
        new_names += [((2 + len(b) // 4) << 16) | 5, v, *struct.unpack(f'<{len(b) // 4}I', b)]
    out, added = words[:5], False
    for w in ins:
        op = w[0] & 0xffff
        if not added and op in (5, 6, 71, 72): out += new_names; added = True
        if op == 5 and w[1] in renamed: continue
        if op == 71 and w[1] in renamed and w[2] == 34: w[3] = 0   # DescriptorSet
        out += w
    open(spv, 'wb').write(struct.pack(f'<{len(out)}I', *out))
    textures = {names.get(v, ''): name for v, name in renamed.items() if variables[v][1] == 0}   # UniformConstant
    buffers = {f'_{pointers[variables[v][0]]}': name for v, name in renamed.items() if variables[v][1] == 2}   # Uniform
    return textures, buffers

def translate(src, stage, key, tmp):
    """GLSL → MSL; returns (msl path, table fields) or an error string"""
    t = src.decode('latin1'); renamed = {}
    v = re.search(r'^[ \t]*#[ \t]*version[ \t]+(\d+)([^\r\n]*)\r?\n?', t, re.M)
    desktop_legacy = bool(v and v.group(1) in ('110', '120', '130'))
    legacy = not v or v.group(1) == '100' or desktop_legacy
    if legacy:
        src = promote_legacy(t.replace(v.group(0), '', 1) if v else t, stage, renamed, desktop=desktop_legacy).encode('latin1')
    elif v and v.group(1) in ('310', '320') and re.match(r'^[ \t]*(?:$|//|/\*)', v.group(2)):
        # GLSL 310/320 exist only in OpenGL ES; NVIDIA accepts them without `es`, while glslang -E requires it.
        src = (t[:v.end(1)] + ' es' + t[v.end(1):]).encode('latin1')
    g = os.path.join(tmp, f'{key}.{stage}'); spv = g + '.spv'; msl = os.path.join(tmp, f'{key}_{stage[0]}.metal')
    open(g + '.in', 'wb').write(src)
    r = subprocess.run(['glslangValidator', '-E', '-S', stage, g + '.in'], capture_output=True, text=True)
    if r.returncode: return 'glslang -E: ' + ' | '.join(l for l in (r.stdout + r.stderr).splitlines() if 'ERROR' in l and 'compilation' not in l)[:400]
    pre = r.stdout[r.stdout.find('#version'):]   # warnings come first
    # Vulkan SPIR-V requires GLSL ES 3.10. Expand macros under the game's
    # original version before raising the version used by the compiler.
    pre = re.sub(r'^#version[ \t]+300[ \t]+es\b', '#version 310 es', pre, count=1)
    if legacy: pre = strip_legacy_precision(pre, desktop=desktop_legacy)
    if desktop_legacy: pre = legacy_desktop_io(pre, stage)
    fixed = fixups(pre, stage, renamed)
    open(g, 'w').write(fixed)
    r = subprocess.run(['glslangValidator', '-V', '-R', '--aml', '--amb', '-q', '-S', stage, g, '-o', spv], capture_output=True, text=True)
    if r.returncode: return 'glslang: ' + ' | '.join(l for l in (r.stdout + r.stderr).splitlines() if 'ERROR' in l and 'compilation' not in l)[:400]
    active = active_default_uniforms(r.stdout)
    if active is None: return 'glslang: could not read active uniform reflection'
    refl = json.loads(subprocess.run([SPIRV_CROSS, spv, '--reflect'], capture_output=True, text=True, check=True).stdout)
    for k in ('separate_images', 'separate_samplers', 'images', 'ssbos', 'push_constants'):
        if refl.get(k): return f'{k} are not implemented'
    for u in refl.get('ubos', []):
        if u.get('array'): return f'uniform block array {u["name"]}'
    for t in refl.get('textures', []):
        if 'array' in t and (len(t['array']) != 1 or not 0 < t['array'][0] <= 96):
            return f'unsupported sampler array shape for {t["name"]}'
    textures, buffers = argument_names(spv)
    cmd = [SPIRV_CROSS, spv, '--msl', '--msl-version', '20300', '--msl-argument-buffers', '--msl-argument-buffer-tier', '1',
           '--msl-texture-buffer-native', '--rename-entry-point', 'main', f'gl_{key}_{stage[0]}', stage, '--output', msl]
    if stage == 'vert': cmd.append('--fixup-clipspace')
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode: return 'spirv-cross: ' + r.stderr.strip()[:300]
    m = open(msl).read()
    # Varyings by name: the out struct of a vertex shader and the in struct of a fragment shader (stage_in members of a vertex shader are [[attribute(n)]])
    m = re.sub(r'(\b(\w+)\s+\[\[)user\(locn\d+\)', lambda x: f'{x.group(1)}user({renamed.get(x.group(2), x.group(2))})', m)
    m = re.sub(r'\bfast::(min|max|clamp)\(', r'precise::\1(', m)   # a NaN operand gives the other one, as on the Switch's GPU (vk_shaders.py PRECISE)
    open(msl, 'w').write(m)
    ids = {n: int(i) for n, i in re.findall(r'\b(\w+)\s+\[\[id\((\d+)\)\]\]', m)}
    nargs = max(ids.values(), default=-1) + 1
    f = []
    if stage == 'vert':
        for v in refl.get('inputs', []): f.append(f'a:{v["name"]}={v["location"]}:{v["type"]}')
    else:
        for v in refl.get('outputs', []): f.append(f'o:{v["name"]}={v["location"]}')
    # a bool uniform is a uint in the SPIR-V block (it has no bool members), but the game asks GL for its declared type: LÖVE's Shader:send takes only a boolean for a GLSL bool
    bools = {n for n in re.findall(r'\buniform\s+(?:(?:lowp|mediump|highp)\s+)?bool\s+(\w+)\s*;', pre)}
    for u in refl.get('ubos', []):
        aid = ids.get(buffers[u['type']])
        if aid is None: continue
        t = refl['types'][u['type']]
        if t['name'] != 'gl_DefaultUniformBlock':   # a named block: the buffer the game binds to its binding point (glUniformBlockBinding, glBindBufferRange)
            f.append(f'n:{t["name"]}={aid}:{u["block_size"]}'); continue
        f.append(f'b:{aid}:{u["block_size"]}')
        e = uni_members(refl, t['members'], '', 0, f, bools, active)
        if e: return e
    # the samplers the MSL takes: spirv-cross leaves out one the shader never reads, but its reflection still lists it, and glslang gives it the binding of a used one (Mega Man Legacy
    # Collection's default_ps.glsl declares T1 for code under #if 0: T0 and T1 both at 0). Listed, the host bound the unit T1 names, an empty one, over T0's texture: every sprite sampled black
    bindings = explicit_sampler_bindings(fixed)
    for t in refl.get('textures', []):
        name = textures[t['name']]
        if name not in ids: continue
        unit = bindings.get(t['name'])
        tex, samp = ids[name], ids.get(name + 'Smplr', -1)
        count = t.get('array', [0])[0]
        nargs = max(nargs, max(tex, samp) + (count or 1))
        suffix = f':{unit if unit is not None else -1}:{count}' if count else (f':{unit}' if unit else '')
        f.append(f't:{t["name"]}={tex}:{samp}:{t["type"]}{suffix}')
    f.insert(0, f'r:{0 if nargs else -1}:{nargs}')
    return msl, game_names(f, renamed)

def one(item, tmp):
    """[(key, stage, fields, msl, where)], or [(key, None, errors, None, where)] when no stage translates"""
    src, stage, where = item
    key = hashlib.sha256(src).hexdigest()[:16]
    out, errs = [], []
    for st in [stage] if stage else ['vert', 'frag']:
        r = translate(src, st, key, tmp)
        if isinstance(r, tuple): out.append((key, st, r[1], r[0], where))
        else: errs.append(f'{st}: {r}')
    return out or [(key, None, errs, None, where)]

def metallib(msls, out, sdk, extra):
    os.makedirs(out, exist_ok=True)
    def comp(m):
        a = m[:-6] + f'.{sdk}.air'
        # -fwrapv: signed integer overflow wraps, as GLSL's (and SPIR-V's) integer arithmetic does; in MSL it is undefined and the compiler folds around it (dmsh_chain.py MAC_METAL)
        r = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-c', '-fpreserve-invariance', '-fwrapv', *extra, m, '-o', a], capture_output=True, text=True)
        if r.returncode: red(f'{m}: metal compile failed: {r.stderr.strip()[:400]}')
        return a
    with cf.ThreadPoolExecutor(os.cpu_count()) as ex: airs = list(ex.map(comp, msls))
    lib = os.path.join(out, 'gl_shaders.metallib')
    subprocess.run(['xcrun', '-sdk', sdk, 'metallib', *airs, '-o', lib], check=True)
    return lib

def capture_shader(stage, path, out):
    if stage not in ('vert', 'frag'):
        red(f'invalid capture stage: {stage}')
    with open(path, 'rb') as f:
        src = text_of(f.read())
    key = hashlib.sha256(src).hexdigest()[:16]
    with tempfile.TemporaryDirectory(dir=out) as tmp:
        result = translate(src, stage, key, tmp)
        if isinstance(result, str):
            red(f'{key}.{stage}: {result}')
        msl, fields = result
        metallib([msl], out, 'macosx', [MAC_MIN_FLAG])
        with open(os.path.join(out, 'gl_shaders.tsv'), 'w') as f:
            f.write(TABLE_ABI + '\n' + '\t'.join((key, stage, ' '.join(fields))) + '\n')

def main():
    a = sys.argv[1:]
    if len(a) == 4 and a[0] == '--capture-shader':
        return capture_shader(*a[1:])
    ios = None
    if '--ios' in a: i = a.index('--ios'); ios = a[i + 1]; del a[i:i + 2]
    fixed = '--fixed-function' in a
    if fixed: a.remove('--fixed-function')
    if len(a) < 2: sys.exit('usage: gl_shaders.py <input path>... <out dir> [--ios <dir>] [--fixed-function]')
    roots, out = a[:-1], a[-1]
    items, seen_src = [], set()
    for s, st, where in sources(roots):
        if (s, st) in seen_src: continue
        seen_src.add((s, st)); items.append((s, st, where))
    if fixed and items: red('GLSL sources found in a fixed-function build')
    if not fixed and not items: red(f'no GLSL shaders in {" ".join(roots)}')
    with tempfile.TemporaryDirectory() as tmp:
        with cf.ThreadPoolExecutor(os.cpu_count()) as ex: rows = [r for rs in ex.map(lambda it: one(it, tmp), items) for r in rs]
        seen, failed = {}, []
        for key, stage, f, msl, where in rows:
            if not stage: failed.append((key, where, f)); continue
            if (key, stage) not in seen: seen[(key, stage)] = (stage, f, msl, where)
        for key, where, errs in failed:
            if not any(k == key for k, _ in seen): print(f'[RED] {where} ({key}) does not translate: {" / ".join(errs)}')
        if not fixed and not seen: red('no GLSL shaders translated')
        tsv = [TABLE_ABI, '# GL shaders (dm/gfx/tools/gl_shaders.py): key = sha256(GLSL source)[:16], Metal function gl_<key>_v / gl_<key>_f; a: vertex inputs, u: default uniform block members '
               '(offset:type:count:stride), r: argument buffer (slot:count), b: the block (id:size), n: named blocks (name=id:size), t: combined samplers (texture-id:sampler-id:type), o: fragment outputs']
        if fixed:
            tsv.append('# Fixed-function OpenGL uses the host Metal shaders')
        else:
            spirv = subprocess.run([SPIRV_CROSS, '--revision'], capture_output=True, text=True)
            tsv.append('# ' + ' '.join(subprocess.run(['glslangValidator', '--version'], capture_output=True, text=True).stdout.split()[:3]) + '; spirv-cross '
                       + ' '.join((spirv.stdout + spirv.stderr).split()))
        tsv += [f'{k[0]}\t{v[0]}\t{" ".join(v[1])}\t{v[3]}' for k, v in sorted(seen.items())]
        dirs = [(os.path.join(out, 'gl'), 'macosx', [MAC_MIN_FLAG])]   # the Mac host's minimum macOS, as the NVN chain's libraries (dmsh_chain.py MAC_MIN)
        if ios: dirs.append((os.path.join(ios, 'gl'), 'iphoneos', list(IOS_FLAGS)))   # relaxed as in vk_shaders.py and ios_metal.py: no NaN / Inf assumption on the iPhone's GPU compiler
        msls = [v[2] for v in seen.values()]
        if fixed:
            placeholder = os.path.join(tmp, 'empty.metal')
            with open(placeholder, 'w') as f: f.write('#include <metal_stdlib>\n')
            msls.append(placeholder)
        for d, sdk, extra in dirs:
            lib = metallib(msls, d, sdk, extra)
            open(os.path.join(d, 'gl_shaders.tsv'), 'w').write('\n'.join(tsv) + '\n')
            print(f'{sdk}: {len(seen)} shaders ({sum(1 for v in seen.values() if v[0] == "vert")} vertex) → {lib}' + (f'; {len(failed)} failed' if failed else ''))

if __name__ == '__main__':
    import nso
    try: main()
    except nso.Error as e: red(str(e))   # a damaged main
