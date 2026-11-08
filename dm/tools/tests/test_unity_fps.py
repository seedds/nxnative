"""Unity's frame-rate settings, found through the engine's internal-call table (dm/tools/unity_fps.py), on synthetic code, no game files: Unity 6000's layout (the QualitySettings
global read directly, levels of 1 << 8 bytes), Unity 2021's (a thunk to the targetFrameRate setter, QualitySettings from the managers array, levels of 0xa8 bytes by madd), a getter
it cannot read, names with no function table after them, and a main without the table."""
import struct
import sys
import unittest
from typing import Any
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from nso_asm import ADRP, ADDI, LDRX, B
import unity_fps

RET = 0xd65f03c0
TEXT, RO, DATA, END = 0x8000, 0x8000, 0xa000, 0xc000
MOD0, DYN, RELA, STRINGS, NAMES = 0x40, 0x9000, 0x9100, 0x8800, 0xa100
TFR_G, QS_G, CTX = 0xa800, 0xa808, 0xa900   # the targetFrameRate global, the QualitySettings global, the managers array
OTHER, REAL = 'UnityEngine.Application::get_targetFrameRate', 'UnityEngine.Internal::SetTargetFrameRate'


def str_w0(n, off): return 0xB9000000 | (off // 4) << 10 | n << 5
def ldrsw(t, n, off): return 0xB9800000 | (off // 4) << 10 | n << 5 | t
def ldr_w(t, n, off): return 0xB9400000 | (off // 4) << 10 | n << 5 | t
def add_lsl(d, n, m, s): return 0x8B000000 | m << 16 | s << 10 | n << 5 | d
def madd(d, n, m, a): return 0x9B000000 | m << 16 | a << 10 | n << 5 | d
def movz(d, v): return 0x52800000 | v << 5 | d
def bl(frm, to): return 0x94000000 | (((to - frm) >> 2) & 0x3FFFFFF)
def ldr_idx(t, n, m): return 0xF860D800 | m << 16 | n << 5 | t   # ldr xT, [xN, wM, sxtw #3]


def setter(at): return [ADRP(8, at, TFR_G), str_w0(8, TFR_G & 0xfff), RET]


def getter6000(at): return [ADRP(8, at, QS_G), LDRX(8, 8, (QS_G & 0xfff) // 8), ldrsw(9, 8, 0x64), LDRX(8, 8, 0x20 // 8), add_lsl(8, 8, 9, 8), ldr_w(0, 8, 0x88), RET]


def context(at):   # GetManagerFromContext(K) at at + 8: mov w0, #11; b there; adrp x8; add x8, x8, #lo; ldr x0, [x8, w0, sxtw #3]; ret
    return [movz(0, 11), B(at + 4, at + 8), ADRP(8, at + 8, CTX), ADDI(8, 8, CTX & 0xfff), ldr_idx(0, 8, 0), RET]


def getter2021(at, ctx):
    return [0xA9BF7BFD, bl(at + 4, ctx), ldrsw(8, 0, 0x4c), movz(10, 0xa8), LDRX(9, 0, 0x30 // 8), madd(8, 8, 10, 9), ldr_w(0, 8, 0x68), 0xA8C17BFD, RET]


def image(funcs: dict, names=(unity_fps.TFR, OTHER, unity_fps.VSYNC), table=True):
    """funcs: {name: code(at) or (code(at, *refs), ref names)}; the name table at NAMES with the function table right after it (relative relocations)"""
    img = bytearray(END)
    struct.pack_into('<I', img, 4, MOD0); img[MOD0:MOD0 + 4] = b'MOD0'; struct.pack_into('<i', img, MOD0 + 4, DYN - MOD0)
    rel, at, s, placed = [], 0x1000, STRINGS, {}
    for name in ('ctx',) + tuple(names):
        f: Any = funcs.get(name)
        if f is None: continue
        code, refs = f if isinstance(f, tuple) else (f, [])
        words = code(at, *[placed[r] for r in refs])
        struct.pack_into('<' + 'I' * len(words), img, at, *words); placed[name] = at; at += 4 * len(words) + 0x20
    for i, name in enumerate(names):
        img[s:s + len(name) + 1] = name.encode() + b'\0'
        rel.append((NAMES + 8 * i, s)); s += len(name) + 1
        if table: rel.append((NAMES + 8 * (len(names) + i), placed.get(name, 0x10)))
    struct.pack_into('<qQqQqQ', img, DYN, 7, RELA, 8, 24 * len(rel), 0, 0)
    for k, (off, val) in enumerate(rel): struct.pack_into('<QQq', img, RELA + 24 * k, off, 1027, val)   # R_AARCH64_RELATIVE
    return img


def find(img): return unity_fps.find(bytes(img), TEXT, RO, DATA)


class FindTest(unittest.TestCase):
    def test_unity6000(self):
        out, why = find(image({unity_fps.TFR: setter, unity_fps.VSYNC: getter6000}))
        self.assertEqual(out, {'target': TFR_G, 'qs': (QS_G, 0x64, 0x20, 0x100, 0x88)}, why)

    def test_unity2021_thunk_and_managers_array(self):
        thunk = (lambda at, real: [B(at, real)], [REAL])
        funcs = {'ctx': context, unity_fps.VSYNC: (getter2021, ['ctx']), REAL: setter, unity_fps.TFR: thunk}
        out, why = find(image(funcs, names=(REAL, unity_fps.TFR, OTHER, unity_fps.VSYNC)))
        self.assertEqual(out, {'target': TFR_G, 'qs': (CTX + 8 * 11, 0x4c, 0x30, 0xa8, 0x68)}, why)

    def test_unread_getter_is_red(self):
        out, why = find(image({unity_fps.TFR: setter, unity_fps.VSYNC: lambda at: [movz(0, 1), RET]}))
        self.assertEqual(out, {'target': TFR_G}, why)
        self.assertIn('[RED] vSyncCount getter', why)

    def test_names_without_functions(self):
        out, why = find(image({unity_fps.TFR: setter, unity_fps.VSYNC: getter6000}, table=False))
        self.assertEqual(out, {}, why)

    def test_not_unity(self):
        out, why = find(image({}, names=('Foo::bar',)))
        self.assertEqual(out, {}, why)


if __name__ == '__main__':
    unittest.main()
