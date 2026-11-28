"""shader_split: Falcom .pkg packages (stored, LZ4 block, zstd), PH3/Falcom .pka deduplicated archives (0x7ff7cf0d),
and Phyre RYHP containers wrapping GLSLC outputs. Synthetic bytes only."""
import contextlib
import hashlib
import io
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import lz4.block

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
sys.path.insert(0, str(REPO / 'dm/tools/fuzz'))
import fuzz_shader_split as fz
import romfs_image

ss = fz.ss
ZSTD = ss.zstd_mod()


def stage(original, word):
    ctl, code = original
    return ctl, code[:0x80] + bytes.fromhex(word) * 8


def make_bnsh(st):
    """Build a minimal 1-variation, 1-vertex-stage BNSH container."""
    ctl, code = st
    d = bytearray(0x300)
    d[:8] = b'BNSH\0\0\0\0'
    d[0x60:0x64] = b'grsc'
    struct.pack_into('<IQ', d, 0x7c, 1, 0x100)
    struct.pack_into('<Q', d, 0x110, 0x140)
    struct.pack_into('<Q', d, 0x148, 0x200)
    at = len(d)
    d += ctl + code
    struct.pack_into('<QQII', d, 0x208, at, at + len(ctl), len(code), len(ctl))
    struct.pack_into('<I', d, 0x1c, len(d))
    return bytes(d)


def ryhp_glslc(stages):
    """Minimal Phyre RYHP container carrying whole GLSLC outputs (NISA/PH3 Trails into Reverie)."""
    return b'RYHP' + bytes(0x3c) + b''.join(fz.glslc([st]) for st in stages)


def falcom_pkg(entries, first_offset=None):
    """Build a Falcom .pkg archive. entries: [(name, raw, flags, stored_override_or_None)]."""
    n = len(entries)
    at = 8 + 0x50 * n if first_offset is None else first_offset
    table, data = bytearray(), bytearray()
    for name, raw, flags, stored in entries:
        if stored is not None:
            packed = stored
        elif flags == 1:
            packed = lz4.block.compress(raw, store_size=False)
        elif flags == 0x10:
            packed = ZSTD.compress(raw)
        else:
            packed = raw
        table += name.encode().ljust(0x40, b'\0') + struct.pack('<4I', len(raw), len(packed), at + len(data), flags)
        data += packed
    gap = bytes(at - (8 + 0x50 * n)) if first_offset is not None and first_offset > 8 + 0x50 * n else b''
    return struct.pack('<II', 0, n) + bytes(table) + gap + bytes(data)


def falcom_pka(packages, blobs):
    """Build a PH3/Falcom .pka archive (0x7ff7cf0d).
    packages: [(pkg_name, [(entry_name, sha256_bytes), ...])]
    blobs: [(sha256_bytes, raw, flags, stored_override_or_None)]
    """
    out = bytearray(ss.FALCOM_PKA + struct.pack('<I', len(packages)))
    for pnm, entries in packages:
        out += pnm.encode().ljust(32, b'\0') + struct.pack('<I', len(entries))
        for enm, digest in entries:
            out += enm.encode().ljust(64, b'\0') + digest
    out += struct.pack('<I', len(blobs))
    data_start = len(out) + len(blobs) * 52
    table, data = bytearray(), bytearray()
    for digest, raw, flags, stored in blobs:
        if stored is not None:
            packed = stored
        elif flags == 1:
            packed = lz4.block.compress(raw, store_size=False)
        elif flags == 0x10:
            packed = ZSTD.compress(raw)
        else:
            packed = raw
        table += digest + struct.pack('<QIII', data_start + len(data), len(packed), len(raw), flags)
        data += packed
    return bytes(out + table + data)


class FalcomPkg(unittest.TestCase):
    def scan(self, name, data):
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()) as log:
            path = Path(tmp) / name
            path.write_bytes(data)
            result = ss.scan_file((str(path), name))
        return result, log.getvalue()

    def test_pkg_reads_lz4_zstd_stored_and_skips_non_shaders(self):
        vs1, vs2 = stage(fz.VS, '1100000000000001'), stage(fz.VS, '2200000000000002')
        ps1 = stage(fz.PS, '3300000000000003')
        vs3 = stage(fz.VS, '4400000000000004')
        blob = falcom_pkg([
            ('asset_NX.xml', b'<xml/>', 0x10, None),
            ('tex.dds.phyre', bytes(0x200), 9, b''),
            ('mesh.dae.phyre', b'corrupt-not-read', 0x10, b'not-zstd'),
            ('lz4_bnsh', make_bnsh(vs1), 1, None),
            ('stored_bnsh', make_bnsh(vs2), 0, None),
            ('ed8_chr.fx#AA.phyre', ryhp_glslc([ps1]), 0x10, None),
            ('bare.glslc', fz.glslc([vs3]), 0x10, None),
        ])
        (rel, keys, pairs, extra), log = self.scan('a.pkg', blob)
        self.assertEqual((rel, log, extra), ('a.pkg', '', {}))
        self.assertEqual(pairs, set())
        want = {ss.skey(*vs1), ss.skey(*vs2), ss.skey(*ps1), ss.skey(*vs3)}
        self.assertEqual(set(keys), want)
        self.assertEqual(keys[ss.skey(*vs1)][0], '/a.pkg/lz4_bnsh#BNSH@0x0#var0')
        self.assertEqual(keys[ss.skey(*vs2)][0], '/a.pkg/stored_bnsh#BNSH@0x0#var0')
        self.assertTrue(keys[ss.skey(*ps1)][0].startswith('/a.pkg/ed8_chr.fx#AA.phyre#GLSLC@'))
        self.assertEqual(keys[ss.skey(*vs3)][0], '/a.pkg/bare.glslc#GLSLC@0x0')

    def test_non_pkg_starting_with_zero_falls_back_to_plain_scan(self):
        vs1 = stage(fz.VS, '5500000000000005')
        blob = falcom_pkg([('a', make_bnsh(vs1), 0, None)], first_offset=8 + 0x50 + 0x10)
        (_, keys, _, extra), log = self.scan('plain.bin', blob)
        self.assertEqual((log, extra), ('', {}))
        self.assertEqual(set(keys), {ss.skey(*vs1)})
        self.assertEqual(keys[ss.skey(*vs1)][0], f'/plain.bin#BNSH@{8 + 0x50 + 0x10:#x}#var0')

    def test_falcom_pkg_rejects_large_count_before_walking_table(self):
        blob = bytearray(8 + 0x50 * 100000 + 0x100)
        struct.pack_into('<II', blob, 0, 0, 100000)
        struct.pack_into('<I', blob, 8 + 0x48, 0x1234)
        with mock.patch.object(ss.struct, 'unpack_from', wraps=struct.unpack_from) as spy:
            self.assertIsNone(ss.falcom_pkg(bytes(blob)))
            self.assertEqual(spy.call_count, 2)

    def test_unknown_flag_warns_and_keeps_other_entries(self):
        vs1, vs2 = stage(fz.VS, '6600000000000006'), stage(fz.VS, '7700000000000007')
        blob = falcom_pkg([
            ('ok', make_bnsh(vs1), 1, None),
            ('weird', make_bnsh(vs2), 4, lz4.block.compress(make_bnsh(vs2), store_size=False)),
        ])
        (_, keys, _, extra), log = self.scan('mix.pkg', blob)
        self.assertEqual((set(keys), extra), ({ss.skey(*vs1)}, {}))
        self.assertEqual(sum(line.startswith('[WARN] /mix.pkg:') for line in log.splitlines()), 1, log)

    def test_lz4_bomb_stops_before_decompress(self):
        hdr = bytearray(falcom_pkg([('a', b'x', 1, b'\0')]))
        struct.pack_into('<4I', hdr, 8 + 0x40, 0x40000000, 1, 8 + 0x50, 1)
        with mock.patch.object(lz4.block, 'decompress', wraps=lz4.block.decompress) as spy:
            (_, keys, _, extra), log = self.scan('bomb.pkg', bytes(hdr))
        self.assertEqual(spy.call_count, 0)
        self.assertEqual(keys, {})
        self.assertEqual(extra[0], 'unreadable')
        self.assertIn('255 to 1', extra[1])
        self.assertTrue(log.startswith('[RED] /bomb.pkg/a:'), log)

    def test_corrupt_streams_are_reported_as_unreadable(self):
        good_zstd = ZSTD.compress(b'12345678')
        cases = (
            ('bad_lz4', falcom_pkg([('a', bytes(0x200), 1, b'\xff' * 0x40)]), 'not an LZ4 block'),
            ('bad_zstd_magic', falcom_pkg([('a', bytes(0x200), 0x10, b'xxxx')]), 'does not start with a zstd frame'),
            ('bad_zstd_len', falcom_pkg([('a', bytes(4), 0x10, good_zstd)]), 'zstd stream length does not match'),
            ('bad_stored_len', falcom_pkg([('a', bytes(0x10), 0, b'1234')]), 'stored bytes, the entry says'),
        )
        for label, data, why in cases:
            with self.subTest(case=label):
                (_, keys, _, extra), log = self.scan(f'{label}.pkg', data)
                self.assertEqual(keys, {})
                self.assertEqual(extra[0], 'unreadable')
                self.assertIn(why, extra[1])
                self.assertTrue(log.startswith(f'[RED] /{label}.pkg/a:'), log)


class FalcomPkaAndRyhp(unittest.TestCase):
    def test_ryhp_glslc_loose_and_native_phyre_unaffected(self):
        ps1 = stage(fz.PS, '8800000000000008')
        out = []
        ss.scan_blob(ryhp_glslc([ps1]), '/loose.fx.phyre', out)
        self.assertEqual(len(out), 1)
        self.assertEqual(ss.skey(out[0][2], out[0][3]), ss.skey(*ps1))

    def test_pka_deduplicates_and_runs_end_to_end_in_romfs_image(self):
        vs1, ps1 = stage(fz.VS, '9900000000000009'), stage(fz.PS, 'aa0000000000000a')
        vs2, ps2 = stage(fz.VS, 'bb0000000000000b'), stage(fz.PS, 'cc0000000000000c')
        fx1 = ryhp_glslc([vs1, ps1])
        bn2 = make_bnsh(vs2)
        tex = bytes(0x100)
        h_fx1, h_bn2, h_tex = (hashlib.sha256(b).digest() for b in (fx1, bn2, tex))
        pka = falcom_pka(
            packages=[
                ('C_CHR000.pkg', [('tex.dds.phyre', h_tex), ('ed8_chr.fx#1.phyre', h_fx1)]),
                ('C_CHR001.pkg', [('ed8_chr.fx#1.phyre', h_fx1), ('extra.bnsh', h_bn2)]),
            ],
            blobs=[
                (h_tex, tex, 0x10, b'not-zstd-skipped'),
                (h_fx1, fx1, 0x10, None),
                (h_bn2, bn2, 1, None),
            ],
        )
        loose_pkg = falcom_pkg([('ed8_chr.fx#2.phyre', ryhp_glslc([ps2]), 0x10, None)])

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            romfs = root / 'RomFS'
            (romfs / 'data/asset/nx').mkdir(parents=True)
            (romfs / 'data/asset/nx/assets.pka').write_bytes(pka)
            (romfs / 'data/asset/nx/loose.pkg').write_bytes(loose_pkg)
            image = root / 'RomFS.img'
            romfs_image.pack(romfs, image)
            main = root / 'main'
            main.write_bytes(b'NSO0' + bytes(0xfc))
            out = [root / name for name in ('pkg', 'map.tsv', 'facts.tsv', 'pairs.tsv', 'pso.tsv')]
            r = subprocess.run(
                [sys.executable, REPO / 'dm/gfx/tools/shader_split.py', image, main, *out],
                env=dict(os.environ, DM_GAME='01007BB017812000', J='2'),
                capture_output=True, text=True,
            )
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertNotIn('[RED]', r.stdout)
            rows = [line.split('\t') for line in out[1].read_text().splitlines() if not line.startswith('#')]
            got = {row[3]: row[4] for row in rows if row[0].endswith('.bin')}
            self.assertEqual(set(got), {ss.skey(*vs1), ss.skey(*ps1), ss.skey(*vs2), ss.skey(*ps2)})
            # Deduplicated hash keeps the first package's name (C_CHR000.pkg, not C_CHR001.pkg).
            self.assertTrue(got[ss.skey(*vs1)].startswith('/data/asset/nx/assets.pka/C_CHR000.pkg/ed8_chr.fx#1.phyre#GLSLC@'))
            self.assertEqual(got[ss.skey(*vs2)], '/data/asset/nx/assets.pka/C_CHR001.pkg/extra.bnsh#BNSH@0x0#var0')
            self.assertTrue(got[ss.skey(*ps2)].startswith('/data/asset/nx/loose.pkg/ed8_chr.fx#2.phyre#GLSLC@'))

    def test_corrupt_pka_is_named_and_does_not_abort(self):
        good = falcom_pka([('A.pkg', [('a.fx.phyre', bytes(32))])], [(bytes(32), b'1234', 0, None)])
        oob = bytearray(good)
        struct.pack_into('<Q', oob, len(good) - 4 - 20, len(good) + 0x100)
        for label, data, why in (
            ('short', ss.FALCOM_PKA + bytes(4), 'shorter than its header'),
            ('pkg_count_overflow', ss.FALCOM_PKA + struct.pack('<I', 1) + bytes(12), 'more than fit in the archive'),
            ('oob', bytes(oob), 'lies outside archive data'),
        ):
            with self.subTest(case=label), tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / 'bad.pka'
                path.write_bytes(data)
                jobs, consumed = ss.falcom_pka_jobs([(str(path), 'bad.pka')])
                self.assertEqual(consumed, {str(path)})
                self.assertEqual(len(jobs), 1)
                with contextlib.redirect_stdout(io.StringIO()) as log:
                    (_, keys, _, extra) = ss.scan_file(jobs[0])
                self.assertEqual(keys, {})
                self.assertEqual(extra[0], 'unreadable')
                self.assertIn(why, extra[1])
                self.assertTrue(log.getvalue().startswith('[RED] /bad.pka:'), log.getvalue())


if __name__ == '__main__':
    unittest.main()
