#!/usr/bin/env python3
"""Names the samples DM_PCSAMPLE wrote (pcsample.txt, hle_nvn.c pcs_main) and ranks them per sampled thread.

usage: dm/tools/pcsample.py <pcsample.txt> <the host binary: the app's DmIOS, or dmhost> [--top N]

Each sample is a running thread's pc, lr and frame-pointer frames. Addresses in the host binary are named with atos
(its load address is in the file's header); addresses in a guest module (the header lists them) as <module>+<offset>
rounded to 256 bytes; anything else is system code, counted under the nearest host function that called it.
Prints, per thread, the self share of each function and the inclusive share (a function anywhere in the sample)."""
import argparse, collections, re, subprocess, sys

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('samples'); ap.add_argument('binary'); ap.add_argument('--top', type=int, default=30)
    a = ap.parse_args()
    text = open(a.samples, errors='replace').read()
    head = text.split('\n', 1)[0]
    m = re.search(r'load address (0x[0-9a-f]+)', head)
    if not m: sys.exit('no DM_PCSAMPLE header in ' + a.samples)
    load = int(m[1], 16)
    names = dict((int(k), v) for k, v in re.findall(r' (\d+)=(.+?)(?= \d+=|$)', head.split(' names', 1)[1]))
    mods = [(n, int(b, 16), int(t, 16)) for n, b, t in re.findall(r'# guest module (\S+) (0x[0-9a-f]+) (0x[0-9a-f]+)', text)]
    samples = [(int(p[0]), [int(x, 16) for x in p[1:]]) for p in (l.split() for l in text.splitlines() if l and not l.startswith('#'))]
    seg = re.search(r'segname __TEXT\n\s+vmaddr (0x[0-9a-f]+)\n\s+vmsize (0x[0-9a-f]+)', subprocess.run(['otool', '-l', a.binary], capture_output=True, text=True).stdout)
    size = int(seg[2], 16)
    host = lambda x: load <= x < load + size
    def guest(x):
        for n, b, t in mods:
            if b <= x < b + t: return '%s+%#x' % (n, (x - b) & ~0xff)
        return None
    addrs = sorted({x for _, fr in samples for x in fr if host(x)}); sym = {}
    for i in range(0, len(addrs), 2000):
        chunk = addrs[i:i + 2000]
        out = subprocess.run(['atos', '-arch', 'arm64', '-o', a.binary, '-l', hex(load)] + [hex(x) for x in chunk], capture_output=True, text=True).stdout.splitlines()
        for x, s in zip(chunk, out): sym[x] = re.sub(r' \(in [^)]+\)', '', re.sub(r' \([^()]*:\d+\)$', '', s)).split(' + ')[0]
    for th in sorted({t for t, _ in samples}):
        rows = [fr for t, fr in samples if t == th]; n = len(rows)
        own, inc = collections.Counter(), collections.Counter()
        for fr in rows:
            pc = fr[0]
            if host(pc): own[sym[pc]] += 1
            elif guest(pc): own['[guest] ' + guest(pc)] += 1; inc['[guest code]'] += 1
            else: own['[system] <- ' + next((sym[x] for x in fr[1:] if host(x)), '?')] += 1
            for s in {sym[x] for x in fr if host(x)}: inc[s] += 1
        print('== %s: %d samples' % (names.get(th, th), n))
        print('  self (system code under its nearest host caller):')
        for s, c in own.most_common(a.top): print('   %5.1f%%  %s' % (100 * c / n, s))
        print('  inclusive:')
        for s, c in inc.most_common(a.top): print('   %5.1f%%  %s' % (100 * c / n, s))

if __name__ == '__main__':
    main()
