"""BNSH stage-8 extraction and normal package output, entirely synthetic."""
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools/fuzz'))
sys.path.insert(0, str(REPO))
import fuzz_shader_split as fz
from dm.gfx.tests.test_dmsh_rt_build import record, replace

ss = fz.ss


def bnsh(variations):
    """Each variation lists (program-pointer offset, control, unchanged code)."""
    programs = 0x100 + len(variations) * 0x40
    descriptors = programs + len(variations) * 0x50
    data = bytearray(descriptors + sum(map(len, variations)) * 0x20)
    data[:8] = b'BNSH\0\0\0\0'
    data[0x60:0x64] = b'grsc'
    struct.pack_into('<IQ', data, 0x7c, len(variations), 0x100)
    marks = []
    descriptor = descriptors
    for i, variation in enumerate(variations):
        program = programs + i * 0x50
        struct.pack_into('<Q', data, 0x110 + i * 0x40, program)
        for slot, control, code in variation:
            struct.pack_into('<Q', data, program + slot, descriptor)
            pctl = len(data)
            pcode = pctl + len(control)
            data.extend(control + code)
            struct.pack_into('<QQII', data, descriptor + 8, pctl, pcode, len(code), len(control))
            marks.append(dict(program=program, descriptor=descriptor, control=pctl, code=pcode))
            descriptor += 0x20
    struct.pack_into('<I', data, 0x1c, len(data))
    return bytes(data), marks


def nvn2_stage(stage):
    control = bytearray(0xe0)
    struct.pack_into('<II', control, 0, 0x98761234, 0xc9)
    struct.pack_into('<I', control, 0x44, 32)
    control[0x56] = stage
    return bytes(control), bytes(32)


class RayTracingSplit(unittest.TestCase):
    def test_every_variation_is_independent_and_keys_remain_standard(self):
        a, _ = record()
        b, _ = record(entries=((('independent_names', 'other_resume'), 0, 64),))
        b['code'] = replace(b['code'], 0, 'I', 0x7918)
        data, _ = bnsh([[(0x38, a['control'], a['code'])], [(0x38, b['control'], b['code'])]])
        got = ss.bnsh_shaders(data, 0, '/unrelated.asset')
        self.assertEqual([(row[0], row[1]) for row in got],
                         [('/unrelated.asset#BNSH@0x0#var0', 'rt'), ('/unrelated.asset#BNSH@0x0#var1', 'rt')])
        for row, source in zip(got, (a, b)):
            self.assertEqual(row[2:], (source['control'], source['code']))
            control = source['control']
            expected = hashlib.sha256(control[:0x30] + bytes(4) + control[0x34:0x48] + source['code']).hexdigest()[:16]
            self.assertEqual(ss.skey(*row[2:]), expected)
            self.assertEqual(ss.nvn2_stage(*row[2:]), 'rt')
        self.assertNotEqual(ss.skey(*got[0][2:]), ss.skey(*got[1][2:]))
        prefixed = bytes(0x80) + data
        self.assertEqual(ss.bnsh_shaders(prefixed, 0x80, 'embedded')[0][2:], got[0][2:])

    def test_existing_stage_slots_task_and_mesh_stay_separate(self):
        rt, _ = record()
        task, task_code = nvn2_stage(6)
        mesh, mesh_code = nvn2_stage(7)
        data, _ = bnsh([[(8, *fz.VS), (0x28, *fz.PS), (0x38, rt['control'], rt['code']),
                          (0x40, task, task_code), (0x48, mesh, mesh_code)]])
        self.assertEqual([row[1] for row in ss.bnsh_shaders(data, 0, 'mixed')], ['vs', 'ps', 'rt', 'ts', 'ms'])
        self.assertEqual([(r[1], r[2], r[3]) for r in ss.bnsh_shaders(fz.bnsh(), 0, 'old')],
                         [('vs', *fz.VS), ('ps', *fz.PS)])

    def test_same_key_different_rt_metadata_is_not_silently_collapsed(self):
        a, _ = record()
        b, _ = record(entries=((('begin', 'continue'), 0, 64),))
        self.assertEqual(a['code'], b['code'])
        key = ss.skey(a['control'], a['code'])
        self.assertEqual(ss.skey(b['control'], b['code']), key)
        self.assertFalse(ss.same_shader((a['control'], a['code']), (b['control'], b['code'])))
        first = ('/first', 'rt', a['control'], a['code'])
        second = ('/second', 'rt', b['control'], b['code'])
        with self.assertRaisesRegex(SystemExit, r'^\[RED\].*control section or code area differs'):
            ss.uniq_keys([first, second])
        with self.assertRaisesRegex(SystemExit, r'^\[RED\].*control section or code area differs'):
            ss.merge_keys([('first', {key: first}, set(), None), ('second', {key: second}, set(), None)])
        declared_length = replace(a['control'], 0x30, 'I', len(a['control']))
        self.assertTrue(ss.same_shader((a['control'], a['code']), (declared_length, a['code'])))
        control, code = nvn2_stage(5)
        self.assertTrue(ss.same_shader((control, code), (replace(control, 0xc0, 'I', 17), code)))

    def test_full_split_rejects_equal_key_conflicting_rt_metadata(self):
        a, _ = record()
        b, _ = record(entries=((('begin', 'continue'), 0, 64),))
        data, _ = bnsh([[(0x38, a['control'], a['code'])], [(0x38, b['control'], b['code'])]])
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            rom = root / 'RomFS'
            rom.mkdir()
            (rom / 'unclassified.asset').write_bytes(data)
            main = root / 'main'
            main.write_bytes(b'NSO0' + bytes(0xfc))
            result = subprocess.run([sys.executable, REPO / 'dm/gfx/tools/shader_split.py', rom, main,
                                     root / 'package', root / 'map.tsv', root / 'facts.tsv'],
                                    capture_output=True, text=True,
                                    env=dict(os.environ, DM_GAME='01007BB017812000', J='1'))
            # scan_file reports damaged/conflicting files through its existing
            # RED/skip channel; it must not quietly retain one of these modules.
            self.assertIn('control section or code area differs', result.stdout + result.stderr)
            self.assertFalse(list((root / 'package').glob('rt_*.bin')))
            (rom / 'unclassified.asset').unlink()
            header = bytearray(b'NSO0' + bytes(0xfc))
            struct.pack_into('<III', header, 0x10, 0x100, 0, len(data))
            for offset in (0x20, 0x30):
                struct.pack_into('<III', header, offset, 0, (len(data) + 0xfff) & ~0xfff, 0)
            main.write_bytes(header + data)
            result = subprocess.run([sys.executable, REPO / 'dm/gfx/tools/shader_split.py', rom, main,
                                     root / 'package', root / 'map.tsv', root / 'facts.tsv'],
                                    capture_output=True, text=True,
                                    env=dict(os.environ, DM_GAME='01007BB017812000', J='1'))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('control section or code area differs', result.stdout + result.stderr)

    def test_legacy_following_words_do_not_create_an_rt_descriptor(self):
        data, marks = bnsh([[(8, *fz.VS), (0x28, *fz.PS)]])
        slot = marks[0]['program'] + 0x38
        for pointer in (0, len(data) + 0x100, (1 << 64) - 1, marks[0]['descriptor']):
            with self.subTest(pointer=pointer):
                got = ss.bnsh_shaders(replace(data, slot, 'Q', pointer), 0, 'legacy')
                self.assertEqual([row[1] for row in got], ['vs', 'ps'])

    def test_identified_rt_bounds_and_metadata_fail_explicitly(self):
        item, _ = record()
        data, (mark,) = bnsh([[(0x38, item['control'], item['code'])]])
        desc, control = mark['descriptor'], mark['control']
        malformed = [replace(data, desc + 0x10, 'Q', len(data) + 16),
                     replace(data, desc + 0x18, 'I', len(data)),
                     replace(data, desc + 0x1c, 'I', len(data)),
                     replace(data, desc + 0x1c, 'I', 0x57),
                     replace(data, control + 0x5c, 'I', 0),
                     replace(data, control + 0x60, 'I', 0x60),
                     replace(data, control + 0x60, 'I', len(data)),
                     replace(data, control + 0x64, 'I', 16),
                     replace(data, control + 0x44, 'I', 16),
                     replace(data, control + 0xe0 + 16, 'I', 0)]
        for bad in malformed:
            with self.subTest(bad=malformed.index(bad)), self.assertRaisesRegex(SystemExit, r'^\[RED\].* rt'):
                ss.bnsh_shaders(bad, 0, 'broken')

    def test_full_split_writes_rt_records_map_rows_and_no_compute_facts(self):
        a, _ = record()
        b, _ = record(entries=((('unrelated',), 8, 0),))
        data, _ = bnsh([[(0x38, a['control'], a['code'])], [(0x38, b['control'], b['code'])]])
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            rom = root / 'RomFS'
            rom.mkdir()
            (rom / 'arbitrary.asset').write_bytes(data)
            main = root / 'main'
            main.write_bytes(b'NSO0' + bytes(0xfc))
            package, mapping, facts = (root / name for name in ('package', 'map.tsv', 'facts.tsv'))
            result = subprocess.run([sys.executable, REPO / 'dm/gfx/tools/shader_split.py', rom, main,
                                     package, mapping, facts], capture_output=True, text=True,
                                    env=dict(os.environ, DM_GAME='01007BB017812000', J='1'))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn('[RED]', result.stdout + result.stderr)
            rows = [line.split('\t') for line in mapping.read_text().splitlines()
                    if line and not line.startswith('#') and line.split('\t')[0].endswith('.bin')]
            self.assertEqual(len(rows), 2)
            by_key = {ss.skey(item['control'], item['code']): item for item in (a, b)}
            for name, index, stage, key, _ in rows:
                self.assertTrue(name.startswith('rt_'))
                self.assertEqual(stage, 'raytracing')
                records = re.split(b'(?=BTCDBTCD)', (package / name).read_bytes())[1:]
                rec = records[int(index)]
                self.assertEqual(rec[8:16].hex(), key)
                ctl_size, code_size = struct.unpack_from('<II', rec, 0x28)
                self.assertEqual(rec[0x38:0x38 + code_size], by_key[key]['code'])
                self.assertEqual(rec[0x38 + code_size:], by_key[key]['control'])
                self.assertEqual(ctl_size, len(by_key[key]['control']))
                self.assertNotIn(key + '\t', facts.read_text())


if __name__ == '__main__':
    unittest.main()
