"""nx_extract (dm/tools/nx_extract) on synthetic NSPs, with no keys and no game files: a plaintext Program NCA (its header and section in the clear, which LibHac reads without a
header key) whose ExeFS section is a PFS0 under a SHA-256 hash table, beside a common ticket, in an NSP's PFS0. What runs: the key file reader on an empty prod.keys, the NSP's
PFS0, the ticket import, the NCA header, the ExeFS copy through the hash check, the build ID line of an NSO, the empty RomFS image of a program without a data section; and, with
one ExeFS byte changed after hashing, the damaged-file report (exit code 3) and --allow-damaged. Not run: NCA decryption, RomFS (IVFC) sections, update layering, DLC, XCI.
NX_EXTRACT names the program (a published nx_extract, dm/tools/publish_port_tools.sh, or a bundle's bin/nx_extract[.exe]); without it the test builds nx_extract with dotnet
(the .NET SDK 10) into a temporary folder, and skips when there is neither."""
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
import romfs  # noqa: E402

TITLE = 0x0100000000ABC000   # not a real title
BUILD_ID = bytes(range(0x20, 0x40))
BLOCK = 0x1000


def pfs0(files):
    """A PFS0 of [(name, bytes)]: header, entries, string table (padded to 0x20), data"""
    names = b''.join(n.encode() + b'\0' for n, _ in files)
    names += b'\0' * (-(16 + 24 * len(files) + len(names)) % 0x20)
    out, data, noff = bytearray(b'PFS0' + struct.pack('<III', len(files), len(names), 0)), bytearray(), 0
    for n, b in files:
        out += struct.pack('<QQII', len(data), len(b), noff, 0); data += b; noff += len(n) + 1
    return bytes(out + names + data)


def nso():   # an NSO header: what nx_extract reads of it is the magic and the build ID at 0x40
    return b'NSO0' + bytes(0x3c) + BUILD_ID + bytes(0x100 - 0x60)


def nca(exefs):
    """A plaintext Program NCA (NCA3) with one section: the ExeFS PFS0 (padded to whole hash blocks) behind its SHA-256 hash table, encryption none"""
    data = exefs + bytes(-len(exefs) % BLOCK)
    table = b''.join(hashlib.sha256(data[i:i + BLOCK]).digest() for i in range(0, len(data), BLOCK))
    data_off = len(table) + (-len(table) % 0x200)
    section = table + bytes(data_off - len(table)) + data
    fs = bytearray(0x200)
    struct.pack_into('<HBBB', fs, 0, 2, 1, 2, 1)   # version 2, PartitionFs, HierarchicalSha256, no encryption
    struct.pack_into('<32sIIQQQQ', fs, 8, hashlib.sha256(table).digest(), BLOCK, 2, 0, len(table), data_off, len(data))
    empty = bytearray(0x200); struct.pack_into('<H', empty, 0, 2)   # the unused sections' headers: version 2, as in every NCA
    start = 0xC00
    h = bytearray(0x400)
    h[0x200:0x204] = b'NCA3'
    struct.pack_into('<BBBBQQII', h, 0x204, 0, 0, 0, 0, start + len(section), TITLE, 0, 0x000C1100)   # download, Program, key generation 0, content size, program ID, index, SDK
    struct.pack_into('<II', h, 0x240, start // 0x200, (start + len(section)) // 0x200)
    h[0x280:0x2A0] = hashlib.sha256(fs).digest()
    return bytes(h + fs + empty * 3) + section


def ticket():   # a common ticket (title key type 0) with a made-up rights ID and title key: nx_extract adds them to its key set
    t = bytearray(0x2C0)
    struct.pack_into('<I', t, 0, 0x10004)
    t[0x180:0x190] = b'\x5a' * 16; t[0x281] = 0; t[0x2A0:0x2B0] = bytes(15) + b'\x01'
    return bytes(t)


NPDM = b'META' + bytes(0x1FFC)   # spans the ExeFS's hash blocks 0 to 2: main and the PFS0 header lie in block 0


def nsp(damaged=False):
    n = bytearray(nca(pfs0([('main', nso()), ('main.npdm', NPDM)])))
    if damaged: n[-1] ^= 0xFF   # a byte of the last hash block (main.npdm's end and the padding) after its hash was taken
    return pfs0([('0123456789abcdef0123456789abcdef.nca', bytes(n)), ('00000000000000000000000000000001.tik', ticket())])


class Extract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='dm-nx-extract-'); cls.root = Path(cls.tmp.name)
        cls.nx = os.environ.get('NX_EXTRACT')
        if cls.nx: return
        if not shutil.which('dotnet'):
            cls.tmp.cleanup(); raise unittest.SkipTest('no NX_EXTRACT and no dotnet (.NET SDK 10) to build nx_extract')
        art = cls.root / 'build'
        r = subprocess.run(['dotnet', 'build', str(REPO / 'dm/tools/nx_extract'), '-c', 'Release', '--artifacts-path', str(art), '-v', 'quiet', '-nologo'],
                           capture_output=True, text=True, timeout=600)
        if r.returncode: cls.tmp.cleanup(); raise AssertionError('dotnet build nx_extract: ' + (r.stdout + r.stderr)[-2000:])
        cls.nx = str(art / 'bin/nx_extract/release' / ('nx_extract.exe' if os.name == 'nt' else 'nx_extract'))

    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()

    def extract(self, name, damaged=False, *extra):
        d = self.root / name; d.mkdir()
        (d / 'prod.keys').write_text('')   # no keys: the NCA is in the clear
        (d / 'game.nsp').write_bytes(nsp(damaged))
        r = subprocess.run([self.nx, str(d / 'prod.keys'), str(d / 'game.nsp'), '-', str(d / 'out'), '--romfs-image', *extra], capture_output=True, text=True, timeout=120)
        print(f'\n{name}: nx_extract exit {r.returncode}\n{r.stdout.strip()}\n{r.stderr.strip()}'.rstrip())
        return d / 'out', r

    def test_usage(self):
        r = subprocess.run([self.nx], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 2); self.assertIn('usage: nx_extract', r.stderr)

    def test_exefs_build_id_and_empty_romfs(self):
        out, r = self.extract('clean')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(f'title {TITLE:016x}', r.stdout)
        self.assertIn(f'main build id {BUILD_ID[:16].hex()}', r.stdout)
        self.assertIn('program has no data section: empty RomFS', r.stdout)
        self.assertEqual((out / 'ExeFS/main').read_bytes(), nso())
        self.assertEqual((out / 'ExeFS/main.npdm').read_bytes(), NPDM)
        self.assertEqual(sorted(p.name for p in (out / 'ExeFS').iterdir()), ['main', 'main.npdm'])
        self.assertEqual(list(romfs.Path(out / 'RomFS.img').iterdir()), [])
        self.assertFalse((out / 'RomFS').exists())

    def test_damaged_exefs(self):
        out, r = self.extract('damaged', True)
        self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
        self.assertIn('[RED] hash check failed: /main.npdm', r.stdout)
        self.assertIn('[RED] the NSP is damaged: 1 files fail', r.stdout)
        out, r = self.extract('allowed', True, '--allow-damaged')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('written as read (--allow-damaged)', r.stdout)


if __name__ == '__main__': unittest.main()
