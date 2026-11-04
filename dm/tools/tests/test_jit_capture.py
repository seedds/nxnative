"""jit_capture.py: captures from two runs in different layouts make one position-independent image (address sequences become adrp / add to the canonical layout, a request
moved off its captured offset keeps its branches out of the block, output words that are host addresses become relocations), and keys follow the profile's request bytes."""
import contextlib
import hashlib
import io
import importlib.util
from pathlib import Path
import shutil
import struct
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('jit_capture', REPO / 'dm/tools/jit_capture.py')
jc = importlib.util.module_from_spec(spec); spec.loader.exec_module(jc)

RX, RO, MAIN = 0x100000, 0x4000, 0x8000


def mov64(rd, v):   # movz xd, #v[63:48], lsl #48; movk lsl #32; movk lsl #16; movk
    return [0xd2e00000 | ((v >> 48) & 0xffff) << 5 | rd, 0xf2c00000 | ((v >> 32) & 0xffff) << 5 | rd, 0xf2a00000 | ((v >> 16) & 0xffff) << 5 | rd, 0xf2800000 | (v & 0xffff) << 5 | rd]


def b(disp): return 0x14000000 | (disp // 4) & 0x3ffffff
BR_X16, RET = 0xd61f0200, 0xd65f03c0


def record(path, cmd, off, words, out, regs, data=b'', key='k'):
    code = struct.pack(f'<{len(words)}I', *words); d32 = data.ljust(32, b'\0')
    blob = b'DMJ5' + struct.pack('<I', cmd) + struct.pack('<4Q', off, RX - off, 0, len(data)) + bytes(32) + d32 + d32
    blob += struct.pack('<Q', len(out)) + struct.pack('<i', 0) + struct.pack('<2Q', off, len(code)) + struct.pack('<2Q', 0, 0) + struct.pack('<I', len(regs))
    for name, base, size in regs: blob += name.encode().ljust(16, b'\0') + struct.pack('<3Q', base, size, 0)
    (path / f'{key}.jit').write_bytes(blob + code + out)


def decode_adrp_add(img, pos):
    adrp, add = struct.unpack_from('<2I', img, pos)
    assert adrp & 0x9f000000 == 0x90000000 and add & 0xffc00000 == 0x91000000
    imm = ((adrp >> 29) & 3) | ((adrp >> 5) & 0x7ffff) << 2
    if imm & 1 << 20: imm -= 1 << 21
    return ((pos >> 12) + imm << 12) + ((add >> 10) & 0xfff)


class JitCapture(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)

    def session(self, name, main_base, rx_base, dyn_pc):
        d = self.tmp / name; d.mkdir()
        regs = [('main', main_base, MAIN), ('jit_rx', rx_base, RX), ('jit_ro', 0x7000000000, RO), ('jit_tmem', 0x7100000000, 0x4000)]
        # The boot request at 0: an address into main, then a jump; its output holds a pointer into its own code (+0x1c)
        boot = mov64(16, main_base + 0x1234) + [BR_X16] + [RET] * 3
        record(d, 1, 0, boot, struct.pack("<QQ", 7, rx_base + 0x1c), regs, key='boot')
        # A request at 0x100 met in play (each session another one): an address into the boot code, a branch back to it, a branch inside itself
        dyn = mov64(17, rx_base + 0x10) + [b(-0x100 - 16), b(8), RET, RET]
        record(d, 4, 0x100, dyn, struct.pack('<II', 3, 0), regs, data=struct.pack('<II', dyn_pc, 1), key=f'dyn{dyn_pc:x}')
        return d

    def test_image(self):
        a = self.session('a', 0x10_0000_0000, 0x20_0000_0000, 0x80001000)
        b_ = self.session('b', 0x10_0400_0000, 0x20_0800_0000, 0x80002000)
        out = self.tmp / 'img'
        self.assertEqual(jc.build([str(a), str(b_)], str(out)), 0)
        img = (out / 'jit.img').read_bytes(); rows = (out / 'jit.tsv').read_text().splitlines()
        mods = {r.split('\t')[1]: int(r.split('\t')[2], 16) for r in rows if r.startswith('mod\t')}
        self.assertEqual(mods['main'], RX)   # the module right after the code memory, 16 KB aligned
        reqs = {r.split('\t')[2] + ':' + r.split('\t')[3]: r.split('\t') for r in rows if r.startswith('req\t')}
        boot = next(v for k, v in reqs.items() if k.startswith('1:'))
        self.assertEqual(int(boot[3], 16), 0)   # in both captures at 0: kept
        self.assertEqual(decode_adrp_add(img, 0), mods['main'] + 0x1234)
        self.assertEqual(struct.unpack_from('<2I', img, 8), (jc.NOP, jc.NOP))
        self.assertEqual(boot[10], '8=jit_rx+1c')   # the pointer that differs by the code memory's base, as an image offset
        dyn = sorted(int(v[3], 16) for k, v in reqs.items() if k.startswith('4:'))
        # Each was met in one capture only, so both move after the code every capture placed alike (the boot request), each to an offset of its own
        self.assertNotIn(0x100, dyn)
        self.assertGreaterEqual(dyn[0], 32); self.assertGreaterEqual(dyn[1], dyn[0] + 32)
        for p in dyn:
            self.assertEqual(decode_adrp_add(img, p), 0x10)   # the address of the boot code, wherever the request sits
            w = struct.unpack_from('<I', img, p + 16)[0]; disp = (((w & 0x3ffffff) ^ 0x2000000) - 0x2000000) * 4
            self.assertEqual(p + 16 + disp, 0, 'the branch back to the boot code')
            self.assertEqual(struct.unpack_from('<I', img, p + 20)[0], b(8), 'a branch inside the request is unchanged')

    def test_session_without_its_boot(self):
        # A session that started after an in-process program switch: its code memory elsewhere, its boot request skipped (already in the folder), only a play request
        # of its own, which calls into boot code it did not capture. It is left out, and the image is the one the two whole sessions make
        a = self.session('a', 0x10_0000_0000, 0x20_0000_0000, 0x80001000)
        b_ = self.session('b', 0x10_0400_0000, 0x20_0800_0000, 0x80002000)
        c = self.tmp / 'c'; c.mkdir(); rx = 0x20_1000_0000
        regs = [('main', 0x10_0800_0000, MAIN), ('jit_rx', rx, RX), ('jit_ro', 0x7000000000, RO), ('jit_tmem', 0x7100000000, 0x4000)]
        record(c, 4, 0x100, mov64(17, rx + 0x10) + [b(-0x100 - 16), RET], struct.pack('<II', 3, 0), regs, data=struct.pack('<II', 0x80003000, 1), key='dyn80003000')
        whole, part = self.tmp / 'whole', self.tmp / 'part'
        self.assertEqual(jc.build([str(a), str(b_)], str(whole)), 0)
        err = io.StringIO()
        with contextlib.redirect_stderr(err): self.assertEqual(jc.build([str(a), str(b_), str(c)], str(part)), 0)
        self.assertIn(f'capture session {rx:#x} left out', err.getvalue())
        for f in ('jit.img', 'jit.out', 'jit.tsv'): self.assertEqual((part / f).read_bytes(), (whole / f).read_bytes(), f)

    def test_output_needs_second_capture(self):
        a = self.session('a', 0x10_0000_0000, 0x20_0000_0000, 0x80001000)
        with self.assertRaises(AssertionError):   # the boot output's pointer cannot be told from data in one capture
            jc.build([str(a)], str(self.tmp / 'img'))

    def test_key_bytes(self):
        # With the profile's request bytes the key takes exactly those bytes, as hle_jit.c jit_key does
        r = {'cmd': 4, 'dsize': 32, 'data': b'\xff' * 32, 'raw': struct.pack('<IIQQII', 0x802bbb5c, 1, 0x1234, 0xd8ed00, 0x4000, 1), 'insha': bytes(range(32))}
        mask = bytes([0xff] * 8 + [0] * 16 + [0xff] * 4 + [0] * 4)
        k = jc.rekey(r, {4: mask})
        want = hashlib.sha256(struct.pack('<QQ', 4, 32) + struct.pack('<IIQQII', 0x802bbb5c, 1, 0, 0, 0x4000, 0) + bytes(range(32))).hexdigest()[:32]
        self.assertEqual(k, want)
        r2 = dict(r, raw=r['raw'][:28] + b'\x07\0\0\0')   # other padding: the same request
        self.assertEqual(jc.rekey(r2, {4: mask}), k)
        self.assertNotEqual(jc.rekey(r, {}), k)   # without request bytes: the value-masked data


if __name__ == '__main__':
    unittest.main()
