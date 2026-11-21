"""BDAT tables (dm/tools/bdat.py) and the mod porter (scripts/bdat_port.py) on synthetic files, no game data: a file reads back and rebuilds
byte for byte with its names and strings scrambled; fields, strings (in place, and appended when longer) and added rows round-trip; and a port
writes only what the mod changed, onto rows the target moved, renumbers the rows the mod added after the target's own, carries that renumbering
into another file's fields (--idmap-out / --idmap-in), and leaves a field the target changed too unless --prefer-mod. Expectations by hand."""
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
import bdat   # noqa: E402

PORT = REPO / 'scripts' / 'bdat_port.py'
# members: (name, value type, count); value types as bdat.VT (2 u16, 6 s32, 7 string, 8 f32)
MEMBERS = [('Name', 7, 1), ('Power', 6, 1), ('Rate', 8, 1), ('Enhance', 2, 1), ('Pair', 2, 2)]


def table(name, rows, base_id=1, key=0x2f85, members=MEMBERS):
    """A scrambled table: header, names, member infos, hash table (empty), rows, strings"""
    names = bytearray(name.encode() + b'\0'); name_offs = []
    for m, _, _ in members: name_offs.append(len(names)); names += m.encode() + b'\0'
    if len(names) % 2: names.append(0)
    infos = bytearray(); pos = 0; layout = []
    for m, vt, cnt in members:
        layout.append((vt, pos, cnt))
        infos += struct.pack('<BBHH', 2 if cnt > 1 else 1, vt, pos, cnt) if cnt > 1 else struct.pack('<BBH', 1, vt, pos)
        pos += bdat.VT[vt][1] * cnt
    row_size = (pos + 3) & ~3
    names_off = 36; mem_off = names_off + len(names); info_off = mem_off + 6 * len(members)
    hash_off = info_off + len(infos) + (len(infos) & 1)
    mems = bytearray()
    io = info_off
    for (m, vt, cnt), no in zip(members, name_offs):
        mems += struct.pack('<HHH', io, 0, names_off + no); io += 6 if cnt > 1 else 4
    rows_off = (hash_off + 15) & ~15
    strings = bytearray(); body = bytearray()
    str_off = rows_off + row_size * len(rows)
    for r in rows:
        b = bytearray(row_size)
        for (m, vt, cnt), (_, p, _) in zip(members, layout):
            vals = r[m] if cnt > 1 else (r[m],)
            for k, v in enumerate(vals):
                if vt == 7: struct.pack_into('<I', b, p + 4 * k, str_off + len(strings)); strings += v.encode() + b'\0'
                else: struct.pack_into('<' + bdat.VT[vt][0], b, p + k * bdat.VT[vt][1], v)
        body += b
    while (str_off + len(strings)) % 16: strings.append(0)
    t = bytearray(rows_off) + body + strings
    t[:4] = b'BDAT'
    bdat.HEAD.pack_into(t, 4, 2, names_off, row_size, hash_off, 0, rows_off, len(rows), base_id, 0, key, str_off, len(strings), mem_off, len(members))
    t[names_off:names_off + len(names)] = names; t[mem_off:mem_off + len(mems)] = mems; t[info_off:info_off + len(infos)] = infos
    bdat.scramble(t, key, names_off, hash_off - names_off); bdat.scramble(t, key, str_off, len(strings))
    return bytes(t)


def bfile(*tables):
    head = 8 + 4 * len(tables); offs, at = [], head
    for t in tables: offs.append(at); at += len(t)
    return struct.pack(f'<II{len(tables)}I', len(tables), at, *offs) + b''.join(tables)


def row(name, power, rate=1.0, enh=0, pair=(0, 0)): return {'Name': name, 'Power': power, 'Rate': rate, 'Enhance': enh, 'Pair': pair}


ENH = [{'Name': f'e{i}', 'Power': i, 'Rate': 0.5, 'Enhance': 0, 'Pair': (0, 0)} for i in range(1, 4)]


class Bdat(unittest.TestCase):
    def test_round_trip(self):
        d = bfile(table('Skills', [row('Fire', 10), row('Ice', 20)]), table('Enh', ENH))
        f = bdat.File(d)
        self.assertEqual([t.name for t in f.tables], ['Skills', 'Enh'])
        self.assertEqual(f['Skills'].row(2), {'Name': ('str', 'Ice'), 'Power': 20, 'Rate': 1.0, 'Enhance': 0, 'Pair': (0, 0)})
        self.assertEqual(f.bytes(set()), d)
        self.assertEqual(f.bytes({'Skills', 'Enh'}), d, 'scrambled again with the same key')

    def test_set_and_append(self):
        f = bdat.File(bfile(table('Skills', [row('Fire', 10), row('Ice', 20)])))
        t = f['Skills']
        self.assertIsNone(t.set(1, 'Power', 99)); self.assertIsNone(t.set(1, 'Name', ('str', 'Fir')))
        self.assertIsNone(t.set(2, 'Name', ('str', 'Blizzard')), 'a longer string goes at the end of the strings')
        self.assertIsNone(t.set(2, 'Pair', (3, 4)))
        t.append([row('Bolt', 5, enh=7)])
        g = bdat.File(f.bytes({'Skills'}))['Skills']
        self.assertEqual(g.count, 3)
        self.assertEqual((g.row(1)['Name'], g.row(1)['Power']), (('str', 'Fir'), 99))
        self.assertEqual((g.row(2)['Name'], g.row(2)['Pair']), (('str', 'Blizzard'), (3, 4)))
        self.assertEqual((g.row(3)['Name'], g.row(3)['Enhance']), (('str', 'Bolt'), 7))
        self.assertEqual(g.str_off % 4, 0)


class Port(unittest.TestCase):
    def port(self, d, original, mod, target, *args):
        for n, data in (('o', original), ('m', mod), ('t', target)): (d / n).write_bytes(data)
        r = subprocess.run([sys.executable, str(PORT), '--original', str(d / 'o'), '--mod', str(d / 'm'), '--target', str(d / 't'), '-o', str(d / 'out'), *args],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return bdat.load(d / 'out'), r.stdout

    def test_three_way(self):
        orig = [row('A', 1), row('B', 2), row('C', 3)]
        mod = [row('A', 1), row('B', 50), row('C', 3, rate=2.0), row('D', 4, enh=4)]          # B's power, C's rate, and a new row whose Enhance names a new Enh row
        tgt = [row('A', 1), row('New', 9), row('B', 2), row('C', 3, rate=1.5)]             # the target inserted a row and changed C's rate
        enh_mod = ENH + [{'Name': 'e4', 'Power': 4, 'Rate': 0.5, 'Enhance': 0, 'Pair': (0, 0)}]
        enh_tgt = ENH + [{'Name': 'x4', 'Power': 40, 'Rate': 0.5, 'Enhance': 0, 'Pair': (0, 0)}]   # the target took ID 4 for its own row
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            out, text = self.port(d, bfile(table('Skills', orig), table('Enh', ENH)), bfile(table('Skills', mod), table('Enh', enh_mod)),
                                  bfile(table('Skills', tgt), table('Enh', enh_tgt)), '--ref', 'Enh=Enhance', '--idmap-out', str(d / 'map.json'), '--report')
            s = out['Skills']
            self.assertEqual(s.row(3)['Power'], 50, "B's change goes to B, at ID 3 in the target")
            self.assertEqual(s.row(2)['Power'], 9, 'the inserted row is untouched')
            self.assertEqual(s.row(4)['Rate'], 1.5, 'a field the target changed too is left')
            self.assertIn('the target changed it too (left)', text)
            self.assertEqual((s.count, s.row(5)['Name'], s.row(5)['Enhance']), (5, ('str', 'D'), 5), "the added row after the target's, naming Enh's renumbered row")
            e = out['Enh']
            self.assertEqual((e.count, e.row(4)['Name'], e.row(5)['Name']), (5, ('str', 'x4'), ('str', 'e4')))
            self.assertEqual(json.loads((d / 'map.json').read_text()), {'Enh': {'4': 5}, 'Skills': {'4': 5}})
            out2, _ = self.port(d, bfile(table('Skills', orig), table('Enh', ENH)), bfile(table('Skills', mod), table('Enh', enh_mod)),
                                bfile(table('Skills', tgt), table('Enh', enh_tgt)), '--ref', 'Enh=Enhance', '--prefer-mod')
            self.assertEqual(out2['Skills'].row(4)['Rate'], 2.0, '--prefer-mod writes the mod value')

    def test_idmap_in(self):
        # a second file whose field names the first file's renumbered rows
        orig = [row('A', 1, enh=1)]; mod = [row('A', 1, enh=4)]
        with tempfile.TemporaryDirectory() as d:
            d = Path(d); (d / 'map.json').write_text(json.dumps({'Enh': {'4': 5}}))
            out, _ = self.port(d, bfile(table('Arts', orig)), bfile(table('Arts', mod)), bfile(table('Arts', orig)),
                               '--ref', 'Enh=Enhance', '--idmap-in', str(d / 'map.json'))
            self.assertEqual(out['Arts'].row(1)['Enhance'], 5)


if __name__ == '__main__':
    unittest.main()
