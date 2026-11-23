# A pixel shader's alpha test and logic operation entry points from one Metal compile (GitHub issue #1036). Every pixel shader needs them, since which shaders draw with either shows only in play,
# and few ever do: compiling each as a library of its own was 57% of Pokopia's 366,805 compiles. So the compile unit holds each entry point's body once, as a static function, with thin
# entry points around it: main0, and the <entry>_a (dmsh_alpha.py) and <entry>_l (dmsh_logic.py) that test or combine what the body returned; the typed-output entry point
# main0_u (dmsh_entry.fs_uint) holds both under function constants, and main0 has no _l beside it.
# A pixel shader then costs about what its base alone cost: 34-36% of the Metal CPU of three compiles, on the Mac's flags and the iPhone's, over 125 shaders of six games (dm/games/0400A3A01EE4C000/README.md).
# metallib gives every entry point a complete module, whether or not the body is shared, and two things in the host's path grow with what one file holds: metal-tt compiles every unspecialized
# entry point of a library, so a base library with them made the pipeline archive twice as large, and the base library is what every shader loads. So the compile's one library is
# cut in two: <key>_b with the plain entry points, <key>_bx with the others and the typed-output entry point main0_u, which the host loads on first need (nvn_mtl.m at_fn, lo_fn, typed_fn). <key>_b.metal stays the plain source, which is
# what every other tool reads. cut() takes functions out of the file as `metallib --function-name` does (byte-identical for one function), and unlike that option takes several, in groups:
# the readable-tile version main0_f (dmsh_fetch.fold) is a third library, <key>_bf, of the same compile. The Metal cache keeps the cut libraries, not the whole one: a change to what cut()
# gives for the same groups changes what the cache holds, so it needs a new dmsh_mtlcache.FORMAT.
# Usage: unit(msl) → (the compile unit, {'alpha': entry point names, 'logic': entry point names}), or (None, {'why': why}); cut(library bytes, names of each further library's functions...)
# → (base bytes, each further library's bytes...); shared_body(msl) → msl with main0's body as dm_body_main0 (None on an unknown shape), calling(e, name, ret, tail) → an entry point calling it
#
# A vertex shader's libraries come from one Metal compile too (vs_unit). Its body was optimized nine times in two compiles, twelve in three with integer inputs: <key>_b held it in main0,
# main0_z and main0_gs, the pulling library <key>_bp (dmsh_pull.py) in main0, main0_z and twice in each wide-line entry, the shadow-input library <key>_bv (dmsh_vin.py) in three more.
# The unit holds the body once, as dm_vs_body, and is cut into the same three libraries:
#   · always_inline in the entry points pipelines are made of: main0 and main0_z, their pulling versions main0_p / main0_zp and their shadow-input versions main0_v / main0_zv (one
#     unit cannot name them all main0: the reflection's `pull` and `vin` give the names, which the host and pso_offline.py read). The optimizer then sees in each what it saw
#     before, the copy of the body, so the GPU gets the code it got: every output compared on the GPU was bit-identical (the libraries are not byte-identical: main0 applies the
#     depth line once, after the call, where the copy had it before every return);
#   · one out-of-line copy, dm_vs_body_o, for the entry points few draws take: the wide-line entries main0_wl / main0_z_wl, which ran the body twice each, and the capture entry
#     main0_gs (dmsh_geometry.capture), which calls it and writes its capture epilogue once, where the copy had the epilogue at every return. A plain static function, never
#     noinline: marked noinline it changed main0_gs's captured words in 24 of 52 shaders tried (up to 12,544 units in the last place);
#   · the rarer entry points (main0_discard, main0_tf, the layer ones) stay whole copies.
# docs/reference/shaders.md, One compile for a vertex shader's libraries, has the measurements.
# Usage: vs_unit(source with every entry point, dmsh_pull.variant's source or None, dmsh_vin.variant's source or None) → (the compile unit, the reflection's names of the pulling
# entry points {'entry', 'entry_z', 'entry_wl', 'entry_z_wl'}, of the shadow-input ones {'entry', 'entry_z'}); raises ValueError on a source of another shape
import re, struct
import dmsh_alpha, dmsh_logic, dmsh_pull

_ENTRY = re.compile(r'^((?:\[\[[^\]]*\]\]\s*)?)fragment\s+(\w+)\s+(\w+)\s*\(', re.M)   # an attribute may come first on the line ([[ early_fragment_tests ]])


def _close(src, i, o, c):
    """the index after the bracket closing the one opened just before i, -1 when there is none"""
    depth = 1
    for m in re.compile(re.escape(o) + '|' + re.escape(c)).finditer(src, i):   # from bracket to bracket: a loop over the characters of a whole function body took 10 ms a shader
        depth += 1 if m.group() == o else -1
        if not depth: return m.end()
    return -1


def _params(s):   # a parameter list's parameters: commas inside <>, () or [] do not separate
    out, at, depth = [], 0, 0
    for i, ch in enumerate(s):
        depth += (ch in '<([') - (ch in '>)]')
        if ch == ',' and not depth: out.append(s[at:i].strip()); at = i + 1
    return out + ([s[at:].strip()] if s[at:].strip() else [])


def _entry(msl, m):
    attr, ret, name = m.groups()
    head = _close(msl, m.end(), '(', ')')
    body = msl.find('{', head) if head >= 0 else -1
    end = _close(msl, body + 1, '{', '}') if body >= 0 else -1
    if end < 0: raise ValueError(f'no body for {name}')
    params = _params(msl[m.end():head - 1]); decl, args = [], []
    for p in params:
        d = re.sub(r'\s*\[\[.*\]\]$', '', p); a = re.search(r'(\w+)$', d)
        if not a or 'function_constant' in p: raise ValueError(f'a parameter of {name}: {p[:80]}')
        decl.append(d); args.append(a.group(1))
    return {'attr': attr, 'ret': ret, 'name': name, 'params': params, 'decl': ', '.join(decl), 'args': ', '.join(args), 'start': m.start(), 'head': head, 'end': end}


def entries(msl, skip=()):
    """The fragment entry points in source order, but those named in skip: attr (the text before `fragment`), ret, name, params, decl (the parameters without attributes), args, start (of
    the entry point), head (after its parameter list), end (after its body). Raises ValueError on a shape it does not know"""
    return [_entry(msl, m) for m in _ENTRY.finditer(msl) if m.group(3) not in skip]


def entry(msl, name):   # one entry point as entries() gives it, None when there is none; the others are not read (a unit's _a and _l take function constants)
    return next((_entry(msl, m) for m in _ENTRY.finditer(msl) if m.group(3) == name), None)


# main0's body once, as a function the entry points that return what it computes call: main0 itself, main0_u (dmsh_entry.py fs_uint) and main0_f (dmsh_fetch.fold).
# Each of them used to be a copy of the body, which the compiler built again for each. always_inline: a caller that passes constants (main0's literals for main0_f's tile read) gets them folded,
# and each entry point is a module of its own in the library anyway (metallib)
_BODY = re.compile(r'static (__attribute__\(\(always_inline\)\) )?main0_out dm_body_main0\(')
BODY = 'static __attribute__((always_inline)) main0_out dm_body_main0('


def body(msl):   # dm_body_main0's definition: (its start, the end of its parameter list, the end of its body), None when there is none
    m = _BODY.search(msl)
    if not m: return None
    head = _close(msl, m.end(), '(', ')'); end = _close(msl, msl.find('{', head) + 1, '{', '}')
    return (m.start(), head, end) if head >= 0 and end >= 0 else None


def shared_body(msl):
    """msl with main0's body as dm_body_main0 and main0 calling it (unit's static body made always_inline), unchanged when it has that already; None when main0 is not of a shape entries()
    knows"""
    m = _BODY.search(msl)
    if m: return msl if m.group(1) else msl[:m.start()] + BODY + msl[m.end():]
    try: e = entry(msl, 'main0')
    except ValueError: return None
    if not e or e['ret'] != 'main0_out': return None
    return msl[:e['start']] + f'{BODY}{e["decl"]})' + msl[e['head']:e['end']] + '\n' + calling(e, 'main0', 'main0_out', 'return out;') + msl[e['end']:]


def calling(e, name, ret, tail):   # entry point name, with main0's (e) attributes and parameters, returning ret: the body's result as out, then tail
    return f'{e["attr"]}fragment {ret} {name}({", ".join(e["params"])})\n{{\n    main0_out out = dm_body_main0({e["args"]});\n    {tail}\n}}\n'


def unit(msl) -> tuple[str | None, dict]:
    # main0_u (dmsh_entry.fs_uint) holds its own alpha test and logic operation under function constants, and takes arguments by them: it stays as it is, and main0 gets no logic operation
    # entry point beside it (that one needed a specialization anyway). main0's alpha test entry point stays: it has no constant, so the alpha-tested draws of the common case take it with no
    # specialization at run time (nvn_mtl.m at_fn)
    typed = bool(re.search(r'\bfragment\s+main0_out_u\s+main0_u\s*\(', msl))
    try: es = entries(msl, skip=('main0_u',))
    except ValueError as e: return None, {'why': str(e)}
    if not es or es[0]['name'] != 'main0': return None, {'why': 'no fragment entry point main0'}
    no_alpha, no_logic = dmsh_alpha.refused(msl), dmsh_logic.refused(msl)
    alpha = {e['name'] + '_a': dmsh_alpha.entry(msl, e) for e in es} if not no_alpha else {}
    if not all(alpha.values()): alpha, no_alpha = {}, 'an entry point without a colour 0'   # the host needs one for each of the shader's own (at_fn)
    logic = {e['name'] + '_l': o for e in es if not (typed and e['name'] == 'main0') and (o := None if no_logic else dmsh_logic.entry(msl, e))}
    if not alpha and not logic: return None, {'why': no_alpha or no_logic or 'no colour output'}
    thin = [a for a in alpha.values() if a] + [o[0] for o in logic.values()]; targets = set().union(*(o[1] for o in logic.values()))
    src, at = [], es[0]['start']
    shared = bool(_BODY.search(msl))   # main0 already calls dm_body_main0 (shared_body): it is made again below as it is
    for e in es:
        src += [msl[at:e['start']]] + ([] if shared and e['name'] == 'main0' else [f'static {e["ret"]} dm_body_{e["name"]}({e["decl"]})', msl[e['head']:e['end']]]); at = e['end']
    plain = [f'{e["attr"]}fragment {e["ret"]} {e["name"]}({", ".join(e["params"])})\n{{\n    return dm_body_{e["name"]}({e["args"]});\n}}\n' for e in es]
    helpers = ('' if 'struct dm_alpha_state ' in msl else dmsh_alpha.HELPER) + ('' if 'struct dm_logic_state ' in msl else dmsh_logic.HELPER)   # main0_u brings them when it uses them
    return msl[:es[0]['start']] + helpers + dmsh_logic.constants(targets) + ''.join(src) + msl[at:] + '\n' + ''.join(plain + thin), {'alpha': list(alpha), 'logic': list(logic)}


_VENTRY = re.compile(r'^vertex (\S+) (\w+)\((.*)\)\n\{\n', re.M)   # a vertex entry point as the translator and the chain's tools write it: its header on one line
_VEND = re.compile(r'\n\}(?:\n|$)')
VS_DEPTH = 'out.gl_Position.z = fma(out.gl_Position.z, 0.5, out.gl_Position.w * 0.5);'   # the line dmsh_entry.vs_depth puts before every `return out;` of main0
VS_BODY = 'static __attribute__((always_inline)) main0_out dm_vs_body('
VS_BODY_OUT = 'static main0_out dm_vs_body_o('   # the out-of-line copy: no noinline (see above)
VS_PULLED = {'main0': 'main0_p', 'main0_z': 'main0_zp'}   # the pulling entry points' names in the unit, where main0 and main0_z are the shader's own
VS_SHADOW = {'main0': 'main0_v', 'main0_z': 'main0_zv'}   # the shadow-input ones


def vs_entries(src):
    """{name: ret, params (the text between its parentheses), start, body (after its opening brace's line), end (after its closing brace's line)} of the vertex entry points. Raises
    ValueError when one has no end"""
    out = {}
    for m in _VENTRY.finditer(src):
        e = _VEND.search(src, m.end() - 1)
        if not e: raise ValueError(f'no end of {m[2]}')
        out[m[2]] = {'ret': m[1], 'params': m[3], 'start': m.start(), 'body': m.end(), 'end': e.end()}
    return out


def vs_unit(full, pulled=None, shadowed=None):
    es = vs_entries(full)
    if 'main0' not in es or 'main0_z' not in es: raise ValueError('no main0 and main0_z')
    e0, ez = es['main0'], es['main0_z']
    if e0['end'] > ez['start'] or (e0['ret'], ez['ret']) != ('main0_out', 'main0_out') or e0['params'] != ez['params']: raise ValueError('main0 and main0_z differ in more than their bodies')
    gl, z = full[e0['body']:e0['end']], full[ez['body']:ez['end']]   # the body as main0 has it, with the depth line, and as main0_z has it
    if 'return out;' not in z or gl != z.replace('return out;', VS_DEPTH + '\n    return out;'): raise ValueError('main0 is not main0_z with the depth line')
    taken = [n for n in (*VS_PULLED.values(), *VS_SHADOW.values(), 'main0_wl', 'main0_z_wl') if n in es] + [n for n in ('dm_vs_body', 'dm_wl_in') if n + '(' in full]
    if taken: raise ValueError(f'the source has a {taken[0]} already')
    decl, args = [], []
    for p in _params(e0['params']):
        d = re.sub(r'\s*\[\[.*\]\]$', '', p); a = re.search(r'(\w+)$', d)
        if not a or 'function_constant' in p: raise ValueError(f'a parameter of main0: {p[:80]}')
        decl.append(d); args.append(a.group(1))
    decl, args = ', '.join(decl), ', '.join(args)
    def calling(text, fn, n_gl, n_z):   # text with its copies of the body as calls of fn; n_gl, n_z: how many copies of each form it must hold
        if (text.count(gl), text.count(z)) != (n_gl, n_z): raise ValueError(f'{text.count(gl)} and {text.count(z)} copies of the body where {int(n_gl)} and {int(n_z)} belong')
        return text.replace(gl, f'    main0_out out = {fn}({args});\n    {VS_DEPTH}\n    return out;\n}}\n').replace(z, f'    return {fn}({args});\n}}\n')
    def renamed(variant, names):   # a variant's main0 / main0_z under the unit's names: each keeps its prologue (the inputs pulled, or the shadows ORed in) and calls the body
        ve, text, got = vs_entries(variant), '', {}
        if 'main0' not in ve: raise ValueError('a variant without main0')
        for n, new in names.items():
            if n not in ve: continue
            one = variant[ve[n]['start']:ve[n]['end']]
            if not one.startswith(f'vertex main0_out {n}('): raise ValueError(f'the variant\'s {n} does not return main0_out')
            text += '\n' + calling(f'vertex main0_out {new}(' + one[len(f'vertex main0_out {n}('):], 'dm_vs_body', n == 'main0', n == 'main0_z'); got['entry' if n == 'main0' else 'entry_z'] = new
        return text, got
    wide = bool(pulled) and '\nstatic inline main0_in dm_wl_in(' in pulled
    eg = es.get('main0_gs'); rest = full[ez['end']:]; gs_call = False   # what follows main0_z: main0_gs, then the rarer entry points, which stay as they are
    if eg:   # the capture entry (dmsh_geometry.capture): its prologue, then the body with the capture epilogue where each `return out;` was → the prologue, the body's call, the epilogue once
        gs = full[eg['body']:eg['end']]; epi = re.search(r'\{ dm_cw\(.*? return; \}', gs)
        params_ok = not e0['params'] or eg['params'] == e0['params'] or eg['params'].startswith(e0['params'] + ', ')
        if not epi or eg['start'] < ez['end'] or eg['ret'] != 'void' or not params_ok: raise ValueError('main0_gs is not the body with the capture epilogue')
        copy = z.replace('return out;', epi[0])
        if gs.endswith(copy):
            gs_call = True
            rest = full[ez['end']:eg['body']] + gs[:-len(copy)] + f'    main0_out out = dm_vs_body_o({args});\n    {epi[0]}\n}}\n' + full[eg['end']:]
        elif 'dm_vertex_viewport_mask' in z and gs.endswith(re.sub(r'\{ if \(\(dm_vertex_viewport_mask & 1u\) == 0u\) out\.\w+ = float4\(2\.0, 0\.0, 0\.0, 1\.0\); return out; \}', lambda _: epi[0], z)):
            pass   # vs_viewport_mask culls raster entries only: main0_gs stays a whole unculled copy
        else: raise ValueError('main0_gs is not the body with the capture epilogue')
    head = full[:e0['start']]
    if pulled:   # the layout block and its fetch, where dmsh_pull.variant puts them
        ui = head.find('using namespace metal;')
        if ui < 0: raise ValueError('no "using namespace metal;"')
        ui += len('using namespace metal;'); head = head[:ui] + '\n\n' + dmsh_pull.FETCH + (dmsh_pull.WL_MSL if wide else '') + head[ui:]
    src = (head + f'{VS_BODY}{decl})\n{{\n' + z + '\n' + (f'{VS_BODY_OUT}{decl})\n{{\n    return dm_vs_body({args});\n}}\n\n' if wide or gs_call else '')
           + calling(full[e0['start']:ez['end']], 'dm_vs_body', 1, 1) + rest)   # main0 and main0_z
    pn, vn = {}, {}
    if pulled:
        text, pn = renamed(pulled, VS_PULLED); src += text
        if wide:   # dm_wl_in, dm_wl_mix, then each entry's body function dm_wlb_<entry> and its wide-line entry <entry>_wl: the pulling variant's last text
            wl = pulled[pulled.index('\nstatic inline main0_in dm_wl_in('):]
            pn.update({k: n + '_wl' for n, k in (('main0', 'entry_wl'), ('main0_z', 'entry_z_wl')) if f'\nvertex main0_out {n}_wl(' in wl})
            src += calling(wl, 'dm_vs_body_o', 'entry_wl' in pn, 'entry_z_wl' in pn)
    if shadowed:
        st = re.search(r'\nstruct main0_in_v\n\{\n.*?\n\};', shadowed, re.S)
        if not st: raise ValueError('a shadow-input variant without main0_in_v')
        text, vn = renamed(shadowed, VS_SHADOW); src += '\n' + st[0] + '\n' + text
    return src, pn, vn


def _tags(b, p, end):   # the tag list at p as [(tag, value)]
    out = []
    while p + 4 <= end:
        tag = b[p:p + 4]; p += 4
        if tag == b'ENDT': return out
        n = struct.unpack_from('<H', b, p)[0]; p += 2; out.append((tag, b[p:p + n])); p += n
    raise ValueError('an unterminated tag list')


def _sized(b, p, n):   # n entries from p, each its size (not counting the size field), then that many bytes → their bytes, with the size fields, and the end
    out = []
    for _ in range(n):
        size = struct.unpack_from('<I', b, p)[0]; out.append(b[p:p + 4 + size]); p += 4 + size
    return out, p


def _take(b, keep, fns, tail, sections):
    """The library of functions keep (indices): the same header, function list and sections as the file, with the others left out"""
    fo, fs, po, ps, qo, qs, bo, bs = sections
    pub, _ = _sized(b, po, len(fns)); priv, _ = _sized(b, qo, len(fns))
    entries_, newpub, newpriv, code = [], b'', b'', b''
    for i in keep:
        t = dict(fns[i]); offs = struct.unpack('<QQQ', t[b'OFFT']); size = struct.unpack('<Q', t[b'MDSZ'])[0]
        body = b''.join(tag + struct.pack('<H', 24 if tag == b'OFFT' else len(v)) + (struct.pack('<QQQ', len(newpub), len(newpriv), len(code)) if tag == b'OFFT' else v) for tag, v in fns[i]) + b'ENDT'
        entries_.append(struct.pack('<I', len(body) + 4) + body)
        newpub += pub[i]; newpriv += priv[i]; code += b[bo + offs[2]:bo + offs[2] + size]
    listing = struct.pack('<I', len(keep)) + b''.join(entries_)
    all_entries = po - len(tail) - fo - 4
    nfo, npo = fo, fo + len(listing) + len(tail); nqo = npo + len(newpub); nbo = nqo + len(newpriv)
    head = bytearray(b[:fo])
    struct.pack_into('<Q', head, 0x10, nbo + len(code))   # the file's size
    struct.pack_into('<QQQQQQQQ', head, 0x18, nfo, fs - all_entries + len(listing) - 4, npo, len(newpub), nqo, len(newpriv), nbo, len(code))
    return bytes(head) + listing + tail + newpub + newpriv + code


def cut(lib, *groups):
    """(the library without the functions named in groups, then the library of each group's), from the layout pso_offline.functions reads: cut(lib, side) gives the base and the side library,
    cut(lib, side, {'main0_f'}) the readable-tile one too. Raises ValueError on a file it does not know, a name it does not hold or a name in two groups"""
    try:
        if lib[:4] != b'MTLB' or struct.unpack_from('<Q', lib, 0x10)[0] != len(lib): raise ValueError('not a whole Metal library')
        sections = struct.unpack_from('<QQQQQQQQ', lib, 0x18); fo = sections[0]; po = sections[2]; bo, bs = sections[6:8]
        if bo + bs != len(lib): raise ValueError('its bitcode is not last')
        n = struct.unpack_from('<I', lib, fo)[0]; p = fo + 4; fns, names = [], []
        for _ in range(n):
            size = struct.unpack_from('<I', lib, p)[0]; t = _tags(lib, p + 4, p + size); fns.append(t); names.append(dict(t)[b'NAME'].rstrip(b'\0').decode()); p += size
        groups = [set(g) for g in groups]; named = set().union(*groups)
        missing = named - set(names)
        if missing: raise ValueError(f'no function {sorted(missing)[0]}')
        if sum(map(len, groups)) != len(named): raise ValueError('a function in two groups')
        tail = lib[p:po]   # what lies between the function list and the public metadata stays
        return tuple(_take(lib, [i for i, x in enumerate(names) if keep(x)], fns, tail, sections) for keep in [lambda x: x not in named] + [g.__contains__ for g in groups])
    except (struct.error, KeyError) as e: raise ValueError(f'an unreadable library ({e})') from e
