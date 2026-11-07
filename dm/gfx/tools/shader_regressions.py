#!/usr/bin/env python3
# shader_regressions.py <library dir> [<chain version>]: the shaders that translated in the game's previous build and fail in this one. The translation chain is one for every game,
# so a translator or host change is checked only on the games its author owns; on another game the first sign was a crash at run time (GitHub issue #272: c3a0880 broke the image
# atomics of SHIN MEGAMI TENSEI V and Yoshi's Crafted World). This compares <library dir>/b-<V>/_b_build.json (default: the current chain, dmsh_chain.py) with the newest other
# b-*/_b_build.json in the same folder, skipping one stamped with another game. It reads only the build reports dmsh_build.py writes, so it is not part of the chain.
# The NSP script runs it after each shader build and stops on a regression unless told to go on (ALLOW_SHADER_REGRESSIONS / --allow-shader-regressions).
# It also holds what counts as a loss: a shader that built, but lost part of what it does (losses). dmsh_build.py prints them and scripts/regress.py compares them; nothing here decides an output.
# Exit status: 0 = none, or no earlier build to compare with; 1 = regressions, one [RED] line each; 2 = no report for this chain.
import collections
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dmsh_chain
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))
import shader_lib

REPORT = '_b_build.json'
DELETED = re.compile(r'translator: Failed to find handle source for bindless access of type "(\w+)"')   # BindlessElimination: the access is removed, its results read 0


def report(lib_dir, name):   # a build's report, in its library's folder or one file (shader_lib.py)
    return shader_lib.view(os.path.join(lib_dir, name)) / REPORT


def load(path): return json.loads(path.read_text())


def previous(lib_dir, chain):
    """The newest b-*/ beside b-<chain>/ with a build report, of the same game when both are stamped; None when there is none"""
    game = shader_lib.read_stamp(os.path.join(lib_dir, 'b-' + chain)).get('game')
    found = []
    for name in {shader_lib.name_of(n) for n in os.listdir(lib_dir) if n.startswith('b-') and not n.endswith('.part')}:
        path = report(lib_dir, name)
        if name == 'b-' + chain or not path.is_file(): continue
        other = shader_lib.read_stamp(os.path.join(lib_dir, name)).get('game')
        if game and other and other != game: continue
        found.append((path.stat().st_mtime, name))
    return max(found)[1] if found else None


def compare(lib_dir, chain):
    """(previous b-* name or None, [(key, status, reason)] for the shaders that were ok there and are not now). Keys missing from either build are new or not rebuilt: not counted"""
    now = load(report(lib_dir, 'b-' + chain))
    prev = previous(lib_dir, chain)
    if prev is None: return None, []
    return prev, lost(load(report(lib_dir, prev)), now)


def lost(prev_rows, now_rows):
    """[(key, status, reason)] for the shaders ok in prev_rows and not in now_rows (two build reports' rows); scripts/regress.py compares two revisions' builds with this too"""
    was_ok = {r['key'] for r in prev_rows if r['st'] == 'ok'}
    return [(r['key'], r['st'], r['err']) for r in now_rows if r['st'] != 'ok' and r['key'] in was_ok]


def losses(refl):
    """What a shader that built lost, from its reflection, as two Counters: the bindless accesses the translator deleted, by type; the extra entry points its library built without"""
    return collections.Counter(m[1] for n in refl.get('notes', ()) if (m := DELETED.match(n))), collections.Counter(refl.get('entries_lost', ()))


def report_losses(refls):
    """One [RED] line per shader, in key order, and the counts per kind. Never stops a build: a player's pack goes on, as a shader the game never reaches costs nothing (docs/reference/shaders.md,
    Losses a shader build reports). A cache hit reports them too: both are in the reflection"""
    types, entries, shaders = collections.Counter(), collections.Counter(), collections.Counter()
    for r in sorted(refls, key=lambda r: r['dm_ctl']):
        deleted, lost = losses(r)
        if deleted:
            types += deleted; shaders['deleted'] += 1
            print(f"[RED] {r['dm_ctl']}: the translator deleted {sum(deleted.values())} bindless accesses ({', '.join(f'{t} {n}' for t, n in sorted(deleted.items()))}): their results read 0", flush=True)
        if lost:
            entries += lost; shaders['lost'] += 1
            print(f"[RED] {r['dm_ctl']}: its library builds only without its {', '.join(lost)} entry points: the draws that need them fail or go without", flush=True)
    if not shaders: print('lost without failing: nothing', flush=True); return
    print(f"[RED] lost without failing: {sum(types.values())} bindless accesses deleted in {shaders['deleted']} shaders ({', '.join(f'{t} {n}' for t, n in sorted(types.items())) or 'none'}), "
          f"{shaders['lost']} shaders without extra entry points ({', '.join(f'{t} {n}' for t, n in sorted(entries.items())) or 'none'})", flush=True)


def main(argv):
    if len(argv) not in (2, 3): sys.exit('usage: shader_regressions.py <library dir> [<chain version>]')
    lib_dir, chain = argv[1], argv[2] if len(argv) == 3 else dmsh_chain.version()
    if not report(lib_dir, 'b-' + chain).is_file():
        print(f'[RED] no build report {report(lib_dir, "b-" + chain)}: dmsh_build.py has not built chain {chain} there'); return 2
    prev, lost = compare(lib_dir, chain)
    if prev is None:
        print(f'shader regressions: no earlier build in {lib_dir} to compare b-{chain} with'); return 0
    print(f'shader regressions against {prev}: {len(lost)}')
    for key, st, err in lost: print(f'[RED] {key}: translated in {prev}, {st} in b-{chain}: {err}')
    return 1 if lost else 0


if __name__ == '__main__': sys.exit(main(sys.argv))
