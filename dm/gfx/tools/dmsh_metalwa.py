#!/usr/bin/env python3
"""dmsh_metalwa.py: MSL rewrites around Metal compiler / GPU bugs of macOS 26, after KosmicKrisp's workarounds.

KosmicKrisp (LunarG's Vulkan-on-Metal driver in Mesa, MIT licence) lists the Metal bugs its CTS runs found in
docs/drivers/kosmickrisp/workarounds.rst and works around them in src/kosmickrisp/compiler/nir_to_msl.c and vulkan/kk_shader.c.
The numbers below are theirs (KK_WORKAROUND_<n>).

Who gets it: the game profile's shader_metalwa row (dmsh_chain.metalwa), e.g. `shader_metalwa kk3 kk9 965043fe128ef98f`: the workarounds named, on the shaders whose
key starts with one of the listed prefixes (every shader when none is listed). dmsh_build.py runs it as the last rewrite before the Metal compile, with the static gate
(the rewritten forms are right on any OS, the bugs' fixed or not). gate='fc' instead puts each one behind a bool function constant (DMSH_MWA_FC + slot, dmsh_abi.h)
for a host that would set them from the running OS (host_defaults); no host sets them yet, so such a library keeps the default written here (DEFAULT_ON).

  KK3  simd_ballot / vote under divergent control flow returns every active lane of the simdgroup, not the lanes in the branch (FB20113490; KosmicKrisp: fixed in
       macOS 27). Seen on an M4 Pro, macOS 26.6.2: `if (c) m = ballot(true); else m = 2;` gives 0xffffffff on the odd lanes. It happens where the compiler
       if-converts (speculates) the branch, so every ballot is rewritten as simd_or of the lane bit (KK's lower_ballot). PRAGMATA's 965043fe128ef98f: a ballot
       waterfall (one key per iteration) never converges once a SIMD group holds two keys, and runs to the loop guard's cap; correct at -O0.
  KK3v (30, opt-in) simd_any / simd_all through the same reduction (KK lowers votes through the ballot; no probe here showed a vote wrong).
  KK9  Metal moves the only break of a loop so the loop reconverges early: a ballot before the break sees all lanes (dEQP-VK.reconvergence.maximal.*).
       Seen on the same Mac. Fix: `if ((ulong)simd_active_threads_mask()) break;`, emitted for loop breaks after a subgroup operation.
  KK6  device memory loses a store across loop iterations of one thread (fixed in 27). NOT seen on the M4 Pro. Option only: `coherent` on the device pointers of
       argument buffers (a type qualifier, so never behind a function constant).
  KK15 macOS 27: `volatile coherent device` accesses miscompile. KK6 never adds `coherent` to a `volatile` pointer.
  KK10 a bcsel the compiler made from an ior misbehaves (subgroup inclusive scans). Not seen. Option: AND a run-time true value into select conditions.
  KK18 stale reads after a device memory barrier (M3+; fixed in 27). Not seen. Option: a second fence behind a never-true run-time condition after each device fence.

apply(msl, enable) returns (msl, stats). It is idempotent and leaves a source without the patterns unchanged.
"""
import re

try:   # DMSH_MWA_FC in dm/gfx/nvn/dmsh_abi.h; slot = FC_BASE + SLOT[n]
    import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import dmsh_abi; FC_BASE = dmsh_abi.MWA_FC
except (ImportError, AttributeError): FC_BASE = 220
SLOT = {3: 0, 9: 1, 6: 2, 10: 3, 18: 4, 30: 5}   # 30 = KK3v: simd_any / simd_all as well (opt-in: no probe or VM reference shows votes wrong)
NAMES = {'kk3': 3, 'kk3v': 30, 'kk9': 9, 'kk6': 6, 'kk10': 10, 'kk18': 18}   # the words of the profile's shader_metalwa row (dmsh_chain.metalwa)
DEFAULT_ON = {3, 9}            # what a library defaults to when its host does not specialize (the bugs seen on macOS 26)
OS_FIXED = {3: 27, 9: 27, 6: 27, 10: 27, 18: 27, 30: 27}   # first macOS major where the bug is gone (KosmicKrisp's log): the host default
MARK = '// dmsh_metalwa:'
LANE = 'gl_SubgroupInvocationID'
_SUBGROUP = re.compile(r'\b(?:simd_\w+|quad_\w+|spvSubgroup\w+|dm_mwa_(?:ballot|any|all))\s*\(')
_SIG = re.compile(r'^(?:kernel\s+|fragment\s+|vertex\s+|\[\[(?:mesh|object)\]\]\s+|static\s+inline\s+__attribute__\(\(always_inline\)\)\s+)?[\w:<>,\s\*&]+?\b(\w+)\((.*)\)\s*$')


def _close(s, i):
    """index of the ')' matching the '(' at s[i]"""
    d = 0
    for j in range(i, len(s)):
        c = s[j]
        if c == '(': d += 1
        elif c == ')':
            d -= 1
            if d == 0: return j
    raise ValueError('unbalanced parentheses')


def _split_args(s):
    out, d, cur = [], 0, ''
    for c in s:
        if c in '([{': d += 1
        elif c in ')]}': d -= 1
        if c == ',' and d == 0: out.append(cur); cur = ''
        else: cur += c
    out.append(cur)
    return out


def functions(msl):
    """[(sig_line, open_line, close_line, name, params)] of SPIRV-Cross's top-level functions (brace alone in column 0)"""
    lines, out, i = msl.split('\n'), [], 0
    while i < len(lines):
        if lines[i] == '{' and i and lines[i - 1].rstrip().endswith(')'):
            s = i - 1
            m = _SIG.match(lines[s].strip())
            if m:
                j = i + 1
                while j < len(lines) and lines[j] != '}': j += 1
                head = lines[s - 1].strip() if s else ''
                ENTRY = ('kernel', 'fragment', 'vertex', '[[mesh]]', '[[object]]')
                entry = lines[s].startswith(ENTRY) or head.startswith(ENTRY)   # entry points take the builtin
                out.append((s, i, j, m.group(1), m.group(2), entry))
                i = j
        i += 1
    return out


def _helper(name): return name.startswith(('spv', 'dm_hg', 'dm_mwa'))   # SPIRV-Cross's helpers, the loop guard's, ours (dm_stage_body, a mesh stage's body, is the shader)


def _gates(enable, gate):
    g = [MARK + ' Metal compiler workarounds after KosmicKrisp (Mesa, MIT): docs/drivers/kosmickrisp/workarounds.rst']
    for n in sorted(enable):
        if n == 6: continue   # build-time only
        if gate == 'fc':
            g.append(f'constant bool dm_mwa{n}_fc [[function_constant({FC_BASE + SLOT[n]})]];')
            g.append(f'constant bool dm_mwa{n} = is_function_constant_defined(dm_mwa{n}_fc) ? dm_mwa{n}_fc : {"true" if n in DEFAULT_ON else "false"};')
        else:
            g.append(f'constant bool dm_mwa{n} = true;')
    return g


HELPERS3 = """// KK_WORKAROUND_3 (KosmicKrisp lower_ballot): a ballot as simd_or of the lane bit, shifted from the predicate (no select feeding the reduction: the KK10 shape)
inline uint4 dm_mwa_ballot(bool value, uint lane)
{
    if (dm_mwa3) return uint4(simd_or(uint(value) << lane), 0u, 0u, 0u);
    simd_vote vote = simd_ballot(value);
    return uint4(as_type<uint2>((simd_vote::vote_t)vote), 0u, 0u);
}
"""
HELPERS3V = """// KK3v (opt-in): votes through the same reduction
inline bool dm_mwa_any(bool value, uint lane) { if (dm_mwa30) return simd_or(uint(value) << lane) != 0u; return simd_any(value); }
inline bool dm_mwa_all(bool value, uint lane) { if (dm_mwa30) return simd_or(uint(!value) << lane) == 0u; return simd_all(value); }
"""
HELPER10 = """// KK_WORKAROUND_10: a run-time true value the compiler cannot fold into a select's condition
inline bool dm_mwa_true() { return !dm_mwa10 || (ulong)simd_active_threads_mask() != 0ul; }
"""


def _rewrite_calls(body, fname, new, add_lane):
    """fname(ARGS) -> new(ARGS, lane) in body"""
    out, i, n = '', 0, 0
    pat = re.compile(r'\b' + re.escape(fname) + r'\s*\(')
    while True:
        m = pat.search(body, i)
        if not m: return out + body[i:], n
        j = _close(body, m.end() - 1)
        args = body[m.end():j]
        out += body[i:m.start()] + new + '(' + args + (', ' + LANE if add_lane else '') + ')'
        i = j + 1; n += 1


_LOOP = re.compile(r'^\s*(?:for\s*\(|while\s*\(|do\s*$)')
_SWITCH = re.compile(r'^\s*switch\s*\(')


def _kk9(body, has_sg):
    """KK_WORKAROUND_9 on the breaks it is about: a `break;` of a loop (never a switch's) with a subgroup operation earlier in that loop's body, at the
    break's own nesting level or an enclosing one (KosmicKrisp emits it only for maximally reconverging shaders; the bug needs the subgroup op before the break)"""
    out, n, stack, pending = [], 0, [], None   # stack: [kind, subgroup op seen directly in this block]
    for l in body.split('\n'):
        t = l.strip()
        if _LOOP.match(l): pending = 'loop'
        elif _SWITCH.match(l): pending = 'switch'
        elif t and t != '{': pending = pending if pending in ('loop', 'switch') and t.startswith(('for', 'while', 'do', 'switch')) else None
        if t == '{': stack.append([pending or 'blk', False]); pending = None; out.append(l); continue
        if t.startswith('}'):
            if stack: stack.pop()
            out.append(l); continue
        if has_sg(l) and stack: stack[-1][1] = True
        if t == 'break;' and stack:
            own = next((k for k in range(len(stack) - 1, -1, -1) if stack[k][0] in ('loop', 'switch')), None)
            if own is not None and stack[own][0] == 'loop' and any(f[1] for f in stack[own:]):
                ind = l[:len(l) - len(l.lstrip())]
                out.append(ind + 'if (!dm_mwa9 || (ulong)simd_active_threads_mask()) break;   // KK_WORKAROUND_9'); n += 1; continue
        out.append(l)
    return '\n'.join(out), n


def apply(msl, enable=frozenset(DEFAULT_ON), gate='fc'):
    """gate: 'fc' (function constants, the host picks per OS) or 'static' (always on). Returns (msl, stats).
    3: register-result ballots (spvSubgroupBallot); 30: simd_any / simd_all too (opt-in); 9: loop breaks after a subgroup op; 10, 18, 6: options.
    A source with a vertex entry is left alone: dmsh_build makes vertex shaders one-lane (spv_single_lane)."""
    stats = {n: 0 for n in enable}
    if MARK in msl or not enable or re.search(r'^vertex\s', msl, flags=re.M): return msl, stats
    lines = msl.split('\n')
    fns = functions(msl)
    # functions that use subgroup operations, through calls as well (KK9's condition follows the call graph)
    bodies = {f[3]: '\n'.join(lines[f[1] + 1:f[2]]) for f in fns}
    sg = {n for n, b in bodies.items() if _SUBGROUP.search(b) or _helper(n) and n.startswith('spvSubgroup')}
    while True:
        more = {n for n, b in bodies.items() if n not in sg and any(re.search(r'\b' + re.escape(m) + r'\s*\(', b) for m in sg)}
        if not more: break
        sg |= more
    call_sg = re.compile(r'\b(?:' + '|'.join(map(re.escape, sorted(sg))) + r')\s*\(') if sg else None
    has_sg = lambda l: bool(_SUBGROUP.search(l) or (call_sg and call_sg.search(l)))
    need_lane, edits = set(), {}
    # pass 1: rewrite bodies
    for (s, o, c, name, params, kern) in fns:
        if _helper(name): continue
        body = bodies[name]
        b = body
        rw = ([('spvSubgroupBallot', 'dm_mwa_ballot', 3)] if 3 in enable else []) + ([('simd_any', 'dm_mwa_any', 30), ('simd_all', 'dm_mwa_all', 30)] if 30 in enable else [])
        for f, nf, kk in rw:
            b, k = _rewrite_calls(b, f, nf, True); stats[kk] += k
            if k: need_lane.add(name)
        if 9 in enable and name in sg:
            b, k = _kk9(b, has_sg); stats[9] += k
        if 10 in enable:
            res, i, k = '', 0, 0
            for m in re.finditer(r'\bselect\s*\(', b):
                if m.start() < i: continue
                j = _close(b, m.end() - 1); a = _split_args(b[m.end():j])
                if len(a) != 3: continue
                res += b[i:m.start()] + f'select({a[0]},{a[1]}, ({a[2].strip()}) == dm_mwa_true())'; i = j + 1; k += 1
            b = res + b[i:]; stats[10] += k
        if 18 in enable:
            b, k = re.subn(r'^(\s*)(atomic_thread_fence\([^;\n]*thread_scope_device\);|threadgroup_barrier\(mem_flags::mem_device[^;\n]*\);)(.*)$',
                           r'\1\2\3\n\1if (dm_mwa18 && (ulong)simd_active_threads_mask() == 0ul) \2   // KK_WORKAROUND_18', b, flags=re.M); stats[18] += k
        if b != body: edits[o] = b
    # pass 2: make the lane id reach every function that now uses it (an entry: the builtin; others: a parameter, passed by every caller, skipped helpers included).
    # Only a parameter counts as in scope: a local of that name could be declared after the call or hold another value
    byname = {f[3]: f for f in fns}
    has_lane = lambda f: re.search(r'\b' + LANE + r'\b', f[4]) is not None
    todo, sig_new = [n for n in need_lane if not has_lane(byname[n])], {}
    seen = set()
    while todo:
        n = todo.pop()
        if n in seen: continue
        seen.add(n); f = byname[n]
        p = f[4].strip()
        if f[5]:
            sig_new[f[0]] = lines[f[0]].rstrip()[:-1] + (', ' if p else '') + f'uint {LANE} [[thread_index_in_simdgroup]])'
            continue
        sig_new[f[0]] = lines[f[0]].rstrip()[:-1] + (', ' if p else '') + f'thread uint& {LANE})'
        for g in fns:   # callers pass it on
            if g[3] == n: continue
            gb = edits.get(g[1], bodies[g[3]])
            if not re.search(r'\b' + re.escape(n) + r'\s*\(', gb): continue
            out, i = '', 0
            for m in re.finditer(r'\b' + re.escape(n) + r'\s*\(', gb):
                if m.start() < i: continue
                j = _close(gb, m.end() - 1); a = gb[m.end():j]
                out += gb[i:j] + (', ' if a.strip() else '') + LANE + ')'; i = j + 1
            edits[g[1]] = out + gb[i:]
            if not has_lane(g): todo.append(g[3])
    if not edits and not sig_new:
        if 6 in enable:
            out6, stats[6] = coherent_args(msl)
            if stats[6]: return _insert_gates(out6, enable, gate), stats
        return msl, stats
    # pass 3: assemble
    out, skip_to = [], -1
    starts = {f[1]: f[2] for f in fns}
    for i, l in enumerate(lines):
        if i < skip_to: continue
        if i in sig_new: out.append(sig_new[i]); continue
        if i in edits:
            out.append(l); out.extend(edits[i].split('\n')); skip_to = starts[i]; continue
        out.append(l)
    msl = '\n'.join(out)
    hdr = '\n'.join(_gates(enable, gate)) + '\n'
    if 3 in enable and stats[3]: hdr += HELPERS3
    if 30 in enable and stats[30]: hdr += HELPERS3V
    if 10 in enable and stats[10]: hdr += HELPER10
    ins = _insert_point(msl)
    msl = msl[:ins] + '\n' + hdr + msl[ins:]
    if 6 in enable: msl, stats[6] = coherent_args(msl)
    return msl, stats


def _insert_gates(msl, enable, gate):
    ins = _insert_point(msl)
    return msl[:ins] + '\n' + '\n'.join(_gates(enable, gate)) + '\n' + msl[ins:]


def _insert_point(msl):
    """after `using namespace metal;` (gates and helpers use only metal_stdlib)"""
    m = re.search(r'^using namespace metal;\n', msl, flags=re.M)
    if not m: raise ValueError('no `using namespace metal;`')
    return m.end()


def coherent_args(msl):
    """KK_WORKAROUND_6 (option): `coherent` on argument-buffer device pointers; never on volatile ones (KK_WORKAROUND_15)."""
    n = 0
    def f(m):
        nonlocal n; n += 1; return m.group(1) + 'coherent device ' + m.group(2)
    msl = re.sub(r'(^|[{;]\s*|\n\s*)(?<!volatile )(?<!coherent )device (\w+\*\s+\w+\s+\[\[id\(\d+\)\]\];)', f, msl)
    return msl, n


def host_defaults(macos_major):
    """the function-constant values a host on macOS <macos_major> sets (nvn_mtl.m mwa_values mirrors this)"""
    return {FC_BASE + SLOT[n]: (n in DEFAULT_ON and macos_major < OS_FIXED[n]) for n in SLOT}


if __name__ == '__main__':
    import sys
    if len(sys.argv) < 3: sys.exit('usage: dmsh_metalwa.py <in.metal> <out.metal> [3,9,...] [fc|static]')
    en = set(map(int, sys.argv[3].split(','))) if len(sys.argv) > 3 else set(DEFAULT_ON)
    src = open(sys.argv[1]).read(); dst, st = apply(src, en, sys.argv[4] if len(sys.argv) > 4 else 'fc')
    open(sys.argv[2], 'w').write(dst); print(st)
