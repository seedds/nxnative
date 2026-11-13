"""patch_nso.py's search for the completer of Trinity's rendezvous and its barrier (trinity_completer_sites, trinity_completer_barrier), on hand-assembled words, without game files."""
import ast
from pathlib import Path
import struct
from typing import Any
import unittest

tree = ast.parse((Path(__file__).resolve().parents[1] / 'patch_nso.py').read_text())   # the program loads and rewrites an image: compile only these two
ns: dict[str, Any] = {'struct': struct}
exec(compile(ast.Module([n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in ('trinity_completer_sites', 'trinity_completer_barrier')], []), 'patch_nso.py', 'exec'), ns)
find, barrier = ns['trinity_completer_sites'], ns['trinity_completer_barrier']

NOP, DMB_ISH = 0xD503201F, 0xD5033BBF
def stp_entry(): return 0xA9BE7BFD   # stp x29,x30,[sp,#-0x20]!
def stp_saved(): return 0xA9014FF4   # stp x20,x19,[sp,#0x10]
def mov_fp(): return 0x910003FD      # mov x29,sp
def ldr_x8(imm=0x1E8): return 0xF9400008 | (imm // 8) << 10   # ldr x8,[x0,#imm]
def mov_w9(): return 0x52800029      # mov w9,#1
def strb(a=0x20C): return 0x39000109 | a << 10                # strb w9,[x8,#a]
def ldrb(a=0x20D): return 0x39400109 | a << 10                # ldrb w9,[x8,#a]
def cbz(): return 0x34000009 | 6 << 5                         # cbz w9,.+24
def ldr_x20(b=0x18): return 0xF9400114 | (b // 8) << 10       # ldr x20,[x8,#b]
def add_x9(c=0x514): return 0x91000289 | c << 10              # add x9,x20,#c
LDAR, CMP2 = 0x88DFFD28, 0x7100091F                           # ldar w8,[x9]; cmp w8,#2
def b_ne(): return 0x54000001 | 6 << 5                        # b.ne .+24

def completer(**k):
    return [stp_entry(), stp_saved(), mov_fp(), ldr_x8(k.get('imm', 0x1E8)), mov_w9(), strb(k.get('a', 0x20C)), ldrb(k.get('a', 0x20C) + 1), cbz(), ldr_x20(k.get('b', 0x18)),
            add_x9(k.get('c', 0x514)), LDAR, CMP2, b_ne()]
def img(*w): return bytearray(b''.join(struct.pack('<I', x) for x in w))
def hits(code): return find(code, [(0, len(code))])

class Search(unittest.TestCase):
    def test_found(self):   # Scarlet 1.0.0 main+0x28f872c
        self.assertEqual(hits(img(NOP, *completer(), NOP)), [4])

    def test_other_offsets_found(self):   # another game on the engine may lay its structures out differently
        self.assertEqual(hits(img(*completer(imm=0x1F0, a=0x30C, b=0x20, c=0x614))), [0])

    def test_each_function_reported(self):   # the caller changes nothing when there are several
        self.assertEqual(hits(img(*completer(), *completer())), [0, 52])

    def test_other_shapes_left_alone(self):
        good = completer()
        for name, i, w in (('a frame with room for locals', 0, 0xA9BD7BFD),   # stp x29,x30,[sp,#-0x30]!
                           ('another saved pair', 1, 0xA90157F4),             # stp x20,x21,[sp,#0x10]
                           ('no frame pointer', 2, NOP),
                           ('another pointer source', 3, 0xF9400028 | 0x3D << 10),   # ldr x8,[x1,#0x1e8]
                           ('another value', 4, 0x52800049),                  # mov w9,#2
                           ('release flag cleared', 5, 0x3900011F | 0x20C << 10),   # strb wzr,[x8,#0x20c]
                           ('flags not adjacent', 6, ldrb(0x20E)),
                           ('another test', 7, 0x35000009 | 6 << 5),          # cbnz w9
                           ('another base', 8, 0xF9400134 | 3 << 10),         # ldr x20,[x9,#0x18]
                           ('state not from x20', 9, 0x91000269 | 0x514 << 10),   # add x9,x19,#0x514
                           ('another load', 10, 0x88DFFD29),                  # ldar w9,[x9]
                           ('another constant', 11, 0x71000D1F),              # cmp w8,#3
                           ('another condition', 12, 0x54000000 | 6 << 5)):   # b.eq
            bad = list(good); bad[i] = w
            self.assertEqual(hits(img(*bad)), [], name)

    def test_already_fenced_left_alone(self):   # its own output, and a build that has the barrier: a dmb before the ldr, the shape is gone
        code = img(*completer())
        barrier(code, 0)
        self.assertEqual(hits(code), [])

    def test_range_is_respected(self):
        code = img(*completer())
        self.assertEqual(find(code, [(0, 48)]), [])     # the b.ne is outside
        self.assertEqual(find(code, [(4, 52)]), [])     # the entry is outside
        self.assertEqual(find(code, [(0, 52)]), [0])

    def test_unaligned_needle_ignored(self):
        code = b'\0\0' + bytes(img(LDAR, CMP2, b_ne(), NOP))
        self.assertEqual(find(code, [(0, len(code))]), [])


class Barrier(unittest.TestCase):
    def test_store_then_barrier_then_the_rest_unchanged(self):
        code = img(NOP, *completer(), NOP)
        barrier(code, 4)
        c = completer()
        self.assertEqual(struct.unpack('<15I', code), (NOP, c[0], c[1], c[3], c[4], c[5], DMB_ISH, *c[6:], NOP))


if __name__ == '__main__':
    unittest.main()
