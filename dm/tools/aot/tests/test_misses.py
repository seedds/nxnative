#!/usr/bin/env python3
"""The host's JIT misses as discovery's input (CTest `misses`; docs/design/aot-recompiler.md sections 1.3, 3.3 and 11, README.md "JIT misses"): the x86-64 engine counts
every entry into its JIT fallback per guest address and reason and, with DM_AOT_PROFILE, writes them beside the profile as <module>.misses.tsv (dm/host/aot.h, "The JIT
fallback's misses"; t_aot_module's step 7 checks that file); `nxaot build --misses <file>` makes them roots of discovery.

The synthetic game module of dm/tools/tests/aot_synth.py, whose hidden function lies in a data block (no symbol, no FDE: discovery never decodes it) and has an observation
gate in the manifest, packed with a misses file in the host's exact form (this text is t_aot_module's run's file with the SHA-256 and the pack's path filled in) naming
the hidden function's address with the reason `indirect` (the dispatcher's miss at its call through a pointer):
- without the file: the build's "[RED] ... is in no discovered function's code" line for the hidden gate, gate sites 2;
- with it: `aot: misses: 1 addresses, 1 new entries, 0 resume points, 0 still unserved (none)`, the hidden function's two words taken out of the data block and compiled
  (nxaot discover lists it, provenance miss, a standard entry), no [RED] line for its gate, gate sites 3 (the descriptor serves it);
- the caches: with --cache, the misses' build misses the analysis cache (as a changed manifest does) and recompiles only the units they change; the same file again with
  other counts hits both (the entries' counts are in no key);
- a file of another image: a [RED] line naming both SHA-256s, the file ignored (gate sites 2); a malformed line stops the build, naming it;
- a file with a resume point (`nonlocal` at the return address after the entry's call of the hidden function: a continuation of that function's call sites, design 3.3)
  and an untranslated encoding (reported, stays a miss): `aot: misses: 3 addresses, 1 new entries, 1 resume points, 1 still unserved (untranslated 1)`, and the pack's
  resume points one more than without them.
Then a corpus NRO with a function taken from discovery's roots (`--no-root <its entry>`: none of discovery's own sources makes it an entry): nxaot discover does not list it,
its pack runs it in the JIT fallback (nxaot_host's mixed run counts JIT runs at its address, the output exactly corpus/expected); with a misses file naming it (`call`: a
bl to an address that is no compiled entry) it is listed again (provenance miss), the pack compiles it and the run needs no JIT at all.

  test_misses.py --nxaot <nxaot> --nxaot-host <nxaot_host> --work <dir>

Exit 0 when everything holds.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, '..', 'corpus')
AOT_SYNTH = os.path.join(HERE, '..', '..', 'tests', 'aot_synth.py')
# A misses file as the host writes it (dm/host/aot_profile.c write_misses; this text is t_aot_module's run's main.misses.tsv with the SHA-256 and the pack's path filled in)
HOST_MISSES = '''\
# nxaot misses: guest addresses the x86-64 engine's JIT fallback ran because the pack had no compiled code there (dm/host/aot_run.c, DM_AOT_PROFILE); nxaot build --misses <this file>
# module {module}, build ID {bid}, image SHA-256 {sha}
# pack {pack}
# {n} addresses, {entries} entries into the JIT fallback; {n} of the addresses this session, the rest from earlier sessions of this image
# <module offset> <reason> <entries>: indirect, call, host, nocode (an entry of the next pack), nonlocal (a resume point), fpcr, refused, untranslated, other (no pack serves them)
'''
MISSES_LINE = re.compile(r'^aot: misses: (\d+) addresses, (\d+) new entries, (\d+) resume points, (\d+) still unserved \((.*)\)$', re.M)


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def misses_file(path, module, bid, sha, pack, rows):
    """rows: (offset, reason, entries)"""
    with open(path, 'w') as f:
        f.write(HOST_MISSES.format(module=module, bid=bid, sha=sha, pack=pack, n=len({r[0] for r in rows}), entries=sum(r[2] for r in rows)))
        for off, reason, n in rows:
            f.write(f'{off:#x} {reason} {n}\n')


def functions(tsv):
    """nxaot discover --tsv's fn rows: {entry: (provenance, address taken)}; its miss rows: {offset: (reasons, result)}"""
    fns, misses = {}, {}
    for line in open(tsv):
        f = line.rstrip('\n').split('\t')
        if f[0] == 'fn':
            fns[int(f[1], 16)] = (f[4], f[15])
        elif f[0] == 'miss':
            misses[int(f[1], 16)] = (f[2], f[3])
    return fns, misses


def numbers(out):
    """The build log's numbers: gate sites, resume points, the misses line, the caches"""
    g = re.search(r'gate sites (\d+) \(design 1\.1\)', out)
    r = re.search(r'resume points (\d+)', out)
    m = MISSES_LINE.search(out)
    c = re.search(r'^aot: cache \S+: (\d+) hits, (\d+) misses', out, re.M)
    a = re.search(r'^aot: analysis cache: (\w+)', out, re.M)
    return {'gates': int(g.group(1)) if g else None, 'resumes': int(r.group(1)) if r else None, 'misses': m.groups() if m else None,
            'hits': int(c.group(1)) if c else None, 'unit_misses': int(c.group(2)) if c else None, 'analysis': a.group(1) if a else None}


class Checks:
    def __init__(self):
        self.bad = 0

    def __call__(self, ok, what, detail=''):
        self.bad += not ok
        print(f'{"ok  " if ok else "FAIL"} {what}' + (f'  [{detail}]' if detail and not ok else ''))
        return ok


def synthetic(a, check):
    synth = os.path.join(a.work, 'synth')
    shutil.rmtree(synth, ignore_errors=True)
    r = run([sys.executable, AOT_SYNTH, synth])
    if not check(r.returncode == 0, 'aot_synth.py writes the synthetic module', (r.stdout + r.stderr)[-300:]):
        return
    sys.path.insert(0, os.path.dirname(os.path.abspath(AOT_SYNTH)))
    import aot_synth as sm
    man = os.path.join(synth, 'main.aot.tsv')
    sha = hashlib.sha256(open(os.path.join(synth, 'main.image.bin'), 'rb').read()).hexdigest()
    hidden = sm.L['f_hidden']
    image = open(os.path.join(synth, 'main.image.bin'), 'rb').read()
    words = [int.from_bytes(image[i:i + 4], 'little') for i in range(0, sm.TEXT_END, 4)]
    ret_after = next((4 * i + 8 for i in range(len(words) - 1) if words[i] == 0xD28000E0 and words[i + 1] == 0xD63F0200), 0)   # mov x0, #7; blr x16: the call of the hidden function
    pack = os.path.join(synth, 'main.aot.so')
    cache = os.path.join(a.work, 'synth-cache')
    shutil.rmtree(cache, ignore_errors=True)
    host = os.path.join(a.work, 'main.misses.tsv')
    misses_file(host, 'main', sm.BUILD_ID, sha, pack, [(hidden, 'indirect', 1)])
    print('the host\'s misses file:')
    for line in open(host):
        print('    | ' + line.rstrip('\n'))

    # Without the misses: the hidden gate is in no discovered function's code
    b = run([a.nxaot, 'build', man, '--shared', os.path.join(a.work, 'synth.plain.so'), '--cache', cache])
    n0 = numbers(b.stdout)
    red_gate = f'the observation gate at main+{hidden:#x} is in no discovered function\'s code'
    check(b.returncode == 0 and n0['gates'] == 2 and red_gate in b.stdout and n0['misses'] is None,
          f'without misses: the hidden function main+{hidden:#x} is no discovered code, its gate is not served (gate sites {n0["gates"]})', b.stdout[-600:])
    # nxaot discover with the misses: the hidden function is an entry, with a standard entry (the dispatcher reaches it by its address)
    tsv = os.path.join(a.work, 'synth.discover.tsv')
    d = run([a.nxaot, 'discover', man, '--misses', host, '--tsv', tsv])
    fns, rows = functions(tsv) if d.returncode == 0 else ({}, {})
    check(d.returncode == 0 and fns.get(hidden, ('', ''))[0] == 'miss' and fns[hidden][1] == '1' and rows.get(hidden) == ('indirect', 'entry'),
          f'nxaot discover --misses: main+{hidden:#x} is a function (provenance {fns.get(hidden, ("none",))[0]}, address taken), its miss row says entry', d.stdout[-600:])
    # With the misses: compiled, its gate served; the analysis cache misses, the units mostly hit
    b = run([a.nxaot, 'build', man, '--shared', os.path.join(a.work, 'synth.misses.so'), '--cache', cache, '--misses', host])
    n1 = numbers(b.stdout)
    print('    ' + '\n    '.join(l for l in b.stdout.splitlines() if l.startswith(('aot: misses', '[RED]', 'aot: line table', 'aot: cache', 'aot: analysis cache'))))
    check(b.returncode == 0 and n1['misses'] == ('1', '1', '0', '0', 'none') and red_gate not in b.stdout and n1['gates'] == 3,
          f'with the misses: "aot: misses: 1 addresses, 1 new entries, 0 resume points, 0 still unserved (none)", no [RED] line for the hidden gate, gate sites {n1["gates"]}',
          b.stdout[-900:])
    check(n1['analysis'] == 'miss' and n1['unit_misses'] is not None and 1 <= n1['unit_misses'] and n1['hits'] >= 1,
          f'the caches: the analysis cache misses (the misses are in its key), {n1["unit_misses"]} of {(n1["hits"] or 0) + (n1["unit_misses"] or 0)} units compiled again')
    # The same addresses with other counts: no key changes
    again = os.path.join(a.work, 'main.misses.again.tsv')
    misses_file(again, 'main', sm.BUILD_ID, sha, pack, [(hidden, 'indirect', 7)])
    os.makedirs(os.path.join(a.work, 'again'), exist_ok=True)
    so_again = os.path.join(a.work, 'again', 'synth.misses.so')   # the same file name (the pack's soname)
    b = run([a.nxaot, 'build', man, '--shared', so_again, '--cache', cache, '--misses', again])
    n2 = numbers(b.stdout)
    same = os.path.isfile(so_again) and open(so_again, 'rb').read() == open(os.path.join(a.work, 'synth.misses.so'), 'rb').read()
    check(b.returncode == 0 and n2['analysis'] == 'hit' and n2['unit_misses'] == 0 and same,
          'another session\'s file with the same address (7 entries): the analysis cache hits, no unit compiled again, the same pack', b.stdout[-600:])
    # Another image's file: ignored
    other = os.path.join(a.work, 'main.misses.other.tsv')
    misses_file(other, 'main', sm.BUILD_ID, '0' * 64, pack, [(hidden, 'indirect', 1)])
    b = run([a.nxaot, 'build', man, '--shared', os.path.join(a.work, 'synth.other.so'), '--misses', other])
    n3 = numbers(b.stdout)
    red = [l for l in b.stdout.splitlines() if l.startswith('[RED] nxaot: the misses') and '0' * 64 in l and sha in l]
    check(b.returncode == 0 and red and n3['gates'] == 2 and n3['misses'] is None, 'a misses file of another image: a [RED] line with both SHA-256s, the file ignored',
          b.stdout[-600:])
    if red: print('    ' + red[0])
    # A malformed line stops the build
    broken = os.path.join(a.work, 'main.misses.broken.tsv')
    with open(broken, 'w') as f:
        f.write(f'# module main, build ID {sm.BUILD_ID}, image SHA-256 {sha}\n{hidden:#x} indirect 1\nnot a row\n')
    b = run([a.nxaot, 'build', man, '--shared', os.path.join(a.work, 'synth.broken.so'), '--misses', broken])
    check(b.returncode != 0 and 'main.misses.broken.tsv:3:' in b.stdout, 'a malformed misses line stops the build, naming line 3', (b.stdout + b.stderr)[-300:])
    # A resume point and an unserved miss beside the entry
    mixed = os.path.join(a.work, 'main.misses.mixed.tsv')
    misses_file(mixed, 'main', sm.BUILD_ID, sha, pack, [(ret_after, 'nonlocal', 1), (hidden, 'indirect', 1), (sm.L['f_obs'], 'untranslated', 2)])
    b = run([a.nxaot, 'build', man, '--shared', os.path.join(a.work, 'synth.mixed.so'), '--misses', mixed])
    n4 = numbers(b.stdout)
    red = [l for l in b.stdout.splitlines() if l.startswith('[RED] discover: the JIT miss at') and 'untranslated' in l]
    check(b.returncode == 0 and ret_after and n4['misses'] == ('3', '1', '1', '1', 'untranslated 1') and red and n4['resumes'] == (n1['resumes'] or 0) + 1,
          f'a resume point (main+{ret_after:#x}, after the call of the hidden function) and an untranslated encoding: 1 new entry, 1 resume point (the pack\'s '
          f'{n1["resumes"]} -> {n4["resumes"]}), 1 still unserved with its [RED] line', b.stdout[-900:])
    if red: print('    ' + red[0])


def corpus(a, check, nro='fnptr-gcc.nro', name='rt_check'):
    path = os.path.join(CORPUS, 'nro', nro)
    program = next(l.split('\t')[1] for l in open(os.path.join(CORPUS, 'corpus.tsv')) if l.startswith(nro + '\t'))
    expected = open(os.path.join(CORPUS, 'expected', program + '.out')).read()
    syms = {}
    for line in open(os.path.join(CORPUS, 'nro', nro[:-4] + '.syms.tsv')):
        f = line.rstrip('\n').split('\t')
        if len(f) >= 5 and not line.startswith('#'):
            syms[f[4]] = int(f[0], 16)
    x = syms[name]
    sha = hashlib.sha256(open(path, 'rb').read()).hexdigest()
    base = os.path.join(a.work, nro[:-4])
    d0 = run([a.nxaot, 'discover', path, '--tsv', base + '.all.tsv'])
    d1 = run([a.nxaot, 'discover', path, '--no-root', f'{x:#x}', '--tsv', base + '.held.tsv'])
    f0, _ = functions(base + '.all.tsv') if d0.returncode == 0 else ({}, {})
    f1, _ = functions(base + '.held.tsv') if d1.returncode == 0 else ({}, {})
    check(x in f0 and x not in f1, f'{nro}: {name} (+{x:#x}) is a discovered function ({f0.get(x, ("none",))[0]}), and with --no-root it is not')
    mf = base + '.misses.tsv'
    misses_file(mf, nro[:-4], '-', sha, base + '.held.so', [(x, 'call', 3)])
    d2 = run([a.nxaot, 'discover', path, '--no-root', f'{x:#x}', '--misses', mf, '--tsv', base + '.back.tsv'])
    f2, rows = functions(base + '.back.tsv') if d2.returncode == 0 else ({}, {})
    check('miss' in f2.get(x, ('',))[0].split(',') and rows.get(x) == ('call', 'entry'),
          f'{nro}: a misses file naming +{x:#x} (call) gives it back to discovery (provenance {f2.get(x, ("none",))[0]}, miss row {rows.get(x)})', d2.stdout[-400:])
    jit = re.compile(r'jit \(mixed\): (\d+) runs from (\d+) addresses')
    b = run([a.nxaot, 'build', path, '--no-root', f'{x:#x}', '--shared', base + '.held.so'])
    r = run([a.nxaot_host, base + '.held.so', path]) if b.returncode == 0 else None
    j = jit.search(r.stderr) if r else None
    held = (int(j.group(1)), int(j.group(2))) if j else None
    logged = [l for l in r.stderr.splitlines() if l.startswith('[RED] aot: ') and f'(module+{x:#x})' in l] if r else []
    check(r is not None and r.returncode == 0 and r.stdout == expected and held and held[0] >= 1 and logged,
          f'{nro} packed without {name}: the run prints its expected output with {name} in the JIT fallback ({held[0] if held else "?"} JIT runs from '
          f'{held[1] if held else "?"} addresses)', ((r.stderr if r else b.stdout) or '')[-400:])
    if logged: print('    ' + logged[0])
    b = run([a.nxaot, 'build', path, '--no-root', f'{x:#x}', '--misses', mf, '--shared', base + '.back.so'])
    n = numbers(b.stdout)
    r = run([a.nxaot_host, base + '.back.so', path]) if b.returncode == 0 else None
    j = jit.search(r.stderr) if r else None
    back = (int(j.group(1)), int(j.group(2))) if j else None
    print('    ' + '\n    '.join(l for l in b.stdout.splitlines() if l.startswith('aot: misses')))
    check(b.returncode == 0 and n['misses'] == ('1', '1', '0', '0', 'none') and r.returncode == 0 and r.stdout == expected and back == (0, 0),
          f'{nro} packed with the misses: {name} is compiled again (1 new entry) and the run needs no JIT fallback ({back[0] if back else "?"} runs)',
          ((r.stderr if r else '') + b.stdout)[-600:])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nxaot', required=True)
    ap.add_argument('--nxaot-host', required=True)
    ap.add_argument('--work', required=True)
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    check = Checks()
    synthetic(a, check)
    corpus(a, check)
    print(f'JIT misses: the synthetic module\'s hidden function and a corpus function held from discovery, {check.bad} failures')
    return 1 if check.bad else 0


if __name__ == '__main__':
    sys.exit(main())
