#!/usr/bin/env python3
"""The player's second pack end to end (CTest t_second_pack; docs/development.md, "The launcher"; dm/launcher/launcher_model.h, "The second pack"), on the synthetic game module
of dm/tools/tests/aot_synth.py (no game files): the launcher starts the real host on a packed game, the host runs the game's code from its pack and writes the JIT misses at exit, the
launcher's "a better pack is possible" turns on from them, its repack runs the packer, which runs `nxaot build --misses` and writes the launch file's made rows, the rule turns off,
and the game runs again from the new pack without the JIT fallback. In a games folder of its own (TMPDIR/t_second_pack):
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
     naming the image. The sample profile main.tsv is written only when a sample fell in compiled code, which the module's microseconds of guest code never give: no main.tsv
  3. --list: better_pack names 1 new JIT miss
  4. nxlauncher --repack: the packer again (the launcher's command, its output in repack.txt): `nxaot build --misses` compiles the hidden function (aot: misses: 1 addresses, 1 new
     entries; gate sites 3; no [RED] line for its gate), a new main.aot.so, the made rows with `misses main 1 <FNV-1a 64 of '<offset> indirect\n'>`
  5. --list: better_pack - (the packs took the misses)
  6. nxlauncher --play again: the hidden function runs from the new pack, its gate served by it ("0 not"), 0 JIT addresses (its call through a pointer now binds an inline cache to
     its standard entry: one dispatcher miss more than the first session's), no misses file written and the old one unchanged; --list: still -
Both plays run with DM_AOT_PROFILE_HZ=1 (PIN_HZ below): at the default rate nxhost-run dies with SIGSEGV at exit in about 60 % of the sessions in which the JIT fallback ran, after
the profile writer, because dynarmic's fault handler made the alternate signal stack of the thread that created the first JIT instance its own 2 MiB block
(dm/third_party/dynarmic/src/dynarmic/backend/exception_handler_posix.cpp SigHandler::SigHandler) and frees it in its static destructor at exit (~SigHandler), while the sampler's
CPU-time timer of that thread is still armed (dm/host/aot_profile.c dm_aot_profile_write stops counting, not the timer): the next SIGPROF cannot be delivered. At 1 Hz no
SIGPROF comes in these short sessions; the misses do not depend on the rate
  t_second_pack.py --build <CMake build folder> [--nxaot <nxaot>]
nxaot: --nxaot, else NXAOT in the environment. Without it, or when it does not run (built in the nxnative-aot image it links that image's libLLVM.so.18.1, which the
nxnative-build image has too), the test is skipped: exit 77 (CTest's SKIP_RETURN_CODE) with a line saying why. Exit 0 when every check holds, 1 otherwise"""
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
PIN_HZ = '1'   # DM_AOT_PROFILE_HZ for both plays: the docstring says why; drop it once the host's exit no longer runs on a freed alternate stack
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
    env = dict(env, DM_AOT_PROFILE_HZ=PIN_HZ)
    print(f'note: DM_AOT_PROFILE_HZ={PIN_HZ} for both plays: at the default rate nxhost-run dies with SIGSEGV at exit after a session that used the JIT fallback (dynarmic frees '
          'the alternate signal stack it installed while the sampler\'s timer still runs; t_second_pack.py\'s docstring)')
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
    check(not pfile.is_file() and f'aot: profile: no samples in any pack\'s code: nothing written to {prof}' in log,
          f'2. no {pfile.name}: no sample in compiled code (aot_profile.c writes a profile only with samples; the module\'s guest code runs for microseconds)')
    misses1 = mfile.read_bytes() if mfile.is_file() else b''

    # 3. The rule turns on
    lst = run([launcher, '--list', games], env)
    row = listed(lst)
    print(f'      better_pack: {row.get("better_pack")}')
    check('1 new JIT miss' in row.get('better_pack', ''), '3. --list: better_pack says 1 new JIT miss (profile_state against the made rows)')

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
    pack2 = (aot / 'main.aot.so').read_bytes() if (aot / 'main.aot.so').is_file() else b''
    check(bool(pack2) and pack2 != pack1, '4. a new main.aot.so in the balanced folder')
    made = made_of(launch)
    digest = fnv64(f'{hidden:x} indirect\n')
    print(f'      launch file: {made}')
    check(made == [('made', 'profile', 'yes'), ('made', 'misses', 'yes'), ('misses', 'main', f'1 {digest}')],
          f'4. the made rows: made profile yes, made misses yes, misses main 1 {digest} (FNV-1a 64 of "{hidden:x} indirect\\n", computed here)')

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
    check('nxhost-run: modules from' in (rd / 'log.prev.txt').read_text(errors='replace') if (rd / 'log.prev.txt').is_file() else False, '6. the first session\'s log kept as log.prev.txt')
    lst = run([launcher, '--list', games], env)
    row = listed(lst)
    print(f'      better_pack: {row.get("better_pack")}')
    check(lst.returncode == 0 and row.get('better_pack') == '-', '6. --list: still -')

    print(f't_second_pack: {checks} checks, {fails} failed ({time.time() - t_start:.1f} s)')
    if not fails: shutil.rmtree(root, ignore_errors=True)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
