# A pixel shader's alpha test and logic operation entry points from one Metal compile (GitHub issue #1036). Every pixel shader needs them, since which shaders draw with either shows only in play,
# and few ever do: compiling each as a library of its own was 57% of Pokopia's 366,805 compiles. So the compile unit holds each entry point's body once, as a static function, with thin
# entry points around it: main0 (and main0_u / main0_m / main0_d), and the <entry>_a (dmsh_alpha.py) and <entry>_l (dmsh_logic.py) that test or combine what the body returned.
# A pixel shader then costs about what its base alone cost: 34-36% of the Metal CPU of three compiles, on the Mac's flags and the iPhone's, over 125 shaders of six games (dm/games/0400A3A01EE4C000/README.md).
# metallib gives every entry point a complete module, whether or not the body is shared, and two things in the host's path grow with what one file holds: metal-tt compiles every unspecialized
# entry point of a library, so a base library with them made the pipeline archive twice as large, and the base library is what every shader loads. So the compile's one library is
# cut in two: <key>_b with the plain entry points, <key>_bx with the others, which the host loads on first need (nvn_mtl.m at_fn, lo_fn). <key>_b.metal stays the plain source, which is
# what every other tool reads. cut() takes functions out of the file as `metallib --function-name` does (byte-identical for one function), and unlike that option takes several. The Metal cache
# keeps the cut libraries, not the whole one: a change to cut() changes what the cache holds, so it needs a new dmsh_mtlcache.FORMAT.
# Usage: unit(msl) → (the compile unit, {'alpha': entry point names, 'logic': entry point names}), or (None, {'why': why}); cut(library bytes, names of the side library's functions) → (base bytes, side bytes)
import re, struct
import dmsh_alpha, dmsh_logic

_ENTRY = re.compile(r'^((?:\[\[[^\]]*\]\]\s*)?)fragment\s+(\w+)\s+(\w+)\s*\(', re.M)   # an attribute may come first on the line ([[ early_fragment_tests ]])


def _close(src, i, o, c):
    """the index after the bracket closing the one opened just before i, -1 when there is none"""
    depth = 1
    while depth and i < len(src): depth += (src[i] == o) - (src[i] == c); i += 1
    return i if not depth else -1


def _params(s):   # a parameter list's parameters: commas inside <>, () or [] do not separate
    out, at, depth = [], 0, 0
    for i, ch in enumerate(s):
        depth += (ch in '<([') - (ch in '>)]')
        if ch == ',' and not depth: out.append(s[at:i].strip()); at = i + 1
    return out + ([s[at:].strip()] if s[at:].strip() else [])


def entries(msl):
    """The fragment entry points in source order: attr (the text before `fragment`), ret, name, params, decl (the parameters without attributes), args, start (of the entry point), head (after its
    parameter list), end (after its body). Raises ValueError on a shape it does not know"""
    out = []
    for m in _ENTRY.finditer(msl):
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
        out.append({'attr': attr, 'ret': ret, 'name': name, 'params': params, 'decl': ', '.join(decl), 'args': ', '.join(args), 'start': m.start(), 'head': head, 'end': end})
    return out


def unit(msl) -> tuple[str | None, dict]:
    try: es = entries(msl)
    except ValueError as e: return None, {'why': str(e)}
    if not es or es[0]['name'] != 'main0': return None, {'why': 'no fragment entry point main0'}
    no_alpha, no_logic = dmsh_alpha.refused(msl), dmsh_logic.refused(msl)
    alpha = {e['name'] + '_a': dmsh_alpha.entry(msl, e) for e in es} if not no_alpha else {}
    if not all(alpha.values()): alpha, no_alpha = {}, 'an entry point without a colour 0'   # the host needs one for each of the shader's own (at_fn)
    logic = {e['name'] + '_l': o for e in es if (o := None if no_logic else dmsh_logic.entry(msl, e))}
    if not alpha and not logic: return None, {'why': no_alpha or no_logic or 'no colour output'}
    thin = [a for a in alpha.values() if a] + [o[0] for o in logic.values()]; targets = set().union(*(o[1] for o in logic.values()))
    src, at = [], es[0]['start']
    for e in es:
        src += [msl[at:e['start']], f'static {e["ret"]} dm_body_{e["name"]}({e["decl"]})', msl[e['head']:e['end']]]; at = e['end']
    plain = [f'{e["attr"]}fragment {e["ret"]} {e["name"]}({", ".join(e["params"])})\n{{\n    return dm_body_{e["name"]}({e["args"]});\n}}\n' for e in es]
    return msl[:es[0]['start']] + dmsh_alpha.HELPER + dmsh_logic.HELPER + dmsh_logic.constants(targets) + ''.join(src) + msl[at:] + '\n' + ''.join(plain + thin), {'alpha': list(alpha), 'logic': list(logic)}


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


def cut(lib, side):
    """(the library without the functions named in side, the library of those), from the layout pso_offline.functions reads. Raises ValueError on a file it does not know"""
    try:
        if lib[:4] != b'MTLB' or struct.unpack_from('<Q', lib, 0x10)[0] != len(lib): raise ValueError('not a whole Metal library')
        sections = struct.unpack_from('<QQQQQQQQ', lib, 0x18); fo = sections[0]; po = sections[2]; bo, bs = sections[6:8]
        if bo + bs != len(lib): raise ValueError('its bitcode is not last')
        n = struct.unpack_from('<I', lib, fo)[0]; p = fo + 4; fns, names = [], []
        for _ in range(n):
            size = struct.unpack_from('<I', lib, p)[0]; t = _tags(lib, p + 4, p + size); fns.append(t); names.append(dict(t)[b'NAME'].rstrip(b'\0').decode()); p += size
        missing = set(side) - set(names)
        if missing: raise ValueError(f'no function {sorted(missing)[0]}')
        tail = lib[p:po]   # what lies between the function list and the public metadata stays
        return tuple(_take(lib, keep, fns, tail, sections) for keep in ([i for i, x in enumerate(names) if x not in side], [i for i, x in enumerate(names) if x in side]))
    except (struct.error, KeyError) as e: raise ValueError(f'an unreadable library ({e})') from e
