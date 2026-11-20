#!/usr/bin/env python3
"""Compare two DM_DETCLOCK_TRACE files (the deterministic guest clock's events, hle_thread.c) and print the first guest time at which they differ, with context.

Two runs under DM_DETCLOCK with the same input must write the same trace: every idle jump (J), present (P), block of a game thread (B), thread start (T), host signal of a
guest event (E), wake of a game thread by host code (H), end of a native HLE wait (N), first reading of the clock after such a wake (R), scripted input change (I) and
the game's first reading of it (K) at the same guest time
(docs/reference/threads.md, the DM_DETCLOCK entry, has the line format). The first time whose events differ is where the runs part: the times before it show what led there.

  detclock_trace_cmp.py <trace A> <trace B> [--kinds OJPBTEHNRIK] [--exact] [--context N]

Traces may be gzip-compressed (.gz). --kinds keeps only those kinds of line in both (for example P: the presents' guest times alone). The lines of one guest time are compared as
a set, since threads that run at the same guest time write them in the order the host ran them. Each line is compared without its jump count (one jump more early on would make
every later line differ), and an idle jump by its times only: which thread blocked last, and which of several threads due at once it names, are races between threads at one
guest time that leave the clock's values alone (two job workers woken together, either of which takes the job), and a block with no deadline is left out (a lock waited for
as long as its holder keeps it); --exact compares whole lines, all of them. The context shows whole lines.
Exit status 0 when the kept lines are the same, 1 when they differ, 2 on a usage error.
"""
import argparse
import gzip
import sys
from collections import Counter
from itertools import groupby

KINDS = 'OJPBTEHNRIK'
NAMES = {'O': 'origin', 'J': 'idle jump', 'P': 'present', 'B': 'block', 'T': 'thread start', 'E': 'host signal', 'H': 'host wake', 'N': 'native wait end',
         'R': 'first reading after a host wake', 'I': 'scripted input change', 'K': 'first reading of the input'}


def read(path, kinds, exact=False):
    """The trace's lines of the kinds kept, each as (guest time, line number in the file, text), in the order of their guest times; unless exact, without the blocks that
    have no deadline (a lock or an address waited for as long as another thread holds it: how often depends on the race, and they set no time)."""
    opener = gzip.open if path.endswith('.gz') else open
    out = []
    with opener(path, 'rt', encoding='utf-8', errors='replace') as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip('\n')
            if not line or line[0] not in kinds or (not exact and line[0] == 'B' and line.endswith(' -')):
                continue
            f3 = line.split(' ', 3)
            if len(f3) < 4 or not f3[1].isdigit() or not f3[2].isdigit():   # the last line of a trace cut by the process's exit (the host's graceful exit flushes nothing)
                continue
            out.append((int(f3[2]), n, line))
    out.sort(key=lambda x: (x[0], x[1]))   # a line written a moment after a later one (a reading, outside the host's lock) goes back to its time
    return out


def key(text, exact):
    """What a line is compared by, unless exact: its kind, its guest time and its own fields, without the jump count; an idle jump without its two thread fields, a block
    without its thread's handle (which of a pool's workers sleeps at one time is the race too), the first reading of an input change by its time and state alone (the frame it
    names is the last present's, which the thread that presents may count at that same time before or after the reading)."""
    if exact:
        return text
    f = text.split(' ')
    if f[0] == 'J':
        return ' '.join([f[0]] + f[2:5])
    if f[0] == 'B' and len(f) > 4:
        return ' '.join([f[0], f[2], f[3].partition(':')[2]] + f[4:])
    if f[0] == 'K' and len(f) > 4:
        return ' '.join([f[0], f[2]] + f[5:])
    return ' '.join([f[0]] + f[2:])


def groups(lines, exact):
    """The lines grouped by guest time: (time, sorted keys, the lines)."""
    return [(t, sorted(key(x[2], exact) for x in g), g) for t, g in ((t, list(g)) for t, g in groupby(lines, key=lambda x: x[0]))]


def first_difference(ga, gb):
    """Index of the first group (guest time) whose lines differ, or None when both are the same."""
    for i, (x, y) in enumerate(zip(ga, gb)):
        if x[0] != y[0] or x[1] != y[1]:
            return i
    return None if len(ga) == len(gb) else min(len(ga), len(gb))


def frame_before(lines, t):
    """The frame of the last present at or before guest time t (0 before the first present)."""
    f = 0
    for lt, _, text in lines:
        if lt > t:
            break
        if text.startswith('P '):
            parts = text.split()
            f = int(parts[3]) if len(parts) > 3 and parts[3].isdigit() else f
    return f


def describe(text):
    """A line spelled out: kind, idle jumps so far, guest time in ms, the rest."""
    parts = text.split(' ', 3)
    if len(parts) < 3 or not parts[1].isdigit() or not parts[2].isdigit():
        return text
    rest = parts[3] if len(parts) > 3 else ''
    return '%-31s jumps %-7s %14.6f ms  %s' % (NAMES.get(parts[0], parts[0]), parts[1], int(parts[2]) / 1e6, rest)


def compare(path_a, path_b, kinds=KINDS, context=8, out=None, exact=False):
    out = out or sys.stdout
    a, b = read(path_a, kinds, exact), read(path_b, kinds, exact)
    for tag, path, lines in (('A', path_a, a), ('B', path_b, b)):
        c = Counter(x[2][0] for x in lines)
        print('%s %s: %d lines (%s)' % (tag, path, len(lines), ', '.join('%s %d' % (k, c[k]) for k in kinds if c[k])), file=out)
    ga, gb = groups(a, exact), groups(b, exact)
    i = first_difference(ga, gb)
    if i is None:
        print('the same: %d lines of kinds %s at %d guest times' % (len(a), kinds, len(ga)), file=out)
        return 0
    t = min(g[i][0] for g in (ga, gb) if i < len(g))
    print('first difference at guest time %.6f ms (frame %d in A, %d in B), the %d-th time with events:' % (t / 1e6, frame_before(a, t), frame_before(b, t), i + 1), file=out)
    if i < len(ga) and i < len(gb) and ga[i][0] == gb[i][0]:   # the same time: what each has that the other has not
        ka, kb = Counter(ga[i][1]), Counter(gb[i][1])
        for tag, only in (('A', ka - kb), ('B', kb - ka)):
            items = sorted(only.elements())
            print('  only in %s at this time (%d): %s' % (tag, len(items), '; '.join(items[:context]) + (' ...' if len(items) > context else '')), file=out)
    before = [x for g in ga[:i] for x in g[2]][-context:] if context else []   # the last lines of the times both share
    for _, n, text in before:
        print('    %8d  %s' % (n, describe(text)), file=out)
    for tag, gs in (('A', ga), ('B', gb)):
        rest = [x for g in gs[i:] for x in g[2]]
        for _, n, text in rest[:max(context, len(gs[i][2]) if i < len(gs) else 0)]:
            print('  %s %8d  %s' % (tag, n, describe(text)), file=out)
        if i >= len(gs):
            print('  %s           (the trace ends here)' % tag, file=out)
    return 1


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    p.add_argument('a'); p.add_argument('b')
    p.add_argument('--kinds', default=KINDS, help='kinds of line to compare (default %s)' % KINDS)
    p.add_argument('--context', type=int, default=8, help='lines of context before and after the difference (default 8)')
    p.add_argument('--exact', action='store_true', help='compare whole lines (the jump count and an idle jump\'s thread fields too)')
    args = p.parse_args(argv)
    if not args.kinds or any(k not in KINDS for k in args.kinds):
        p.error('--kinds takes letters of %s' % KINDS)
    return compare(args.a, args.b, args.kinds, max(0, args.context), exact=args.exact)


if __name__ == '__main__':
    sys.exit(main())
