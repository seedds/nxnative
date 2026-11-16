#!/usr/bin/env python3
"""dmsh_metalwa.py: MSL rewrites around Metal compiler bugs of macOS 26, after KosmicKrisp's workarounds.

KosmicKrisp (LunarG's Vulkan-on-Metal driver in Mesa, MIT licence) lists the Metal bugs its CTS runs found in docs/drivers/kosmickrisp/workarounds.rst and works
around them in src/kosmickrisp/compiler/nir_to_msl.c. The numbers below are theirs (KK_WORKAROUND_<n>).

Who gets it: the game profile's shader_metalwa row (dmsh_chain.metalwa), e.g. `shader_metalwa kk3 kk9 0123456789abcdef`: the workarounds named, on the shaders whose key starts
with one of the listed prefixes (every shader when none is listed). dmsh_build.py runs it as the last rewrite before the Metal compile.

Where the bug is: the offline front end (metal -O1) keeps the ballot inside its branch in the AIR; the GPU back end of the running OS (the compiler service that turns AIR into
GPU code when a pipeline is made) speculates it. So whether a library needs KK3 depends on the OS it runs on, not the one that built it: KK3 is behind a bool function constant
(DMSH_MWA_FC in dm/gfx/nvn/dmsh_abi.h), on by default, which the host turns off on macOS 27 and later (nvn_mtl.m mwa_on; DM_NVN_METALWA=0 / 1 overrides it). KK9 has no
Apple report or fix (KosmicKrisp keeps it on macOS 27 too) and is written unconditionally. Both rewritten forms are right on any OS; gate='static' writes KK3 unconditionally too.

  KK3  simd_ballot under divergent control flow returns every active lane of the SIMD group, not the lanes in the branch (KosmicKrisp: seen since macOS 15.4, FB20113490,
       fixed in macOS 27, build 26A5353q). Seen on an M4 Pro, macOS 26.6.2: `if (c) m = ballot(true); else m = 2;` gives 0xffffffff on the lanes with c. It happens where the compiler if-converts (speculates) the
       branch, i.e. where the ballot's value merges after it; -O0 is right. Every spvSubgroupBallot call is rewritten as simd_or of the lane bit (KosmicKrisp's
       lower_ballot), the lane id brought to the function that needs it. A ballot waterfall (one key per iteration until no lane is left) then converges again.
  KK9  the compiler moves the only break of a loop so the loop reconverges early: a ballot before the break sees all lanes (dEQP-VK.reconvergence.maximal.*; KosmicKrisp:
       seen on 26.4.1, not reported to Apple, no fix noted). Seen on the same Mac; the KK3 rewrite alone does not fix it. Fix: `if ((ulong)simd_active_threads_mask()) break;` for loop breaks after a subgroup operation.

KosmicKrisp lists more (KK6 device stores across loop iterations, KK10 selects from an ior, KK18 stale reads after a device barrier); they did not reproduce in probes on the
Mac above and are not here.

apply(msl, enable) returns (msl, stats). It leaves a source without the patterns unchanged (the same object), and a source it rewrote once as it is (by its mark,
whatever was asked the first time: the build makes one call). It parses SPIRV-Cross's layout (signatures on one line, braces alone in column 0) and raises ValueError where
that would leave a ballot unrewritten. Geometry shaders the build turns into vertex entry points (gs_slice) are not rewritten.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dmsh_abi   # noqa: E402

FC_KK3 = dmsh_abi.MWA_FC   # bool function constant: KK3's rewrite is used (default true); the host sets it false where the OS's compiler is fixed
NAMES = {'kk3': 3, 'kk9': 9}   # the words of the profile's shader_metalwa row (dmsh_chain.metalwa)
ALL = frozenset(NAMES.values())
MARK = '// dmsh_metalwa:'
LANE = 'gl_SubgroupInvocationID'
_SUBGROUP = re.compile(r'\b(?:simd_\w+|quad_\w+|spvSubgroup\w+|dm_mwa_ballot)\s*\(')
_ATTR = re.compile(r'__attribute__\s*\(\((?:[^()]|\([^()]*\))*\)\)')   # __attribute__((always_inline)), ((noinline)): not the function's name
_ENTRY = ('kernel', 'fragment', 'vertex', '[[mesh]]', '[[object]]')


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


def _sig(line):
    """(name, params) of a one-line function signature `<qualifiers and type> name(params)`, None for anything else (a control statement ending in ')')"""
    t = _ATTR.sub(' ', line.strip())
    m = re.match(r'^(?:template\s*<[^>]*>\s*)?([^()]*?)\b(\w+)\s*\(', t)
    if not m or not m.group(1).strip() or m.group(2) in ('if', 'for', 'while', 'switch', 'return'): return None
    try: j = _close(t, m.end() - 1)
    except ValueError: return None
    if t[j + 1:].strip(): return None
    return m.group(2), t[m.end():j]


def functions(msl):
    """[(sig_line, open_line, close_line, name, params, entry)] of SPIRV-Cross's top-level functions (signature on one line, braces alone in column 0)"""
    lines, out, i = msl.split('\n'), [], 0
    while i < len(lines):
        if lines[i] == '{' and i and lines[i - 1].rstrip().endswith(')'):
            s = i - 1
            m = _sig(lines[s])
            if m:
                j = i + 1
                while j < len(lines) and lines[j] != '}': j += 1
                head = lines[s - 1].strip() if s else ''
                entry = lines[s].startswith(_ENTRY) or head.startswith(_ENTRY)   # entry points take the builtin
                out.append((s, i, j, m[0], m[1], entry))
                i = j
        i += 1
    return out


def _helper(name): return name.startswith(('spv', 'dm_mwa'))   # SPIRV-Cross's helpers and ours are left as they are


HELPER3 = """// KK_WORKAROUND_3 (KosmicKrisp lower_ballot): a ballot as simd_or of the lane bit, shifted from the predicate (no select feeding the reduction)
inline uint4 dm_mwa_ballot(bool value, uint lane)
{
    if (dm_mwa3) return uint4(simd_or(uint(value) << lane), 0u, 0u, 0u);
    simd_vote vote = simd_ballot(value);
    return uint4(as_type<uint2>((simd_vote::vote_t)vote), 0u, 0u);
}
"""


def _gate3(gate):
    if gate == 'static': return 'constant bool dm_mwa3 = true;\n'
    return (f'constant bool dm_mwa3_fc [[function_constant({FC_KK3})]];\n'
            'constant bool dm_mwa3 = is_function_constant_defined(dm_mwa3_fc) ? dm_mwa3_fc : true;   // the host sets it false where the OS is fixed\n')


def _rewrite_calls(body, fname, new):
    """fname(ARGS) -> new(ARGS, lane) in body"""
    out, i, n = '', 0, 0
    pat = re.compile(r'\b' + re.escape(fname) + r'\s*\(')
    while True:
        m = pat.search(body, i)
        if not m: return out + body[i:], n
        j = _close(body, m.end() - 1)
        out += body[i:m.start()] + new + '(' + body[m.end():j] + ', ' + LANE + ')'
        i = j + 1; n += 1


def _pass_lane(body, fname):
    """every call fname(ARGS) in body, nested ones too, with the lane id as a last argument"""
    pat = re.compile(r'\b' + re.escape(fname) + r'\s*\(')
    out, i = '', 0
    while True:
        m = pat.search(body, i)
        if not m: return out + body[i:]
        j = _close(body, m.end() - 1); a = _pass_lane(body[m.end():j], fname)
        out += body[i:m.end()] + a + (', ' if a.strip() else '') + LANE + ')'; i = j + 1


_LOOP = re.compile(r'^\s*(?:for\s*\(|while\s*\(|do\s*$)')
_SWITCH = re.compile(r'^\s*switch\s*\(')


def _kk9(body):
    """KK_WORKAROUND_9: every `break;` of a loop (never a switch's) in a function that uses subgroup operations, as KosmicKrisp guards every break of a maximally
    reconverging shader. A subgroup op anywhere in the loop can run before a break (an earlier block, or the previous iteration), and the guard is a no-op on its own"""
    out, n, stack, pending = [], 0, [], None   # stack: [kind]
    for l in body.split('\n'):
        t = l.strip()
        if _LOOP.match(l): pending = 'loop'
        elif _SWITCH.match(l): pending = 'switch'
        elif t and t != '{': pending = pending if pending in ('loop', 'switch') and t.startswith(('for', 'while', 'do', 'switch')) else None
        if t == '{': stack.append([pending or 'blk']); pending = None; out.append(l); continue
        if t.startswith('}'):
            if stack: stack.pop()
            out.append(l); continue
        if t == 'break;' and stack:
            own = next((k for k in range(len(stack) - 1, -1, -1) if stack[k][0] in ('loop', 'switch')), None)
            if own is not None and stack[own][0] == 'loop':
                ind = l[:len(l) - len(l.lstrip())]
                out.append(ind + 'if ((ulong)simd_active_threads_mask()) break;   // KK_WORKAROUND_9'); n += 1; continue
        out.append(l)
    return '\n'.join(out), n


def apply(msl, enable: 'set[int] | frozenset[int]' = ALL, gate='fc'):
    """enable: KosmicKrisp numbers (3: spvSubgroupBallot calls; 9: loop breaks after a subgroup operation). gate: 'fc' (KK3 behind the function constant FC_KK3, default on)
    or 'static' (always on). Returns (msl, stats {n: rewrites}).
    A source with a vertex entry is left alone: dmsh_build makes vertex shaders' subgroups one lane wide (spv_single_lane)."""
    stats = {n: 0 for n in enable}
    if MARK in msl or not enable or re.search(r'^vertex\s', msl, flags=re.M): return msl, stats
    lines = msl.split('\n')
    fns = functions(msl)
    names = [f[3] for f in fns if not _helper(f[3])]
    if len(names) != len(set(names)): raise ValueError('dmsh_metalwa: two functions of one name (overloads are not handled)')
    # functions that use subgroup operations, through calls as well (KK9's condition follows the call graph)
    bodies = {f[3]: '\n'.join(lines[f[1] + 1:f[2]]) for f in fns}
    sg = {n for n, b in bodies.items() if _SUBGROUP.search(b) or n.startswith('spvSubgroup')}
    while True:
        more = {n for n, b in bodies.items() if n not in sg and any(re.search(r'\b' + re.escape(m) + r'\s*\(', b) for m in sg)}
        if not more: break
        sg |= more
    need_lane, edits = set(), {}
    # pass 1: rewrite bodies
    for (s, o, c, name, params, entry) in fns:
        if _helper(name): continue
        body = bodies[name]
        b = body
        if 3 in enable:
            b, k = _rewrite_calls(b, 'spvSubgroupBallot', 'dm_mwa_ballot'); stats[3] += k
            if k: need_lane.add(name)
        if 9 in enable and name in sg:
            b, k = _kk9(b); stats[9] += k
        if b != body: edits[o] = b
    if 3 in enable:   # a ballot in a function functions() did not parse would be left as it is: say so rather than build the bug
        left = re.sub(r'^inline uint4 spvSubgroupBallot\(bool value\)$', '', msl, flags=re.M).count('spvSubgroupBallot(') - sum(bodies[f[3]].count('spvSubgroupBallot(') for f in fns if _helper(f[3]))
        if left != stats[3]: raise ValueError(f'dmsh_metalwa: {left - stats[3]} ballot call(s) outside the functions it parses')
    if not edits: return msl, stats
    # pass 2: make the lane id reach every function that now uses it (an entry: the builtin; others: a parameter, passed by every caller, skipped helpers included).
    # Only a parameter counts as in scope: a local of that name could be declared after the call or hold another value
    byname = {f[3]: f for f in fns}
    has_lane = lambda f: re.search(r'\b' + LANE + r'\b', f[4]) is not None
    todo, sig_new, seen = [n for n in need_lane if not has_lane(byname[n])], {}, set()
    while todo:
        n = todo.pop()
        if n in seen: continue
        seen.add(n); f = byname[n]
        p = f[4].strip()
        if f[5]:
            if re.search(r'\b' + LANE + r'\b', bodies[n]): raise ValueError(f'dmsh_metalwa: entry point {n} names {LANE} in its body')
            sig_new[f[0]] = lines[f[0]].rstrip()[:-1] + (', ' if p else '') + f'uint {LANE} [[thread_index_in_simdgroup]])'
            continue
        sig_new[f[0]] = lines[f[0]].rstrip()[:-1] + (', ' if p else '') + f'thread uint& {LANE})'
        for g in fns:   # callers pass it on
            if g[3] == n: continue
            gb = edits.get(g[1], bodies[g[3]])
            if not re.search(r'\b' + re.escape(n) + r'\s*\(', gb): continue
            edits[g[1]] = _pass_lane(gb, n)
            if not has_lane(g): todo.append(g[3])
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
    hdr = MARK + ' ' + ' '.join(f'KK{n}' for n in sorted(enable)) + ' (Metal compiler workarounds after KosmicKrisp, Mesa, MIT: docs/drivers/kosmickrisp/workarounds.rst)\n'
    if stats.get(3): hdr += _gate3(gate) + HELPER3
    m = re.search(r'^using namespace metal;\n', msl, flags=re.M)   # the helper uses only metal_stdlib
    if not m: raise ValueError('dmsh_metalwa: no `using namespace metal;`')
    return msl[:m.end()] + '\n' + hdr + msl[m.end():], stats


if __name__ == '__main__':
    if len(sys.argv) < 3: sys.exit('usage: dmsh_metalwa.py <in.metal> <out.metal> [kk3,kk9] [fc|static]')
    en = {NAMES[w] for w in sys.argv[3].lower().split(',')} if len(sys.argv) > 3 else ALL
    dst, st = apply(open(sys.argv[1]).read(), en, sys.argv[4] if len(sys.argv) > 4 else 'fc')
    open(sys.argv[2], 'w').write(dst); print({f'kk{n}': k for n, k in st.items()})
