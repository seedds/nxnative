#!/usr/bin/env python3
"""The player's second pack end to end (CTest t_second_pack; docs/development.md, "The launcher"; dm/launcher/launcher_model.h, "The second pack"), on the synthetic game module
of dm/tools/tests/aot_synth.py (no game files): the launcher starts the real host on a packed game, the host runs the game's code from its pack and writes the JIT misses at exit, the
launcher's "a better pack is possible" turns on from them and from the sample profile the host writes, its repack runs the packer, which runs `nxaot build --profile --misses` and
writes the launch file's made rows, the rule turns off, and the game runs again from the new pack without the JIT fallback. In a games folder of its own (TMPDIR/t_second_pack):
  0. the game: <NSP folder>/<id>_build/aot_v0/ with the module's behaviour image and manifest (aot_synth.py's, with synth_hidden set in the image: the entry calls the hidden function,
     which has no symbol and no FDE, so nxaot's discovery does not compile it and only the JIT fallback can run it; t_aot_module sets the same word at run time), the launch file
     (launch-linux.tsv: path host, cwd, choices, log, profile, package, packer, python, nxaot; the pack row; env rows: the save, DM_WINDOW, DM_AOT_RAW=1 since the module has no SDK);
     the host row is a script that runs this build's nxhost-run with the module named (the launcher passes only the pack folder, and nxhost-run takes the module list from the game
     profile it was built with: this tree's is another game's)
  1. the first pack: the packer (dm/host/tests/second_pack/packer.py: the real packer's nxaot_options, aot_packs, made_rows, packs_ready on the prepared folder; no NSP, keys or
     extraction) run with the arguments the launcher's repack passes: main.aot.so from the manifest alone (nxaot's [RED] line for the hidden function's gate, gate sites 2), the
     made rows (made profile yes, made misses yes, no module rows); --list: the balanced pack, better_pack -
  2. nxlauncher --play: the host runs the module to ExitProcess with DM_AOT_PROFILE set to the profile row's folder (Play records by default); the hidden function in the JIT
     fallback (its gate unserved by the pack: the loader's [RED] line, "1 not"); at exit <profile>/main.misses.tsv with the hidden function's row `<offset> indirect 1` under the header
     naming the image; and the sample profile <profile>/main.tsv under the header naming the image, most of its samples in synth_hot's loop (add, subs, b.ne: the entry's last
     call runs aot_synth.py's HOT_PASSES passes of it, tens of milliseconds of compiled code; the loop's range from aot_synth.py's labels), each sampled function's rows after
     its line `# function 0x<entry> <samples> <rows>` (dm/host/aot_profile.c), synth_hot's among them
  3. --list: better_pack names 1 new JIT miss and the profile's sampled functions the packs were made without (made profile yes, no samples row)
  4. nxlauncher --repack: the packer again (the launcher's command, its output in repack.txt): `nxaot build --profile --misses` takes the profile (aot: profile ...: its samples, 0
     outside every function; aot: profile weights: at least 1 function weighed; aot: tiers with synth_hot at H) and compiles the hidden function (aot: misses: 1 addresses, 1 new
     entries; gate sites 3; no [RED] line for its gate), a new main.aot.so, the made rows with `samples main <functions> <samples> <FNV-1a 64 of their entries>` and
     `misses main 1 <FNV-1a 64 of '<offset> indirect\n'>`, both computed here from the files
  5. --list: better_pack - (the packs took the profile and the misses)
  6. nxlauncher --play again: the hidden function runs from the new pack, its gate served by it ("0 not"), 0 JIT addresses (its call through a pointer now binds an inline cache to
     its standard entry: one dispatcher miss more than the first session's), no misses file written and the old one unchanged; main.tsv written again with this session's samples,
     most in the loop; --list: what profile_state's rule says of this profile against the samples row, computed here from the file. The rule counts sampled functions (the
     function lines), not rows: the entry runs the same loop, so the sampled functions are synth_hot's (and whichever others a sample happened to land in) again, however many
     of its instructions this session sampled: `-` when they are the same functions; a session that sampled 10 % more functions, or another set of functions with 25 % more
     samples, shows its reason
"""
import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True   # no __pycache__ in the checkout (its children get PYTHONDONTWRITEBYTECODE=1)
REPO = Path(__file__).resolve().parents[3]
PACKER = REPO / 'dm/host/tests/second_pack/packer.py'
sys.path.insert(0, str(REPO / 'dm/tools/tests'))
sys.path.insert(0, str(REPO / 'dm/tools'))
SKIP = 77
ID = '0100AA0700000000'   # a title ID no game has (aot_synth.py's build ID is aa07...)
TITLE = 'Synthetic Game'

checks = fails = 0


def check(ok, what):
    global checks, fails
    checks += 1; fails += not ok
    print(f'{"ok  " if ok else "FAIL"} {what}', flush=True)
    return ok


def show(text, lines):   # the lines of text that hold any of lines' words, for the report
    for line in text.splitlines():
        if any(w in line for w in lines): print(f'      | {line}')


def fnv64(text):   # FNV-1a 64 (build_ipa_from_nsp.py fnv64, launcher_model.cpp fnv64), computed here on its own
    h = 0xcbf29ce484222325
    for b in text.encode():
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return f'{h:016x}'


def run(cmd, env, cwd=None, timeout=60):
    print(f'$ {" ".join(str(c) for c in cmd)}', flush=True)
    t0 = time.time()
    r = subprocess.run([str(c) for c in cmd], env=env, cwd=cwd, capture_output=True, text=True, errors='replace', timeout=timeout)
    print(f'  (status {r.returncode}, {time.time() - t0:.2f} s)', flush=True)
    return r


def rows_of(path):
    lines = path.read_text(encoding='utf-8').splitlines()
    return [tuple((line.split('\t', 2) + ['', ''])[:3]) for line in lines if line and not line.startswith('#')]


def made_of(path): return [r for r in rows_of(path) if r[0] in ('made', 'samples', 'misses')]


def listed(r):   # --list's row of the game: {column: value}
    cols = ['title_id', 'title', 'version', 'packs', 'last_played', 'folder', 'problem', 'better_pack']
    for line in r.stdout.splitlines():
        if line.startswith(ID + '\t'): return dict(zip(cols, line.split('\t')))
    return {}


def end_line(log):   # nxhost-run's end line: (status, dispatcher misses, JIT addresses), None without it
    m = re.search(r'guest code ended: status (\d+) \([^)]*\); \d+ SVCs, \d+ host calls, (\d+) dispatcher misses, (\d+) JIT addresses', log)
    return tuple(int(x) for x in m.groups()) if m else None


def misses_rows(path):   # the host's misses file: (header's image SHA-256, [(offset, reason, entries)])
    sha, rows = None, []
    for line in path.read_text().splitlines():
        m = re.match(r'^# module main, build ID \S+, image SHA-256 ([0-9a-f]{64})$', line)
        if m: sha = m.group(1)
        if line and not line.startswith('#'):
            o, why, n = line.split()
            rows.append((int(o, 16), why, int(n)))
    return sha, rows


def profile_rows(path):
    """The host's sample profile: (header's image SHA-256, {offset: samples} of its rows, {entry: samples} of its function lines `# function 0x<entry> <samples> <rows>`, whether
    every row follows a function line, each line is followed by as many rows as it says, none below its entry, and its samples are at least their sum)"""
    sha, rows, fns, ok, cur = None, {}, {}, True, None   # cur: [entry, samples, rows said, rows seen, their samples]
    def close():
        return cur is None or (cur[3] == cur[2] and cur[4] <= cur[1])
    for line in path.read_text().splitlines():
        m = re.match(r'^# module main, build ID \S+, image SHA-256 ([0-9a-f]{64})$', line)
        if m: sha = m.group(1)
        f = re.match(r'^# function 0x([0-9a-f]+) (\d+) (\d+)$', line)
        if f:
            ok = ok and close()
            cur = [int(f.group(1), 16), int(f.group(2)), int(f.group(3)), 0, 0]
            fns[cur[0]] = fns.get(cur[0], 0) + cur[1]
            continue
        body = line.split('#', 1)[0].split()
        if not body: continue
        o, n = int(body[0], 16), int(body[1])
        rows[o] = rows.get(o, 0) + n
        if cur is None or o < cur[0]: ok = False
        else: cur[3] += 1; cur[4] += n
    return sha, rows, fns, ok and close()


def recorded(rows, fns):
    """(sampled functions, samples, digest) as made_rows writes a samples row and profile_state reads the file (build_ipa_from_nsp.py profile_summary, launcher_model.cpp read_profile),
    computed here on its own: the function lines' entries with samples and their samples; for a file without them its rows' offsets and samples"""
    counts = fns or rows
    hot = sorted(o for o, n in counts.items() if n > 0)
    return len(hot), sum(counts.values()), fnv64(''.join(f'{o:x}\n' for o in hot))


def profile_reason(now, had):   # launcher_model.cpp profile_state's profile branch for a module whose packs took a profile (made profile yes, its samples row): its reason or ''
    (nc, ns, nd), (hc, hs, hd) = now, had
    if nc > hc and nc * 100 >= hc * 110: return f'{nc - hc:,} new sampled function' + ('' if nc - hc == 1 else 's')
    if nd != hd and nc >= hc and ns > hs and ns * 100 >= hs * 125: return f'a longer session\'s profile (+{(ns - hs) * 100 // hs if hs else 100} % samples)'
    return ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', required=True)
    ap.add_argument('--nxaot', default='')
    a = ap.parse_args()
    build = Path(a.build).resolve()
    host, launcher = build / 'nxhost-run', build / 'nxlauncher'
    nxaot = a.nxaot or os.environ.get('NXAOT', '')
    if not nxaot:
        print('t_second_pack: skipped: no nxaot (NXAOT=<path> in the environment, or the CMake option NXAOT; dm/tools/aot/README.md builds it in the nxnative-aot image)')
        return SKIP
    nxaot = Path(nxaot).resolve()
    try: usage = subprocess.run([str(nxaot), 'build'], capture_output=True, text=True, errors='replace', timeout=60)
    except OSError as e: usage = None; why = str(e)
    if usage is None or 'nxaot build' not in usage.stdout + usage.stderr:
        print(f't_second_pack: skipped: {nxaot} does not run as nxaot ({why if usage is None else (usage.stdout + usage.stderr).strip()[-300:] or "no output"}); built in the '
              'nxnative-aot image it links that image\'s libLLVM.so.18.1, which the nxnative-build image has too: run it there')
        return SKIP
    for p in (host, launcher):
        if not p.is_file(): print(f'FAIL: no {p} (the CMake targets nxhost-run and nxlauncher)'); return 1
    print(f't_second_pack: host {host}, launcher {launcher}, nxaot {nxaot}, packer {PACKER}')

    # 0. The game, packed as build_ipa_from_nsp.py --platform linux lays it out
    root = Path(os.environ.get('TMPDIR') or '/tmp') / 't_second_pack'
    shutil.rmtree(root, ignore_errors=True)
    games = root / 'games'; nsp_dir = games / 'synth'
    work, wdir = nsp_dir / f'{ID}_build', nsp_dir / f'{ID}-work'
    rd, prof, aot = wdir / 'run', wdir / 'profile', work / 'aot_v0'
    aot.mkdir(parents=True)
    import aot_synth as sm
    t = sm.aot_tables()
    img = bytearray(t['image'])
    hidden = sm.L['f_hidden']
    global HOT_LOOP, HOT_ENTRY
    HOT_LOOP = (sm.L['f_hot_loop'], sm.L['f_hot_loop'] + 12)   # synth_hot's loop: add, subs, b.ne (aot_synth.py's labels, from which its image is assembled)
    HOT_ENTRY = sm.L['f_hot']                                   # synth_hot's entry: its function line's
    assert struct.unpack_from('<I', img, sm.HIDDEN)[0] == 0
    struct.pack_into('<I', img, sm.HIDDEN, hidden ^ sm.HIDDEN_KEY)   # synth_hidden: the entry calls the hidden function (aot_synth.py HIDDEN)
    t['image'] = bytes(img)
    sha = hashlib.sha256(t['image']).hexdigest()
    (aot / 'main.image.bin').write_bytes(t['image'])
    (aot / 'main.aot.tsv').write_text(sm.manifest_text(t))
    print(f'the game: {work}: main.image.bin (SHA-256 {sha}, synth_hidden = main+{hidden:#x} ^ {sm.HIDDEN_KEY:#x}), main.aot.tsv')
    wrapper = root / 'bin/nxhost-run'
    wrapper.parent.mkdir(parents=True)
    wrapper.write_text('#!/bin/sh\n# the launch file\'s host: this build\'s nxhost-run with the module named (its game profile lists another game\'s modules)\n'
                       f'dir=$1; shift\nexec \'{host}\' "$dir" main "$@"\n')
    wrapper.chmod(0o755)
    rows = [('game', 'title', TITLE), ('game', 'title_id', ID), ('game', 'version', '1.0.0'), ('game', 'package', 'v0'), ('game', 'platform', 'linux'), ('game', 'graphics', 'nvn'),
            ('path', 'host', wrapper), ('path', 'cwd', rd), ('path', 'choices', rd / 'env.txt'), ('path', 'log', rd / 'log.txt'), ('path', 'profile', prof),
            ('path', 'package', nsp_dir), ('path', 'packer', PACKER), ('path', 'python', sys.executable), ('path', 'nxaot', nxaot),
            ('pack', 'balanced', aot),
            ('env', 'DM_ROOT', work), ('env', 'DM_SAVE', rd / 'save'), ('env', 'DM_TEMP', rd / 'temp'), ('env', 'DM_LANG', 'en-US'), ('env', 'DM_WINDOW', '1'),
            ('env', 'DM_AOT_RAW', '1'), ('env', 'DM_DBG', '1')]
    launch = work / 'launch-linux.tsv'
    launch.write_text('# nxnative launch 1\n# the synthetic module, written by t_second_pack.py\n' + ''.join('\t'.join(str(x) for x in r) + '\n' for r in rows))
    env = {k: v for k, v in os.environ.items() if k not in ('NXHOST', 'NXPACK', 'DM_AOT_PROFILE', 'DM_AOT_PROFILE_HZ', 'DM_LAUNCHER', 'DM_SAVED_CHOICES')}
    env.update(SDL_VIDEO_DRIVER='dummy', SDL_VIDEODRIVER='dummy', SDL_AUDIO_DRIVER='dummy', DM_VK_HEADLESS='1', XDG_CONFIG_HOME=str(root / 'config'), PYTHONDONTWRITEBYTECODE='1')
    t_start = time.time()

    # 1. The first pack: the packer as the launcher's repack runs it (repack_command's argv), on an empty profile folder
    r = run([sys.executable, PACKER, nsp_dir, '--platform', 'linux', '--pack-mode', 'balanced', '--nxaot', nxaot], env, cwd=nsp_dir, timeout=300)
    print(r.stdout + r.stderr, end='')
    nxlog = (work / 'logs/nxaot_main.log').read_text(errors='replace') if (work / 'logs/nxaot_main.log').is_file() else ''
    red_gate = f'the observation gate at main+{hidden:#x} is in no discovered function\'s code'
    show(nxlog, ['gate sites', red_gate, 'aot: misses'])
    check(r.returncode == 0 and (aot / 'main.aot.so').is_file(), '1. the first pack: main.aot.so built by nxaot from the manifest')
    check('gate sites 2 (design 1.1)' in nxlog and red_gate in nxlog and 'aot: misses' not in nxlog,
          f'1. no misses yet: the hidden function main+{hidden:#x} is no discovered code, its gate not served (nxaot: gate sites 2 and its [RED] line)')
    made = made_of(launch)
    print(f'      launch file: {made}')
    check(made == [('made', 'profile', 'yes'), ('made', 'misses', 'yes')], '1. the launch file\'s made rows: made profile yes, made misses yes, no module rows (made_rows)')
    check([(n, v) for k, n, v in rows_of(launch) if k == 'pack'] == [('balanced', str(aot))], '1. the pack row: balanced (packs_ready)')
    lst = run([launcher, '--list', games], env)
    row = listed(lst)
    print(f'      {row}')
    check(lst.returncode == 0 and row.get('packs') == 'balanced' and row.get('problem') == '-' and row.get('better_pack') == '-',
          '1. --list: the game with its balanced pack, no problem, better_pack - (no profile recorded)')
    pack1 = (aot / 'main.aot.so').read_bytes()

    # 2. Play: the host on the pack, recording
    r = run([launcher, '--games', games, '--play', ID], env)
    print(r.stderr, end='')
    log = (rd / 'log.txt').read_text(errors='replace') if (rd / 'log.txt').is_file() else ''
    show(log, ['nxhost-run: modules from', 'DM_AOT_PROFILE', 'gates (', 'that its pack does not call', '[guest]', 'guest code ended', 'aot: misses', 'aot: profile'])
    end = end1 = end_line(log)
    check(r.returncode == 0 and f'nxhost-run: modules from {aot};' in log and 'with the JIT fallback' in log and end and end[0] == 0,
          f'2. --play: nxlauncher started nxhost-run on {aot} (the JIT fallback in it), and the module ran to ExitProcess: status 0 (launcher status {r.returncode})')
    check('every guest thread sampled' in log and f'the profile goes to {prof}/<module>.tsv' in log, '2. DM_AOT_PROFILE set to the launch file\'s profile folder (Play records by default)')
    check('aot: main: 2 gates (2 observation gates, 1 spin wait back-edges) called by its pack, 1 not' in log and
          f'has an observation gate at main+{hidden:#x} that its pack does not call' in log, '2. the loader: the hidden function\'s gate is the one the pack does not call ([RED] line, 1 not)')
    check('[guest] aot module: done' in log, '2. the module\'s OutputDebugString line')
    check(end is not None and end[2] >= 1, f'2. the JIT fallback ran guest code: {end[2] if end else "?"} JIT addresses, {end[1] if end else "?"} dispatcher misses')
    mfile = prof / 'main.misses.tsv'
    msha, mrows = misses_rows(mfile) if mfile.is_file() else (None, [])
    if mfile.is_file(): print('      main.misses.tsv:\n' + ''.join(f'      | {line}\n' for line in mfile.read_text().splitlines()))
    check(msha == sha and mrows == [(hidden, 'indirect', 1)] and f'aot: misses: {mfile}: 1 addresses, 1 entries into the JIT fallback, 1 of the addresses this session (main)' in log,
          f'2. {mfile}: the hidden function\'s row ({hidden:#x} indirect 1) under the header naming this image')
    pfile = prof / 'main.tsv'
    psha, prows, pfns, plines = profile_rows(pfile) if pfile.is_file() else (None, {}, {}, False)
    if pfile.is_file(): print('      main.tsv:\n' + ''.join(f'      | {line}\n' for line in pfile.read_text().splitlines()))
    total = sum(prows.values())
    in_loop = sum(n for o, n in prows.items() if HOT_LOOP[0] <= o < HOT_LOOP[1])
    top = max(prows, key=prows.get) if prows else None
    check(psha == sha and prows and f'aot: profile: {pfile}: ' in log and plines and pfns.get(HOT_ENTRY, 0) > 0,
          f'2. {pfile}: the sample profile written at exit under the header naming this image ({len(prows)} rows, {total} samples), each sampled function\'s rows after its '
          f'function line, synth_hot\'s (main+{HOT_ENTRY:#x}) among them: {", ".join(f"{e:#x} {n}" for e, n in sorted(pfns.items()))}')
    check(top is not None and HOT_LOOP[0] <= top < HOT_LOOP[1] and in_loop * 2 > total,
          f'2. its samples in synth_hot\'s loop [main+{HOT_LOOP[0]:#x}, main+{HOT_LOOP[1]:#x}) (add, subs, b.ne: aot_synth.py\'s labels): {in_loop} of {total}, the most at '
          f'main+{top if top is not None else 0:#x} ({prows.get(top, 0)})')
    rec1 = recorded(prows, pfns)
    misses1 = mfile.read_bytes() if mfile.is_file() else b''

    # 3. The rule turns on
    lst = run([launcher, '--list', games], env)
    row = listed(lst)
    print(f'      better_pack: {row.get("better_pack")}')
    without = f'{rec1[0]} sampled function{"" if rec1[0] == 1 else "s"} the packs were made without'
    check(row.get('better_pack') == f'1 new JIT miss; {without}',
          f'3. --list: better_pack says 1 new JIT miss; {without} (profile_state against the made rows: made profile yes, no samples row)')

    # 4. The repack, through the launcher
    r = run([launcher, '--games', games, '--repack', ID], env, timeout=300)
    print(r.stderr, end='')
    rlog = (rd / 'repack.txt').read_text(errors='replace') if (rd / 'repack.txt').is_file() else ''
    print(''.join(f'      repack.txt | {line}\n' for line in rlog.splitlines()), end='')
    want = ['--platform', 'linux', '--pack-mode', 'balanced', '--nxaot', str(nxaot)]
    check(r.returncode == 0 and 'the packer started for the balanced pack' in r.stderr and 'nxlauncher: the packer exited with status 0' in r.stderr and
          'packer stand-in: 1 packs built (balanced)' in rlog,
          f'4. --repack: the launcher ran the launch file\'s packer ({" ".join(want)}) and waited: status 0')
    nxlog = (work / 'logs/nxaot_main.log').read_text(errors='replace') if (work / 'logs/nxaot_main.log').is_file() else ''
    show(nxlog, ['gate sites', red_gate, 'aot: misses'])
    check('aot: misses: 1 addresses, 1 new entries, 0 resume points, 0 still unserved (none)' in nxlog and 'gate sites 3 (design 1.1)' in nxlog and red_gate not in nxlog,
          '4. nxaot build --misses: the hidden function compiled (1 new entry), its gate served (gate sites 3), no [RED] line for it')
    show(nxlog, ['aot: profile', 'aot: tiers'])
    pw = re.search(r'^aot: profile weights: (\d+) functions, (\d+) branches weighted', nxlog, re.M)
    tiers = re.search(r'^aot: tiers C (\d+) W (\d+) H (\d+) H\+ (\d+) \(functions\)$', nxlog, re.M)
    check(f'aot: profile {prof / "main.tsv"}: {total} samples at {len(prows)} offsets, 0 outside every function' in nxlog and pw and int(pw.group(1)) >= 1 and
          tiers and int(tiers.group(3)) >= 1,
          f'4. nxaot build --profile: the profile\'s rows taken ({total} samples at {len(prows)} offsets: nxaot skips the function lines), {pw.group(1) if pw else "?"} functions '
          f'weighed, {tiers.group(3) if tiers else "?"} at tier H')
    pack2 = (aot / 'main.aot.so').read_bytes() if (aot / 'main.aot.so').is_file() else b''
    check(bool(pack2) and pack2 != pack1, '4. a new main.aot.so in the balanced folder')
    made = made_of(launch)
    digest = fnv64(f'{hidden:x} indirect\n')
    print(f'      launch file: {made}')
    samples_row = f'{rec1[0]} {rec1[1]} {rec1[2]}'
    check(made == [('made', 'profile', 'yes'), ('made', 'misses', 'yes'), ('samples', 'main', samples_row), ('misses', 'main', f'1 {digest}')],
          f'4. the made rows: made profile yes, made misses yes, samples main {samples_row} (sampled functions, their samples, FNV-1a 64 of their entries, computed here), '
          f'misses main 1 {digest} (FNV-1a 64 of "{hidden:x} indirect\\n")')

    # 5. The rule turns off
    lst = run([launcher, '--list', games], env)
    row = listed(lst)
    print(f'      better_pack: {row.get("better_pack")}')
    check(lst.returncode == 0 and row.get('better_pack') == '-' and row.get('packs') == 'balanced', '5. --list: better_pack - (the packs took the misses)')

    # 6. Play from the new pack
    r = run([launcher, '--games', games, '--play', ID], env)
    print(r.stderr, end='')
    log = (rd / 'log.txt').read_text(errors='replace') if (rd / 'log.txt').is_file() else ''
    show(log, ['gates (', 'that its pack does not call', '[guest]', 'guest code ended', 'aot: misses', 'aot: profile'])
    end = end_line(log)
    check(r.returncode == 0 and end and end[0] == 0 and '[guest] aot module: done' in log, '6. --play again: the module ran to ExitProcess, status 0')
    check('aot: main: 3 gates (2 observation gates, 1 spin wait back-edges) called by its pack, 0 not' in log and 'that its pack does not call' not in log,
          '6. the loader: the new pack calls every gate, the hidden function\'s too (0 not, no [RED] line)')
    check(end is not None and end1 is not None and end[2] == 0 and end[1] == end1[1] + 1,
          f'6. no JIT fallback: 0 JIT addresses ({end[2] if end else "?"}); {end[1] if end else "?"} dispatcher misses, the first session\'s {end1[1] if end1 else "?"} and the hidden '
          'function\'s call binding its inline cache to its new standard entry (aot.h dm_aot_stats_t misses)')
    check('aot: misses: the JIT fallback ran for no pack this session: no misses file written' in log and mfile.is_file() and mfile.read_bytes() == misses1,
          f'6. no misses file written; {mfile.name} unchanged')
    psha2, prows2, pfns2, plines2 = profile_rows(pfile) if pfile.is_file() else (None, {}, {}, False)
    rec2 = recorded(prows2, pfns2)
    print(f'      main.tsv: {len(prows2)} rows: ' + ', '.join(f'{o:#x} {n}' for o, n in sorted(prows2.items())) + '; function lines: ' +
          ', '.join(f'{e:#x} {n}' for e, n in sorted(pfns2.items())))
    check(psha2 == sha and f'aot: profile: {pfile}: ' in log and plines2 and sum(n for o, n in prows2.items() if HOT_LOOP[0] <= o < HOT_LOOP[1]) * 2 > sum(prows2.values()),
          f'6. {pfile.name} written again, this session\'s: {len(prows2)} rows in {rec2[0]} sampled functions, {rec2[1]} samples, most in synth_hot\'s loop (the first session: '
          f'{len(prows)} rows in {rec1[0]} functions, {rec1[1]} samples)')
    check('nxhost-run: modules from' in (rd / 'log.prev.txt').read_text(errors='replace') if (rd / 'log.prev.txt').is_file() else False, '6. the first session\'s log kept as log.prev.txt')
    lst = run([launcher, '--list', games], env)
    row = listed(lst)
    print(f'      better_pack: {row.get("better_pack")}')
    want = profile_reason(rec2, rec1) or '-'
    check(lst.returncode == 0 and row.get('better_pack') == want,
          f'6. --list: {want} (the rule on this session\'s profile against the samples row: {rec2[0]} sampled functions vs {rec1[0]}, {rec2[1]} samples vs {rec1[1]}, '
          f'{"the same" if rec2[2] == rec1[2] else "another"} hot set)')

    print(f't_second_pack: {checks} checks, {fails} failed ({time.time() - t_start:.1f} s)')
    if not fails: shutil.rmtree(root, ignore_errors=True)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
