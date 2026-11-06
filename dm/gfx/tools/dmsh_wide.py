"""Storage-buffer reads the translator split into words, read whole again (wide_loads, a step of dmsh_build.py).

Ampere reads a storage buffer 64 or 128 bits at a time (LDG.E.64 / .128); the translator splits each into 32-bit LoadGlobal operations, and spirv-cross writes
every word with an address of its own:

    int _3487 = _3483 + 32;  uint _3499 = uint(int(uint(_3487) >> uint(2)));  float _3501 = as_type<float>((*spvDescriptorSet0.fp_s3)._m0[int(_3499)]);
    int _3503 = _3487 + 4;   uint _3505 = uint(int(uint(_3503) >> uint(2)));  float _3507 = as_type<float>((*spvDescriptorSet0.fp_s3)._m0[int(_3505)]);

Metal's front end cannot tell that such words are neighbours (each index is the shift of its own sum, then widened), so they stay separate loads, each with its
own address arithmetic. Splatoon Raiders' material shader (Hoian_UBER) reads its lights this way, 80 bytes a light in a loop over the lights, about 240 words in
all: on an M1 the scene's lit pass took 70 ms, 16 ms with the words read as below (median GPU time of the pass, the opening dialogue scene).

Here the words of one read-only buffer (const device) at byte offsets k, k+4, ... from one value, with no brace between them (one block of straight-line code),
become one read of a packed_uint2/3/4 at the first of them, and each word reads its lane. A packed vector needs only 4-byte alignment, so it may sit wherever
the words could; the shader does not write the buffer through its own binding, and a run ends at any line that may write memory or change a value (a store
through a subscript or a pointer, an atomic, a barrier, an image write, an assignment to an existing variable), so a write through another binding to the
same memory, or a common value that changes, cannot fall between its words: reading the later words earlier gives the same values. A run starts on a 16-byte
step of the common value and holds at most four words, as the original wide reads did; a run whose lowest word is not read first is left alone.
"""
import re

_IDENT = r'_[0-9A-Za-z_]+'
_BUF = r'[a-z]p_s\d+'   # storage buffer bindings: vp_ / fp_ / cp_ / gp_ ... s<n>
_ADD = re.compile(r'^\s*int (' + _IDENT + r') = (' + _IDENT + r') \+ (-?\d+);\s*$')
_IDX = re.compile(r'^\s*uint (' + _IDENT + r') = uint\(int\(uint\((' + _IDENT + r')\) >> uint\(2\)\)\);\s*$')
_LOAD = re.compile(r'\(\*spvDescriptorSet0\.(' + _BUF + r')\)\._m0\[int\((' + _IDENT + r')\)\]')
_RO = re.compile(r'^\s*const device ' + _IDENT + r'\* (' + _BUF + r') \[\[id\(\d+\)\]\];', re.M)
_OP = r'(?:[-+*/%|&^]|<<|>>)?=(?!=)'
_WRITE = re.compile(r'atomic_|_barrier\(|\.write\(|\]\s*' + _OP + r'|^\s*\*|^\s*' + _IDENT + r'\s*' + _OP)   # lines a run does not reach across
_VEC = {2: 'packed_uint2', 3: 'packed_uint3', 4: 'packed_uint4'}


def _root(v, adds):   # (value, byte offset) the sums lead back to
    off = 0
    for _ in range(64):
        if v not in adds: break
        v, k = adds[v]; off += k
    return v, off


def wide_loads(msl):
    if ' >> uint(2)' not in msl: return msl
    ro = set(_RO.findall(msl))
    if not ro: return msl
    lines = msl.split('\n')
    adds, idx = {}, {}
    for ln in lines:
        m = _ADD.match(ln)
        if m: adds[m.group(1)] = (m.group(2), int(m.group(3))); continue
        m = _IDX.match(ln)
        if m: idx[m.group(1)] = m.group(2)
    words, block = {}, 0   # (block, buffer, value) → {byte offset: (line, match)}
    for i, ln in enumerate(lines):
        if '{' in ln or '}' in ln or _WRITE.search(ln): block += 1; continue
        ms = list(_LOAD.finditer(ln))
        if len(ms) != 1 or ms[0].group(1) not in ro or ms[0].group(2) not in idx: continue
        v, off = _root(idx[ms[0].group(2)], adds)
        if off % 4 == 0: words.setdefault((block, ms[0].group(1), v), {}).setdefault(off, (i, ms[0]))
    edits, n = {}, 0
    for (_, buf, _), by_off in words.items():
        for base in sorted({off - off % 16 for off in by_off}):
            run = []
            while len(run) < 4 and base + 4 * len(run) in by_off: run.append(by_off[base + 4 * len(run)])
            if len(run) < 2 or any(i < run[0][0] for i, _ in run): continue
            name, vec = f'_dmw{n}', _VEC[len(run)]; n += 1
            decl = f'{vec} {name} = *(const device {vec}*)&(*spvDescriptorSet0.{buf})._m0[int({run[0][1].group(2)})];'
            for w, (i, m) in enumerate(run): edits.setdefault(i, []).append((m.start(), m.end(), f'{name}.{"xyzw"[w]}', decl if w == 0 else None))
    for i, es in edits.items():
        ln, pre = lines[i], []
        for s, e, rep, decl in sorted(es, reverse=True):
            ln = ln[:s] + rep + ln[e:]
            if decl: pre.append(decl)
        indent = lines[i][:len(lines[i]) - len(lines[i].lstrip())]
        lines[i] = ''.join(indent + d + '\n' for d in pre) + ln
    return '\n'.join(lines)


# Raw global memory that reuses the range its last lookup found (raw_ranges, the last step before dmsh_build.py compiles a shader).
#
# A word whose address the translator traced to no storage buffer goes through a helper GlobalToStorage.TryGenerateRawStorageOp writes (spirv-cross keeps it a function):
#
#     static inline __attribute__((always_inline))
#     int _56(thread const int& lo, thread const int& hi, const device _27& cp_s3)
#     {
#         ... the hint of the address's 2 MiB chunk ...      int sel = hint_index;
#         if (!hint_fits) { ... the unrolled binary search ...  sel = index; }
#         int row = sel * 8;  ... the end check, the delta, the read ...
#     }
#
# called once for each 32-bit word, and a shader reads a structure word after word from one range. Ocarina of Time's foliage mesh shader (73dc0b90c4082e43, 135 such reads)
# took 10.9 ms of a 683x384 frame. Each call site gets the cache of the function it is in, a range {start, end, entry}: an address inside the range the last lookup chose takes
# that entry without the hint or the search; everything after (the end check, the delta, the read or write) is unchanged. The GMEM map's ranges are disjoint (nvn_mtl.m gmem
# map), so an address inside an entry's range is that entry's in the search too, and a cache lives in one invocation, so the table cannot change under it.
_RAW_HELPER = re.compile(r'(?m)^static inline __attribute__\(\(always_inline\)\)\nint (_\d+)\(thread const int& (_\d+), thread const int& (_\d+), '
                         r'((?:const )?device _\d+& ([a-z]p_s\d+))\)\n\{\n')
_RAW_CACHE = 'struct dm_raw_range { ulong s; ulong e; int i; };   // the range the last raw global memory lookup chose (dmsh_wide.py raw_ranges)\n\n'


def _raw_helper(msl, m):
    """The helper with its search behind the cache, and where it ends; None when it is not the shape above."""
    name, lo, hi, param, table = m.groups()
    end = msl.index('\n}\n', m.end())
    body = msl[m.end():end].split('\n')
    i = next((k for k in range(len(body) - 2) if re.fullmatch(r'    int _\d+ = _\d+;', body[k]) and re.fullmatch(r'    if \(!_\d+\)', body[k + 1])
              and body[k + 2] == '    {'), None)
    if i is None: return None
    sel, hint = re.fullmatch(r'    int (_\d+) = (_\d+);', body[i]).groups()
    close = next((k for k in range(i + 3, len(body)) if body[k] == '    }'), None)
    if close is None or close + 1 >= len(body) or not re.fullmatch(r'    int _\d+ = ' + sel + r' \* 8;', body[close + 1]): return None
    new = ([f'    ulong _ra = as_type<ulong>(uint2(uint({lo}), uint({hi})));', f'    int {sel};',
            f'    if (_ra - _rr.s < _rr.e - _rr.s) {sel} = _rr.i;', '    else', '    {']
           + ['    ' + ln for ln in body[:i]] + [f'        {sel} = {hint};'] + ['    ' + ln for ln in body[i + 1:close + 1]]
           + [f'        int _rw = {sel} * 8;',
              f'        _rr.s = as_type<ulong>(uint2({table}._m0[_rw], {table}._m0[_rw + 1])); _rr.e = as_type<ulong>(uint2({table}._m0[_rw + 2], {table}._m0[_rw + 3])); _rr.i = {sel};',
              '    }'] + body[close + 1:])
    head = msl[m.start():m.end()].replace(f'{param})\n{{\n', f'{param}, thread dm_raw_range& _rr)\n{{\n')
    return name, head + '\n'.join(new), end


def _close_paren(s, i):
    depth = 0
    for j in range(i, len(s)):
        depth += (s[j] == '(') - (s[j] == ')')
        if depth == 0: return j
    raise ValueError('unbalanced parentheses')


def raw_ranges(msl):
    if 'reinterpret_cast<' not in msl: return msl
    helpers, out, pos = [], [], 0
    for m in _RAW_HELPER.finditer(msl):
        r = _raw_helper(msl, m)
        if not r: return msl   # a shape this does not know: the shader stays as it is
        name, text, end = r
        out += [msl[pos:m.start()], text]; pos = end
        helpers.append(name)
    if not helpers: return msl
    src = ''.join(out) + msl[pos:]
    opens = [m.start() for m in re.finditer(r'(?m)^\{$', src)]   # function bodies open at column 0
    edits, decl = [], {}
    for name in helpers:
        for m in re.finditer(r'(?<![\w.])' + name + r'\(', src):
            if src.startswith('int ' + name + '(', src.rfind('\n', 0, m.start()) + 1): continue   # its definition
            edits.append((_close_paren(src, m.end() - 1), f', _rr{name}'))
            decl.setdefault(max(p for p in opens if p < m.start()), set()).add(name)
    if not edits: return msl
    for at, names in decl.items(): edits.append((at + 2, ''.join(f'    dm_raw_range _rr{n} = {{0ul, 0ul, 0}};\n' for n in sorted(names))))
    for at, text in sorted(edits, key=lambda e: -e[0]): src = src[:at] + text + src[at:]
    first = src.index('static inline __attribute__((always_inline))\nint ' + helpers[0] + '(')
    return src[:first] + _RAW_CACHE + src[first:]
