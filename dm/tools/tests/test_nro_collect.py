#!/usr/bin/env python3
"""nro_collect.py: an NRR's hash table in both header layouts, and an NRO no file holds (a game that decodes it at run time, listed in missing.tsv for the NSP script's capture)."""
import hashlib, os, struct, subprocess, sys, tempfile, unittest
TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'nro_collect.py')


def nrr(hashes, version):
    table = 0x4d0 if version else 0x350
    d = bytearray(table + 32 * len(hashes))
    d[:4] = b'NRR0'; d[5] = version
    struct.pack_into('<II', d, 0x4c0 if version else 0x340, table, len(hashes))
    for i, h in enumerate(hashes): d[table + 32 * i:table + 32 * i + 32] = h
    return bytes(d)


def nro(fill):
    d = bytearray(0x100); d[0x10:0x14] = b'NRO0'; struct.pack_into('<I', d, 0x18, len(d)); d[0x40:] = bytes([fill]) * 0xc0
    return bytes(d)


class Collect(unittest.TestCase):
    def run_tool(self, version, *flags):
        here, gone = nro(1), nro(2)
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        rom, out = os.path.join(tmp.name, 'rom'), os.path.join(tmp.name, 'out')
        os.makedirs(os.path.join(rom, '.nrr'))
        open(os.path.join(rom, '.nrr', 'game.nrr'), 'wb').write(nrr([hashlib.sha256(here).digest(), hashlib.sha256(gone).digest()], version))
        open(os.path.join(rom, 'here.nro'), 'wb').write(here)
        open(os.path.join(rom, 'archive'), 'wb').write(b'CRA.' + bytes(0x100))   # scrambled: not scanned
        p = subprocess.run([sys.executable, TOOL, rom, out, *flags], capture_output=True, text=True)
        return p, out, hashlib.sha256(here).hexdigest(), hashlib.sha256(gone).hexdigest()

    def test_missing_ok(self):
        for version in (0, 1):
            p, out, here, gone = self.run_tool(version, '--missing-ok')
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
            self.assertIn(here, open(os.path.join(out, 'nro.tsv')).read())
            self.assertEqual(open(os.path.join(out, 'missing.tsv')).read(), f'{gone}\tgame.nrr\n')

    def test_missing_stops_without_the_flag(self):
        p, out, _, _ = self.run_tool(1)
        self.assertEqual(p.returncode, 1)
        self.assertFalse(os.path.exists(os.path.join(out, 'missing.tsv')))


if __name__ == '__main__': unittest.main()
