"""BAHS pipeline pairs come from complete programs, without bypassing intermediate stages. No game data required."""
from pathlib import Path
import os
import struct
import sys
import unittest

os.environ.setdefault('DM_GAME', '010015100B514000')
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split


def arrays(programs):
    """The binary/program arrays read by bahs_variations; programs = [(name, [binary kinds])]."""
    data = bytearray(0x20)
    records, bases = [], []
    for name, kinds in programs:
        bases.append((name, len(records), kinds))
        records.extend(kinds)
    data += struct.pack('<II', 8 + 0x20 * len(records), len(records))
    entries = []
    for kind in records:
        entries.append(len(data) + 0x10)
        data += struct.pack('<IIII', 0x20, kind, 0, 0) + bytes(0x10)
    progs = bytearray()
    for name, base, kinds in bases:
        name = name.encode() + b'\0'
        progs += struct.pack('<IIII', 16 + len(name), len(name), sum(1 << k for k in set(kinds)), base) + name
    data += struct.pack('<II', 8 + len(progs), len(programs)) + progs
    return bytes(data), entries, records


def found_shaders(label, entries, kinds, omit=()):
    found = []
    for i, (entry, kind) in enumerate(zip(entries, kinds)):
        if i in omit: continue
        ctl = bytearray(0x880)
        struct.pack_into('<I', ctl, 0x20, len(ctl))
        found.append((f'{label}#BAHS@{entry:#x}', {0: 'vs', 1: 'ps', 2: 'gs', 3: 'cs', 4: 'hs', 5: 'ds'}.get(kind, 'gs'), bytes(ctl), bytes([i])))
    return found


class BahsPairs(unittest.TestCase):
    def setUp(self):
        before = split.BAHS_VAR.copy()
        split.BAHS_VAR.clear()
        self.addCleanup(lambda: (split.BAHS_VAR.clear(), split.BAHS_VAR.update(before)))

    def pairs(self, programs, omit=()):
        data, entries, kinds = arrays(programs)
        found = found_shaders('fixture', entries, kinds, omit)
        split.bahs_variations(data, 'fixture', [x[0] for x in found])
        return found, split.var_pairs(found)

    def test_direct_program_variations_pair_in_order(self):
        found, pairs = self.pairs([('direct', [0, 0, 1, 1])])
        key = lambda i: split.skey(*found[i][2:])
        self.assertEqual(pairs, {(key(0), key(2), 'prog'), (key(1), key(3), 'prog')})

    def test_geometry_and_tessellation_programs_have_no_direct_pair(self):
        for intermediate in (2, 4, 5, 6):
            with self.subTest(kind=intermediate):
                split.BAHS_VAR.clear()
                _, pairs = self.pairs([('intermediate', [0, 1, intermediate])])
                self.assertEqual(pairs, set())

    def test_unextracted_intermediate_still_prevents_direct_pair(self):
        # The geometry record need not be understood by the reader to disprove a direct VS -> FS link.
        _, pairs = self.pairs([('geometry', [0, 1, 2])], omit=(2,))
        self.assertEqual(pairs, set())

    def test_other_programs_stay_paired_and_stages_stay_extracted(self):
        found, pairs = self.pairs([('direct-before', [0, 1]), ('geometry', [0, 1, 2]), ('direct-after', [0, 1])])
        key = lambda i: split.skey(*found[i][2:])
        self.assertEqual(pairs, {(key(0), key(1), 'prog'), (key(5), key(6), 'prog')})
        self.assertEqual(len(found), 7)
        self.assertEqual(found[4][1], 'gs')


if __name__ == '__main__':
    unittest.main()
