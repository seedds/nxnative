"""shader_split: Big Ant's chunked zstd files are decoded only when their chunks end exactly at the end of the file (bigant_chunks), and the shader pack inside one names each
shader's control section and code area through its records, with a program's records 8 bytes apart (bigant_shaders). No game files needed."""
import os
import random
import struct
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '010029B0118E8000'   # shader_split loads a profile at import; other tests expect no default game
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had

CTL_LEN = 0x880


def control(code_len):
    """An NVN control section: the magic, its own length at +0x20 and the code area's length at +0x704"""
    c = bytearray(CTL_LEN)
    c[:4] = ss.CTL_MAGIC
    struct.pack_into('<I', c, 0x20, CTL_LEN)
    struct.pack_into('<I', c, 0x704, code_len)
    return bytes(c)


def code(stage, n, fill):
    """A code area of n bytes: a graphics header with the SPH's shader type at +0x30, or a compute header"""
    c = bytearray(bytes([fill]) * n)
    if stage == 'cs': c[:4] = ss.CS_MAGIC
    else:
        c[:4] = ss.GFX_MAGIC
        struct.pack_into('<I', c, 0x30, {'vs': 1, 'ps': 5}[stage] << 10 | 1)
    return bytes(c)


def pack(shaders, programs, stray=()):
    """shaders: [(stage, code length)]; programs: [[shader index, ...]], each written as records 8 bytes apart; stray: shader indices that also get a record whose code offset is 0.
    Layout: header, records, control sections, then the code areas from the first code header on"""
    n_rec = sum(len(p) for p in programs) + len(stray)
    rec_at = 0x40
    ctl_at = (rec_at + 0x80 * (len(programs) + len(stray)) + 0xff) & ~0xff
    code_at = ctl_at + CTL_LEN * len(shaders)
    offs, o = [], 0
    for _, n in shaders: offs.append(o); o += n
    d = bytearray(code_at + o)
    d[:4] = ss.BIGANT_SHADERS; struct.pack_into('<I', d, 8, 0x13)
    for i, (st, n) in enumerate(shaders):
        d[ctl_at + CTL_LEN * i:ctl_at + CTL_LEN * (i + 1)] = control(n)
        d[code_at + offs[i]:code_at + offs[i] + n] = code(st, n, 0x10 + i)
    def record(p, i, code_off): struct.pack_into('<II', d, p, code_off, ctl_at + CTL_LEN * i - (p + 4))
    p = rec_at
    for prog in programs:
        for k, i in enumerate(prog): record(p + 8 * k, i, offs[i])
        p += 0x80
    for i in stray: record(p, i, 0); p += 0x80
    assert n_rec
    return bytes(d), [code_at + x for x in offs]


def chunks(data, sizes, stored=()):
    """data split at sizes, each part a zstd frame behind a u32 that counts the chunk (those 4 bytes included); the parts whose index is in stored are the bytes themselves"""
    z, out, o = ss.zstd_mod(), b'', 0
    for i, n in enumerate(sizes):
        f = data[o:o + n] if i in stored else z.compress(data[o:o + n]); out += struct.pack('<I', 4 + len(f)) + f; o += n
    return out


class Chunks(unittest.TestCase):
    def test_a_chain_of_chunks_decodes_to_the_joined_frames(self):
        data = bytes(range(256)) * 300
        self.assertEqual(ss.bigant_chunks(chunks(data, [20000, 30000, 26800]), 'f'), data)
        self.assertEqual(ss.bigant_chunks(chunks(b'one', [3]), 'f'), b'one')

    def test_blocks_zstd_did_not_shrink_are_stored_in_place(self):
        data = random.Random(24).randbytes(3 * ss.BIGANT_BLOCK + 5000)   # bytes zstd does not shrink
        sizes = [ss.BIGANT_BLOCK] * 3 + [5000]
        self.assertEqual(ss.bigant_chunks(chunks(data, sizes, stored={1, 2}), 'mid'), data)   # two stored blocks in a row between frames
        self.assertEqual(ss.bigant_chunks(chunks(data, sizes, stored={3}), 'last'), data)   # a short stored block at the end
        self.assertIsNone(ss.bigant_chunks(chunks(data, sizes, stored={0}), 'first'))   # the first chunk is a frame
        over = chunks(data, [1000, ss.BIGANT_BLOCK + 1], stored={1})
        self.assertEqual(ss.bigant_chunks(chunks(data, [1000, ss.BIGANT_BLOCK], stored={1}), 'block'), data[:1000 + ss.BIGANT_BLOCK])
        self.assertIsNone(ss.bigant_chunks(over, 'over'))   # a stored block is at most BIGANT_BLOCK bytes

    def test_other_size_prefixed_frames_are_not_chains(self):
        f = ss.zstd_mod().compress(b'x' * 5000)
        self.assertIsNone(ss.bigant_chunks(struct.pack('<I', 5000) + f, 'hal'))   # HAL's .cmp: the word is the uncompressed size
        good = chunks(b'abc' * 1000, [3000])
        self.assertIsNone(ss.bigant_chunks(good + b'\0\0', 'tail'))   # bytes after the last chunk
        self.assertIsNone(ss.bigant_chunks(good[:-1], 'short'))   # a chunk running past the end
        self.assertIsNone(ss.bigant_chunks(struct.pack('<I', len(good)) + b'NOPE' + good[8:], 'magic'))


class ShaderPack(unittest.TestCase):
    def test_programs_pair_their_vertex_and_pixel_shaders(self):
        shaders = [('ps', 0x200), ('vs', 0x300), ('ps', 0x100), ('vs', 0x300), ('cs', 0x400)]
        d, at = pack(shaders, [[0, 1], [2, 3], [4]])
        found = ss.bigant_shaders(d, '/data/e4/pack')
        self.assertEqual(sorted(st for _, st, _, _ in found), ['cs', 'ps', 'ps', 'vs', 'vs'])
        for (_, st, ctl, cd), (s, n) in zip(sorted(found, key=lambda f: f[3][4]), shaders):
            self.assertEqual((st, len(ctl), len(cd)), (s, CTL_LEN, n))
        pairs = ss.var_pairs(found)
        keys = {cd[4]: ss.skey(ctl, cd) for _, _, ctl, cd in found}   # the fill byte names the shader
        self.assertEqual(pairs, {(keys[0x11], keys[0x10]), (keys[0x13], keys[0x12])})
        self.assertTrue(all('#var' not in w for w, st, _, _ in found if st == 'cs'))

    def test_a_zero_word_before_a_control_pointer_does_not_take_the_first_code_area(self):
        shaders = [('vs', 0x300), ('ps', 0x300), ('ps', 0x100)]   # shader 1's code is as long as the first one
        d, at = pack(shaders, [[1], [0], [2]], stray=[1])
        found = {cd[4]: cd for _, _, _, cd in ss.bigant_shaders(d, '/pack')}
        self.assertEqual(sorted(found), [0x10, 0x11, 0x12])
        self.assertEqual(found[0x11], d[at[1]:at[1] + 0x300])


if __name__ == '__main__':
    unittest.main()
