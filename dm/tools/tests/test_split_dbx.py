"""shader_split: DrinkBox's SHADERS container (Guacamelee! STCE / Guacamelee! 2 shaders.dat.nx) is parsed structurally from its header, string table, uniform table,
shader data blobs and trailing 29-byte entry table; malformed containers, overlapping blobs, mismatched metadata and invalid shaders fail loudly. No game files needed."""
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '010015100B514000'   # shader_split loads a profile at import; other tests expect no default game
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def shader(stage, code_len=0x100, tag=0):
    ctl, code = bytearray(0x838), bytearray(code_len)   # STCE / Guacamelee! 2 control length
    ctl[:4] = ss.CTL_MAGIC
    struct.pack_into('<I', ctl, 0x20, len(ctl))
    struct.pack_into('<I', ctl, 0x704, len(code))
    code[:4] = ss.GFX_MAGIC
    struct.pack_into('<I', code, 0x30, (ss.STAGES.index(stage) + 1) << 10 | 1)
    if stage == 'ps' and code_len >= 0x4c: struct.pack_into('<I', code, 0x48, 1)
    if code_len > 0x80: code[0x80] = tag
    return bytes(ctl), bytes(code)


def build_dbx(specs, *, endian=1, trailing=b''):
    """Build a synthetic DrinkBox SHADERS container.
    specs: [(name, entry, stage, n_uniforms, code_len)] -> (bytes, [(ctl, code), ...])."""
    strtab = bytearray()
    uniforms = bytearray()
    data = bytearray()
    entries = bytearray()
    pairs = []
    u_cursor = 0
    for sid, (name, entry, stage, n_uni, code_len) in enumerate(specs):
        name_off = len(strtab); strtab += name.encode('utf-8') + b'\0'
        entry_off = len(strtab); strtab += entry.encode('utf-8') + b'\0'
        ctl, code = shader(stage, code_len=code_len, tag=sid + 1)
        pairs.append((ctl, code))
        u_start = u_cursor
        for u in range(n_uni):
            rec = bytearray(ss.DBX_UNIFORM)
            label = f'u_{sid}_{u}'.encode()
            rec[:len(label)] = label
            uniforms += rec
        u_cursor += n_uni
        if len(data) % 16: data += bytes(16 - len(data) % 16)
        d_off = len(data)
        blob = struct.pack('<5I', len(code), len(ctl), n_uni, n_uni * 16, 0) + bytes(n_uni * ss.DBX_UNIFORM) + code + ctl
        data += blob
        kind = 0 if stage == 'vs' else 1
        entries += struct.pack('<6IBI', name_off, entry_off, u_start, n_uni, d_off, len(blob), kind, sid)
    if len(data) % 16: data += bytes(16 - len(data) % 16)
    out = (ss.DBX
           + struct.pack('<BI', endian, len(strtab)) + bytes(strtab)
           + struct.pack('<I', u_cursor) + bytes(uniforms)
           + struct.pack('<I', len(data)) + bytes(data)
           + struct.pack('<I', len(specs)) + bytes(entries)
           + trailing)
    return out, pairs


class DbxTests(unittest.TestCase):
    def test_reads_entries_with_names_uniforms_and_separable_pairing(self):
        specs = [
            ('vs_2d_main.vsh', 'main', 'vs', 0, 0x100),
            ('vs_3d_lit.vsh', 'main', 'vs', 2, 0x200),
            ('ps_alphaglow.psh', 'main', 'ps', 3, 0x180),
        ]
        blob, pairs = build_dbx(specs)
        found = ss.dbx_shaders(blob, '/shaders.dat.nx')
        self.assertEqual([(w, st) for w, st, _, _ in found], [
            ('/shaders.dat.nx#SHDR#vs_2d_main.vsh:main', 'vs'),
            ('/shaders.dat.nx#SHDR#vs_3d_lit.vsh:main', 'vs'),
            ('/shaders.dat.nx#SHDR#ps_alphaglow.psh:main', 'ps'),
        ])
        self.assertEqual([(ctl, code) for _, _, ctl, code in found], pairs)
        self.assertTrue(all(not ss.check(w, st, ctl, code) for w, st, ctl, code in found))
        keys = ss.uniq_keys(found)
        self.assertEqual(len(keys), 3)
        cover_pairs, cover = ss.separable_pairs(keys)
        vs0, vs1, ps0 = [ss.skey(*p) for p in pairs]
        self.assertEqual(cover, [ps0])
        self.assertEqual(cover_pairs, {(vs0, ps0), (vs1, ps0)})

    def test_empty_container_returns_no_shaders(self):
        blob, _ = build_dbx([])
        self.assertEqual(ss.dbx_shaders(blob, '/empty.dat.nx'), [])

    def test_file_scanner_and_blob_scanner_dispatch_on_magic(self):
        blob, pairs = build_dbx([('v.vsh', 'main', 'vs', 1, 0x100), ('p.psh', 'main', 'ps', 1, 0x100)])
        out = []
        ss.scan_blob(blob, '/shaders.dat.nx', out)
        self.assertEqual([st for _, st, _, _ in out], ['vs', 'ps'])
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'shaders.dat.nx'
            path.write_bytes(blob)
            _, keys, _, extra = ss.scan_file((str(path), path.name))
            self.assertEqual(extra, {})
            self.assertEqual(sorted(k for k, v in keys.items() if v[1] == 'ps'), [ss.skey(*pairs[1])])

    def test_malformed_containers_fail_loudly(self):
        good, _ = build_dbx([('v.vsh', 'main', 'vs', 1, 0x100), ('p.psh', 'main', 'ps', 1, 0x100)])
        # Locate entry table and first blob inside `good`
        strtab_len = struct.unpack_from('<I', good, 9)[0]
        u_pos = 13 + strtab_len
        u_cnt = struct.unpack_from('<I', good, u_pos)[0]
        d_pos = u_pos + 4 + u_cnt * ss.DBX_UNIFORM
        d_len = struct.unpack_from('<I', good, d_pos)[0]
        data_start = d_pos + 4
        t_pos = data_start + d_len + 4

        def mut_entry(idx, **changes):
            b = bytearray(good)
            fields = list(struct.unpack_from('<6IBI', b, t_pos + idx * ss.DBX_ENTRY))
            names = ('name_off', 'entry_off', 'u_start', 'u_cnt', 'd_off', 'd_sz', 'kind', 'sid')
            for k, v in changes.items(): fields[names.index(k)] = v
            struct.pack_into('<6IBI', b, t_pos + idx * ss.DBX_ENTRY, *fields)
            return bytes(b)

        def mut_blob0(off, fmt, val):
            b = bytearray(good)
            struct.pack_into('<' + fmt, b, data_start + off, val)
            return bytes(b)

        bad_endian, _ = build_dbx([('v.vsh', 'main', 'vs', 0, 0x100)], endian=0)
        trailing, _ = build_dbx([('v.vsh', 'main', 'vs', 0, 0x100)], trailing=b'\0')
        bad_tab, _ = build_dbx([('v\tbad.vsh', 'main', 'vs', 0, 0x100)])
        short_sph, _ = build_dbx([('p.psh', 'main', 'ps', 0, 0x40)])
        non_utf8 = bytearray(good); non_utf8[13] = 0xff
        unterminated = bytearray(good); unterminated[13:13 + strtab_len] = b'a' * strtab_len
        nvn2_ctl = bytearray(good)
        ctl0 = nvn2_ctl.find(ss.CTL_MAGIC)
        nvn2_ctl[ctl0 + 4:ctl0 + 8] = b'\xc9\0\0\0'
        no_gfx = bytearray(good)
        gfx0 = data_start + ss.DBX_BLOB_HEAD + ss.DBX_UNIFORM
        no_gfx[gfx0:gfx0 + 4] = b'\0\0\0\0'
        huge_uniforms = ss.DBX + struct.pack('<BII', 1, 0, 0x10000000)

        cases = {
            'truncated header': ss.DBX + b'\x01\0',
            'unsupported endian': bad_endian,
            'huge uniform count': huge_uniforms,
            'trailing bytes after entry table': trailing,
            'truncated entry table': good[:-1],
            'non-UTF-8 name': bytes(non_utf8),
            'unterminated name': bytes(unterminated),
            'tab in name': bad_tab,
            'out-of-bounds string offset': mut_entry(0, name_off=strtab_len),
            'uniform range out of bounds': mut_entry(0, u_start=u_cnt, u_cnt=1),
            'unsupported shader kind': mut_entry(0, kind=2),
            'blob outside data section': mut_entry(0, d_off=d_len),
            'overlapping shader blobs': mut_entry(1, d_off=0),
            'blob uniform count mismatch': mut_blob0(8, 'I', 0),
            'blob span size mismatch': mut_blob0(0, 'I', 0x80),
            'stage mismatch vs kind': mut_entry(0, kind=1),
            'missing GFX magic': bytes(no_gfx),
            'truncated graphics SPH': short_sph,
            'NVN2 control section': bytes(nvn2_ctl),
        }
        for name, data in cases.items():
            with self.subTest(name), self.assertRaisesRegex(SystemExit, r'^\[RED\] '):
                ss.dbx_shaders(data, '/bad')


if __name__ == '__main__':
    unittest.main()
