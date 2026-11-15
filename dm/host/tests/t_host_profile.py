#!/usr/bin/env python3
"""One nxhost-run for every game profile (CTest t_host_profile; dm/host/game_rt.c; dm/games/game.py header --runtime, table, runtime): this tree's nxhost-run, configured
with one profile (cmake -DGAME, the default: host_tests.tsv's "# game:" line), runs t_host_program's synthetic module (dm/tools/tests/aot_synth.py, packed by
dm/tools/aot/tests/build_packs.sh into <packs>/module; no SDK, so DM_AOT_RAW=1; SDL's dummy drivers, the headless Vulkan presenter) under three profiles, and its log shows
each one's constants as game.py computes them: the profile line (id, name, where it came from), the guest address block's size (mem_reserve: hle_svc.c's "guest AS" line),
the frame step (fps_target: loader.c's line for the module's state page), and the run's status 0:
  1. no option: the default;
  2. --game <title ID>: the first profile of dm/games whose mem_reserve and fps_target both differ from the default's, from the table built into the program;
  3. --profile <file>: game.py runtime writes the file of a profile found through DM_GAMES_DIR in a temporary folder, a copy of the default's game.tsv with another title ID,
     name, mem_reserve and fps_target, the module list "main" (so the run names no module: the list is the profile's) and game_args (main_x86.c logs them as the profile's);
     then the same file through DM_GAME_PROFILE=<file>.
And the refusals, each a [RED] line naming what is wrong and status 2 before any module is read: --game of an unknown title ID, a file with an unknown key, a file without a
key, a malformed number, --game and a file together.
Without the packs (--packs empty and NXAOT_PACKS unset: the Windows runner without them, a tree built without them) only the refusals run, and a note says so.
  t_host_profile.py --build <build folder> [--packs <folder>]
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games'))
import game   # noqa: E402

NEW_ID = '0100AA0800000000'   # a title ID no game has (t_second_pack's is ...AA07...)
checks = fails = 0


def check(ok, what):
    global checks, fails
    checks += 1; fails += not ok
    print(f'{"ok" if ok else "FAIL":<5} {what}', flush=True)


def line_with(out, needle):
    return next((l for l in out.split('\n') if needle in l), '(no such line)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', required=True)
    ap.add_argument('--packs', default='')
    a = ap.parse_args()
    build = Path(a.build).resolve()
    exe = build / ('nxhost-run.exe' if os.name == 'nt' else 'nxhost-run')
    if not exe.is_file(): print(f'FAIL: no host program at {exe} (the CMake target nxhost-run, NXHOST_PROGRAM)'); return 1
    head = (build / 'host_tests.tsv').read_text(encoding='utf-8')
    default_id = next(l.split(':', 1)[1].strip() for l in head.split('\n') if l.startswith('# game:'))
    default = game.load(default_id)
    dc = game.constants(default)
    env = {k: v for k, v in os.environ.items() if k not in ('DM_GAME_PROFILE', 'DM_WINDOW', 'DM_FPS_TARGET', 'DM_ALIAS_SIZE', 'DM_PROGRAM_ID', 'DM_GAMES_DIR', 'DM_GAMES_FIRST')}
    env.update(SDL_VIDEO_DRIVER='dummy', SDL_AUDIO_DRIVER='dummy', DM_VK_HEADLESS='1', DM_DBG='1', DM_AOT_RAW='1')

    def run(args, extra=None):
        r = subprocess.run([str(exe)] + args, env=dict(env, **(extra or {})), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=240)
        return r.returncode, r.stdout.decode('utf-8', 'replace')

    tmp = Path(tempfile.mkdtemp(prefix='t_host_profile.'))
    try:
        # The profile files: the default's as game.py runtime writes it, and changed copies of it
        good = tmp / 'default.tsv'
        subprocess.run([sys.executable, str(REPO / 'dm/games/game.py'), 'runtime', default_id, str(good)], check=True, env=env)
        text = good.read_text(encoding='utf-8')

        def changed(name, key, row):   # the default's file with the row of key replaced (row None: removed; a key it lacks: row added)
            rows = [l for l in text.split('\n') if not l.startswith(key + '\t')]
            if row is not None: rows.insert(len(rows) - 1, row)
            p = tmp / name; p.write_text('\n'.join(rows), encoding='utf-8'); return p

        print(f't_host_profile: {exe}, default {default_id} ({dc["name"]})')
        # Refusals: before any module is read (no "modules from" line), whatever the folder
        nowhere = str(tmp / 'no-such-folder')
        for what, args, extra, want in (
                ('an unknown title ID', ['--game', '0123456789ABCDEF', nowhere], None, ['[RED] --game 0123456789ABCDEF: no such game profile']),
                ('a file with an unknown key', ['--profile', str(changed('unknown.tsv', 'bogus_key', 'bogus_key\t1')), nowhere], None, ['[RED] game profile file', "unknown key 'bogus_key'"]),
                ('a file without a key', ['--profile', str(changed('missing.tsv', 'mem_alias', None)), nowhere], None, ['[RED] game profile file', "missing key 'mem_alias'"]),
                ('a malformed number', ['--profile', str(changed('malformed.tsv', 'mem_reserve', 'mem_reserve\t0x14000000g')), nowhere], None,
                 ['[RED] game profile file', "key 'mem_reserve'", 'malformed']),
                ('--game and a file together', ['--game', default_id, nowhere], {'DM_GAME_PROFILE': str(good)}, ['[RED] --game', 'both name the game profile'])):
            st, out = run(args, extra)
            print(f'      {line_with(out, "[RED]")}')
            check(st == 2 and all(w in out for w in want) and 'modules from' not in out, f'{what}: refused with a [RED] line naming it, status {st} (2), no module read')

        packs = a.packs or os.environ.get('NXAOT_PACKS', '')
        module = Path(packs) / 'module' if packs else None
        if not module or not (module / 'main.aot.tsv').is_file():
            print(f'note: no synthetic module ({module / "main.aot.tsv" if module else "--packs and NXAOT_PACKS unset"}; dm/tools/aot/tests/build_packs.sh makes it): the runs under '
                  'three profiles are skipped')
            print(f't_host_profile: {checks} checks, {fails} failed')
            return fails != 0

        def ran(what, st, out, c, source, named=True):
            print('      ' + line_with(out, 'game profile '))
            print('      ' + line_with(out, 'guest AS '))
            print('      ' + line_with(out, '(game profile fps_target)'))
            frame = 'frame step as the game sets it (game profile fps_target)' if not c['fps_target'] else \
                    f'frame step in whole vsyncs decided by the host, capped at {c["fps_target"]} fps (game profile fps_target)'
            check(st == 0 and 'guest code ended: status 0 (the process ended)' in out, f'{what}: the synthetic module ran to its end, status {st}')
            check(f'game profile {c["id"]} ({c["name"]}): {source}' in out, f'{what}: the profile line names {c["id"]} ({c["name"]}) and its source')
            check(f'+{c["mem_reserve"]:#x}  heap' in line_with(out, 'guest AS '), f'{what}: the guest address block is its mem_reserve, {c["mem_reserve"]:#x}')
            check(frame in out, f'{what}: the frame step follows its fps_target, {c["fps_target"]}')
            if not named: check('aot: 1 modules from' in out and 'main: base' in out, f'{what}: its module list ({" ".join(c["modules"])}) loaded with no module named')

        # 1. The default
        st, out = run([str(module), 'main'])
        ran('the default', st, out, dc, 'the default the program was configured with (cmake -DGAME')
        # 2. A registered profile from the table, chosen by title ID
        other = next((g for g in game.all_games() if g.dir.parent == game.HERE and (lambda c: c['mem_reserve'] != dc['mem_reserve'] and c['fps_target'] != dc['fps_target'])(game.constants(g))), None)
        if not other: check(False, 'dm/games has a profile whose mem_reserve and fps_target both differ from the default\'s')
        else:
            st, out = run(['--game', other.id.lower(), str(module), 'main'])
            ran(f'--game {other.id.lower()}', st, out, game.constants(other), 'the table built into the program (--game)')
        # 3. A profile through DM_GAMES_DIR: a copy of the default's game.tsv with other constants, its file written by game.py runtime
        prof = tmp / 'profiles' / NEW_ID; prof.mkdir(parents=True)
        fps = 30 if dc['fps_target'] != 30 else 60
        new = {'title_id': NEW_ID, 'program_id': NEW_ID, 'name': 'A profile through DM_GAMES_DIR', 'mem_reserve': f'{dc["mem_reserve"] + 0x10000000:#x}', 'fps_target': str(fps),
               'modules': 'main', 'game_args': '-t_host_profile=1'}
        rows = []
        for l in (default.dir / 'game.tsv').read_text(encoding='utf-8').split('\n'):
            k = l.split('\t', 1)[0].strip()
            if k in new and not l.lstrip().startswith('#'): rows.append(f'{k}\t{new.pop(k)}')
            else: rows.append(l)
        rows += [f'{k}\t{v}' for k, v in new.items()]
        (prof / 'game.tsv').write_text('\n'.join(rows) + '\n', encoding='utf-8')
        pfile = tmp / f'{NEW_ID}.tsv'
        r = subprocess.run([sys.executable, str(REPO / 'dm/games/game.py'), 'runtime', NEW_ID, str(pfile)], env=dict(env, DM_GAMES_DIR=str(tmp / 'profiles')))
        check(r.returncode == 0 and pfile.is_file(), f'game.py runtime {NEW_ID} with DM_GAMES_DIR={tmp / "profiles"} writes the profile file')
        nc = game.constants(game.Game(prof))
        for how, args, extra in (('--profile', ['--profile', str(pfile), str(module)], None), ('DM_GAME_PROFILE', [str(module)], {'DM_GAME_PROFILE': str(pfile)})):
            st, out = run(args, extra)
            ran(f'a DM_GAMES_DIR profile through {how}', st, out, nc, f'the file {pfile} ({how})', named=False)
            print('      ' + line_with(out, 'game arguments'))
            check('game arguments from the game profile: -t_host_profile=1' in out, f'a DM_GAMES_DIR profile through {how}: its game_args')
        print(f't_host_profile: {checks} checks, {fails} failed')
        return fails != 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
