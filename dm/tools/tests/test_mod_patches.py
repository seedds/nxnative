"""Mod patches through the rewriter (DM_MOD_PATCHES) and the launch-time tool's choice of files, on a synthetic module."""
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import mods_rewrite
import nso
import nso_asm

RET, MOV_X18_X0, MOV_X1_X2 = 0xd65f03c0, 0xaa0003f2, 0xaa0203e1


def module(functions):
    from test_nso_match import image

    raw = image(functions)
    raw[0x1100:0x111c] = raw[0x20:0x3c]
    struct.pack_into('<I', raw, 4, 0x1100)
    struct.pack_into('<ii', raw, 0x1110, 0x3000 - 0x1100, 0x300c + 8 * len(functions) - 0x1100)
    raw[0x10:0x14] = b'NRO0'
    struct.pack_into('<I', raw, 0x18, len(raw))
    struct.pack_into('<6I', raw, 0x20, 0, 0x1000, 0x1000, 0x2000, 0x3000, 0x1000)
    struct.pack_into('<I', raw, 0x38, 0)
    return raw


class RewriterModPatches(unittest.TestCase):
    def rewrite(self, patches, cuts=(), stub=None):
        raw = module({0x100: [nso_asm.NOP, MOV_X1_X2, nso_asm.NOP, RET]})
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            src = tmp / 'input.nro'
            src.write_bytes(raw)
            build = nso.cli_read(str(src)).build_id[:0x10].hex()
            (tmp / 'games/T').mkdir(parents=True)
            stubs = f'STUBS = {{{build!r}: lambda c: c.put({stub:#x}, [{nso_asm.NOP:#x}], "a test stub")}}\n' if stub is not None else ''
            (tmp / 'games/T/patches.py').write_text(f'CUTS = {{{build!r}: {list(cuts)!r}}}\n' + stubs)
            env = os.environ | {'DM_GAME': '', 'DM_REWRITE_J': '1', 'DM_GAMES_DIR': str(tmp / 'games')}
            if patches is not None:
                (tmp / 'mod.tsv').write_text(''.join(f'{off:x}\t{data.hex()}\t{label}\n' for off, data, label in patches))
                env['DM_MOD_PATCHES'] = str(tmp / 'mod.tsv')
            run = subprocess.run([sys.executable, str(TOOLS / 'patch_nso.py'), str(src), str(tmp / 'out'), 'nro0'], env=env, capture_output=True, text=True)
            read = lambda ext: (tmp / 'out' / f'nro0.{ext}').read_text() if (tmp / 'out' / f'nro0.{ext}').exists() else ''
            image = (tmp / 'out/nro0.patched.bin').read_bytes() if (tmp / 'out/nro0.patched.bin').exists() else b''
            return run, read('sites.tsv'), read('changed.tsv'), read('fixups.tsv'), image, build

    def test_no_patch_file_changes_nothing(self):
        plain = self.rewrite(None)
        empty = self.rewrite([])
        self.assertEqual(plain[0].returncode, 0, plain[0].stdout + plain[0].stderr)
        self.assertEqual(plain[4], empty[4])
        self.assertNotIn('0x100\t', plain[1])

    def test_an_x18_instruction_of_a_mod_becomes_a_site(self):
        run, sites, changed, fixups, image, build = self.rewrite([(0x100, struct.pack('<I', MOV_X18_X0), '/mods/A/exefs/a.pchtxt')])
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn('0x100\tx18\n', sites)
        self.assertIn('0x100\t0x4\n', changed)
        self.assertIn(f'# BUILD_ID\t{build}\n', fixups)
        self.assertNotEqual(image[0x100:0x104], struct.pack('<I', MOV_X18_X0))
        self.assertIn('1 mod patch(es) of 1 file(s) applied before the rewrite', run.stdout)

    def test_a_plain_patch_stays_as_written_and_is_not_a_changed_word(self):
        run, sites, changed, _, image, _ = self.rewrite([(0x108, struct.pack('<I', MOV_X1_X2), 'b')])
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(image[0x108:0x10c], struct.pack('<I', MOV_X1_X2))
        self.assertNotIn('0x108\t', changed)

    def test_a_patch_over_a_cut_is_refused_by_name(self):
        cut = (0x104, struct.pack('<I', MOV_X1_X2).hex(), struct.pack('<I', nso_asm.NOP).hex(), 'a test cut')
        both = [(0x100, struct.pack('<I', MOV_X18_X0), 'fine'), (0x102, b'\x00' * 4, '/mods/B/exefs/b.ips')]
        run, *_ = self.rewrite(both, cuts=[cut])
        self.assertEqual(run.returncode, 4, run.stdout + run.stderr)
        refused = [line.split('\t') for line in run.stdout.splitlines() if line.startswith('MOD_REFUSED\t')]
        self.assertEqual([r[1] for r in refused], ['/mods/B/exefs/b.ips'])
        self.assertIn('nro0+0x104', refused[0][2])
        self.assertIn('a test cut', refused[0][2])
        run, sites, _, _, image, _ = self.rewrite(both[:1], cuts=[cut])
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn('0x100\tx18\n', sites)
        self.assertEqual(image[0x104:0x108], struct.pack('<I', nso_asm.NOP))

    def test_a_patch_over_a_stub_site_is_refused_by_name(self):
        run, *_ = self.rewrite([(0x104, struct.pack('<I', MOV_X1_X2), '/mods/C/exefs/c.pchtxt')], stub=0x104)
        self.assertEqual(run.returncode, 4, run.stdout + run.stderr)
        refused = [line.split('\t') for line in run.stdout.splitlines() if line.startswith('MOD_REFUSED\t')]
        self.assertEqual([r[1] for r in refused], ['/mods/C/exefs/c.pchtxt'])
        self.assertIn('a stub site (a test stub)', refused[0][2])
        run, _, _, _, image, _ = self.rewrite([], stub=0x104)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(image[0x104:0x108], struct.pack('<I', nso_asm.NOP))

    def test_a_patch_outside_the_image_is_refused(self):
        run, *_ = self.rewrite([(0x100000, b'\x00', 'far')])
        self.assertEqual(run.returncode, 4, run.stdout + run.stderr)
        self.assertIn('MOD_REFUSED\tfar\t', run.stdout)


class LaunchToolChoice(unittest.TestCase):
    def file(self, path, status, *recs):
        return {'module': 'main', 'build': 'b', 'status': status, 'mod': path, 'path': path, 'sha': path, 'why': '-', 'recs': [(o, bytes(n)) for o, n in recs]}

    def test_only_refused_files_and_those_sharing_their_bytes_are_rewritten_in(self):
        low, hit, mid, chained, alone = (self.file('low', 'applied', (0x100, 8)), self.file('hit', 'refused', (0x104, 4), (0x200, 4)),
                                         self.file('mid', 'applied', (0x202, 4)), self.file('chained', 'applied', (0x205, 1)), self.file('alone', 'applied', (0x300, 4)))
        files = [alone, chained, low, mid, hit]
        self.assertEqual([f['path'] for f in mods_rewrite.bake_set(files)], ['chained', 'low', 'mid', 'hit'])
        self.assertEqual(mods_rewrite.bake_set([low, alone]), [])

    def test_cuts_and_gates_of_the_built_module_are_read(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, 'main.cuts.tsv').write_text('# BUILD_ID\tb\n0x104\t1f2003d51f2003d5\t0000000000000000\twhy\n')
            Path(tmp, 'main.obs.tsv').write_text('# BUILD_ID\tb\n0x300\t7\td503201f\tgate\n')
            self.assertEqual(mods_rewrite.registered_sites(tmp, 'main'), [(0x104, 8, 'a cut (why)'), (0x300, 4, 'an observation gate (gate)')])

    def test_no_patch_files_means_no_host_start(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, 'A/romfs/x').mkdir(parents=True)
            Path(tmp, 'A/romfs/x/skipped.ips').write_text('')
            self.assertFalse(mods_rewrite.has_patch_files(tmp))
            Path(tmp, 'A/exefs').mkdir()
            Path(tmp, 'A/exefs/p.pchtxt').write_text('')
            self.assertTrue(mods_rewrite.has_patch_files(tmp))


if __name__ == '__main__':
    unittest.main()
