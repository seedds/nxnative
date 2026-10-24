#!/usr/bin/env python3
# The iOS launcher's settings for one game, from settings.tsv (its header describes the columns): the rows whose condition holds for the game, with the game's defaults, as JSON.
# build_ipa.sh writes it into the app as dm/launcher.json; Launcher.swift draws the screen from it (an app without the file starts the game at once).
# Usage: settings.py <game id> <rewritten modules dir> <control.nacp> <out.json>   (DM_GAMES_DIR finds provisional profiles, as in game.py; build:<VAR> defaults come from the environment)
import json, os, re, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE)); sys.path.insert(0, str(HERE.parent / 'tools'))
import game, nacp


def rows():
    for n, line in enumerate((HERE / 'settings.tsv').read_text(encoding='utf-8').splitlines(), 1):
        if not line.strip() or line.startswith('#'): continue
        c = line.split('\t')
        if len(c) not in (6, 7): sys.exit(f'settings.tsv:{n}: 6 or 7 columns expected, {len(c)} found')
        yield n, c + [''] * (7 - len(c))


def fixups_keys(path):   # '# KEY<TAB>value' header rows of a rewriter table
    if not path.is_file(): return set()
    return {m[1] for m in re.finditer(r'^# (\w+)\t', path.read_text(encoding='utf-8'), re.M)}


def build(gid, patched, control):
    g = game.load(gid)
    macros = dict(re.findall(r'^#define (DM_GAME_\w+) (.*)$', game.header(g), re.M))   # the host's own defaults, as it is compiled for this game
    keys = fixups_keys(Path(patched) / 'main.fixups.tsv')
    langs = [(c, n) for c, n in nacp.LANGUAGES if c in nacp.languages(control)]

    def macro(name, n):
        if name not in macros: sys.exit(f'settings.tsv:{n}: no {name} in the game profile header (game.py header)')
        return macros[name].strip('"')

    def shown(cond, n):
        if '&' in cond: return all(shown(c, n) for c in cond.split('&'))
        if cond == 'always': return True
        if cond == 'languages>1': return len(langs) > 1
        if cond.startswith('fixups:'): return cond[7:] in keys
        m = re.fullmatch(r'(DM_GAME_\w+)(!=|==)(.*)', cond)
        if m: return (macro(m[1], n) == m[3]) == (m[2] == '==')
        sys.exit(f'settings.tsv:{n}: unknown condition {cond}')

    def value(d, n):
        if d.startswith('DM_GAME_'): return macro(d, n)
        if d.startswith('build:'):
            v = os.environ.get(d[6:], '')
            return {'YES': '1', 'NO': '0', '': '0'}.get(v, v)   # build_ipa.sh's yes / no switches
        return d

    out = []
    for n, (var, kind, values, default, cond, label, note) in rows():
        if not shown(cond, n): continue
        s = {'env': var, 'kind': kind, 'label': label, 'note': note, 'default': value(default, n)}
        if kind == 'choice':
            opts = [list(l) for l in langs] if values == 'languages' else [v.split('=', 1) for v in values.split(',')]
            if any(len(o) != 2 for o in opts): sys.exit(f'settings.tsv:{n}: choice values are value=label,...')
            if s['default'] not in [o[0] for o in opts]: s['default'] = opts[0][0]   # a default the game does not offer (a language it lacks): its first
            s['options'] = opts
        elif kind == 'range':
            lo, hi, st = (float(x) for x in values.split(':'))
            s.update(min=lo, max=hi, step=st)
        elif kind == 'toggle':
            if s['default'] not in ('0', '1'): sys.exit(f'settings.tsv:{n}: a toggle defaults to 0 or 1: {s["default"]}')
        else: sys.exit(f'settings.tsv:{n}: unknown kind {kind}')
        out.append(s)
    return {'game': g.id, 'settings': out}


if __name__ == '__main__':
    if len(sys.argv) != 5: sys.exit('usage: settings.py <game id> <rewritten modules dir> <control.nacp> <out.json>')
    d = build(*sys.argv[1:4])
    Path(sys.argv[4]).write_text(json.dumps(d, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
    print(' '.join(s['env'] for s in d['settings']))
