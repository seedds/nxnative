#!/usr/bin/env python3
"""MEMBAR as a fence and BAR.SYNC as a barrier, as SPIRV-Cross writes them with the chain's options (dmsh_chain.CROSS_MSL, MSL 3.2): OpMemoryBarrier (MEMBAR.CTA, Workgroup scope;
MEMBAR.GL / .SYS and Ampere's MEMBAR, Device scope) is an atomic_thread_fence of that scope with no execution barrier, and an OpControlBarrier (BAR.SYNC) right after one stays a
threadgroup_barrier. Below MSL 3.2 SPIRV-Cross wrote both MEMBARs as one threadgroup_barrier and left that BAR.SYNC out, and the text rule that made the line a fence (membar_fence) left
the group with no barrier at all (GitHub issue #2786: Fire Emblem: Three Houses hung an A17 Pro's GPU). The modules are written here word by word, so only spirv-cross is needed; with
DMSH_TEST_METAL=1 the MSL also compiles with the Mac's and the iPhone's compile, and with DMSH_TEST_XLATE a Maxwell MEMBAR.CTA; BAR.SYNC goes through the translator too."""
from pathlib import Path
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_chain

DEVICE, WORKGROUP = 1, 2   # SPIR-V scopes
SEMANTICS = 0xd48          # the translator's DefaultMemorySemantics: AcquireRelease | UniformMemory | WorkgroupMemory | AtomicCounterMemory | ImageMemory
BAR_SEMANTICS = 0x108      # GenerateBarrier's: AcquireRelease | WorkgroupMemory
FENCE = 'atomic_thread_fence(mem_flags::mem_device | mem_flags::mem_threadgroup | mem_flags::mem_texture, memory_order_seq_cst, thread_scope_%s);'
BAR = 'threadgroup_barrier(mem_flags::mem_threadgroup);'


def module(body):
    """A compute module of 64 threads whose entry runs body: ('membar', scope) and ('bar',) in order"""
    void, fn, uint, main, label = 1, 2, 3, 4, 5
    consts = {}
    def const(v): return consts.setdefault(v, 6 + len(consts))
    code = []
    for op in body:
        if op[0] == 'membar': code += [3 << 16 | 225, const(op[1]), const(SEMANTICS)]
        else: code += [4 << 16 | 224, const(WORKGROUP), const(WORKGROUP), const(BAR_SEMANTICS)]
    words = [2 << 16 | 17, 1, 3 << 16 | 14, 0, 1, 5 << 16 | 15, 5, main, *struct.unpack('<2I', b'main\0\0\0\0'), 6 << 16 | 16, main, 17, 64, 1, 1,
             2 << 16 | 19, void, 3 << 16 | 33, fn, void, 4 << 16 | 21, uint, 32, 0]
    words += [w for v, i in consts.items() for w in (4 << 16 | 43, uint, i, v)]
    words += [5 << 16 | 54, void, main, 0, fn, 2 << 16 | 248, label, *code, 1 << 16 | 253, 1 << 16 | 56]
    return struct.pack('<%dI' % (5 + len(words)), 0x07230203, 0x00010000, 0, 6 + len(consts), 0, *words)


def cross(spv_bytes):
    with tempfile.TemporaryDirectory(prefix='membar_') as d:
        p = Path(d) / 'k.spv'; p.write_bytes(spv_bytes)
        r = subprocess.run([dmsh_chain.SPIRV_CROSS, *dmsh_chain.CROSS_MSL, str(p)], capture_output=True, text=True)
    if r.returncode: raise AssertionError(r.stderr)
    return r.stdout


def barriers(msl):   # the kernel's fence and barrier lines, in order
    return [ln.strip() for ln in msl[msl.index('kernel void'):].splitlines() if 'barrier(' in ln or 'fence(' in ln]


class MembarFence(unittest.TestCase):
    def test_membar_cta_then_bar_sync(self):   # GLSL's memoryBarrierShared(); barrier(): a threadgroup fence, then the barrier
        self.assertEqual(barriers(cross(module([('membar', WORKGROUP), ('bar',)]))), [FENCE % 'threadgroup', BAR])

    def test_membar_gl_then_bar_sync(self):   # memoryBarrier(); barrier(): a device fence, then the barrier
        self.assertEqual(barriers(cross(module([('membar', DEVICE), ('bar',)]))), [FENCE % 'device', BAR])

    def test_two_pairs_keep_both_barriers(self):   # the shape that lost both barriers: two fences and two barriers
        msl = cross(module([('membar', WORKGROUP), ('bar',), ('membar', DEVICE), ('bar',)]))
        self.assertEqual(barriers(msl), [FENCE % 'threadgroup', BAR, FENCE % 'device', BAR])

    def test_membar_alone_waits_for_no_one(self):
        # PRAGMATA's GPU culling queue 12a52b683eec95c1: MEMBAR.SC.GPU; ERRBAR in a persistent loop and no BAR.SYNC. Lanes that are not converged must not wait for each other there
        msl = cross(module([('membar', DEVICE), ('membar', DEVICE)]))
        self.assertNotIn('threadgroup_barrier', msl)
        self.assertEqual(barriers(msl), [FENCE % 'device'] * 2)

    @unittest.skipUnless(os.environ.get('DMSH_TEST_METAL') and shutil.which('xcrun'), 'set DMSH_TEST_METAL=1 to compile with the Metal compiler')
    def test_it_compiles_for_the_mac_and_the_iphone(self):
        sys.path.insert(0, str(TOOLS.parents[1] / 'ios/tools')); import ios_metal
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'k.metal'; p.write_text(cross(module([('membar', WORKGROUP), ('bar',), ('membar', DEVICE), ('bar',)])))
            for name, cmd in (('mac', dmsh_chain.MAC_METAL), ('ios', ios_metal.METAL)):
                r = subprocess.run([*cmd, str(p), '-o', str(Path(d) / f'{name}.air')], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, name + ': ' + r.stderr)


# Maxwell encodings (test_dmsh_maxwell_ftz.py has the scheduling word's layout)
SCHED, NOP, EXIT = 0x001f8000fc0007e0, 0x50b0000000070f00, 0xe30000000007000f
S2R_TID_X = 0xf0c8000000270003          # S2R R3, SR_TID.X
STS = 0xef5c000000870303                # STS.B32 [R3 + 8], R3
MEMBAR_CTA, MEMBAR_GL = 0xef98000000070000, 0xef98000000070100
BAR_SYNC = 0xf0a81b8000070000           # BAR.SYNC 0x0


def maxwell(*insts):
    out = b''
    for k in range(0, len(insts), 3):
        out += struct.pack('<4Q', SCHED, *(tuple(insts[k:k + 3]) + (NOP, NOP))[:3])
    return out


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class TranslatedMembar(unittest.TestCase):
    def test_maxwell_membar_then_bar_sync(self):
        with tempfile.TemporaryDirectory(prefix='membar_xlate_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(maxwell(S2R_TID_X, STS, MEMBAR_CTA, BAR_SYNC, STS, MEMBAR_GL, BAR_SYNC, EXIT))
            (work / 'jobs.json').write_text(json.dumps([dict(key='k', stage='cs', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[64, 1, 1], debug=True, shared=0x1000)]))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            import dmsh_geometry
            dmsh_geometry.flatten_sets(work / 'k.spv')
            msl = cross((work / 'k.spv').read_bytes())
        self.assertEqual(barriers(msl), [FENCE % 'threadgroup', BAR, FENCE % 'device', BAR])


if __name__ == '__main__':
    unittest.main()
