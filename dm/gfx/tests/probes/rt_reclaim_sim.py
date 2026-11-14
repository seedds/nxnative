#!/usr/bin/env python3
# Render-target memory simulation from a render-target event log, rt_events.tsv (W frame serial pool off size w h format samples levels target flags; R frame serial).
# Models the Metal memory the host would hold, three ways:
#   base  every texture attached and not yet finalized keeps its own Metal texture (today)
#   dead  (c): a texture whose whole pool range has been covered by other textures' attachment writes since its own last write is dead by overwrite;
#         its Metal texture goes to a free list keyed on the full descriptor (w h format samples levels target flags). A later write to a texture with no Metal
#         texture (new, or dead and written again) takes one from that list if one fits, else a new one. The free list is trimmed after IDLE frames unused.
#   dead0 (c) with no free list: dead textures are released at once and every write to one without a texture allocates
# Memory counted at the texture's guest size (the log's size field); Metal sizes differ by up to about 1.12x.
# usage: rt_reclaim_sim.py rt_events.tsv [idle frames for the free list, default 60]
import sys, collections

path = sys.argv[1]; IDLE = int(sys.argv[2]) if len(sys.argv) > 2 else 60

class Tex:
    __slots__ = ('pool', 'lo', 'hi', 'size', 'key', 'cov', 'has', 'dead')
    def __init__(s, pool, lo, size, key): s.pool, s.lo, s.hi, s.size, s.key, s.cov, s.has, s.dead = pool, lo, lo + size, size, key, [], False, False

def add_cov(t, a, b):   # merge [a, b) into t.cov (sorted disjoint intervals); return True when it covers [t.lo, t.hi)
    a, b = max(a, t.lo), min(b, t.hi)
    if a >= b: return False
    out, placed = [], False
    for x, y in t.cov:
        if y < a or x > b: out.append((x, y))
        else: a, b = min(a, x), max(b, y)
    out.append((a, b)); out.sort(); t.cov = out
    return len(out) == 1 and out[0][0] <= t.lo and out[0][1] >= t.hi

def run(mode):
    tex = {}; by_pool = collections.defaultdict(set)
    free = collections.defaultdict(list)   # key -> [(size, last frame freed)]
    mem = 0; peak = (0, 0); series = {}; allocs = reuses = resurrect = deaths = 0; frame = 0
    def alloc(t, f):
        nonlocal mem, allocs, reuses
        if mode == 'dead' and free[t.key]: free[t.key].pop(); reuses += 1
        else: mem += t.size; allocs += 1
        t.has = True
    def trim(f):
        nonlocal mem
        if mode != 'dead': return
        for k, lst in free.items():
            keep = [(s, lf) for s, lf in lst if f - lf < IDLE]
            for s, lf in lst:
                if f - lf >= IDLE: mem -= s
            free[k] = keep
    for line in open(path):
        p = line.rstrip('\n').split('\t')
        if not p or p[0] not in ('W', 'R'): continue
        f = int(p[1]); s = p[2]
        if f != frame:
            trim(f); frame = f
            if f // 100 not in series: series[f // 100] = mem
        if p[0] == 'R':
            t = tex.pop(s, None)
            if t:
                by_pool[t.pool].discard(s)
                if t.has: mem -= t.size
            continue
        pool, off, size = p[3], int(p[4], 0) if p[4].startswith('0x') else int(p[4]), int(p[5])
        key = tuple(p[6:13])
        t = tex.get(s)
        if t is None: t = tex[s] = Tex(pool, off, size, key); by_pool[pool].add(s)
        t.cov = []
        if not t.has:
            if t.dead: resurrect += 1
            alloc(t, f)
        t.dead = False
        if mode != 'base':   # this write covers part of every other texture of the pool it overlaps
            for o in list(by_pool[pool]):
                if o == s: continue
                u = tex[o]
                if u.dead or u.hi <= t.lo or u.lo >= t.hi: continue
                if add_cov(u, t.lo, t.hi):
                    u.dead = True; deaths += 1
                    if u.has:
                        u.has = False
                        if mode == 'dead': free[u.key].append((u.size, f))
                        else: mem -= u.size
        if mem > peak[0]: peak = (mem, f)
    return mem, peak, series, allocs, reuses, resurrect, deaths, frame

MB = 1 << 20
res = {m: run(m) for m in ('base', 'dead0', 'dead')}
print(f'events to frame {res["base"][7]}; free-list idle trim {IDLE} frames; memory at guest size')
for m, (mem, peak, series, allocs, reuses, res_, deaths, fr) in res.items():
    print(f'{m:6s} end {mem / MB:8.0f} MB  peak {peak[0] / MB:8.0f} MB at f{peak[1]}  allocations {allocs}  free-list reuses {reuses}  dead-then-written-again {res_}  deaths {deaths}')
print('frame   ' + '  '.join(f'{m:>8s}' for m in res))
for k in sorted(res['base'][2]):
    print(f'f{k * 100:<6d} ' + '  '.join(f'{res[m][2].get(k, 0) / MB:8.0f}' for m in res))
