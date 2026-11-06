# "Readable-tile" version of a fragment shader (merge B step 2): within one pass, when a later draw samples, at its own pixel, a color attachment written by an earlier draw,
# it is changed to read the color directly from the tile (framebuffer fetch); the two render passes become one, and the intermediate attachment need not be stored to main memory and loaded back just to be sampled.
# Built offline; at runtime only function-constant specialization is done (no source compilation). Usage: variant(msl, bindings, om_only_ok=False) → (new MSL, metadata) or (None, reason)
# Function constant numbering: FX_i (which color index the i-th rewritable sample reads, -1 = sample as usual) = i; FB_k (enable color input k) = 32 + k;
#               OM_j_k (the j-th output writes to attachment k) = 64 + 8 j + k
# Rewritable samples: .sample(sampler, select(_N, float2(0.0), isnan(_N))) on a 2D float texture, where _N = float2(in.m_X.x, in.m_X.y) (interpolated output unchanged),
# or _N = float2(in.m_X.x / in.m_X.w, in.m_X.y / in.m_X.w) (projective: 'proj', the screen position deferred decals sample their G-buffer at; screen_varyings gives the vertex side),
# or _N = float2(gl_FragCoord.x [/ render scale] * U, gl_FragCoord.y [/ render scale] * V) with U, V uniform components ('frag_pos': their [slot, vector, component]; Splatoon Raiders'
# ink stamps read their map at gl_FragCoord / 6400);
# whether the texel is really fetched at this pixel is proven at runtime (correspondence between vertex uv and position, a screen varying with the viewport over the whole target,
# or U and V = 1 / the target's size; texture size = target size); if it cannot be proven, the original version is used
import collections
import re
import dmsh_abi as abi
import dmsh_side

NFB = 8

def _once(msl, name):   # name is assigned exactly once in the source (its declaration): spirv-cross also declares variables first and assigns them in branches
    # Found as text, with a look at the character before: a pattern that starts with a lookbehind has no literal prefix to skip to, so re tries every character (4 ms each on Pokopia's 220 KB sources,
    # and 24 calls a pixel shader made this 38% of the builder's Python, under the GIL, GitHub issue #1036)
    text, n = name + ' = ', 0
    i = msl.find(text)
    while i >= 0:
        if not i or not (msl[i - 1] in '._' or msl[i - 1].isalnum()): n += 1
        i = msl.find(text, i + len(text))
    return n == 1

def _proj(msl, var, comp, locn):
    """var = in.m_X.<comp> / in.m_X.w, written as a product with 1.0 / w or as a quotient (each name assigned once) → m_X, or None"""
    def src(n, c):   # float n = in.m_X.c; → m_X
        m = re.search(r'float %s = in\.(m_\d+)\.%s;' % (re.escape(n), c), msl)
        return m.group(1) if m and _once(msl, n) and m.group(1) in locn else None
    if not _once(msl, var): return None
    m = re.search(r'float %s = (_\d+) \* (_\d+);' % re.escape(var), msl)
    if m:
        for x, r in ((m.group(1), m.group(2)), (m.group(2), m.group(1))):
            d = re.search(r'float %s = 1\.0 / (_\d+);' % re.escape(r), msl)
            if d and _once(msl, r) and src(x, comp) and src(x, comp) == src(d.group(1), 'w'): return src(x, comp)
        return None
    m = re.search(r'float %s = (_\d+) / (_\d+);' % re.escape(var), msl)
    return src(m.group(1), comp) if m and src(m.group(1), comp) == src(m.group(2), 'w') else None

_UNI = re.compile(r'\(\*spvDescriptorSet0\.fp_c(\d+)\)\._m0\[(\d+)\]\.([xyzw])')
_SCALE = '(*spvDescriptorSet0.support_buffer)._m5[0].x'   # the support buffer's render scale (Ryujinx's layout), which the host fills with 1 (nvn_mtl.m support buffer)

def _frag_pos(msl, var, comp):
    """var = gl_FragCoord.<comp> [/ render scale] * U (either order), U a fragment uniform component, each name assigned once → [uniform slot, vector, component] (fp_cN is slot N − 3,
    as dmsh_build.py raster_facts reads vp_cN), or None"""
    def one(n, pat):
        m = re.search(r'float %s = %s;' % (re.escape(n), pat), msl)
        return m if m and _once(msl, n) else None
    def pixel(n):   # n = gl_FragCoord.<comp>, or that over the render scale
        if one(n, r'gl_FragCoord\.%s' % comp): return True
        q = one(n, r'(_\d+) / (_\d+)')
        return bool(q and one(q.group(1), r'gl_FragCoord\.%s' % comp) and one(q.group(2), re.escape(_SCALE)))
    m = one(var, r'(\S+) \* (\S+)')
    if not m: return None
    for a, u in ((m.group(1), m.group(2)), (m.group(2), m.group(1))):
        k = _UNI.fullmatch(u)
        if k and int(k.group(1)) >= 3 and re.fullmatch(r'_\d+', a) and pixel(a): return [int(k.group(1)) - 3, int(k.group(2)), 'xyzw'.index(k.group(3))]
    return None

# ---- Screen varyings (vertex side of a projective sample) ----
# Interpolated output locn K is the clip → screen varying when, in every vertex entry point (main0, main0_z, ...), its .x = (P.x + P.w) / 2, .y = (P.w − P.y) / 2 and .w = P.w,
# P being the final gl_Position: in.m_K.xy / in.m_K.w is then this fragment's position over the viewport (u from the left, v from the top, the convention of nvn_mtl.m uv_proof).
# Proven on the source: the final assignments of those components are at the function's top level after any branch, the main function returns only at its end, and every
# variable they read is assigned once, as a linear form of P's components with constant coefficients (+, -, *, / by a constant, fma). Breath of the Wild's and Tears of the
# Kingdom's G-buffer decals (095cd6ad97fd779a, bb19fab2f950e587)
_TOK = re.compile(r'\s*(?:(out\.gl_Position\.[xyzw])|(_\d+)|(\d+\.\d*(?:e[+-]?\d+)?|\d+)f?|(fma)|([-+*/(),]))')

def _lin(expr, env):
    """expr as {symbol: coefficient} ('1' the constant), or None: env(name) gives a variable's form"""
    toks, pos = [], 0
    while pos < len(expr):
        m = _TOK.match(expr, pos)
        if not m or m.end() == pos: return None
        toks.append(m.group(1) or m.group(2) or ('#' + m.group(3) if m.group(3) else None) or m.group(4) or m.group(5)); pos = m.end()
    toks = [t for t in toks if t]; i = [0]
    def add(a, b, s=1.0):
        r = dict(a)
        for k_, v in b.items(): r[k_] = r.get(k_, 0.0) + s * v
        return {k_: v for k_, v in r.items() if v}
    def const(a): return set(a) <= {'1'}
    def scale(a, c): return {k_: v * c for k_, v in a.items() if v * c}
    def peek(): return toks[i[0]] if i[0] < len(toks) else None
    def take(t=None):
        x = peek()
        if x is None or (t and x != t): raise ValueError
        i[0] += 1; return x
    def atom():
        t = take()
        if t == '-': return scale(unary(), -1.0)
        if t == '(': v = add_(); take(')'); return v
        if t == 'fma':
            take('('); a = add_(); take(','); b = add_(); take(','); c = add_(); take(')')
            return add(mul(a, b), c)
        if t.startswith('#'): return {'1': float(t[1:])} if float(t[1:]) else {}
        v = env(t)
        if v is None: raise ValueError
        return v
    def unary(): return atom()
    def mul(a, b):
        if const(a): return scale(b, a.get('1', 0.0))
        if const(b): return scale(a, b.get('1', 0.0))
        raise ValueError
    def mul_():
        v = unary()
        while peek() in ('*', '/'):
            op = take(); w = unary()
            if op == '*': v = mul(v, w)
            elif const(w) and w.get('1'): v = scale(v, 1.0 / w['1'])
            else: raise ValueError
        return v
    def add_():
        v = mul_()
        while peek() in ('+', '-'):
            op = take(); v = add(v, mul_(), 1.0 if op == '+' else -1.0)
        return v
    try:
        v = add_()
        return v if peek() is None else None
    except (ValueError, ZeroDivisionError):
        return None

def _entry_screen(body, locn):
    """The locns (out struct members → K) that are screen varyings in one entry point's body (the text between its braces)"""
    if len(re.findall(r'\breturn\b', body)) != 1 or not re.search(r'\n    return out;\n$', body + '\n'): return set()
    depth, stmts = 0, []   # (depth, text) of each statement line
    lines = [ln.strip() for ln in body.split('\n')]
    for n, s in enumerate(lines):   # spirv-cross braces every block on lines of their own: a control statement without one is not read
        if re.match(r'(if|else|for|while|do|switch)\b', s) and (n + 1 >= len(lines) or lines[n + 1] != '{'): return set()
    for s in lines:
        if s == '{': depth += 1; continue
        if s == '}': depth -= 1; continue
        if s: stmts.append((depth, s))
    decl, last = {}, {}   # variable → (statement index, expression); output component → (statement index, depth, expression)
    nassign = collections.Counter(re.findall(r'(?<![\w.])(_\d+) = ', body))
    for n, (dp, s) in enumerate(stmts):
        m = re.fullmatch(r'(?:float|int|uint|bool|half) (_\d+) = (.+);', s)
        if m: decl[m.group(1)] = (n, dp, m.group(2)); continue
        m = re.fullmatch(r'out\.(gl_Position|m_\d+)\.([xyzw]) = (.+);', s)
        if m: last[(m.group(1), m.group(2))] = (n, dp, m.group(3)); continue
        m = re.fullmatch(r'out\.(gl_Position|m_\d+) = float4\((.+)\);', s)
        if m:
            parts = [p.strip() for p in m.group(2).split(',')]
            for c, p in zip('xyzw', parts if len(parts) == 4 else [None] * 4): last[(m.group(1), c)] = (n, dp, p)
    pos = {}
    for c in 'xyw':
        p = last.get(('gl_Position', c))
        if p is None or p[1] != 0 or p[2] is None: return set()
        pos[c] = p
    sym = {p[2]: 'P' + c for c, p in pos.items() if re.fullmatch(r'_\d+', p[2])}   # a final position component that is a variable: that variable is the symbol
    def env_at(at):
        memo = {}
        def env(name):
            if name.startswith('out.gl_Position.'):
                c = name[-1]; p = pos.get(c)
                return {'P' + c: 1.0} if p and p[0] < at else None   # read after its final assignment
            if name in sym: return {sym[name]: 1.0}
            if name in memo: return memo[name]
            d = decl.get(name)
            if not d or d[0] >= at or nassign[name] != 1: return None
            memo[name] = None; memo[name] = _lin(d[2], env_at(d[0])); return memo[name]
        return env
    want = {'x': {'Px': 0.5, 'Pw': 0.5}, 'y': {'Py': -0.5, 'Pw': 0.5}, 'w': {'Pw': 1.0}}
    got = set()
    for mem, k in locn.items():
        ok = True
        for c, w in want.items():
            a = last.get((mem, c))
            if not a or a[1] != 0 or a[2] is None or _lin(a[2], env_at(a[0])) != w: ok = False; break
        if ok: got.add(k)
    return got

def screen_varyings(msl):
    """The interpolated outputs (locn numbers, sorted) of a vertex shader that are its clip → screen varying in every entry point that returns the output struct"""
    ost = re.search(r'struct main0_out\s*\{(.*?)\};', msl, re.S)
    locn = {m.group(1): int(m.group(2)) for m in re.finditer(r'float4 (m_\d+) \[\[user\(locn(\d+)\)\]\]', ost.group(1) if ost else '')}
    entries = list(re.finditer(r'\nvertex main0_out \w+\(.*?\)\n\{\n(.*?)\n\}\n', msl, re.S))
    if not locn or not entries: return []
    got = set(locn.values())
    for e in entries: got &= _entry_screen(e.group(1), locn)
    return sorted(got)

def _close(src, i, o, c):
    return dmsh_side._close(src, i, o, c)


def _output_members(msl):
    om = re.search(r'struct main0_out\s*\{(.*?)\};', msl, re.S)
    if not om:
        return None, 'no output struct'
    outs = [(t, n, int(c)) for t, n, c in re.findall(r'(\w+) (m_\d+) \[\[color\((\d+)\)\]\];', om.group(1))]
    if not outs or len(outs) > 4 or re.sub(r'\w+ m_\d+ \[\[color\(\d+\)\]\];', '', om.group(1)).strip():
        return None, 'the output struct is not plain color outputs (or has more than 4)'
    return outs, None


def _rewrites(msl, bindings):
    """Same detection as the old standalone variant: list of (start, end, replacement) and call metadata."""
    tex = {m.group(1) for m in re.finditer(r'texture2d<float> (\w+) \[\[id\(\d+\)\]\];', msl)}
    locn = {m.group(1): int(m.group(2)) for m in re.finditer(r'float4 (m_\d+) \[\[user\(locn(\d+)\)\]\];', msl.split('struct main0_in', 1)[-1].split('};', 1)[0])}
    reg = {b['msl_name']: b['reg'] for b in bindings if b.get('kind') == abi.KIND_TEX and b.get('msl_name') and b.get('reg') is not None}
    pat = re.compile(r'(dm_bs\()?spvDescriptorSet0\.(\w+)\.sample\(spvDescriptorSet0\.(\w+)Smplr, select\((_\d+), float2\(0\.0\), isnan\(\4\)\)\)'
                     r'(?(1), spvDescriptorSet0\.%s, \d+u, spvDescriptorSet0\.\2, spvDescriptorSet0\.\3Smplr%s, select\(\4, float2\(0\.0\), isnan\(\4\)\))' % (abi.BORDER_MEMBER, abi.BORDER_TWIN))
    calls, spans = [], []
    for m in pat.finditer(msl):
        member, var = m.group(2), m.group(4)
        if member not in tex or member != m.group(3) or member not in reg: continue
        d = re.search(r'float2 %s = float2\((_\d+), (_\d+)\);' % var, msl)
        a = d and re.search(r'float %s = in\.(m_\d+)\.x;' % d.group(1), msl); b = d and re.search(r'float %s = in\.(m_\d+)\.y;' % d.group(2), msl)
        if a and b and a.group(1) == b.group(1) and a.group(1) in locn: how = {'varying_locn': locn[a.group(1)]}
        else:
            pa, pb = (_proj(msl, d.group(1), 'x', locn), _proj(msl, d.group(2), 'y', locn)) if d else (None, None)
            fx, fy = (_frag_pos(msl, d.group(1), 'x'), _frag_pos(msl, d.group(2), 'y')) if d else (None, None)
            if pa and pa == pb: how = {'varying_locn': locn[pa], 'proj': 1}
            elif fx and fy: how = {'frag_pos': [fx, fy]}
            else: continue
        i = len(calls)
        if i >= 32: break
        sel = ''.join(f'FX_{i} == {k} ? fb{k} : ' for k in range(NFB))
        spans.append((m.start(), m.end(), f'({sel}{m.group(0)})'))
        calls.append({'fc': i, 'tex_reg': reg[member], **how})
    conv = {}
    for c in calls: conv[c['tex_reg']] = conv.get(c['tex_reg'], 0) + 1
    mixed = sorted({reg[mb] for mb in reg if mb in tex and msl.count(f'spvDescriptorSet0.{mb}.') > conv.get(reg[mb], 0)})
    return calls, spans, mixed


def _params(s):
    return dmsh_side._params(s)


def _parse_entry(msl, name):
    """One fragment entry, ignoring siblings (a unit's _a/_l parameters carry function constants, which dmsh_side.entries refuses)."""
    m = re.search(r'^((?:\[\[[^\]]*\]\]\s*)?)fragment\s+(\w+)\s+' + re.escape(name) + r'\s*\(', msl, re.M)
    if not m:
        return None
    attr, ret = m.group(1), m.group(2)
    head = _close(msl, m.end(), '(', ')')
    body = msl.find('{', head) if head >= 0 else -1
    end = _close(msl, body + 1, '{', '}') if body >= 0 else -1
    if end < 0:
        raise ValueError(f'no body for {name}')
    params = _params(msl[m.end():head - 1])
    decl, args = [], []
    for pr in params:
        d = re.sub(r'\s*\[\[.*\]\]$', '', pr)
        a = re.search(r'(\w+)$', d)
        if not a:
            raise ValueError(f'a parameter of {name}: {pr[:80]}')
        decl.append(d); args.append(a.group(1))
    return {'attr': attr, 'ret': ret, 'name': name, 'params': params, 'decl': ', '.join(decl), 'args': ', '.join(args),
            'start': m.start(), 'head': head, 'end': end}


def _find_calls(src, name):
    """Spans of `name(` ... `)` that are calls, not the definition."""
    out = []
    for m in re.finditer(rf'(?<![\w]){re.escape(name)}\(', src):
        head_before = src[max(0, m.start() - 120):m.start()]
        if re.search(r'static\s+(?:__attribute__\(\(always_inline\)\)\s+)?\w+\s+$', head_before):
            continue
        end = _close(src, m.end(), '(', ')')
        if end < 0:
            raise ValueError(f'unclosed call to {name}')
        out.append((m.start(), end))
    return out


def _cut_extras(msl):
    """Drop appended integer/dual entries. After fs_uint's shared-body shape the
    `struct main0_out_u` marker still sits after thin main0, so the shared
    `dm_body_main0` and main0 stay."""
    cut = msl.find('\nstruct main0_out_u\n')
    if cut >= 0:
        msl = msl[:cut + 1]
    cut = msl.find('\nstruct main0_out_d\n')
    if cut >= 0:
        msl = msl[:cut + 1]
    return msl


def _has_body(msl):
    return re.search(r'static\s+(?:__attribute__\(\(always_inline\)\)\s+)?main0_out\s+dm_body_main0\s*\(', msl)


def _consts(calls, outs):
    fx_fc = ''.join(f'constant int FX_{c["fc"]} [[function_constant({c["fc"]})]];\n' for c in calls)
    fb_fc = ''.join(f'constant bool FB_{k} [[function_constant({32 + k})]];\n' for k in range(NFB))
    om_fc = ''.join(f'constant bool OM_{j}_{k} [[function_constant({64 + 8 * j + k})]];\n' for j in range(len(outs)) for k in range(NFB))
    return fx_fc + fb_fc + om_fc


def _insert_consts(msl, consts):
    if 'using namespace metal;\n' in msl:
        return msl.replace('using namespace metal;\n', 'using namespace metal;\n\n' + consts, 1)
    return consts + msl


def _main0_f(main0, calls, outs, n):
    members = ''.join(
        f'    {t} {nm}_at{k} [[color({k}), function_constant(OM_{j}_{k})]];\n'
        for j, (t, nm, c) in enumerate(outs) for k in range(NFB)
    )
    struct_f = f'struct main0_out_f\n{{\n{members}}};\n'
    pcomma = ', ' if main0['params'] else ''
    fb_params = ', '.join(f'float4 fb{k} [[color({k}), function_constant(FB_{k})]]' for k in range(NFB))
    load_fbs = '\n'.join(f'    float4 _fb{k} = float4(0.0);\n    if (FB_{k}) _fb{k} = fb{k};' for k in range(NFB))
    if n:
        extra = ', '.join([f'FX_{i}' for i in range(n)] + [f'_fb{k}' for k in range(NFB)])
        ac = ', ' if main0['args'] else ''
        call = f"dm_body_main0({main0['args']}{ac}{extra})"
    else:
        call = f"dm_body_main0({main0['args']})"
    put = ''.join(
        f'    if (OM_{j}_{k}) out.{nm}_at{k} = tmp.{nm};\n'
        for j, (t, nm, c) in enumerate(outs) for k in range(NFB)
    )
    thin_f = (
        f"fragment main0_out_f main0_f({', '.join(main0['params'])}{pcomma}{fb_params})\n"
        "{\n"
        f"{load_fbs}\n"
        f"    main0_out tmp = {call};\n"
        "    main0_out_f out = {};\n"
        f"{put}"
        "    return out;\n"
        "}\n"
    )
    return struct_f + thin_f


def _meta(calls, mixed, outs):
    return {'calls': calls, 'mixed_regs': mixed, 'outputs': [{'color': c, 'type': t} for t, n, c in outs]}


def _fold_plain(msl, bindings, om_only_ok=False):
    """Extract main0 as dm_body_main0 (parameterized when there are samples) and append main0_f."""
    calls, spans, mixed = _rewrites(msl, bindings)
    if not calls and not om_only_ok:
        return None, 'no rewritable sample'
    outs, err = _output_members(msl)
    if err:
        return None, err
    rewritten = msl
    for start, end, repl in reversed(spans):
        rewritten = rewritten[:start] + repl + rewritten[end:]
    try:
        main0 = _parse_entry(rewritten, 'main0')
    except ValueError as e:
        return None, str(e)
    if not main0:
        return None, 'main function signature not recognized'
    n = len(calls)
    extra_decl = ', '.join([f'int FX_{i}' for i in range(n)] + [f'float4 fb{k}' for k in range(NFB)]) if n else ''
    extra_lits = ', '.join(['-1'] * n + ['float4(0.0)'] * NFB) if n else ''
    decl = main0['decl'] + ((', ' + extra_decl) if extra_decl and main0['decl'] else extra_decl)
    args = main0['args'] + ((', ' + extra_lits) if extra_lits and main0['args'] else extra_lits)
    shared = f'static __attribute__((always_inline)) main0_out dm_body_main0({decl})' + rewritten[main0['head']:main0['end']] + '\n'
    thin_main0 = (
        f'{main0["attr"]}fragment {main0["ret"]} main0({", ".join(main0["params"])})\n'
        '{\n'
        f'    return dm_body_main0({args});\n'
        '}\n'
    )
    preamble = _insert_consts(rewritten[:main0['start']], _consts(calls, outs))
    tail = rewritten[main0['end']:]
    new = preamble + shared + thin_main0 + '\n' + _main0_f(main0, calls, outs, n) + tail
    return new, _meta(calls, mixed, outs)


def _fold_unit(msl, bindings, om_only_ok=False):
    """Parameterize an existing dm_body_main0 and append main0_f. Existing callers pass literals."""
    calls, spans, mixed = _rewrites(msl, bindings)
    if not calls and not om_only_ok:
        return None, 'no rewritable sample'
    outs, err = _output_members(msl)
    if err:
        return None, err
    rewritten = msl
    for start, end, repl in reversed(spans):
        rewritten = rewritten[:start] + repl + rewritten[end:]
    n = len(calls)
    defm = _has_body(rewritten)
    if not defm:
        return None, 'dm_body_main0 disappeared after rewrite'
    if n:
        extra_decl = ', '.join([f'int FX_{i}' for i in range(n)] + [f'float4 fb{k}' for k in range(NFB)])
        extra_lits = ', '.join(['-1'] * n + ['float4(0.0)'] * NFB)
        dend = _close(rewritten, defm.end(), '(', ')')
        if dend < 0:
            return None, 'unclosed dm_body_main0 definition'
        inside = rewritten[defm.end():dend - 1].strip()
        new_inside = (inside + ', ' if inside else '') + extra_decl
        head = rewritten[defm.start():defm.end()]
        if 'always_inline' not in head:
            prefix = 'static __attribute__((always_inline)) main0_out dm_body_main0('
        else:
            prefix = head
        rewritten = rewritten[:defm.start()] + prefix + new_inside + rewritten[dend - 1:]
        for start, end in reversed(_find_calls(rewritten, 'dm_body_main0')):
            args = rewritten[start + len('dm_body_main0('):end - 1]
            new_call = f"dm_body_main0({args}{', ' if args.strip() else ''}{extra_lits})"
            rewritten = rewritten[:start] + new_call + rewritten[end:]
    rewritten = _insert_consts(rewritten, _consts(calls, outs))
    try:
        main0 = _parse_entry(rewritten, 'main0')
    except ValueError as e:
        return None, str(e)
    if not main0:
        return None, 'no main0 in unit after edit'
    rewritten = rewritten + '\n' + _main0_f(main0, calls, outs, n)
    return rewritten, _meta(calls, mixed, outs)


def fold(msl, bindings, om_only_ok=False):
    """Parameterize the one shared body and append main0_f (replaces a separate _bf compile).

    When `dm_body_main0` already exists (fs_uint and/or unit()), it gains FX/fb
    parameters and existing callers pass literals that disable fetch. Otherwise
    main0 is extracted as that body. → (new MSL, metadata) or (None, reason)
    """
    if _has_body(msl):
        return _fold_unit(msl, bindings, om_only_ok)
    return _fold_plain(msl, bindings, om_only_ok)


def variant(msl, bindings, om_only_ok=False):
    # main0_u (dmsh_build.py int_outputs: integer colour targets) is appended to the source after main0; the host takes it from the shader's own library, never from this variant.
    # After fs_uint's shared-body shape the integer entries still begin at struct main0_out_u, so the cut keeps dm_body_main0 and thin main0.
    # The folded form emits a parameterized body + main0_f wrapper instead of rewriting main0 itself (the host looks up @"main0_f" in <key>_bf.metallib).
    return fold(_cut_extras(msl), bindings, om_only_ok)
