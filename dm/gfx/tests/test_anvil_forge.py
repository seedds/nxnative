#!/usr/bin/env python3
"""Anvil v28 .forge archive and loose GPUCODEONLY shader extraction (shader_split.py, profile_guess.py)."""
import io, os, struct, sys, tempfile, unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'dm/gfx/tools'))
sys.path.insert(0, str(ROOT / 'dm/games'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '010015100B514000'   # shader_split loads a profile at import; other tests expect no default game
import profile_guess, shader_split   # noqa: E402
if _had is None: os.environ.pop('DM_GAME', None)
else: os.environ['DM_GAME'] = _had


def make_shader_blob(stage='vs', seed=1):
    """Minimal valid GLSLC code area + control section for shader_split.check()."""
    if stage == 'cs':
        code = shader_split.CS_MAGIC + b'\0' * 0x2c + bytes([seed]) * 0x50
    else:
        sph_stage = {'vs': 1, 'ps': 5}[stage]
        sph = struct.pack('<I', sph_stage << 10) + b'\0' * 0x4c
        code = shader_split.GFX_MAGIC + b'\0' * 0x2c + sph + bytes([seed]) * 0x20
    ctl = bytearray(0x760)
    ctl[0:4] = shader_split.CTL_MAGIC
    struct.pack_into('<I', ctl, 0x20, len(ctl))
    struct.pack_into('<I', ctl, 0x700, 0)
    struct.pack_into('<I', ctl, 0x704, len(code))
    if stage == 'cs':
        struct.pack_into('<IIII', ctl, 0x748, 32, 1, 1, 0)
    return bytes(code), bytes(ctl)


def make_gpucodeonly(code, ctl):
    return shader_split.ANVIL_CODE + struct.pack('<I', len(code)) + code + struct.pack('<I', len(ctl)) + ctl


def make_container(chunks):
    """Build one FORGE_DATA container from [(raw_size, stored_bytes), ...]."""
    out = bytearray(shader_split.FORGE_DATA)
    out += struct.pack('<HBII', 0, 8, 0x10000, len(chunks))
    for u, blk in chunks:
        out += struct.pack('<II', u, len(blk))
    for _, blk in chunks:
        out += struct.pack('<I', 0) + blk
    return bytes(out)


def make_forge(files_list):
    """Build a synthetic Anvil v28 .forge with two chained index headers."""
    # Layout: 0x15 forge header | 0x2c data header | index headers + tables | payloads
    buf = bytearray(0x15 + 0x2c)
    buf[0:9] = shader_split.FORGE
    struct.pack_into('<IQ', buf, 9, 28, 0x15)
    # Split files_list across two chained index headers to exercise chaining
    split = max(1, len(files_list) // 2)
    groups = [files_list[:split], files_list[split:]] if len(files_list) > 1 else [files_list]
    payloads = []
    # Reserve space for headers and tables first
    ih_offs, tab_offs = [], []
    for grp in groups:
        ih_offs.append(len(buf))
        buf += bytearray(0x18)
        tab_offs.append(len(buf))
        buf += bytearray(20 * len(grp))
    for gi, grp in enumerate(groups):
        nxt = ih_offs[gi + 1] if gi + 1 < len(groups) else 0xffffffffffffffff
        struct.pack_into('<IIQQ', buf, ih_offs[gi], len(grp), 0, tab_offs[gi], nxt)
        for ei, (fid, payload) in enumerate(grp):
            off = len(buf)
            buf += payload
            struct.pack_into('<QQI', buf, tab_offs[gi] + 20 * ei, off, fid, len(payload))
    struct.pack_into('<I', buf, 0x15, len(files_list))
    struct.pack_into('<Q', buf, 0x15 + 0x24, ih_offs[0])
    return bytes(buf)


class AnvilForgeTests(unittest.TestCase):
    def test_forge_stored_and_oodle_chunks(self):
        code1, ctl1 = make_shader_blob('vs', 1)
        code2, ctl2 = make_shader_blob('cs', 2)
        rec1 = make_gpucodeonly(code1, ctl1)
        rec2 = make_gpucodeonly(code2, ctl2)
        # File 1: metadata container + stored resource container
        f1 = make_container([(4, b'meta')]) + make_container([(len(rec1), rec1)])
        # File 2: Oodle-compressed chunk (raw != stored, decoded via stubbed shader_split.oodle)
        fake_oodle = b'\x8c\x0aFAKE'
        f2 = make_container([(len(rec2), fake_oodle)])
        forge_bytes = make_forge([(0x111, f1), (0x222, f2)])

        entries = shader_split.forge_entries(io.BytesIO(forge_bytes), '/test.forge')
        self.assertEqual([fid for _, _, fid in entries], [0x111, 0x222])

        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'DataPC.forge')
            Path(p).write_bytes(forge_bytes)
            jobs, taken = shader_split.forge_jobs([(p, 'DataPC.forge')], parts=2)
            self.assertEqual(taken, {p})
            self.assertGreaterEqual(len(jobs), 1)
            found = []
            with mock.patch.object(shader_split, 'oodle', side_effect=lambda blk, u: rec2 if blk == fake_oodle and u == len(rec2) else b''):
                for job in jobs:
                    _, keys, _, _ = shader_split.scan_file_in(job)
                    found.extend(keys.values())
            stages = sorted(st for _, st, _, _ in found)
            self.assertEqual(stages, ['cs', 'vs'])

    def test_loose_shader_data_nvn_bin(self):
        code1, ctl1 = make_shader_blob('ps', 3)
        blob = make_gpucodeonly(code1, ctl1)
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'ShaderDataNVN.bin')
            Path(p).write_bytes(blob)
            _, keys, _, _ = shader_split.scan_file_in((p, 'ShaderDataNVN.bin'))
            self.assertEqual(len(keys), 1)
            self.assertEqual(next(iter(keys.values()))[1], 'ps')

    def test_profile_guess_detects_anvil_oodle_packs(self):
        src = profile_guess.shader_sources({'nvnBootstrapLoader'}, 0, [], 0, 0, anvil_forges=3)
        self.assertTrue(src['oodle_packs'][0])
        self.assertIn('Anvil forge', src['oodle_packs'][1])


if __name__ == '__main__':
    unittest.main()
