#!/usr/bin/env python3
"""Diagnostic loop guard (side libraries only, never a game cache): every loop names itself when it runs out.

loop_guard (dmsh_build.py) already ends every loop after LOOP_CAP iterations a thread, but it ends it silently and the cap bounds iterations, not time: a spin wait
in a persistent-queue kernel (PRAGMATA 12a52b68 / 8c48c7ff / 251426e9) polls one contended device word per iteration, so 4,096 threads x 2^18 polls of it can
keep one dispatch on the GPU for many seconds, and queue state left behind by the capped exit makes the next frame's dispatches spin to the cap as well (a wedge
that outlives the process: PRAGMATA_PORT_NOTES.md 2026-11-05, "GPU hang hunt").

This pass rewrites a guarded library's MSL (the output of loop_guard) so that:
- every guard site `++dm_lg<N> < CAPu` calls dm_hgc / dm_hgb with a site number (1, 2, ... in source order; `sites()` gives the table);
- the cap comes from word 1 of a debug buffer (compute: `device atomic_uint* dm_hg [[buffer(28)]]`, bound by the host with DM_NVN_LOOPCAP), or DEFAULT_CAP
  in stages without the buffer (no report there);
- the first time a thread's counter reaches the cap, the first active lane of its SIMD group appends a record (key high, key low, site, 0) to the buffer;
- with abort armed (word 2 != 0) a hit also sets word 3, and every loop on the shared counter dm_lg then leaves within 1,024 iterations, in every kernel
  that has the buffer, so the GPU drains instead of running each spinning dispatch to its cap. Loops on their own counter (dm_lg1, ...: a barrier in the
  body) never leave early on the abort word: threads of one group could read it on different iterations and strand the others at the barrier; they still end
  at the cap, on the same iteration for every thread (loop_guard's reason for their own counter).

The counter of each helper function is no longer its own: a helper that loops gets the caller's buffer and cap as extra parameters, so a helper called
in a loop still records; its counter stays per call (as in loop_guard).

Buffer layout (uint words): [0] records appended, [1] cap (0 = DEFAULT_CAP), [2] abort armed, [3] abort raised, [8] raw global-memory lookups that found no pool,
[9] the value a miss reads (0 = as before; the host sets it with DM_NVN_GMEM_POISON, e.g. 0x7fc0dead, a quiet NaN as a float, so what a miss feeds shows on screen),
[16 + 4 i ...] record i (SLOTS of them), then MISS_SLOTS miss records (key hi, key lo, address lo, address hi). A miss reads word 9, else 0, as before; it is counted (lunks
574a38f0 / nxnative #1326: a pool-table miss read 0 and a task shader then looped about 2^27 times). Stages without the buffer read 0.
"""
import re

DEFAULT_CAP = 1 << 16
SLOTS = 1024
MISS_SLOTS = 64
BUF_INDEX = 28
FRAG_BUF_INDEX = 30   # fragment entries (the host binds the same buffer there with DM_NVN_LOOPCAP; 28 is the blame mesh fragment's, 29 the logic-op state)
_SITE = re.compile(r'\+\+(dm_lg\d*) < (\d+)u')
_DEREF = re.compile(r'\s*\w+ (_\d+) = \*\(reinterpret_cast<(?:const )?device \w+\*>\(as_type<ulong>\(uint2\(uint\((_\d+)\), uint\((_\d+)\)\)\)\)\);')
_GMEM_SIG = re.compile(r'(?:static inline __attribute__\(\(always_inline\)\) )?int \w+\(thread const int& (_\d+), thread const int& (_\d+), (?:const )?device \w+& (?:cp|fp|vp)_s\d+\)')
_ENTRY = re.compile(r'^(?:kernel|vertex|fragment|\[\[mesh\]\]|\[\[object\]\])\s')

PRELUDE = '''
// dmsh_loopdiag: diagnostic loop guard (see dm/gfx/tools/dmsh_loopdiag.py)
#define DM_HG_KEY_HI {hi}u
#define DM_HG_KEY_LO {lo}u
static inline __attribute__((always_inline)) void dm_hg_hit(uint site, device atomic_uint* hg)
{{
    if (hg == nullptr) return;
    if (simd_is_first())
    {{
        uint n = atomic_fetch_add_explicit(&hg[0], 1u, memory_order_relaxed);
        if (n < {slots}u)
        {{
            atomic_store_explicit(&hg[16u + 4u * n + 0u], DM_HG_KEY_HI, memory_order_relaxed);
            atomic_store_explicit(&hg[16u + 4u * n + 1u], DM_HG_KEY_LO, memory_order_relaxed);
            atomic_store_explicit(&hg[16u + 4u * n + 2u], site, memory_order_relaxed);
        }}
    }}
    if (atomic_load_explicit(&hg[2], memory_order_relaxed) != 0u) atomic_store_explicit(&hg[3], 1u, memory_order_relaxed);
}}
// a raw global-memory lookup that found no pool (the shader then reads 0): counted, and the first misses' addresses kept (words 8.., ring after the loop records)
static inline __attribute__((always_inline)) void dm_hg_miss(uint lo, uint hi, device atomic_uint* hg)
{{
    if (hg == nullptr) return;
    uint n = atomic_fetch_add_explicit(&hg[8], 1u, memory_order_relaxed);
    if (n < {mslots}u)
    {{
        uint b = 16u + 4u * {slots}u + 4u * n;
        atomic_store_explicit(&hg[b + 0u], DM_HG_KEY_HI, memory_order_relaxed);
        atomic_store_explicit(&hg[b + 1u], DM_HG_KEY_LO, memory_order_relaxed);
        atomic_store_explicit(&hg[b + 2u], lo, memory_order_relaxed);
        atomic_store_explicit(&hg[b + 3u], hi, memory_order_relaxed);
    }}
}}
// a lookup that FOUND an entry but whose resolved address is below 4 GB (no memory pool is there: a table row being rewritten, or an entry over address 0): counted in
// word 10, the first address kept in words 11-12, and read as a miss instead of dereferenced (hangguard: PRAGMATA's hg25 BIF0 page fault, a compute read at 0x5840)
static inline __attribute__((always_inline)) bool dm_hg_lowaddr(uint lo, uint hi, device atomic_uint* hg)
{{
    if (hi != 0u) return false;
    if (hg != nullptr && atomic_fetch_add_explicit(&hg[10], 1u, memory_order_relaxed) == 0u)
    {{
        atomic_store_explicit(&hg[11], lo, memory_order_relaxed);
        atomic_store_explicit(&hg[12], DM_HG_KEY_LO, memory_order_relaxed);
    }}
    return true;
}}
// what a lookup that found no pool returns: word 9 (DM_NVN_GMEM_POISON), 0 when unset or in a stage without the buffer
static inline __attribute__((always_inline)) int dm_hg_poison(device atomic_uint* hg)
{{
    return hg == nullptr ? 0 : int(atomic_load_explicit(&hg[9], memory_order_relaxed));
}}
// a loop on the shared counter: ends at the cap, or within 1024 iterations of an abort
static inline __attribute__((always_inline)) bool dm_hgc(thread uint& c, uint site, device atomic_uint* hg, uint cap)
{{
    c += 1u;
    if (c < cap)
    {{
        if ((c & 1023u) != 0u || hg == nullptr) return true;
        return atomic_load_explicit(&hg[3], memory_order_relaxed) == 0u;
    }}
    if (c == cap) dm_hg_hit(site, hg);
    return false;
}}
// a loop with a barrier (its own counter): ends at the cap only
static inline __attribute__((always_inline)) bool dm_hgb(thread uint& c, uint site, device atomic_uint* hg, uint cap)
{{
    c += 1u;
    if (c < cap) return true;
    if (c == cap) dm_hg_hit(site, hg);
    return false;
}}
'''


def _close(s, i):
    """index of the ')' matching the '(' at s[i]"""
    d = 0
    for j in range(i, len(s)):
        if s[j] == '(': d += 1
        elif s[j] == ')':
            d -= 1
            if d == 0: return j
    return None


def _functions(lines):
    """[(name, header line, brace line, end line, is_entry)] for every function body (brace alone in column 0 after a header ending in ')')"""
    out, cur = [], None
    for i, l in enumerate(lines):
        if l == '{' and i and lines[i - 1].rstrip().endswith(')') and cur is None:
            h = lines[i - 1]
            m = next((x for x in re.finditer(r'(\w+)\s*\(', h) if x.group(1) != '__attribute__' and h[x.end():x.end() + 1] != '('), None)   # not the always_inline attribute
            cur = [m.group(1) if m else None, i - 1, i, None, bool(_ENTRY.match(h))]
        elif l == '}' and cur is not None:
            cur[3] = i; out.append(tuple(cur)); cur = None
    return out


def transform(msl, key, stage='compute'):
    """MSL with loop_guard sites -> MSL with reporting sites; returns (msl, sites) where sites = [(site, counter, function, line in the new MSL)]."""
    lines = msl.split('\n')
    fns = _functions(lines)
    names = {f[0] for f in fns if f[0] and not f[4]}
    body = {f[0]: '\n'.join(lines[f[2]:f[3] + 1]) for f in fns if f[0] and not f[4]}
    # raw global-memory lookups (translator GlobalToStorage, table DMSH_GMEM_MAP_*): a helper (int lo, int hi, table) that binary-searches the table from entry 2048 and dereferences
    # the found pool's pointer, else returns 0. Each gets a miss report before its final return 0, which then returns word 9 (dm_hg_poison: 0 unless DM_NVN_GMEM_POISON)
    gmem = {}
    for f in fns:
        if not f[0] or f[4]: continue
        m = _GMEM_SIG.fullmatch(lines[f[1]].strip())
        if m and '+ 2048u)' in body[f[0]] and ('reinterpret_cast<device uint*>(as_type<ulong>(uint2(' in body[f[0]] or 'reinterpret_cast<const device uint*>(as_type<ulong>(uint2(' in body[f[0]]): gmem[f[0]] = (m.group(1), m.group(2), f)
    if not _SITE.search(msl) and not gmem: return msl, []
    # helpers that need the buffer: a guard site of their own, a gmem lookup, or a call to such a helper (to a fixed point)
    need = {n for n in names if _SITE.search(body[n])} | set(gmem)
    while True:
        more = {n for n in names - need if any(re.search(r'\b' + re.escape(c) + r'\s*\(', body[n]) for c in need)}
        if not more: break
        need |= more
    with_buf = stage in ('compute', 'fragment')
    entry_decl = (f'    uint dm_hgcap = atomic_load_explicit(&dm_hg[1], memory_order_relaxed); if (dm_hgcap == 0u) dm_hgcap = {DEFAULT_CAP}u;' if with_buf else
                  f'    device atomic_uint* dm_hg = nullptr; uint dm_hgcap = {DEFAULT_CAP}u;')
    head_of = {f[1]: f for f in fns}
    out, sites, n = [], [], 0
    sites_miss = []
    call = re.compile(r'\b(' + '|'.join(sorted(map(re.escape, need), key=len, reverse=True)) + r')\s*\(') if need else None
    fn_at = {}
    for f in fns:
        for i in range(f[1], f[3] + 1): fn_at[i] = f[0]
    for i, l in enumerate(lines):
        f = head_of.get(i)
        if f is not None:
            j = l.rstrip().rfind(')')
            if f[4]:
                if with_buf: l = l[:j] + f', device atomic_uint* dm_hg [[buffer({BUF_INDEX if stage == "compute" else FRAG_BUF_INDEX})]]' + l[j:]
            elif f[0] in need:
                o = re.search(r'\b' + re.escape(f[0]) + r'\s*\(', l).end() - 1
                empty = not l[o + 1:j].strip()
                l = l[:j] + ('' if empty else ', ') + 'device atomic_uint* dm_hg, uint dm_hgcap' + l[j:]
            out.append(l); continue
        if i and (g := head_of.get(i - 1)) is not None and g[4] and l == '{':
            out += [l, entry_decl]; continue
        dm_ = _DEREF.match(l)
        if dm_ and fn_at.get(i) in gmem:   # the found-entry load of a pool-table helper: a resolved address below 4 GB is not dereferenced
            ind = l[:len(l) - len(l.lstrip())]
            out.append(ind + f'if (dm_hg_lowaddr(uint({dm_.group(2)}), uint({dm_.group(3)}), dm_hg)) return dm_hg_poison(dm_hg);')
            out.append(l); continue
        g = next((v for v in gmem.values() if v[2][3] - 1 == i and l.strip() == 'return 0;'), None)
        if g:
            ind = l[:len(l) - len(l.lstrip())]
            out.append(ind + f'dm_hg_miss(uint({g[0]}), uint({g[1]}), dm_hg);'); sites_miss.append(fn_at.get(i))
            out.append(ind + 'return dm_hg_poison(dm_hg);'); continue   # DM_NVN_GMEM_POISON: the miss reads word 9
        if call is not None and i not in head_of:
            pos = 0
            while (m := call.search(l, pos)):
                o = m.end() - 1; c = _close(l, o)
                if c is None: raise ValueError(f'{key}: call to {m.group(1)} at line {i + 1} does not close on its line')
                ins = ('' if not l[o + 1:c].strip() else ', ') + 'dm_hg, dm_hgcap'
                l = l[:c] + ins + l[c:]; pos = m.end()
        def site(m):
            nonlocal n
            n += 1
            sites.append((n, m.group(1), fn_at.get(i), None))
            fn = 'dm_hgc' if m.group(1) == 'dm_lg' else 'dm_hgb'
            return f'{fn}({m.group(1)}, {n}u, dm_hg, dm_hgcap)'
        l = _SITE.sub(site, l)
        out.append(l)
    text = '\n'.join(out)
    hi, lo = int(key[:8], 16), int(key[8:16], 16)
    prelude = PRELUDE.format(hi=hex(hi), lo=hex(lo), slots=SLOTS, mslots=MISS_SLOTS)
    anchor = 'using namespace metal;'
    if anchor not in text: raise ValueError(f'{key}: no "{anchor}" line to put the prelude after')
    text = text.replace(anchor, anchor + '\n' + prelude, 1)
    # the line of each site in the final text
    tl = text.split('\n')
    want = re.compile(r'\bdm_hg[cb]\(dm_lg\d*, (\d+)u,')
    at = {}
    for ln, x in enumerate(tl, 1):
        for m in want.finditer(x): at[int(m.group(1))] = ln
    sites = [(s, c, fn, at.get(s)) for s, c, fn, _ in sites]
    return text, sites


def stage_of(msl):
    if re.search(r'^kernel\s', msl, re.M): return 'compute'
    if re.search(r'^\[\[mesh\]\]', msl, re.M): return 'mesh'
    if re.search(r'^\[\[object\]\]', msl, re.M): return 'task'
    if re.search(r'^fragment\s', msl, re.M): return 'fragment'
    return 'vertex'


MAC_METAL = ['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance']   # dmsh_build.py MAC_METAL


def main(argv):
    """dmsh_loopdiag.py <library dir b-<chain>> <out dir> <key|keys file> ...: <out>/<key>_b.{metal,metallib} and <key>_b.sites.tsv for DM_MSL_OVERRIDE=<out>."""
    import os, subprocess, sys
    if len(argv) < 3: sys.exit(main.__doc__)
    lib, out, rest = argv[0], argv[1], argv[2:]
    keys = []
    for r in rest:
        keys += [x.strip().split(':')[-1] for x in open(r) if x.strip() and not x.startswith('#')] if os.path.isfile(r) else [r.split(':')[-1]]
    keys = sorted({k[:16] for k in keys if re.fullmatch(r'[0-9a-f]{16}', k[:16])})
    if not keys: sys.exit('dmsh_loopdiag.py: no keys; refusing (side builds never run on an empty list)')
    os.makedirs(out, exist_ok=True)
    ok = bad = plain = 0
    for k in keys:
        src = os.path.join(lib, k + '_b.metal')
        if not os.path.exists(src): print(f'{k}: no {src}'); bad += 1; continue
        msl = open(src).read(); st = stage_of(msl)
        new, sites = transform(msl, k, st)
        if new == msl: plain += 1; continue   # no loop and no raw global-memory lookup: the cache's library stays
        m = os.path.join(out, k + '_b.metal'); open(m, 'w').write(new)
        with open(os.path.join(out, k + '_b.sites.tsv'), 'w') as f:
            f.write('site\tcounter\tfunction\tline\tstage\n' + ''.join(f'{s}\t{c}\t{fn}\t{ln}\t{st}\n' for s, c, fn, ln in sites))
        air = m[:-6] + '.air'
        r = subprocess.run(MAC_METAL + ['-w', m, '-o', air], capture_output=True, text=True)
        if r.returncode == 0: r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', air, '-o', m[:-6] + '.metallib'], capture_output=True, text=True)
        if os.path.exists(air): os.remove(air)
        if r.returncode: print(f'{k}: compile failed\n{r.stderr[-2000:]}'); bad += 1; continue
        ok += 1; print(f'{k} {st}: {len(sites)} sites')
    print(f'built {ok}, without loops {plain}, failed {bad}')
    return 1 if bad else 0


if __name__ == '__main__':
    import sys
    sys.exit(main(sys.argv[1:]))
