"""uepak / shader_split: legacy version 8 paks (UE 4.22 to 4.24, SHIN MEGAMI TENSEI V): version 11's footer with its compression method names, the per-file index, and the deletion
records (FPakEntry flag 2) a patch pak lists for the files it removes, which take the base pak's copy out of the view ue_legacy_jobs scans. Synthetic paks; no game files or keys."""
import os
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import uepak
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '010063B012DC6000'   # shader_split loads a profile at import; other tests expect no default game
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def fstr(s): b = s.encode() + b'\0'; return struct.pack('<i', len(b)) + b


def pak8(files):
    """Version 8: the data (each file behind its 53-byte FPakEntry, a compressed one's block table after it), the per-file index (blocks from the entry's start), then version 11's
    footer: key GUID, encrypted index flag, magic, version, index offset and size, SHA-1, five method names. files: (name, bytes, compressed, deleted)"""
    data, index = bytearray(), fstr('../../../') + struct.pack('<i', len(files))
    for name, raw, meth, deleted in files:
        stored = zlib.compress(raw) if meth else raw
        at = len(data); head = 53 + (4 + 16 if meth else 0); data += bytes(head) + stored
        index += fstr(name) + struct.pack('<qqqi', at, len(stored), len(raw), meth) + bytes(20)
        if meth: index += struct.pack('<i', 1) + struct.pack('<qq', head, head + len(stored))
        index += struct.pack('<BI', 2 if deleted else 0, 0x10000)
    methods = b''.join(m.ljust(32, b'\0') for m in (b'Zlib', b'', b'', b'', b''))
    foot = bytes(16) + b'\0' + struct.pack('<IIQQ', uepak.PAK_MAGIC, 8, len(data), len(index)) + bytes(20) + methods
    return bytes(data) + index + foot


class Version8(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup); self.root = Path(tmp.name)

    def pak(self, name, files):
        p = self.root / name; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(pak8(files)); return str(p)

    def test_footer_index_and_reads(self):
        body = b'cooked package bytes ' * 40
        p = self.pak('Content/Paks/Game-Switch.pak', [('Game/A.uexp', body, 0, False), ('Game/B.uexp', body * 2, 1, False), ('Game/Gone.uexp', b'', 0, True)])
        with open(p, 'rb') as f: enc, _, _, methods, ver = uepak.footer(f)
        self.assertEqual((enc, ver, methods[0]), (0, 8, 'Zlib'))
        pk = uepak.Pak(p)
        self.assertEqual(sorted(pk.files), ['Game/A.uexp', 'Game/B.uexp'])
        self.assertEqual(pk.deleted, {'Game/Gone.uexp'})
        self.assertEqual(pk.files['Game/B.uexp']['method'], 'Zlib')
        self.assertEqual((pk.read('Game/A.uexp'), pk.read('Game/B.uexp')), (body, body * 2))

    def test_method_the_footer_does_not_name(self):
        p = self.pak('Content/Paks/Game-Switch.pak', [('Game/A.uexp', b'x' * 64, 2, False)])
        with self.assertRaisesRegex(ValueError, 'names compression method 2, which the footer does not'): uepak.Pak(p)

    def test_patch_deletion_takes_the_base_copy_out_of_the_view(self):
        base = self.pak('RomFS/Content/Paks/Game-Switch.pak', [('Game/A.uexp', b'a' * 64, 0, False), ('Game/B.uexp', b'b' * 64, 0, False)])
        patch = self.pak('RomFS/Content/Paks/Game-Switch_0_P.pak', [('Game/A.uexp', b'', 0, True), ('Game/C.uexp', b'c' * 64, 0, False)])
        jobs, taken = ss.ue_legacy_jobs([(base, 'Content/Paks/Game-Switch.pak'), (patch, 'Content/Paks/Game-Switch_0_P.pak')])
        scanned = sorted((os.path.basename(p), n) for p, _, (_, names, _) in jobs for n in names)
        self.assertEqual(scanned, [('Game-Switch.pak', 'Game/B.uexp'), ('Game-Switch_0_P.pak', 'Game/C.uexp')])
        self.assertEqual(taken, {base, patch})


if __name__ == '__main__':
    unittest.main()
