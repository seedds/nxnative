#!/usr/bin/env python3
"""The player's second pack end to end (CTest t_second_pack; docs/development.md, "The launcher"; dm/launcher/launcher_model.h, "The second pack"), on the synthetic game module
of dm/tools/tests/aot_synth.py (no game files): the launcher starts the real host on a packed game, the host runs the game's code from its pack and writes the JIT misses at exit, the
launcher's "a better pack is possible" turns on from them and from the sample profile and the known targets the host writes, its repack runs the packer, which runs `nxaot build
--profile --misses` (nxaot reads the targets beside the profile) and writes the launch file's made rows, the rule turns off, and the game runs again from the new pack without the JIT
fallback. In a games folder of its own (TMPDIR/t_second_pack):
  0. the game: <NSP folder>/<id>_build/aot_v0/ with the module's behaviour image and manifest (aot_synth.py's, with synth_hidden set in the image: the entry calls the hidden function,
     which has no symbol and no FDE, so nxaot's discovery does not compile it and only the JIT fallback can run it; t_aot_module sets the same word at run time), its game profile
     where the real packer puts a provisional one, <id>_build/profile/<id>/game.tsv (a copy of this tree's default profile, host_tests.tsv's "# game:", with this title ID, the
     name Synthetic Game and the module list "main"), the launch file (launch-linux.tsv: path host, game_profile, cwd, choices, log, profile, package, packer, python, nxaot; the
     pack row; env rows: the save, DM_WINDOW, DM_AOT_RAW=1 since the module has no SDK); the host row is this build's nxhost-run itself, one program for every game, which the
     launcher starts with --profile <the game_profile row's file> and the pack folder: the profile's module list names the module
  1. the first pack: the packer (dm/host/tests/second_pack/packer.py: the real packer's port_game_profile, nxaot_options, aot_packs, made_rows, packs_ready on the prepared folder;
     no NSP, keys or extraction) run with the arguments the launcher's repack passes: <id>_build/game-profile-linux.tsv written by game.py runtime from the provisional profile
     (found through DM_GAMES_DIR, as the real packer finds it) and named by the launch file's game_profile row; main.aot.so from the manifest alone (nxaot's [RED] line for the
     hidden function's gate, gate sites 2), the made rows (made profile yes, made misses yes, no module rows); --list: the balanced pack, better_pack -
  2. nxlauncher --play: the host's profile line names the provisional profile and its file (--profile); it runs the module to ExitProcess with DM_AOT_PROFILE set to the profile
     row's folder (Play records by default); the hidden function in the JIT
     fallback (its gate unserved by the pack: the loader's [RED] line, "1 not"); at exit <profile>/main.misses.tsv with the hidden function's row `<offset> indirect 1` under the header
     naming the image; and the sample profile <profile>/main.tsv under the header naming the image, most of its samples in synth_hot's loop (add, subs, b.ne: the entry's last
     call runs aot_synth.py's HOT_PASSES passes of it, tens of milliseconds of compiled code; the loop's range from aot_synth.py's labels), each sampled function's rows after
     its line `# function 0x<entry> <samples> <rows>` (dm/host/aot_profile.c), synth_hot's among them; and the known targets <profile>/main.targets.tsv (dm/host/aot_targets.c)
     under the header naming the image: the entry's call through synth_fnptr (`blr x16` at no_fault + 12) bound to synth_observed, once; the hidden function's call is no row
     (its target has no compiled entry: the JIT fallback's, the misses file has it)
  3. --list: better_pack names 1 new JIT miss, the profile's sampled functions and the known call target the packs were made without (made profile yes and made targets yes, no
     samples or targets row)
  4. nxlauncher --repack, after the profile's game.tsv changed its name (a newer bundle's profile): the packer again (the launcher's command, its output in repack.txt) writes the
     game profile's file again with the new name; `nxaot build --profile --misses` takes the profile (aot: profile ...: its samples, 0
     outside every function; aot: profile weights: at least 1 function weighed; aot: tiers with synth_hot at H) and compiles the hidden function (aot: misses: 1 addresses, 1 new
     entries; gate sites 3; no [RED] line for its gate; aot: profile targets: the file's sites), a new main.aot.so, the made rows with `samples main <functions> <samples>
     <FNV-1a 64 of their entries>`, `misses main 1 <FNV-1a 64 of '<offset> indirect\n'>` and `targets main 1 <FNV-1a 64 of '<site>:<synth_observed>\n'>`, computed here from
     the files
  5. --list: better_pack - (the packs took the profile, the misses and the known targets)
  6. nxlauncher --play again: the host's profile line names the profile's new name; the hidden function runs from the new pack, its gate served by it ("0 not"), 0 JIT addresses (its call through a pointer now binds an inline cache to
     its standard entry: one dispatcher miss more than the first session's), no misses file written and the old one unchanged; main.tsv written again with this session's samples,
     most in the loop; main.targets.tsv merged with the first session's: the hidden function's call site now bound to it (a new site with a dominant target); --list: what
     profile_state's rule says of this profile and these targets against the samples and targets rows, computed here from the files (the new site is one more than the one the
     packs took: `1 new known call target`, beside what the profile's rule says). The rule counts sampled functions (the
     function lines), not rows: the entry runs the same loop, so the sampled functions are synth_hot's (and whichever others a sample happened to land in) again, however many
     of its instructions this session sampled: `-` when they are the same functions; a session that sampled 10 % more functions, or another set of functions with 25 % more
     samples, shows its reason
That is the Linux run with nxaot and the JIT fallback (33 checks). What else it runs, the platform from sys.platform (PLATFORMS below holds every difference):
  Windows  nxhost-run.exe and nxlauncher.exe, launch-windows.tsv and game-profile-windows.tsv, main.aot.dll packs, the packer with --platform windows, APPDATA for the launcher's
           own settings
  no JIT   a host built without the JIT fallback (CMake NXHOST_JIT off: the Windows cross-build's image has no Boost), which the first session's log says: the hidden function
           stops the module with status 3 and the [RED] line that it needs the JIT fallback, its miss recorded all the same (aot_run.c note_jit), before synth_hot's loop (no
           profile to check); the repack is then what makes the game run to its end. The rule's answers are computed here from the files either way
  no nxaot there is no Windows nxaot: the packs dm/tools/aot/tests/build_packs.sh prebuilt with nxaot for this game (<packs>/second_pack: the folder --packs or NXAOT_PACKS names,
           the Windows packs folder on Windows), which the packer stand-in puts in place of its nxaot build when T_SECOND_PACK_PREBUILT names that folder (packer.py prebuilt):
           the first pack from the manifest alone, the repack with the misses a first session records (`0x<hidden> indirect 1`); every step runs as above (the launcher's
           repack too), but the packs took no profile (made profile unused), so `nxaot build --profile` is not checked and the rule names only misses (no profile and no
           known targets: made targets unused)
  t_second_pack.py --prebuild <folder>   writes that game for build_packs.sh: main.image.bin, main.aot.tsv and main.misses.tsv
  t_second_pack.py --dry linux|windows   runs nothing but game.py runtime: writes the platform's game, profile and launch file into a temporary folder (the platform as
                                         sys.platform would give it), prints them and compares the launch file with what the packer's port_launch_file writes for --platform
                                         <platform>, and the profile file with what its port_game_profile writes
"""
import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
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
PREBUILT = 'second_pack'   # the folder of the prebuilt packs in the packs folder (build_packs.sh)

# Every difference between the platforms: the programs' suffix, the launch file the launcher reads there (launcher_model.cpp find_games: launch-<kPlatform>.tsv), a pack's name
# after its module's (kPackSuffix, aot.h DM_AOT_PACK_SUFFIX), the packer's --platform (which names the game profile's file, game-profile-<platform>.tsv), and the variable that
# names the launcher's own settings folder (launcher_model.cpp config_path)
PLATFORMS = {
    'linux': dict(exe='', launch='launch-linux.tsv', pack='.aot.so', packer='linux', config='XDG_CONFIG_HOME'),
    'windows': dict(exe='.exe', launch='launch-windows.tsv', pack='.aot.dll', packer='windows', config='APPDATA'),
}


def platform_of(sys_platform): return 'windows' if sys_platform == 'win32' else 'linux'


checks = fails = 0


def check(ok, what):
    global checks, fails
    checks += 1; fails += not ok
    print(f'{"ok  " if ok else "FAIL"} {what}', flush=True)
    return ok


def skipped(what): print(f'[skipped] {what}', flush=True)


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


def made_of(path): return [r for r in rows_of(path) if r[0] in ('made', 'samples', 'misses', 'targets')]


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


def targets_rows(path):   # the host's known targets (dm/host/aot_targets.c): (header's image SHA-256, [(site, target, binds)])
    sha, rows = None, []
    for line in path.read_text().splitlines():
        m = re.match(r'^# module main, build ID \S+, image SHA-256 ([0-9a-f]{64})$', line)
        if m: sha = m.group(1)
        body = line.split('#', 1)[0].split()
        if body: rows.append((int(body[0], 16), int(body[1], 16), int(body[2])))
    return sha, rows


def dominant(rows):
    """The (site, dominant target) pairs by site, nxaot's rule (dm/tools/aot/src/build/pack.cpp TargetsFromProfile; build_ipa_from_nsp.py targets_summary, launcher_model.cpp
    read_targets), computed here on its own: a site's binds per target, the target with the most (the lowest of equal ones) when it has at least 90 % of them"""
    sites = {}
    for s, t, n in rows:
        if n: sites.setdefault(s, {}); sites[s][t] = sites[s].get(t, 0) + n
    out = []
    for s in sorted(sites):
        t = min(sites[s], key=lambda x: (-sites[s][x], x))
        if 10 * sites[s][t] >= 9 * sum(sites[s].values()): out.append((s, t))
    return out


def pairs_digest(pairs): return fnv64(''.join(f'{s:x}:{t:x}\n' for s, t in pairs))   # a targets row's digest (build_ipa_from_nsp.py targets_digest)


def counted(n, one, many): return f'{n:,} {one if n == 1 else many}'   # launcher_model.cpp counted


def better_pack(launch, prof, sha):
    """What --list's better_pack says of the module main (launcher_model.cpp profile_state, the same rule, computed here on its own from the profile folder's files of this image and
    the launch file's made rows): the reasons joined with '; ', '-' for none"""
    if not prof.is_dir() or not any(prof.glob('*.tsv')): return '-'   # no file recorded: no reason
    made = {(k, n): v for k, n, v in made_of(launch)}
    reasons, mfile, pfile, tfile = [], prof / 'main.misses.tsv', prof / 'main.tsv', prof / 'main.targets.tsv'
    if made.get(('made', 'misses')) == 'yes' and mfile.is_file():
        msha, mrows = misses_rows(mfile)
        if msha == sha and mrows:
            pairs = sorted({(o, why) for o, why, _ in mrows})
            now = (len({o for o, _ in pairs}), fnv64(''.join(f'{o:x} {why}\n' for o, why in pairs)))
            had = made.get(('misses', 'main'), '0 ').split(' ')
            if now[0] > int(had[0]): reasons.append(counted(now[0] - int(had[0]), 'new JIT miss', 'new JIT misses'))
            elif now[1] != had[1]: reasons.append('JIT misses the packs did not take')
    if made.get(('made', 'profile')) in ('yes', 'no') and pfile.is_file():
        psha, rows, fns, _ = profile_rows(pfile)
        now = recorded(rows, fns)
        if psha in (None, sha) and now[0]:
            had = made.get(('samples', 'main'))
            if made[('made', 'profile')] != 'yes' or had is None: reasons.append(counted(now[0], 'sampled function', 'sampled functions') + ' the packs were made without')
            else:
                c, s, d = had.split(' ')
                why = profile_reason(now, (int(c), int(s), d))
                if why: reasons.append(why)
    # the known targets: read beside a profile of this image only; made targets, or for a launch file before that row as made profile says
    mt = made.get(('made', 'targets')) or ('unused' if made.get(('made', 'profile')) == 'unused' else '')
    if mt != 'unused' and tfile.is_file() and pfile.is_file() and profile_rows(pfile)[0] in (None, sha):
        tsha, trows = targets_rows(tfile)
        pairs = dominant(trows) if tsha == sha else []
        had = made.get(('targets', 'main'))
        if pairs and (mt != 'yes' or had is None): reasons.append(counted(len(pairs), 'known call target', 'known call targets') + ' the packs were made without')
        elif pairs:
            hc, hd = int(had.split(' ')[0]), had.split(' ')[1]
            more = len(pairs) - hc if len(pairs) > hc else 1 if len(pairs) == hc and pairs_digest(pairs) != hd else 0   # at least that many sites the packs lack
            if more and more * 100 >= hc * 10: reasons.append(counted(more, 'new known call target', 'new known call targets'))
    return '; '.join(reasons) or '-'


def write_game(aot):
    """The game in aot: main.image.bin (aot_synth.py's image with synth_hidden set: the entry calls the hidden function, aot_synth.py HIDDEN) and main.aot.tsv, its manifest.
    Returns {sha, hidden, build_id, hot_loop, hot_entry}"""
    import aot_synth as sm
    t = sm.aot_tables()
    img = bytearray(t['image'])
    hidden = sm.L['f_hidden']
    assert struct.unpack_from('<I', img, sm.HIDDEN)[0] == 0
    struct.pack_into('<I', img, sm.HIDDEN, hidden ^ sm.HIDDEN_KEY)
    t['image'] = bytes(img)
    aot.mkdir(parents=True, exist_ok=True)
    (aot / 'main.image.bin').write_bytes(t['image'])
    manifest = sm.manifest_text(t)
    (aot / 'main.aot.tsv').write_text(manifest, newline='\n')
    return dict(sha=hashlib.sha256(t['image']).hexdigest(), hidden=hidden, build_id=manifest.split('\n', 1)[0].split('\t')[2], key=sm.HIDDEN_KEY,
                hot_loop=(sm.L['f_hot_loop'], sm.L['f_hot_loop'] + 12),   # synth_hot's loop: add, subs, b.ne (aot_synth.py's labels, from which its image is assembled)
                hot_entry=sm.L['f_hot'],                                  # synth_hot's entry: its function line's
                fnptr_site=sm.L['no_fault'] + 12, observed=sm.L['f_obs'],  # the entry's call through synth_fnptr (adrp, ldr, movz, blr x16) and its target, synth_observed
                hidden_site=sm.L['no_hidden'] - 8)                        # the entry's call of the hidden function (blr x16, then the str of its result)


def first_misses(g):   # the misses file a first session of the game records (the host's header lines that nxaot and the packer read, and its one row)
    return (f'# nxaot misses: what a first session of t_second_pack\'s game records (dm/host/tests/t_second_pack.py --prebuild; the host writes them in dm/host/aot_profile.c)\n'
            f'# module main, build ID {g["build_id"]}, image SHA-256 {g["sha"]}\n'
            f'0x{g["hidden"]:x} indirect 1\n')


def default_profile(build):   # the profile this tree was configured with (cmake -DGAME; host_tests.tsv's "# game:" line), which the synthetic module ran under before
    head = (build / 'host_tests.tsv').read_text(encoding='utf-8') if (build / 'host_tests.tsv').is_file() else ''
    return next((l.split(':', 1)[1].strip() for l in head.split('\n') if l.startswith('# game:')), None)


def write_profile(work, default_id, name=TITLE):
    """The game's profile where the real packer writes a provisional one (<id>_build/profile/<id>/game.tsv, found through DM_GAMES_DIR: build_ipa_from_nsp.py main): the default
    profile's game.tsv (its memory layout and constants, which the synthetic module has always run under) with this title ID, name and the module list "main". Returns its folder"""
    sys.path.insert(0, str(REPO / 'dm/games'))
    import game
    d = work / 'profile' / ID
    d.mkdir(parents=True, exist_ok=True)
    new, rows = {'title_id': ID, 'program_id': ID, 'name': name, 'modules': 'main'}, []
    for line in (game.load(default_id).dir / 'game.tsv').read_text(encoding='utf-8').split('\n'):
        k = line.split('\t', 1)[0].strip()
        if k in new and not line.lstrip().startswith('#'): rows.append(f'{k}\t{new.pop(k)}')
        elif line.strip(): rows.append(line)
    (d / 'game.tsv').write_text('\n'.join(rows + [f'{k}\t{v}' for k, v in new.items()]) + '\n', encoding='utf-8', newline='\n')
    return d.resolve()   # as game.py names a profile's folder (Game.dir), which the profile file's first line gives


def profile_file(work, plat): return work / f'game-profile-{plat["packer"]}.tsv'   # build_ipa_from_nsp.py game_profile_file


def launch_rows(plat, host, rd, prof, nsp_dir, nxaot, aot, work):
    return [('game', 'title', TITLE), ('game', 'title_id', ID), ('game', 'version', '1.0.0'), ('game', 'package', 'v0'), ('game', 'platform', plat['packer']),
            ('game', 'graphics', 'nvn'),
            ('path', 'host', host), ('path', 'game_profile', profile_file(work, plat)), ('path', 'cwd', rd), ('path', 'choices', rd / 'env.txt'), ('path', 'log', rd / 'log.txt'),
            ('path', 'profile', prof),
            ('path', 'package', nsp_dir), ('path', 'packer', PACKER), ('path', 'python', sys.executable)] + ([('path', 'nxaot', nxaot)] if nxaot else []) + \
           [('pack', 'balanced', aot),
            ('env', 'DM_ROOT', work), ('env', 'DM_SAVE', rd / 'save'), ('env', 'DM_TEMP', rd / 'temp'), ('env', 'DM_LANG', 'en-US'), ('env', 'DM_WINDOW', '1'),
            ('env', 'DM_AOT_RAW', '1'), ('env', 'DM_DBG', '1')]


def write_launch(path, rows):
    path.write_text('# nxnative launch 1\n# the synthetic module, written by t_second_pack.py\n' + ''.join('\t'.join(str(x) for x in r) + '\n' for r in rows), encoding='utf-8',
                    newline='\n')


def prebuild(folder):   # --prebuild: the game and its first session's misses, for build_packs.sh's prebuilt packs
    g = write_game(folder)
    (folder / 'main.misses.tsv').write_text(first_misses(g), newline='\n')
    print(f't_second_pack.py --prebuild: {folder}: main.image.bin (SHA-256 {g["sha"]}), main.aot.tsv, main.misses.tsv (0x{g["hidden"]:x} indirect 1)')
    return 0


def dry(name):
    """--dry: the platform's game folder, profile and launch file as a run writes them, nothing run but game.py runtime; the launch file against port_launch_file's for --platform
    <platform>, the profile file against what port_game_profile writes"""
    import importlib.util
    import types
    plat_name = platform_of({'windows': 'win32', 'linux': 'linux'}[name])   # as sys.platform gives it on that platform
    plat = PLATFORMS[plat_name]
    spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
    nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
    sys.path.insert(0, str(REPO / 'dm/games'))
    import game
    default_id = next(p.id for p in game.all_games() if p.dir.parent == game.HERE)   # a registered profile, as a tree's -DGAME is
    with tempfile.TemporaryDirectory(prefix='t_second_pack_dry_') as tmp:
        root = Path(tmp)
        build = root / 'build'
        nsp_dir = root / 'games/synth'
        work, wdir = nsp_dir / f'{ID}_build', nsp_dir / f'{ID}-work'
        rd, prof, aot = wdir / 'run', wdir / 'profile', work / 'aot_v0'
        g = write_game(aot)
        pdir = write_profile(work, default_id)
        host, launcher = build / f'nxhost-run{plat["exe"]}', build / f'nxlauncher{plat["exe"]}'
        launch = work / plat['launch']
        nxaot = build / f'nxaot{plat["exe"]}'
        rows = launch_rows(plat, host, rd, prof, nsp_dir, nxaot, aot, work)
        write_launch(launch, rows)
        # the game profile's file as the packer stand-in writes it: the real packer's port_game_profile, the provisional profile found through DM_GAMES_DIR
        gprof = nsp.port_game_profile(sys.executable, REPO, work, types.SimpleNamespace(id=ID), plat_name, dict(os.environ, DM_GAMES_DIR=str(pdir.parent)), work / 'logs')
        print(f'--dry {name} (sys.platform {"win32" if name == "windows" else "linux"}: {plat_name}): host {host}, launcher {launcher}, pack {aot / ("main" + plat["pack"])}')
        print(f'{pdir / "game.tsv"} (from {default_id}):\n' + ''.join(f'  | {line}\n' for line in (pdir / 'game.tsv').read_text().splitlines() if not line.startswith('#')), end='')
        print(f'{gprof.name}:\n' + ''.join(f'  | {line}\n' for line in gprof.read_text().splitlines()), end='')
        print(f'{launch.name}:\n' + ''.join(f'  | {line}\n' for line in launch.read_text().splitlines()), end='')
        # The packer's own launch file for the platform (its rows for a pack of this folder, its paths under a stand-in repository)
        pw, pwork = root / 'port', root / 'port' / f'{ID}_build'
        pwork.mkdir(parents=True)
        made = [('made', 'profile', 'yes'), ('made', 'misses', 'yes'), ('made', 'targets', 'yes')]
        port = nsp.port_launch_file(plat_name, root / 'repo', pwork, pw / f'{ID}-work', aot, root / 'extracted', root / 'msl', None, 'gl', 'en-US',
                                    types.SimpleNamespace(id=ID, table=lambda n: root / n), TITLE, '1.0.0', 'v0', ['balanced'],
                                    repack={'package': nsp_dir, 'packer': PACKER, 'python': sys.executable, 'nxaot': nxaot}, made=made)
        print(f'port_launch_file({plat_name!r}, ...): {port.name}:\n' + ''.join(f'  | {line}\n' for line in port.read_text().splitlines() if not line.startswith('# ')), end='')
        prow = rows_of(port)
        mine = rows_of(launch)
        names = lambda rs, k: [n for kk, n, _ in rs if kk == k]
        port_host = nsp.port_host(root / 'repo', plat_name)[0]
        check(port.name == launch.name, f'the launch file\'s name: {launch.name} (port_launch_file: {port.name})')
        check(port.read_text().split('\n', 1)[0] == launch.read_text().split('\n', 1)[0] == nsp.LAUNCH_HEAD, f'its first line: {nsp.LAUNCH_HEAD}')
        check(names(mine, 'game') == names(prow, 'game') and dict((n, v) for k, n, v in prow if k == 'game')['platform'] == plat['packer'] ==
              dict((n, v) for k, n, v in mine if k == 'game')['platform'], f'the game rows, in order, platform {plat["packer"]}')
        check(all(n in names(prow, 'path') for n in names(mine, 'path')) and
              [n for n in names(prow, 'path') if n in names(mine, 'path')] == names(mine, 'path'), f'the path rows {", ".join(names(mine, "path"))}: the packer\'s, in its order')
        check([(n, v) for k, n, v in mine if k == 'pack'] == [(n, v) for k, n, v in prow if k == 'pack'] == [('balanced', str(nsp.pack_dir(aot, 'balanced')))],
              f'the pack row: balanced {aot} (pack_dir)')
        kinds = lambda rs: [k for i, (k, _, _) in enumerate(rs) if not i or rs[i - 1][0] != k]
        check(kinds(prow) == ['game', 'path', 'made', 'pack', 'env'] and kinds(mine) == ['game', 'path', 'pack', 'env'],
              'the rows\' order: game, path, (made: the packer stand-in puts them before the pack rows, as port_launch_file does), pack, env')
        check(plat['pack'] == nsp.PACK_SUFFIXES[plat_name] and plat['exe'] == port_host.suffix and host.name == port_host.name,
              f'the pack suffix {plat["pack"]} (PACK_SUFFIXES) and the host\'s name {host.name} (port_host: {port_host.name})')
        pfile = dict((n, v) for k, n, v in prow if k == 'path')['game_profile']
        prof_rows = dict(l.split('\t', 1) for l in gprof.read_text(encoding='utf-8').splitlines() if l and not l.startswith('#'))
        check(gprof == profile_file(work, plat) == Path(dict((n, v) for k, n, v in mine if k == 'path')['game_profile']) and Path(pfile).name == gprof.name,
              f'the game profile\'s file {gprof.name}: where port_game_profile writes it and the launch file\'s game_profile row names it (port_launch_file: {Path(pfile).name})')
        check((prof_rows.get('id'), prof_rows.get('name'), prof_rows.get('modules')) == (ID, TITLE, 'main') and
              gprof.read_text(encoding='utf-8') == game.runtime(game.Game(pdir)),
              f'{gprof.name}: game.py runtime of the provisional profile {pdir} (id {ID}, name {TITLE}, modules main)')
        check((aot / 'main.aot.tsv').read_text().split('\n', 1)[0].split('\t')[3] == g['sha'], f'main.aot.tsv names the image\'s SHA-256 {g["sha"][:16]}...')
    print(f't_second_pack --dry {name}: {checks} checks, {fails} failed')
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', help='the build folder that holds nxhost-run and nxlauncher')
    ap.add_argument('--nxaot', default='')
    ap.add_argument('--packs', default='', help=f'the packs folder of build_packs.sh (else NXAOT_PACKS): its {PREBUILT}/ when there is no nxaot')
    ap.add_argument('--prebuild', metavar='FOLDER', help='write the game and its first session\'s misses into FOLDER (build_packs.sh) and exit')
    ap.add_argument('--dry', choices=('linux', 'windows'), help='write and compare the platform\'s launch file and wrapper; run nothing')
    a = ap.parse_args()
    if a.prebuild: return prebuild(Path(a.prebuild))
    if a.dry: return dry(a.dry)
    if not a.build: ap.error('--build is required')
    plat_name = platform_of(sys.platform)
    plat = PLATFORMS[plat_name]
    build = Path(a.build).resolve()
    host, launcher = build / f'nxhost-run{plat["exe"]}', build / f'nxlauncher{plat["exe"]}'
    # The recompiler: nxaot when it runs here, else the prebuilt packs (route)
    nxaot, route, why = a.nxaot or os.environ.get('NXAOT', ''), None, 'no nxaot (NXAOT=<path> in the environment, or the CMake option NXAOT)'
    if nxaot:
        nxaot = Path(nxaot).resolve()
        try: usage = subprocess.run([str(nxaot), 'build'], capture_output=True, text=True, errors='replace', timeout=60)
        except OSError as e: usage, err = None, str(e)
        if usage is not None and 'nxaot build' in usage.stdout + usage.stderr: route = 'nxaot'
        else: why = (f'{nxaot} does not run as nxaot ({err if usage is None else (usage.stdout + usage.stderr).strip()[-300:] or "no output"}); built in the nxnative-aot image it '
                     'links that image\'s libLLVM.so.18.1, which the nxnative-build image has too: run it there')
    packs = a.packs or os.environ.get('NXAOT_PACKS', '')
    prebuilt = Path(packs).resolve() / PREBUILT if packs else None
    if not route:
        if prebuilt and all((prebuilt / d / f'main{plat["pack"]}').is_file() for d in ('first', 'misses')):
            route, nxaot = 'prebuilt', ''
            print(f't_second_pack: {why}: the prebuilt packs of {prebuilt} instead')
        else:
            print(f't_second_pack: skipped: {why}, and no prebuilt packs ({prebuilt or "NXAOT_PACKS / --packs not set"}: dm/tools/aot/tests/build_packs.sh writes '
                  f'<out>/{PREBUILT} and <out>/win64/{PREBUILT}; on Windows the win64-packs artifact of the aot workflow)')
            return SKIP
    for p in (host, launcher):
        if not p.is_file(): print(f'FAIL: no {p} (the CMake targets nxhost-run and nxlauncher)'); return 1
    print(f't_second_pack: {plat_name}: host {host}, launcher {launcher}, ' + (f'nxaot {nxaot}' if route == 'nxaot' else f'no nxaot: the prebuilt packs of {prebuilt}') +
          f', packer {PACKER}')

    # 0. The game, packed as build_ipa_from_nsp.py --platform <platform> lays it out
    root = Path(os.environ.get('TMPDIR') or tempfile.gettempdir()) / 't_second_pack'
    shutil.rmtree(root, ignore_errors=True)
    games = root / 'games'; nsp_dir = games / 'synth'
    work, wdir = nsp_dir / f'{ID}_build', nsp_dir / f'{ID}-work'
    rd, prof, aot = wdir / 'run', wdir / 'profile', work / 'aot_v0'
    g = write_game(aot)
    sha, hidden = g['sha'], g['hidden']
    hot_loop, hot_entry = g['hot_loop'], g['hot_entry']
    print(f'the game: {work}: main.image.bin (SHA-256 {sha}, synth_hidden = main+{hidden:#x} ^ {g["key"]:#x}), main.aot.tsv')
    if route == 'prebuilt' and not check((prebuilt / 'main.aot.tsv').is_file() and (prebuilt / 'main.aot.tsv').read_bytes() == (aot / 'main.aot.tsv').read_bytes() and
                                         (prebuilt / 'main.image.bin').read_bytes() == (aot / 'main.image.bin').read_bytes(),
                                         f'0. the prebuilt packs\' game ({prebuilt}) is this one: its manifest and image byte for byte (build_packs.sh of this commit)'):
        print(f't_second_pack: {checks} checks, {fails} failed'); return 1
    default_id = default_profile(build)
    if not default_id: print(f'FAIL: no "# game:" line in {build / "host_tests.tsv"} (the profile the tree was configured with)'); return 1
    pdir = write_profile(work, default_id)
    gprof = profile_file(work, plat)
    print(f'its game profile: {pdir / "game.tsv"} (a provisional profile where the packer puts one: {default_id}\'s with this title ID, {TITLE}, modules main)')
    launch = work / plat['launch']
    write_launch(launch, launch_rows(plat, host, rd, prof, nsp_dir, nxaot, aot, work))
    env = {k: v for k, v in os.environ.items() if k.upper() not in ('NXHOST', 'NXPACK', 'DM_AOT_PROFILE', 'DM_AOT_PROFILE_HZ', 'DM_LAUNCHER', 'DM_SAVED_CHOICES',
                                                                     'T_SECOND_PACK_PREBUILT', plat['config'])}
    env.update(SDL_VIDEO_DRIVER='dummy', SDL_VIDEODRIVER='dummy', SDL_AUDIO_DRIVER='dummy', DM_VK_HEADLESS='1', PYTHONDONTWRITEBYTECODE='1')
    env[plat['config']] = str(root / 'config')   # the launcher's own settings (launcher.txt) in the test's folder, not the user's
    if route == 'prebuilt': env['T_SECOND_PACK_PREBUILT'] = str(prebuilt)   # the packer stand-in puts these packs in place of its nxaot build (packer.py prebuilt)
    pack_name = f'main{plat["pack"]}'
    made_by = 'nxaot' if route == 'nxaot' else f'nxaot (prebuilt by build_packs.sh: {prebuilt / "first" / pack_name})'
    t_start = time.time()

    # 1. The first pack: the packer as the launcher's repack runs it (repack_command's argv), on an empty profile folder
    r = run([sys.executable, PACKER, nsp_dir, '--platform', plat['packer'], '--pack-mode', 'balanced'] + (['--nxaot', nxaot] if nxaot else []), env, cwd=nsp_dir, timeout=300)
    print(r.stdout + r.stderr, end='')
    nxlog = (work / 'logs/nxaot_main.log').read_text(errors='replace') if (work / 'logs/nxaot_main.log').is_file() else ''
    red_gate = f'the observation gate at main+{hidden:#x} is in no discovered function\'s code'
    show(nxlog, ['gate sites', red_gate, 'aot: misses'])
    check(r.returncode == 0 and (aot / pack_name).is_file(), f'1. the first pack: {pack_name} built by {made_by} from the manifest')
    prow = dict(l.split('\t', 1) for l in gprof.read_text(encoding='utf-8').splitlines() if l and not l.startswith('#')) if gprof.is_file() else {}
    print(f'      {gprof.name}: id {prow.get("id")}, name {prow.get("name")}, modules {prow.get("modules")}; {gprof.read_text(encoding="utf-8").splitlines()[0] if gprof.is_file() else ""}')
    check((prow.get('id'), prow.get('name'), prow.get('modules')) == (ID, TITLE, 'main') and f'from {pdir}/game.tsv' in gprof.read_text(encoding='utf-8') and
          ('path', 'game_profile', str(gprof)) in rows_of(launch),
          f'1. {gprof.name}: written by the packer (game.py runtime) from the provisional profile {pdir} (DM_GAMES_DIR), named by the launch file\'s game_profile row')
    check('gate sites 2 (design 1.1)' in nxlog and red_gate in nxlog and 'aot: misses' not in nxlog,
          f'1. no misses yet: the hidden function main+{hidden:#x} is no discovered code, its gate not served (nxaot: gate sites 2 and its [RED] line)')
    made = made_of(launch)
    print(f'      launch file: {made}')
    took = 'yes' if route == 'nxaot' else 'unused'   # the prebuilt packs took no profile: the nxaot the stand-in names has no --profile (made_rows)
    check(made == [('made', 'profile', took), ('made', 'misses', 'yes'), ('made', 'targets', took)],
          f'1. the launch file\'s made rows: made profile {took}, made misses yes, made targets {took}, no module rows (made_rows)')
    check([(n, v) for k, n, v in rows_of(launch) if k == 'pack'] == [('balanced', str(aot))], '1. the pack row: balanced (packs_ready)')
    lst = run([launcher, '--list', games], env)
    row = listed(lst)
    print(f'      {row}')
    check(lst.returncode == 0 and row.get('packs') == 'balanced' and row.get('problem') == '-' and row.get('better_pack') == '-',
          '1. --list: the game with its balanced pack, no problem, better_pack - (no profile recorded)')
    pack1 = (aot / pack_name).read_bytes()

    # 2. Play: the host on the pack, recording
    r = run([launcher, '--games', games, '--play', ID], env)
    print(r.stderr, end='')
    log = (rd / 'log.txt').read_text(errors='replace') if (rd / 'log.txt').is_file() else ''
    show(log, ['game profile ', 'nxhost-run: modules from', 'DM_AOT_PROFILE', 'gates (', 'that its pack does not call', 'needs the JIT fallback', '[guest]', 'guest code ended',
               'aot: misses', 'aot: profile'])
    check(f'[dm] game profile {ID} ({TITLE}): the file {gprof} (--profile)' in log.splitlines(),
          f'2. the host\'s profile line: {ID} ({TITLE}) from the file {gprof.name} (--profile, which the launcher passes from the game_profile row)')
    end = end1 = end_line(log)
    jit = 'with the JIT fallback' in log   # the host's engine, as its first line says (main_x86.c): a host built without it stops at the hidden function
    red_jit = f'at main+{hidden:#x} needs the JIT fallback (design section 11), which this host is built without'
    if jit:
        check(r.returncode == 0 and f'nxhost-run: modules from {aot};' in log and end and end[0] == 0,
              f'2. --play: nxlauncher started nxhost-run on {aot} (the JIT fallback in it), and the module ran to ExitProcess: status 0 (launcher status {r.returncode})')
    else:
        check(r.returncode == 3 and f'nxhost-run: modules from {aot};' in log and 'without the JIT fallback' in log and end and end[0] == 3 and red_jit in log,
              f'2. --play: nxlauncher started nxhost-run on {aot} (built without the JIT fallback), and the hidden function stopped the module: its [RED] line, status 3 (the JIT '
              f'fallback needed; launcher status {r.returncode})')
    hp = lambda name: f'{prof}/{name}'   # a path in the profile folder as the host writes it (the folder DM_AOT_PROFILE names, '/', the name)
    check('every guest thread sampled' in log and f'the profile goes to {hp("<module>.tsv")}' in log, '2. DM_AOT_PROFILE set to the launch file\'s profile folder (Play records by default)')
    check('aot: main: 2 gates (2 observation gates, 1 spin wait back-edges) called by its pack, 1 not' in log and
          f'has an observation gate at main+{hidden:#x} that its pack does not call' in log, '2. the loader: the hidden function\'s gate is the one the pack does not call ([RED] line, 1 not)')
    if jit: check('[guest] aot module: done' in log, '2. the module\'s OutputDebugString line')
    else: check('[guest] aot module: done' not in log, '2. no OutputDebugString line: the module stopped at the hidden function, before its end')
    check(end is not None and end[2] >= 1, f'2. the JIT fallback {"ran" if jit else "was needed for"} guest code: {end[2] if end else "?"} JIT addresses, '
          f'{end[1] if end else "?"} dispatcher misses')
    mfile = prof / 'main.misses.tsv'
    msha, mrows = misses_rows(mfile) if mfile.is_file() else (None, [])
    if mfile.is_file(): print('      main.misses.tsv:\n' + ''.join(f'      | {line}\n' for line in mfile.read_text().splitlines()))
    check(msha == sha and mrows == [(hidden, 'indirect', 1)] and
          f'aot: misses: {hp("main.misses.tsv")}: 1 addresses, 1 entries into the JIT fallback, 1 of the addresses this session (main)' in log,
          f'2. {mfile}: the hidden function\'s row ({hidden:#x} indirect 1) under the header naming this image')
    pfile = prof / 'main.tsv'
    psha, prows, pfns, plines = profile_rows(pfile) if pfile.is_file() else (None, {}, {}, False)
    if pfile.is_file(): print('      main.tsv:\n' + ''.join(f'      | {line}\n' for line in pfile.read_text().splitlines()))
    total = sum(prows.values())
    in_loop = sum(n for o, n in prows.items() if hot_loop[0] <= o < hot_loop[1])
    top = max(prows, key=prows.get) if prows else None
    if jit:
        check(psha == sha and prows and f'aot: profile: {hp("main.tsv")}: ' in log and plines and pfns.get(hot_entry, 0) > 0,
              f'2. {pfile}: the sample profile written at exit under the header naming this image ({len(prows)} rows, {total} samples), each sampled function\'s rows after its '
              f'function line, synth_hot\'s (main+{hot_entry:#x}) among them: {", ".join(f"{e:#x} {n}" for e, n in sorted(pfns.items()))}')
        check(top is not None and hot_loop[0] <= top < hot_loop[1] and in_loop * 2 > total,
              f'2. its samples in synth_hot\'s loop [main+{hot_loop[0]:#x}, main+{hot_loop[1]:#x}) (add, subs, b.ne: aot_synth.py\'s labels): {in_loop} of {total}, the most at '
              f'main+{top if top is not None else 0:#x} ({prows.get(top, 0)})')
    else:
        skipped(f'2. the sample profile in synth_hot\'s loop: the module stopped before the loop (no JIT fallback); {pfile.name} '
                + (f'written with {total} samples' if pfile.is_file() else 'not written (no sample)'))
    tfile = prof / 'main.targets.tsv'
    tsha, trows = targets_rows(tfile) if tfile.is_file() else (None, [])
    if tfile.is_file(): print('      main.targets.tsv:\n' + ''.join(f'      | {line}\n' for line in tfile.read_text().splitlines()))
    fnptr = (g['fnptr_site'], g['observed'], 1)
    check(tsha == sha and trows == [fnptr] and f'aot: targets: {hp("main.targets.tsv")}: 1 sites, 1 targets, 1 binds, 1 of the sites this session (main)' in log,
          f'2. {tfile.name}: the entry\'s call through synth_fnptr (main+{fnptr[0]:#x}) bound once to synth_observed (main+{fnptr[1]:#x}) under the header naming this image; the hidden '
          f'function\'s call no row (no compiled entry: the misses file has it)')
    rec1, had_profile = recorded(prows, pfns), pfile.is_file()
    targets1 = tfile.read_bytes() if tfile.is_file() else b''
    misses1 = mfile.read_bytes() if mfile.is_file() else b''

    # 3. The rule turns on
    lst = run([launcher, '--list', games], env)
    row = listed(lst)
    print(f'      better_pack: {row.get("better_pack")}')
    want = better_pack(launch, prof, sha)
    check(row.get('better_pack') == want and want.startswith('1 new JIT miss'),
          f'3. --list: better_pack says {want} (profile_state against the made rows: made profile {took}, no samples row)')

    # 4. The repack, through the launcher, after the profile's game.tsv changed (a newer bundle's profile): the packer writes its file again
    renamed = f'{TITLE} (repacked)'
    write_profile(work, default_id, renamed)
    r = run([launcher, '--games', games, '--repack', ID], env, timeout=300)
    print(r.stderr, end='')
    rlog = (rd / 'repack.txt').read_text(errors='replace') if (rd / 'repack.txt').is_file() else ''
    print(''.join(f'      repack.txt | {line}\n' for line in rlog.splitlines()), end='')
    argv = ['--platform', plat['packer'], '--pack-mode', 'balanced'] + (['--nxaot', str(nxaot)] if nxaot else [])
    check(r.returncode == 0 and 'the packer started for the balanced pack' in r.stderr and 'nxlauncher: the packer exited with status 0' in r.stderr and
          'packer stand-in: 1 packs built (balanced)' in rlog,
          f'4. --repack: the launcher ran the launch file\'s packer ({" ".join(argv)}) and waited: status 0')
    check(gprof.is_file() and f'name\t{renamed}\n' in gprof.read_text(encoding='utf-8') and ('path', 'game_profile', str(gprof)) in rows_of(launch),
          f'4. {gprof.name} written again from the changed profile (name {renamed}), the launch file\'s row unchanged')
    nxlog = (work / 'logs/nxaot_main.log').read_text(errors='replace') if (work / 'logs/nxaot_main.log').is_file() else ''
    show(nxlog, ['gate sites', red_gate, 'aot: misses'])
    check('aot: misses: 1 addresses, 1 new entries, 0 resume points, 0 still unserved (none)' in nxlog and 'gate sites 3 (design 1.1)' in nxlog and red_gate not in nxlog,
          f'4. nxaot build --misses{"" if route == "nxaot" else " (the prebuilt repack: " + str(prebuilt / "misses" / pack_name) + ")"}: the hidden function compiled '
          '(1 new entry), its gate served (gate sites 3), no [RED] line for it')
    show(nxlog, ['aot: profile', 'aot: tiers'])
    # the known targets the packs take: beside the profile they took, of this image (the packer's aot_targets); nxaot guards the sites of those at tier H (the entry's only when
    # a sample landed in it): guarded, from its log, is how many of the first session's binds the new pack's guards take off the dispatcher
    pairs1 = dominant(trows) if route == 'nxaot' and had_profile and tsha == sha else None
    gm = re.search(r'^aot: profile targets: (\d+) sites, (\d+) guarded', nxlog, re.M)
    guarded = int(gm.group(2)) if gm else 0
    if route == 'nxaot' and had_profile:
        pw = re.search(r'^aot: profile weights: (\d+) functions, (\d+) branches weighted', nxlog, re.M)
        tiers = re.search(r'^aot: tiers C (\d+) W (\d+) H (\d+) H\+ (\d+) \(functions\)$', nxlog, re.M)
        tline = (f'aot: profile targets: {len({s for s, _, _ in trows})} sites, ' if pairs1 is not None else 'aot: profile targets: none')
        check(f'aot: profile {prof / "main.tsv"}: {total} samples at {len(prows)} offsets, 0 outside every function' in nxlog and pw and int(pw.group(1)) >= 1 and
              tiers and int(tiers.group(3)) >= 1 and tline in nxlog,
              f'4. nxaot build --profile: the profile\'s rows taken ({total} samples at {len(prows)} offsets: nxaot skips the function lines), {pw.group(1) if pw else "?"} functions '
              f'weighed, {tiers.group(3) if tiers else "?"} at tier H; the known targets beside it read ({tline}..., {guarded} guarded: the entry\'s site when a sample landed in '
              f'the entry)')
    else:
        skipped('4. nxaot build --profile: ' + ('the prebuilt repack was made from the misses alone (no profile: made profile unused)' if route == 'prebuilt' else
                                                'the first session wrote no sample profile'))
    pack2 = (aot / pack_name).read_bytes() if (aot / pack_name).is_file() else b''
    check(bool(pack2) and pack2 != pack1, f'4. a new {pack_name} in the balanced folder')
    made = made_of(launch)
    digest = fnv64(f'{hidden:x} indirect\n')
    print(f'      launch file: {made}')
    samples_row = f'{rec1[0]} {rec1[1]} {rec1[2]}'
    targets_row = f'{len(pairs1)} {pairs_digest(pairs1)}' if pairs1 is not None else None
    want = [('made', 'profile', took), ('made', 'misses', 'yes'), ('made', 'targets', took)] + \
           ([('samples', 'main', samples_row)] if route == 'nxaot' and had_profile else []) + [('misses', 'main', f'1 {digest}')] + \
           ([('targets', 'main', targets_row)] if targets_row else [])
    check(made == want,
          f'4. the made rows: made profile {took}, made misses yes, made targets {took}, ' +
          (f'samples main {samples_row} (sampled functions, their samples, FNV-1a 64 of their entries, computed here), ' if route == 'nxaot' and had_profile else '') +
          f'misses main 1 {digest} (FNV-1a 64 of "{hidden:x} indirect\\n")' +
          (f', targets main {targets_row} (the sites with a dominant target, FNV-1a 64 of "<site>:<target>\\n", computed here)' if targets_row else ''))

    # 5. The rule turns off
    lst = run([launcher, '--list', games], env)
    row = listed(lst)
    print(f'      better_pack: {row.get("better_pack")}')
    check(lst.returncode == 0 and row.get('better_pack') == '-' and row.get('packs') == 'balanced', '5. --list: better_pack - (the packs took the misses, and the profile and the known targets when they took any)')

    # 6. Play from the new pack
    r = run([launcher, '--games', games, '--play', ID], env)
    print(r.stderr, end='')
    log = (rd / 'log.txt').read_text(errors='replace') if (rd / 'log.txt').is_file() else ''
    show(log, ['game profile ', 'gates (', 'that its pack does not call', 'needs the JIT fallback', '[guest]', 'guest code ended', 'aot: misses', 'aot: profile'])
    end = end_line(log)
    check(r.returncode == 0 and end and end[0] == 0 and '[guest] aot module: done' in log, '6. --play again: the module ran to ExitProcess, status 0')
    check(f'[dm] game profile {ID} ({renamed}): the file {gprof} (--profile)' in log.splitlines(), f'6. the host\'s profile line: the repacked profile\'s name, {renamed}')
    check('aot: main: 3 gates (2 observation gates, 1 spin wait back-edges) called by its pack, 0 not' in log and 'that its pack does not call' not in log,
          '6. the loader: the new pack calls every gate, the hidden function\'s too (0 not, no [RED] line)')
    if jit:
        check(end is not None and end1 is not None and end[2] == 0 and end[1] == end1[1] + 1 - guarded,
              f'6. no JIT fallback: 0 JIT addresses ({end[2] if end else "?"}); {end[1] if end else "?"} dispatcher misses, the first session\'s {end1[1] if end1 else "?"} and the '
              f'hidden function\'s call binding its inline cache to its new standard entry (aot.h dm_aot_stats_t misses), less the {guarded} the new pack\'s guards call directly')
    else:
        check(end is not None and end[2] == 0 and red_jit not in log,
              f'6. the JIT fallback not needed: 0 JIT addresses ({end[2] if end else "?"}), no [RED] line (the first session stopped at the hidden function: its '
              f'{end1[1] if end1 else "?"} dispatcher misses are not this one\'s {end[1] if end else "?"})')
    check('aot: misses: the JIT fallback ran for no pack this session: no misses file written' in log and mfile.is_file() and mfile.read_bytes() == misses1,
          f'6. no misses file written; {mfile.name} unchanged')
    psha2, prows2, pfns2, plines2 = profile_rows(pfile) if pfile.is_file() else (None, {}, {}, False)
    rec2 = recorded(prows2, pfns2)
    print(f'      main.tsv: {len(prows2)} rows: ' + ', '.join(f'{o:#x} {n}' for o, n in sorted(prows2.items())) + '; function lines: ' +
          ', '.join(f'{e:#x} {n}' for e, n in sorted(pfns2.items())))
    check(psha2 == sha and f'aot: profile: {hp("main.tsv")}: ' in log and plines2 and sum(n for o, n in prows2.items() if hot_loop[0] <= o < hot_loop[1]) * 2 > sum(prows2.values()),
          f'6. {pfile.name} written again, this session\'s: {len(prows2)} rows in {rec2[0]} sampled functions, {rec2[1]} samples, most in synth_hot\'s loop (the first session: '
          f'{len(prows)} rows in {rec1[0]} functions, {rec1[1]} samples)')
    tsha2, trows2 = targets_rows(tfile) if tfile.is_file() else (None, [])
    if tfile.is_file(): print('      main.targets.tsv:\n' + ''.join(f'      | {line}\n' for line in tfile.read_text().splitlines()))
    want_rows = sorted(([(fnptr[0], fnptr[1], 2 - min(guarded, 1))] if targets1 else []) + [(g['hidden_site'], hidden, 1)])
    check(tsha2 == sha and trows2 == want_rows and f'aot: targets: {hp("main.targets.tsv")}: ' in log,
          f'6. {tfile.name} merged with the first session\'s: the hidden function\'s call (main+{g["hidden_site"]:#x}) now bound to it (main+{hidden:#x}), a new site with a dominant '
          f'target; the entry\'s call through synth_fnptr ' + ('bound again unless the new pack guards it' if targets1 else 'not recorded by the first session') +
          f': {", ".join(f"{s:#x} {t:#x} {n}" for s, t, n in trows2)}')
    check('nxhost-run: modules from' in (rd / 'log.prev.txt').read_text(errors='replace') if (rd / 'log.prev.txt').is_file() else False, '6. the first session\'s log kept as log.prev.txt')
    lst = run([launcher, '--list', games], env)
    row = listed(lst)
    print(f'      better_pack: {row.get("better_pack")}')
    want = better_pack(launch, prof, sha)
    check(lst.returncode == 0 and row.get('better_pack') == want,
          f'6. --list: {want} (the rule on this session\'s profile and known targets against the made rows: made profile {took}; {rec2[0]} sampled functions vs {rec1[0]}, '
          f'{rec2[1]} samples vs {rec1[1]}, {"the same" if rec2[2] == rec1[2] else "another"} hot set; {len(dominant(trows2))} sites with a dominant target vs the packs\' '
          f'{targets_row.split(" ")[0] if targets_row else "none"})')

    print(f't_second_pack: {checks} checks, {fails} failed ({time.time() - t_start:.1f} s)')
    if not fails: shutil.rmtree(root, ignore_errors=True)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
