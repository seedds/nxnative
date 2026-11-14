#!/usr/bin/env python3
"""NVN2 buffer addresses through the offline translator (dmsh_xlate, arch sm86), as PRAGMATA's compute shaders read them: a buffer entry the game pushed into push block 1
(c0 layout +0xaa, after push block 0 and before the uniform entries, mapped to c[24] + 0x100), and a bindless constant buffer read (cx[UR][offset]) through a pushed handle. Needs DMSH_TEST_XLATE (the
translator) like the other translator tests."""
import json, os, struct, subprocess, tempfile, unittest
from pathlib import Path

EXIT = (0x000fea0003800000, 0x000000000000794d)
ULDC64_C0_20 = (0x000fe20000000a00, 0x0000080000047ab9)   # ULDC.64 UR4, c[0][0x20]
ULDC64_C0_10 = (0x000fe20000000a00, 0x0000040000047ab9)   # ULDC.64 UR4, c[0][0x10]
LDG_UR4 = (0x000ea200081e8900, 0x00000004ff057981)       # LDG.E R5, [UR4] (Ra = RZ)
ULDCX = (0x000fe40008000800, 0x0000040004057ab9)         # ULDC UR5, cx[UR4][0x10], a word (bit 91: the bank's handle in the uniform register at 24)
MOV_R5_UR5 = (0x000fe20008000000, 0x0000000500057c02)    # MOV R5, UR5
SUATOM_R5 = (0x004ea200001ee000, 0x00000c05030c7593)     # SUATOM.ADD on image slot 0 (c0 word 12), adding R5: keeps R5 alive
ULDC64_C29_10 = (0x000fe20000000a00, 0x0000040000047ab9 | 29 << 54)   # ULDC.64 UR4, c[29][0x10]: the pixel stage's buffer bank, entry 1
LDG_R0_UR4 = (0x000ea200081e8900, 0x00000004ff007981)    # LDG.E R0, [UR4] (Ra = RZ): colour 0's x, so it stays
ISETP_NE_P0_R5 = (0x004fc20003f05070, 0x000000ff0500720c)   # ISETP.NE.U32.AND P0, PT, R5, RZ, PT, waiting on the LDG's scoreboard (2), as SUATOM_R5
MOV_R6_C3_10_NP0 = (0x000fe20000000f00, (0xa02 | 0x8 << 12 | 6 << 16 | 0x10 << 38 | 3 << 54))   # @!P0 MOV R6, c[3][0x10]: a word of the game's uniform buffer
MOV_R6_R5_P0 = (0x000fe20000000f00, (0x202 | 6 << 16 | 5 << 32))                                # @P0 MOV R6, R5: the word loaded from memory
LOP3_R6_OR_SAMPLER = (0x000fe20000000000 | 0xff | 0xfc << 8 | 7 << 17 | 7 << 23, (0x812 | 0x7000 | 6 << 16 | 6 << 24 | 0x10400000 << 32))   # LOP3.LUT R6, R6, 0x10400000, RZ, 0xfc: | sampler 0x104
TEX_LZ_R6 = (0x000fe20000000000 | 0xff | 1 << 8 | 7 << 17 | 1 << 23, (0x361 | 0x7000 | 7 << 16 | 2 << 24 | 6 << 32 | 1 << 61))   # TEX.LZ R7, R2, R6 (the handle), 2D, x only
MOV_R6_C3_10 = (MOV_R6_C3_10_NP0[0], MOV_R6_C3_10_NP0[1] & ~(0xf << 12) | 0x7 << 12)   # MOV R6, c[3][0x10], unconditional
TEX_LZ_R6_ARRAY = (TEX_LZ_R6[0], TEX_LZ_R6[1] & ~(7 << 61) | 5 << 61)                  # the same read as ARRAY_2D
SUATOM_R7 = (SUATOM_R5[0], (SUATOM_R5[1] & ~(0xff << 32)) | 7 << 32)   # adding R7: keeps the texture read
PS_SPH = struct.pack('<32I', *([0x000c5482] + [0] * 4 + [0xb0000000] + [0] * 12 + [0xf] + [0] * 13))   # a pixel shader that writes colour 0 (as test_dmsh_ampere_mem.py's)


def inst(hi, lo): return struct.pack('<QQ', lo, hi)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class Nvn2Buffers(unittest.TestCase):
    def translate(self, code, nvn2_cs):
        with tempfile.TemporaryDirectory(prefix='nvn2_buffers_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(code)
            job = [dict(key='k', stage='cs', arch='sm86', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], nvn2_cs=nvn2_cs)]
            (work / 'jobs.json').write_text(json.dumps(job))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            return json.loads((work / 'k.res.json').read_text())

    def translate_ps(self, code, stage_bank):
        with tempfile.TemporaryDirectory(prefix='nvn2_buffers_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(PS_SPH + code)
            (work / 'jobs.json').write_text(json.dumps([dict(key='k', stage='ps', arch='sm86', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, stage_bank=stage_bank)]))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            return json.loads((work / 'k.res.json').read_text())

    def test_a_graphics_stage_bank_is_its_push_block(self):
        # The pixel stage (index 4) reads its buffer entries from c[29]: what the game pushes into graphics push block 5 (PRAGMATA's material pass), at c[24] + 5 * 0x100
        res = self.translate_ps(inst(*ULDC64_C29_10) + inst(*LDG_R0_UR4) + inst(*EXIT), 29)
        self.assertIn((24, (5 * 0x100 + 0x10) // 4), [(s['cb_slot'], s['cb_offset']) for s in res['sbufs']])

    def test_another_stage_bank_is_refused(self):
        with tempfile.TemporaryDirectory(prefix='nvn2_buffers_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(PS_SPH + inst(*ULDC64_C29_10) + inst(*LDG_R0_UR4) + inst(*EXIT))
            (work / 'jobs.json').write_text(json.dumps([dict(key='k', stage='ps', arch='sm86', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, stage_bank=25)]))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
            self.assertIn("not this stage's buffer bank", r.stdout + r.stderr)

    def test_a_pushed_buffer_entry_is_read_from_push_block_1(self):
        # c0: no push words, push block 1 at 0x10 (+0xaa: 4 words, one entry), one uniform entry at 0x20, images from 0x30 (image slot 0 = word 12)
        res = self.translate(inst(*ULDC64_C0_10) + inst(*LDG_UR4) + inst(*SUATOM_R5) + inst(*EXIT), [1, 0, 0, 0x40, 0, 0, 0, 0x10])
        self.assertIn((24, 0x100 // 4), [(s['cb_slot'], s['cb_offset']) for s in res['sbufs']])   # the storage buffer's address: c[24] + 0x100

    def test_a_uniform_entry_follows_push_block_1(self):
        res = self.translate(inst(*ULDC64_C0_20) + inst(*LDG_UR4) + inst(*SUATOM_R5) + inst(*EXIT), [1, 0, 0, 0x40, 0, 0, 0, 0x10])
        self.assertIn((0, 0x210 // 4), [(s['cb_slot'], s['cb_offset']) for s in res['sbufs']])   # uniform slot 0's address: c[0] + 0x210

    def test_a_bindless_constant_read_loads_through_a_pushed_handle(self):
        # UR4:UR5 = the first 8 bytes of push block 1 (a handle the game pushed, in the hardware's layout: address >> 4, size >> 4 at 45), then a word of that buffer through it.
        # The address the shader decodes (handle << 4) is traced back to the handle's words, which the host decodes (nvn_mtl.m ucb_handle_addr)
        res = self.translate(inst(*ULDC64_C0_10) + inst(*ULDCX) + inst(*MOV_R5_UR5) + inst(*SUATOM_R5) + inst(*EXIT), [1, 0, 0, 0x40, 0, 0, 0, 0x10])
        self.assertIn((24, 0x100 // 4), [(s['cb_slot'], s['cb_offset']) for s in res['sbufs']])

    def test_a_texture_id_loaded_on_one_path_indexes_the_pools(self):
        # The handle is (a word loaded from memory, or on the other path a word of c[3]) | constant sampler 0x104, as PRAGMATA's compute shader 02e1292114121308 reads a material's texture.
        # The loaded word may name any texture, so the read indexes the pools by its ID; the uniform word alone was taken as the handle, a binding the host has no slot for
        code = [ULDC64_C0_10, LDG_UR4, ISETP_NE_P0_R5, MOV_R6_C3_10_NP0, MOV_R6_R5_P0, LOP3_R6_OR_SAMPLER, TEX_LZ_R6, SUATOM_R7, EXIT]
        res = self.translate(b''.join(inst(*i) for i in code), [1, 0, 0, 0x40, 0, 0, 0, 0x10])
        self.assertEqual([t for t in res['textures'] if t['cbuf_slot'] not in (0, 2)], [], res['textures'])   # no texture named by a word of the uniform buffer
        self.assertTrue(any(t['array'] > 32 for t in res['textures']), res['textures'])   # the pool array

    def test_indexed_native_c0_texture_handles_use_the_driver_table(self):
        # Fast Fusion's fog: the byte offset comes from a uniform, LDC reads c0[offset], and a fixed c0 sampler word is ORed in. With 1 UBO, 4 combined textures and 4 separate
        # textures, native separate textures start at 0x40 and the sampler at 0x60. Canonical c2 has the separate textures at 0x168 and the sampler at 0x568.
        ldc = (0x000fe20000000800, 0x0000000006067b82)
        sampler = (MOV_R6_C3_10[0], 0xa02 | 0x7000 | 7 << 16 | 0x60 << 38)
        combine = (LOP3_R6_OR_SAMPLER[0], 0x212 | 0x7000 | 6 << 16 | 6 << 24 | 7 << 32)
        code = [MOV_R6_C3_10, ldc, sampler, combine, TEX_LZ_R6, SUATOM_R7, EXIT]
        res = self.translate(b''.join(inst(*i) for i in code), [1, 0, 4, 0x70, 0, 4, 1])
        self.assertTrue(any(t['cbuf_slot'] == (2 | 3 << 16) and t['handle'] == 0x10568000 for t in res['textures']), res['textures'])
        self.assertFalse(any(t['cbuf_slot'] & 0xffff == 0 for t in res['textures']), res['textures'])
        self.assertFalse(any(c['slot'] == 0 for c in res['cbufs']), res['cbufs'])

    def test_indexed_native_c0_combined_handles_use_the_driver_table(self):
        ldc = (0x000fe20000000800, 0x0000000006067b82)
        res = self.translate(b''.join(inst(*i) for i in [MOV_R6_C3_10, ldc, TEX_LZ_R6, SUATOM_R7, EXIT]), [1, 0, 4, 0x50, 0])
        self.assertTrue(any(t['cbuf_slot'] == 2 and t['handle'] == 0 for t in res['textures']), res['textures'])
        self.assertFalse(any(c['slot'] == 0 for c in res['cbufs']), res['cbufs'])


    def test_a_handle_read_as_2d_and_as_a_2d_array_is_an_array(self):
        # One handle, TEX 2D and TEX ARRAY_2D (PRAGMATA: 24 pixel shaders, 21ba8fea5a5eb03c among them, whose Metal source did not compile: the binding was declared an array
        # and the 2D read gave no layer). The binding is an array for both, in the declaration and in the descriptor, and the 2D read takes layer 0
        code = [MOV_R6_C3_10, LOP3_R6_OR_SAMPLER, TEX_LZ_R6, SUATOM_R7, TEX_LZ_R6_ARRAY, SUATOM_R7, EXIT]
        res = self.translate(b''.join(inst(*i) for i in code), [1, 0, 0, 0x40, 0, 0, 0, 0x10])
        self.assertEqual([t['type'] for t in res['textures'] if t['cbuf_slot'] not in (0, 2)], [0x103], res['textures'])   # Texture2D | Array, once
        only = self.translate(b''.join(inst(*i) for i in code[:4] + [EXIT]), [1, 0, 0, 0x40, 0, 0, 0, 0x10])
        self.assertEqual([t['type'] for t in only['textures'] if t['cbuf_slot'] not in (0, 2)], [3], only['textures'])    # a 2D read alone stays 2D


if __name__ == '__main__':
    unittest.main()
