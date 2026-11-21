#!/usr/bin/env python3
# Port a Switch game's BDAT mod to another version of the game's data (Xenoblade Chronicles 2 -> its Switch 2 Edition, whose tables gained rows
# and columns): a three-way merge. What the mod changed from the data it was made from (--original) is written into the other version's
# table (--target), nothing else, so the target's own changes stay:
#   - rows are matched by content, not by ID: identical rows in order, then rows of a changed stretch that agree on half their fields
#     (XC2's Switch 2 Edition inserted a row in BLD_RareList, so an ID-for-ID port wrote gacha odds onto the wrong blades);
#   - a field the target changed too is reported and left, unless --prefer-mod;
#   - a string is written only where it fits in the bytes of the one it replaces and no other field shares it;
#   - rows the mod added go after the target's last row, with new IDs; fields named in --ref TABLE=REGEX that hold one of those rows' old IDs
#     get the new one (a value in that range in a field the rule does not name is listed as a hint, never changed). --idmap-out writes those
#     renumberings as JSON; --idmap-in reads another file's, so its fields follow (XC2's gb/common_ms.bdat captions named by common.bdat).
# Exit status 0, also when fields were reported; 1 on an error (nothing written).
# Usage: bdat_port.py --original <bdat the mod was made from> --mod <mod's bdat> --target <other version's bdat> -o <out>
#                     [--ref BTL_Enhance='Enhance\d*' ...] [--idmap-in <json> ...] [--idmap-out <json>] [--prefer-mod] [--report]
# Several mods into one file: port the first, then pass its output as --target of the next (they must change different fields).
import argparse, difflib, json, re, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'dm' / 'tools'))
import bdat


def sig(row, keys): return tuple((k, row.get(k)) for k in keys)


def align(ot, tt):
    """{original row ID: target row ID} for the rows that are the same row"""
    orows, trows = ot.rows(), tt.rows()
    keys = [m[0] for m in ot.members if m[0] in tt.by_name]
    sm = difflib.SequenceMatcher(None, [sig(r, keys) for _, r in orows], [sig(r, keys) for _, r in trows], autojunk=False); out = {}
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == 'equal':
            for k in range(i2 - i1): out[orows[i1 + k][0]] = trows[j1 + k][0]
        elif tag == 'replace':
            j = j1
            for i in range(i1, i2):
                for jj in range(j, j2):
                    if 2 * sum(1 for k in keys if orows[i][1].get(k) == trows[jj][1].get(k)) >= len(keys):
                        out[orows[i][0]] = trows[jj][0]; j = jj + 1; break
    return out


def main():
    ap = argparse.ArgumentParser(description='Port a BDAT mod to another version of the game data (three-way merge)')
    ap.add_argument('--original', required=True); ap.add_argument('--mod', required=True); ap.add_argument('--target', required=True)
    ap.add_argument('-o', '--out', required=True)
    ap.add_argument('--ref', action='append', default=[], help='TABLE=REGEX: fields matching REGEX refer to rows of TABLE')
    ap.add_argument('--prefer-mod', action='store_true', help="write the mod's value where the target changed the field too")
    ap.add_argument('--idmap-in', action='append', default=[], help="another port's --idmap-out: its tables' renumbered rows")
    ap.add_argument('--idmap-out', help='write the renumbered rows of this port as JSON')
    ap.add_argument('--report', action='store_true', help='list every field not written')
    a = ap.parse_args()
    try:
        O, M = bdat.load(a.original), bdat.load(a.mod)
        T = bdat.load(a.target)
    except (OSError, bdat.Error) as e:
        print(f'[RED] {e}', file=sys.stderr); return 1
    refs = {}
    for r in a.ref:
        tab, _, rx = r.partition('=')
        if not rx: print(f'[RED] --ref {r}: TABLE=REGEX', file=sys.stderr); return 1
        refs.setdefault(tab, []).append(re.compile(rx + r'\Z'))
    counts, notes, changed = {}, [], set()
    def note(kind, text=None):
        counts[kind] = counts.get(kind, 0) + 1
        if text: notes.append(f'{kind}: {text}')

    # rows the mod added: their new IDs in the target
    added, idmap = {}, {}
    for mt in M.tables:
        ot = O.by_name.get(mt.name)
        if ot is None: continue
        if mt.count < ot.count: note('table shorter in the mod (rows removed, not ported)', mt.name); continue
        if mt.count > ot.count:
            if mt.name not in T: note('table with added rows not in the target', mt.name); continue
            tt = T[mt.name]; first = ot.base_id + ot.count
            added[mt.name] = [mt.row(i) for i in range(first, mt.base_id + mt.count)]
            idmap[mt.name] = {first + k: tt.base_id + tt.count + k for k in range(len(added[mt.name]))}
    for p in a.idmap_in:
        for tab, m in json.loads(Path(p).read_text()).items(): idmap.setdefault(tab, {}).update({int(k): v for k, v in m.items()})
    hints = {}
    def remap(table, member, value):
        if isinstance(value, tuple) and not (len(value) == 2 and value[0] == 'str'): return tuple(remap(table, member, v) for v in value)
        if not isinstance(value, int): return value
        for g, m in idmap.items():
            if value in m:
                if any(rx.match(member) for rx in refs.get(g, ())): return m[value]
                hints.setdefault((g, table, member), 0); hints[(g, table, member)] += 1
        return value

    for mt in M.tables:
        ot, tt = O.by_name.get(mt.name), T.by_name.get(mt.name)
        if ot is None: note('table not in the original', mt.name); continue
        amap = None; orows = dict(ot.rows())
        for rid in mt.ids():
            orow = orows.get(rid)
            if orow is None: continue   # an added row: below
            mrow = mt.row(rid)
            for member, mv in mrow.items():
                ov = orow.get(member)
                if mv == ov: continue
                note('changed by the mod')
                if tt is None: note('table not in the target', mt.name); continue
                if amap is None: amap = align(ot, tt)
                tid = amap.get(rid)
                if tid is None or member not in tt.by_name: note('row or member not in the target', f'{mt.name}#{rid}.{member}'); continue
                if tid != rid: note('row at another ID in the target')
                tv = tt.row(tid)[member]; nv = remap(mt.name, member, mv)
                if tv == nv: note('the target has it already'); continue
                if tv != ov and not a.prefer_mod: note('the target changed it too (left)', f'{mt.name}#{rid}.{member}: original {ov}, mod {mv}, target {tv}'); continue
                if tv != ov: note('the target changed it too (mod written)', f'{mt.name}#{rid}.{member}: original {ov}, mod {mv}, target {tv}')
                why = tt.set(tid, member, nv)
                if why: note('not written', f'{mt.name}#{rid}.{member} = {mv}: {why}'); continue
                note('written'); changed.add(mt.name)
    for name, rows in added.items():
        tt = T[name]
        T[name].append([{k: remap(name, k, v) for k, v in row.items()} for row in rows]); changed.add(name)
        note('rows added', f'{name}: {len(rows)} rows as IDs {min(idmap[name].values())}..{max(idmap[name].values())}')
    out = T.bytes(changed)
    Path(a.out).write_bytes(out)
    if a.idmap_out: Path(a.idmap_out).write_text(json.dumps({t: {str(k): v for k, v in m.items() if k != v} for t, m in idmap.items() if t in added}, indent=1))
    for k in sorted(counts): print(f'  {k}: {counts[k]}')
    for (g, table, member), n in sorted(hints.items()):
        print(f'  hint: {table}.{member} holds {n} value(s) in the IDs of {g}\'s added rows; renumbered only if --ref {g}=... names it')
    if a.report:
        for n in notes: print('   ', n)
    return 0


if __name__ == '__main__':
    sys.exit(main())
