"""shader_split: Cornfox & Bros. VFS2 archives (Oceanhorn: Monster of Uncharted Seas data.vfs) index directory and file tables with raw, bitwise-inverted, or length-prefixed zlib payloads. No game files needed."""
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '010015100B514000'   # shader_split loads a profile at import; other tests expect no default game
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def glslc_record(stage='vs', ctl_len=0x838):
    code = bytearray(0x100)
    code[:4] = ss.GFX_MAGIC
    struct.pack_into('<I', code, 0x30, (ss.STAGES.index(stage) + 1) << 10 | 1)
    if stage == 'ps': struct.pack_into('<I', code, 0x48, 1)
    control = bytearray(ctl_len)
    control[:4] = ss.CTL_MAGIC
    struct.pack_into('<I', control, 0x20, len(control))
    struct.pack_into('<I', control, 0x704, len(code))
    header = bytearray(0x48)
    header[:4] = ss.GLSLC_MAGIC
    raw = bytearray(header + control + code)
    struct.pack_into('<I', raw, 0x44, len(raw))
    return bytes(raw), bytes(control), bytes(code)


def build_vfs2(dirs, files, *, with_names=True):
    """Build a synthetic VFS2 container. dirs: [(name, parent)]; files: [(dir_idx, name, flags, payload_bytes)]."""
    dtab = b''.join(struct.pack('<IIiii', i, i, parent, -1, -1) for i, (_, parent) in enumerate(dirs))
    payload, ftab = bytearray(), bytearray()
    for i, (dir_idx, _, flg, blob) in enumerate(files):
        ftab += struct.pack('<6I', i, i, flg, dir_idx, len(payload), len(blob))
        payload += blob
    data_start = 8 + len(dtab) + 4 + len(ftab) + 4
    names_off = data_start + len(payload) if with_names else 0
    out = bytearray(ss.VFS2 + struct.pack('<I', len(dirs)) + dtab + struct.pack('<I', len(files)) + ftab + struct.pack('<I', names_off) + payload)
    if with_names:
        out += struct.pack('<I', len(files))
        for _, name, _, _ in files:
            enc = name.encode(); out += struct.pack('<I', len(enc)) + enc
        out += struct.pack('<I', len(dirs))
        for name, _ in dirs:
            enc = name.encode(); out += struct.pack('<I', len(enc)) + enc
    return bytes(out)


class Vfs2Tests(unittest.TestCase):
    def test_reads_zlib_raw_and_inverted_entries_with_nested_dir_paths(self):
        vs_raw, vs_ctl, vs_code = glslc_record('vs')
        ps_raw, ps_ctl, ps_code = glslc_record('ps')
        gs_raw, gs_ctl, gs_code = glslc_record('gs')
        junk_z = struct.pack('<I', 2400) + zlib.compress(b'not a shader' * 200)
        dirs = [('', -1), ('effects', 0), ('sub', 1)]
        files = [
            (1, 'a.vs', 2, struct.pack('<I', len(vs_raw)) + zlib.compress(vs_raw)),
            (2, 'b.ps', 0, ps_raw),
            (1, 'c.gs', 1, gs_raw.translate(ss._VFS2_INV)),
            (1, 'note.xtfx', 2, junk_z),
            (1, 'font.ttf', 0, b'OTTO' + bytes(64)),
        ]
        archive = build_vfs2(dirs, files)
        got = ss.vfs2_shaders(archive, '/data.vfs')
        self.assertEqual([(w, st, ctl, code) for w, st, ctl, code in got], [
            ('/data.vfs/effects/a.vs#GLSLC@0x0', 'vs', vs_ctl, vs_code),
            ('/data.vfs/effects/sub/b.ps#GLSLC@0x0', 'ps', ps_ctl, ps_code),
            ('/data.vfs/effects/c.gs#GLSLC@0x0', 'gs', gs_ctl, gs_code),
        ])

    def test_reads_archive_without_optional_name_table_and_via_scan_file_mmap(self):
        vs_raw, vs_ctl, vs_code = glslc_record('vs')
        archive = build_vfs2([('', -1)], [(0, 'a.vs', 2, struct.pack('<I', len(vs_raw)) + zlib.compress(vs_raw))], with_names=False)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'pack.vfs'
            path.write_bytes(archive)
            _, keys, _, extra = ss.scan_file((str(path), path.name))
            self.assertEqual(extra, {})
            self.assertEqual(list(keys), [ss.skey(vs_ctl, vs_code)])

    def test_malformed_containers_and_shader_entries_fail_loudly(self):
        vs_raw, _, _ = glslc_record('vs')
        vs_z = zlib.compress(vs_raw)
        cases = {
            'truncated header': b'VFS2\0\0',
            'oversized dir count': b'VFS2' + struct.pack('<III', 0x100000, 0, 0),
            'entry past data end': build_vfs2([('', -1)], [(0, 'a.vs', 0, vs_raw)], with_names=False)[:-10],
            'wrong inflated length': build_vfs2([('', -1)], [(0, 'a.vs', 2, struct.pack('<I', len(vs_raw) + 1) + vs_z)]),
            'huge inflated length bomb': build_vfs2([('', -1)], [(0, 'a.vs', 2, struct.pack('<I', 0x7fffffff) + vs_z)]),
            'corrupt zlib stream': build_vfs2([('', -1)], [(0, 'a.vs', 2, struct.pack('<I', len(vs_raw)) + b'\x78\x9c\xff\xff\xff\xff')]),
            'unsupported mode': build_vfs2([('', -1)], [(0, 'a.vs', 3, vs_raw)]),
            'directory parent cycle': build_vfs2([('a', 1), ('b', 0)], [(0, 'a.vs', 0, vs_raw)]),
        }
        for name, data in cases.items():
            with self.subTest(name), self.assertRaisesRegex(SystemExit, r'^\[RED\] '):
                ss.vfs2_shaders(data, '/bad.vfs')


if __name__ == '__main__':
    unittest.main()
