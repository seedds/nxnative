"""Checked duplicate representatives through the full split and packing gate."""
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from dm.tools.tests.test_nsp_incremental import game, nsp
from dm.tools.tests.test_split_rt import REPO, bnsh, fz, nvn2_stage, record, ss


def nso(data):
    header = bytearray(b'NSO0' + bytes(0xfc))
    struct.pack_into('<III', header, 0x10, 0x100, 0, len(data))
    for offset in (0x20, 0x30):
        struct.pack_into('<III', header, offset, 0, (len(data) + 0xfff) & ~0xfff, 0)
    return header + data


def archive(shaders):
    """A decoded UE v2 archive of NVN2 vertex records, in table order."""
    records = []
    for ctl, code in shaders:
        name = b'Synthetic/Vertex\0'
        records.append(bytes(0x3e) + struct.pack('<I', len(name)) + name
                       + struct.pack('<I', len(code)) + code + struct.pack('<I', len(ctl)) + ctl + bytes(12))
    n = len(records)
    data = struct.pack('<II', 2, 1) + bytes(20) + struct.pack('<I', n) + bytes(20 * n)
    data += struct.pack('<IIIII', 1, 0, n, 0, 0) + struct.pack('<I', n)
    offset = 0
    for rec in records:
        data += struct.pack('<QIIB', offset, len(rec), len(rec), 0)
        offset += len(rec)
    return data + struct.pack('<II', 0, 0) + b''.join(records)


class Representatives(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.rom = self.root / 'RomFS'
        self.rom.mkdir()
        self.main = self.root / 'main'
        self.main.write_bytes(nso(b''))

    def split_env(self, workers=1):
        return dict(os.environ, DM_GAME='01007BB017812000', J=str(workers), DM_SPLIT_REGISTERED_MAP='',
                    DM_UE_SHADERS=str(self.root / 'ue_shaders'), DM_GLSLC_CAPTURE=str(self.root / 'glslc_capture'))

    def split(self, workers=1, versions=()):
        out = [self.root / name for name in ('pkg', 'map.tsv', 'facts.tsv', 'pairs.tsv', 'pso.tsv')]
        roms = ','.join(str(r) for r, _ in [(self.rom, self.main), *versions])
        mains = ','.join(str(m) for _, m in [(self.rom, self.main), *versions])
        result = subprocess.run([sys.executable, REPO / 'dm/gfx/tools/shader_split.py', roms, mains, *out],
                                capture_output=True, text=True, env=self.split_env(workers))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn('[RED]', result.stdout + result.stderr)
        rows = [line.split('\t') for line in out[1].read_text().splitlines()
                if line and not line.startswith(('#', 'package\t'))]
        snapshot = {path.name: path.read_bytes() for path in out[1:] + sorted(out[0].glob('*.bin'))}
        return rows, snapshot

    def assert_records(self, rows, expected):
        self.assertEqual(len(rows), len(expected))
        self.assertEqual({r[3] for r in rows}, set(expected))
        for name, index, stage, key, source in rows:
            want_name, want_index, want_stage, want_source, ctl, code = expected[key]
            self.assertEqual((name, index, stage, source), (want_name, str(want_index), want_stage, want_source))
            records = re.split(b'(?=BTCDBTCD)', (self.root / 'pkg' / name).read_bytes())[1:]
            rec = records[int(index)]
            self.assertEqual(rec[8:16].hex(), key)
            self.assertEqual(struct.unpack_from('<II', rec, 0x28), (len(ctl), len(code)))
            self.assertEqual(rec[0x38:], code + ctl)

    def test_bnsh_first_variations_determine_labels_indices_payloads_and_pairs(self):
        control, code = fz.VS
        other = code[:-1] + bytes([code[-1] ^ 1])
        # Trailing descriptor padding lies beyond the control's own length and
        # is not semantic. Its first copy must still be the serialized payload.
        a, b = (control + b'first', code), (control, other)
        later_a = (control + b'later', code)
        data, _ = bnsh([[(8, *b), (0x28, *fz.PS)], [(8, *a), (0x28, *fz.PS)],
                        [(8, *later_a)], [(8, *b)]])
        ka, kb, kp = (ss.skey(*s) for s in (a, b, fz.PS))
        for location in ('main', 'romfs'):
            with self.subTest(location=location):
                asset = self.rom / 'duplicates.asset'
                self.main.write_bytes(nso(data if location == 'main' else b''))
                if location == 'romfs': asset.write_bytes(data)
                label, group = ('main', 'main') if location == 'main' else ('/duplicates.asset', 'other')
                prefix = label + '#BNSH@0x0#var'
                rows, before = self.split()
                self.assert_records(rows, {
                    kb: (f'vs_{group}.bin', 0, 'vertex', prefix + '0', *b),
                    ka: (f'vs_{group}.bin', 1, 'vertex', prefix + '1', *a),
                    kp: (f'ps_{group}.bin', 0, 'pixel', prefix + '0', *fz.PS),
                })
                pairs = {line for line in before['pairs.tsv'].decode().splitlines() if line and not line.startswith('#')}
                self.assertEqual(pairs, {ka + '\t' + kp, kb + '\t' + kp})
                self.assertEqual(self.split(workers=2)[1], before)

    def test_version_union_preserves_base_sources_and_indices(self):
        ctl, code = fz.VS
        a = ctl, code
        b = ctl, code[:-1] + bytes([code[-1] ^ 1])
        c = ctl, code[:-1] + bytes([code[-1] ^ 2])
        base, _ = bnsh([[(8, *b)], [(8, *a)], [(8, *a)], [(8, *b)]])
        update, _ = bnsh([[(8, *a)], [(8, *b)], [(8, *c)], [(8, *a)]])
        self.main.write_bytes(nso(base))
        version = self.root / 'v327680'
        (version / 'RomFS').mkdir(parents=True)
        (version / 'main').write_bytes(nso(bytes(0x80) + update))
        rows, _ = self.split(versions=[(version / 'RomFS', version / 'main')])
        self.assert_records(rows, {
            ss.skey(*b): ('vs_main.bin', 0, 'vertex', 'main#BNSH@0x0#var0', *b),
            ss.skey(*a): ('vs_main.bin', 1, 'vertex', 'main#BNSH@0x0#var1', *a),
            ss.skey(*c): ('vs_main.bin', 2, 'vertex', 'main@v327680#BNSH@0x80#var2', *c),
        })

    def test_packing_splits_base_update_and_dlc_under_a_path_with_commas(self):
        root = self.root / "Game, with spaces"
        roms = [root / version / 'RomFS' for version in ('v0', 'v65536')]
        mains = [rom.parent / 'main' for rom in roms]
        dlc = root / 'aoc/7'
        for path in (*roms, dlc):
            path.mkdir(parents=True)
        for main in mains:
            main.write_bytes(nso(b''))

        ctl, code = fz.VS
        expected = {}
        for i, (folder, prefix) in enumerate(zip((*roms, dlc), ('', '/v65536', '/aoc/7'))):
            shader = code[:-1] + bytes([code[-1] ^ i])
            data, _ = bnsh([[(8, ctl, shader)]])
            (folder / 'one.bnsh').write_bytes(data)
            expected[ss.skey(ctl, shader)] = f'{prefix}/one.bnsh#BNSH@0x0#var0'

        profile = root / 'profile'
        profile.mkdir()
        (profile / 'game.tsv').write_text('modules\tmain\n')
        with mock.patch.object(nsp, 'PROG'):
            nsp.split_shaders(sys.executable, REPO, roms, mains, root / 'pkg',
                              self.split_env(), root / 'logs', game.Game(profile), provisional=True, dlc_roms=[dlc])

        rows = nsp.data_rows((profile / 'shader_map.tsv').read_bytes())
        actual = dict(line.decode().split('\t')[3:] for line in rows if not line.startswith(b'package\t'))
        self.assertEqual(actual, expected)

    def test_ue_duplicates_keep_first_archive_entry_and_record_order(self):
        ctl, _ = nvn2_stage(0)
        a = ctl, struct.pack('<I', 0x461) + bytes(28)
        b = ctl, a[1][:-1] + b'\x01'
        ue = self.root / 'ue_shaders'
        ue.mkdir()
        # Create B before A: directory creation order must not pick the source.
        (ue / 'ShaderArchive-B.ushaderbytecode').write_bytes(archive([a, b]))
        (ue / 'ShaderArchive-A.ushaderbytecode').write_bytes(archive([b, a, a, b]))
        rows, before = self.split()
        prefix = '/ue_shaders/ShaderArchive-A.ushaderbytecode#UE'
        self.assert_records(rows, {
            ss.skey(*b): ('vs_other.bin', 0, 'vertex', prefix + '0', *b),
            ss.skey(*a): ('vs_other.bin', 1, 'vertex', prefix + '1', *a),
        })
        self.assertEqual(self.split(workers=2)[1], before)

    def test_packing_keeps_registered_representatives_and_package_indices(self):
        for location in ('main', 'unreal'):
            with self.subTest(location=location):
                if location == 'main':
                    ctl, code = fz.VS
                    a, b = (ctl, code), (ctl, code[:-1] + bytes([code[-1] ^ 1]))
                    # The registered last copy of b has different descriptor padding.
                    old_b = ctl + b'padding', b[1]
                    data, _ = bnsh([[(8, *b)], [(8, *a)], [(8, *a)], [(8, *old_b)]])
                    self.main.write_bytes(nso(data))
                    package, prefix = 'vs_main.bin', 'main#BNSH@0x0#var'
                else:
                    self.main.write_bytes(nso(b''))
                    ctl, _ = nvn2_stage(0)
                    a = ctl, struct.pack('<I', 0x461) + bytes(28)
                    b = old_b = ctl, a[1][:-1] + b'\x01'
                    ue = self.root / 'ue_shaders'; ue.mkdir()
                    (ue / 'ShaderArchive-A.ushaderbytecode').write_bytes(archive([b, a, a, b]))
                    package, prefix = 'vs_other.bin', '/ue_shaders/ShaderArchive-A.ushaderbytecode#UE'
                ka, kb = (ss.skey(*s) for s in (a, b))
                rows, snapshot = self.split()
                self.assertEqual([r[3:] for r in rows], [[kb, prefix + '0'], [ka, prefix + '1']])

                profile = self.root / f'{location}-profile'; profile.mkdir()
                (profile / 'game.tsv').write_text('modules\tmain\n')
                shader_map = profile / 'shader_map.tsv'
                registered = ''.join(f'{package}\t{i}\tvertex\t{k}\t{prefix}{i + 2}\n' for i, k in enumerate((ka, kb)))
                header = 'package\tindex\tstage\tdm_ctl_sha16\tsource\n'
                shader_map.write_text(header + registered)
                for name, fixture in (('shader_facts.tsv', 'facts.tsv'), ('shader_pairs.tsv', 'pairs.tsv'), ('pso_pairs.tsv', 'pso.tsv')):
                    (profile / name).write_bytes(snapshot[fixture])

                pkg = self.root / 'pkg'
                with mock.patch.object(nsp, 'PROG'):
                    args = (sys.executable, REPO, [self.rom], [self.main], pkg, self.split_env(), self.root / 'logs', game.Game(profile))
                    key = nsp.split_shaders(*args)
                    self.assertEqual(key, nsp.package_key(pkg))
                    self.assert_records([line.split('\t') for line in registered.splitlines()], {
                        ka: (package, 0, 'vertex', prefix + '2', *a),
                        kb: (package, 1, 'vertex', prefix + '3', *old_b),
                    })
                    self.assertEqual(nsp.split_shaders(*args), key)
                    self.assertEqual(shader_map.read_text(), header + registered)
                    shader_map.write_text(header + registered.replace(prefix + '3', prefix + '99'))
                    with self.assertRaisesRegex(nsp.BuildError, 'the split shader tables differ'):
                        nsp.split_shaders(*args)

    def test_packing_keeps_a_registered_source_across_romfs_files(self):
        ctl, code = fz.VS
        other = ctl, code[:-1] + bytes([code[-1] ^ 1])
        registered, _ = bnsh([[(8, ctl + b'padding', code)]])
        duplicate, _ = bnsh([[(8, ctl, code)]])
        middle, _ = bnsh([[(8, *other)]])
        (self.rom / 'middle.asset').write_bytes(middle)
        registered_file = self.rom / 'z.asset'
        registered_file.write_bytes(registered)
        _, snapshot = self.split()
        profile = self.root / 'registered-profile'
        profile.mkdir()
        (profile / 'game.tsv').write_text('modules\tmain\n')
        for table, name in (('map', 'shader_map'), ('facts', 'shader_facts'), ('pairs', 'shader_pairs'), ('pso', 'pso_pairs')):
            (profile / f'{name}.tsv').write_bytes(snapshot[f'{table}.tsv'])

        (self.rom / 'a.asset').write_bytes(duplicate)
        standalone, _ = self.split()
        self.assertEqual(standalone[0][3:], [ss.skey(ctl, code), '/a.asset#BNSH@0x0#var0'])
        pkg = self.root / 'pkg'
        with mock.patch.object(nsp, 'PROG'):
            args = (sys.executable, REPO, [self.rom], [self.main], pkg, self.split_env(), self.root / 'logs', game.Game(profile))
            self.assertEqual(nsp.split_shaders(*args), nsp.package_key(pkg))
            self.assertEqual((pkg / 'vs_other.bin').read_bytes(), snapshot['vs_other.bin'])
            registered_file.unlink()
            with self.assertRaisesRegex(nsp.BuildError, 'the split shader tables differ'):
                nsp.split_shaders(*args)

    def test_registered_representative_cannot_hide_a_conflicting_rt_copy(self):
        a, _ = record()
        b, _ = record(entries=((('begin', 'continue'), 0, 64),))
        first = ('main#BNSH@0x0#var0', 'rt', a['control'], a['code'])
        registered = ('main#BNSH@0x0#var1', *first[1:])
        conflict = ('main#BNSH@0x0#var2', 'rt', b['control'], b['code'])
        key = ss.skey(*first[2:])
        shader_map = self.root / 'registered.tsv'
        shader_map.write_text(f'rt_main.bin\t0\traytracing\t{key}\t{registered[0]}\n')
        sources = ss.read_registered_sources(str(shader_map))
        with self.assertRaisesRegex(SystemExit, 'control section or code area differs'):
            ss.uniq_keys([first, registered, conflict], sources)
        results = [(copy[0], {key: copy}, set(), {}) for copy in (first, registered, conflict)]
        with self.assertRaisesRegex(SystemExit, 'control section or code area differs'):
            ss.merge_keys(results, sources)


if __name__ == '__main__':
    unittest.main()
