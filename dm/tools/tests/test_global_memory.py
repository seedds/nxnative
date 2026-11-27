"""Maxwell global memory through the translator, on synthetic compute shaders only (no game inputs): ATOM.CAS, and the signed displacement of LDG / STG / RED / ATOM
addresses with the extended (.E) high word.

  DMSH_TEST_XLATE=/absolute/path/to/dmsh_xlate python3 -m unittest -v dm/tools/tests/test_global_memory.py

Requires spirv-val on PATH (spirv-cross is the in-repo build). DMSH_TEST_METAL=1 also compiles each shader with the builds' own Metal compiles (DMSH_TEST_METAL_SDKS:
macosx, iphoneos or both; default macosx), and DMSH_TEST_GPU=1 with it runs each one on the Mac's GPU (dm/gfx/tests/t_global_memory.m) and checks all 1,024 words of
its buffer against the reference model below.

Every shader reaches one 4 KB buffer at a 40-bit guest address, which c0 holds as a descriptor (the address at c0 0x140 / 0x144, the size at 0x148). Two routes:
- 'c0': the address is the descriptor's, plus an advance from the thread ID (a 64-bit IADD.CC / IADD.X, as compiled code adds an index). The translator turns the
  access into the descriptor's storage buffer (GlobalToStorage), at an offset from the low words.
- 'pointer': the address is a pointer the shader loads from words 2 / 3 of the buffer, which the translator cannot trace to a descriptor, so the access goes
  through the memory pool table (dmsh_abi.h DMSH_GMEM_MAP_*), whose lookup takes the whole 40-bit address. There an extended address with a negative displacement
  missed its pool before: its high word came out one too high, 4 GB up.
"""
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
RYU = ROOT / 'dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader'
sys.path[:0] = [str(ROOT / 'dm/gfx/tools'), str(ROOT / 'dm/ios/tools')]
import dmsh_abi, dmsh_chain, ios_metal   # noqa: E402  (the builds' Metal compiles)
SPIRV_CROSS = ROOT / 'dm/gfx/tools/spirv_cross_bin/spirv-cross'
XC = dmsh_chain.CROSS_MSL
OP_ATOMIC_COMPARE_EXCHANGE = 230
INIT = [0xabcd0000 + i for i in range(1024)]   # t_global_memory.m's starting words
PLACEHOLDER = 123   # R4 (and Rd) before the instruction under test


def encoder():
    table = (RYU / 'Decoders/InstTable.cs').read_text()
    def base(name):
        found = re.search(r'Add\("([01x]{64})", InstName\.' + name + r'\s*,', table)
        assert found, name
        return int(found[1].replace('x', '0'), 2)
    return base


def shader(op, *, via='c0', disp=0, advance=0, extended=True, pred=7, pred_inv=False, runtime_pred=False, compare=0, value=0, rd=4, rb=6, size64=False, bits50=0):
    """op: 'cas' (ATOM.CAS Rd, [R2 + disp], Rb, Rb+1), 'ldg' (LDG.32 Rd, [R2 + disp]), 'stg' (STG.32 [R2 + disp], R7) or 'red' (RED.ADD.U32 [R2 + disp], R7). R2 / R3: the guest address from c0
    plus the advance (via 'c0'), or the pointer in words 2 / 3 (via 'pointer'). The result register goes to word 8 (STG through c0's address)"""
    base = encoder()
    def mov(r, n): return base('Mov32i') | ((n & 0xffffffff) << 20) | (7 << 16) | (15 << 12) | r
    def ldc(r, off): return base('Ldc') | (4 << 48) | (off << 20) | (7 << 16) | (255 << 8) | r
    e = int(extended)
    if via == 'pointer':   # LDG.E.64 R2, [R12 + 8] with R12 / R13 the address from c0
        ops = [ldc(12, 0x140), ldc(13, 0x144), base('Ldg') | (5 << 48) | (1 << 45) | (8 << 20) | (7 << 16) | (12 << 8) | 2]
    else:
        ops = [ldc(2, 0x140), ldc(3, 0x144)]
    ops += [mov(4, PLACEHOLDER)] + ([mov(rb, compare), mov(rb + 1, value)] if rb < 255 else [])
    if rd not in (4, 255, rb, rb + 1, 2): ops.append(mov(rd, PLACEHOLDER))
    if advance and via == 'c0':   # S2R R10, SR_TID.X (0 on the one thread); IADD32I R10, R10, advance; IADD.CC R2, R2, R10; IADD.X R3, R3, RZ
        ops += [base('S2r') | (0x21 << 20) | (7 << 16) | 10, base('Iadd32i') | ((advance & 0xffffffff) << 20) | (7 << 16) | (10 << 8) | 10,
                base('Iadd') | (1 << 47) | (10 << 20) | (7 << 16) | (2 << 8) | 2, base('Iadd') | (1 << 43) | (255 << 20) | (7 << 16) | (3 << 8) | 3]
    if runtime_pred:   # ISETP.EQ P0, R10, R11 with R10 = c0 0x150, R11 = 1: not foldable, unlike PT (no case has both this and an advance, which also uses R10)
        ops += [ldc(10, 0x150), mov(11, 1), base('Isetp') | (2 << 49) | (7 << 39) | (11 << 20) | (7 << 16) | (10 << 8) | 7]
        pred = 0
    guard = (pred << 16) | (int(pred_inv) << 19)
    if op == 'cas':
        # gm107.c's 0xeef0 row: a signed byte displacement at 28..47, E at 48, .64 at 49
        ops.append(base('AtomCas') | (e << 48) | (int(size64) << 49) | (bits50 << 50) | ((disp & 0xfffff) << 28) | (rb << 20) | guard | (2 << 8) | rd)
    elif op == 'ldg':
        ops.append(base('Ldg') | (4 << 48) | (e << 45) | ((disp & 0xffffff) << 20) | guard | (2 << 8) | rd)
    elif op == 'stg':
        ops.append(base('Stg') | (4 << 48) | (e << 45) | ((disp & 0xffffff) << 20) | guard | (2 << 8) | (rb + 1))
    else:
        assert op == 'red'
        ops.append(base('Red') | (e << 48) | ((disp & 0xfffff) << 28) | guard | (2 << 8) | (rb + 1))
    out = 4 if rd == 255 else rd
    ops += [ldc(8, 0x140), ldc(9, 0x144), base('Stg') | (4 << 48) | (e << 45) | (0x20 << 20) | (7 << 16) | (8 << 8) | out, base('Exit') | (7 << 16) | 15]
    words = []
    for i, o in enumerate(ops):
        if i % 3 == 0: words.append(0)   # the scheduling word
        words.append(o)
    return struct.pack('<%dQ' % len(words), *words)


def expected(op, *, guest, via='c0', disp=0, advance=0, runs=True, compare=0, value=0, rd=4, **_):
    """The reference model: the words after the shader ran. advance: the address's offset from the buffer, by the thread ID (via 'c0') or in the pointer"""
    w = list(INIT)
    if via == 'pointer':
        w[2], w[3] = (guest + advance) & 0xffffffff, (guest + advance) >> 32
    at = (advance + disp) // 4   # the word, from the buffer's start
    assert 0 <= at < 1024
    result = PLACEHOLDER
    if runs:
        if op == 'cas':
            result = w[at]
            if w[at] == compare: w[at] = value
        elif op == 'ldg':
            result = w[at]
        elif op == 'stg':
            w[at] = value & 0xffffffff
        else:
            w[at] = (w[at] + value) & 0xffffffff
    w[8] = PLACEHOLDER if op in ('red', 'stg') or rd == 255 else result
    return w


def spirv_opcodes(path):
    data = path.read_bytes(); words = struct.unpack('<%dI' % (len(data) // 4), data)
    result = []; i = 5
    while i < len(words):
        result.append(words[i] & 0xffff); i += words[i] >> 16
    return result


# (name, op, shader and model arguments, the buffer's guest address (high, low), the predicate input at c0 0x150)
HI, LO = 2, 0x10000   # the buffer at guest address 0x2_0001_0000
CASES = [
    ('cas_swap', 'cas', dict(compare=INIT[0], value=99), (HI, LO), 1),
    ('cas_mismatch', 'cas', dict(compare=7, value=99), (HI, LO), 1),
    ('cas_positive_disp', 'cas', dict(disp=4, compare=INIT[1], value=99), (HI, LO), 1),
    ('cas_negative_disp', 'cas', dict(disp=-4, advance=8, compare=INIT[1], value=99), (HI, LO), 1),
    ('cas_negative_disp_borrow', 'cas', dict(disp=-8, advance=12, compare=INIT[1], value=99), (1, 0xfffffff8), 1),   # 0x1_fffffff8 + 12 - 8 = 0x1_fffffffc
    ('cas_not_executed', 'cas', dict(pred_inv=True, compare=INIT[0], value=99, runs=False), (HI, LO), 1),
    ('cas_runtime_pred', 'cas', dict(runtime_pred=True, compare=INIT[0], value=99), (HI, LO), 1),
    ('cas_runtime_pred_false', 'cas', dict(runtime_pred=True, compare=INIT[0], value=99, runs=False), (HI, LO), 0),
    ('cas_32bit_address', 'cas', dict(extended=False, compare=INIT[0], value=99), (0, LO), 1),
    ('cas_rd_is_ra', 'cas', dict(rd=2, rb=8, compare=INIT[0], value=99), (HI, LO), 1),   # EA SPORTS FC 27's form: ATOM.E.CAS R6, [R6], R8, R9
    ('cas_rd_is_rb', 'cas', dict(rd=6, compare=INIT[0], value=99), (HI, LO), 1),
    ('cas_rd_is_rb1', 'cas', dict(rd=7, compare=INIT[0], value=99), (HI, LO), 1),
    ('cas_rd_rz', 'cas', dict(rd=255, compare=INIT[0], value=99), (HI, LO), 1),
    ('ldg_negative_disp', 'ldg', dict(disp=-4, advance=8), (HI, LO), 1),
    ('ldg_negative_disp_borrow', 'ldg', dict(disp=-8, advance=12), (1, 0xfffffff8), 1),
    ('red_negative_disp', 'red', dict(disp=-4, advance=8, value=5), (HI, LO), 1),
    ('stg_negative_disp', 'stg', dict(disp=-4, advance=8, value=0x5a5a1234), (HI, LO), 1),
    ('red_negative_disp_32bit_address', 'red', dict(extended=False, disp=-4, advance=8, value=5), (0, LO), 1),
    ('pointer_cas', 'cas', dict(via='pointer', compare=INIT[0], value=99), (HI, LO), 1),
    ('pointer_cas_negative_disp', 'cas', dict(via='pointer', disp=-4, advance=8, compare=INIT[1], value=99), (HI, LO), 1),
    ('pointer_cas_negative_disp_borrow', 'cas', dict(via='pointer', disp=-8, advance=12, compare=INIT[1], value=99), (1, 0xfffffff8), 1),
    ('pointer_ldg_negative_disp', 'ldg', dict(via='pointer', disp=-4, advance=8), (HI, LO), 1),
    ('pointer_ldg_negative_disp_borrow', 'ldg', dict(via='pointer', disp=-8, advance=12), (1, 0xfffffff8), 1),
    ('pointer_red_negative_disp', 'red', dict(via='pointer', disp=-4, advance=8, value=5), (HI, LO), 1),
    ('pointer_stg', 'stg', dict(via='pointer', advance=16, value=0x5a5a1234), (HI, LO), 1),
    ('pointer_stg_negative_disp', 'stg', dict(via='pointer', disp=-4, advance=8, value=0x5a5a1234), (HI, LO), 1),
    ('pointer_stg_negative_disp_borrow', 'stg', dict(via='pointer', disp=-8, advance=12, value=0x5a5a1234), (1, 0xfffffff8), 1),
]
SHADER_ARGS = ('via', 'disp', 'advance', 'extended', 'pred_inv', 'runtime_pred', 'compare', 'value', 'rd', 'rb')


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'translator not configured (DMSH_TEST_XLATE)')
class GlobalMemory(unittest.TestCase):
    def translate(self, work, key, code):
        (work / (key + '.bin')).write_bytes(code)
        (work / 'jobs.json').write_text(json.dumps([dict(key=key, stage='cs', code=str(work / (key + '.bin')), local=[1, 1, 1], debug=True)]))
        return subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), str(work)], capture_output=True, text=True)

    def test_cases(self):
        with tempfile.TemporaryDirectory(prefix='dm-global-memory-') as tmp:
            work = Path(tmp)
            for name, op, args, (hi, lo), pred_input in CASES:
                with self.subTest(name):
                    result = self.translate(work, name, shader(op, **{k: v for k, v in args.items() if k in SHADER_ARGS}))
                    self.assertEqual(result.returncode, 0, result.stderr)
                    res = json.loads((work / (name + '.res.json')).read_text())
                    self.assertFalse([n for n in res['notes'] if 'not implemented' in n], res['notes'])
                    pool = {(b['cb_slot'], b['cb_offset']) for b in res['sbufs']} & {(255, 0), (255, 1)}   # the pool table and its write log
                    self.assertEqual(bool(pool), args.get('via') == 'pointer', res['sbufs'])
                    spv = work / (name + '.spv')
                    subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(spv)], check=True)
                    if op == 'cas' and args.get('runs', True):
                        self.assertIn(OP_ATOMIC_COMPARE_EXCHANGE, spirv_opcodes(spv), 'ATOM.CAS must compare and exchange')
                    msl = work / (name + '.metal')
                    subprocess.run([str(SPIRV_CROSS), *XC, str(spv), '--output', str(msl)], check=True)
                    c0 = {0x140: lo, 0x144: hi, 0x148: 4096, 0x150: pred_input}
                    want = expected(op, guest=hi << 32 | lo, **args)
                    self.metal(work, name, msl, res['sbufs'], c0, want, [f'w{i}={want[i]:#x}' for i in (2, 3)] + [f'guest={hi << 32 | lo:#x}'] if pool else [])

    def metal(self, work, name, msl, sbufs, c0, want, extra):
        if os.environ.get('DMSH_TEST_METAL') != '1':
            return
        for sdk in os.environ.get('DMSH_TEST_METAL_SDKS', 'macosx').split(','):
            self.assertIn(sdk, ('macosx', 'iphoneos'))
            air, lib = work / f'{name}.{sdk}.air', work / f'{name}.{sdk}.metallib'
            subprocess.run([*(dmsh_chain.MAC_METAL if sdk == 'macosx' else ios_metal.METAL), str(msl), '-o', str(air)], check=True)
            subprocess.run(['xcrun', '-sdk', sdk, 'metallib', str(air), '-o', str(lib)], check=True)
            if sdk != 'macosx' or os.environ.get('DMSH_TEST_GPU') != '1':
                continue
            exe = work / 't_global_memory'
            if not exe.exists():
                subprocess.run(['clang', '-fobjc-arc', '-O1', '-Wall', '-Werror', str(ROOT / 'dm/gfx/tests/t_global_memory.m'),
                                '-framework', 'Foundation', '-framework', 'Metal', '-o', str(exe)], check=True)
            ids = dict(re.findall(r'\b(cp_\w+) \[\[id\((\d+)\)\]\]', msl.read_text()))   # argument buffer members
            role = {(255, 0): 'map', (255, 1): 'log'}
            members = [f'{role[b["cb_slot"], b["cb_offset"]]}={ids["cp_s%d" % b["slot"]]}' for b in sbufs if (b['cb_slot'], b['cb_offset']) in role]
            members.append('s=' + ','.join(ids['cp_s%d' % b['slot']] for b in sbufs if b['cb_slot'] == 0))   # every one of the c0 descriptor's storage buffers
            if 'cp_c0' in ids: members.append('c0=' + ids['cp_c0'])
            sblen_key = f'cp_c{dmsh_abi.SBLEN_BANK}'
            if sblen_key in ids:
                bounded = ','.join(str(b['slot']) for b in sbufs if b['cb_slot'] == 0 and b['flags'] & 2)
                members.append(f'sblen={ids[sblen_key]}:{bounded}')
            run = subprocess.run([str(exe), str(lib), *members, *(f'{o:#x}={v:#x}' for o, v in c0.items()), *extra],
                                 check=True, capture_output=True, text=True, env=dict(os.environ, MTL_DEBUG_LAYER='1'))
            got = [int(x, 16) for x in run.stdout.split()]
            bad = [f'word {i}: {g:#x}, want {w:#x}' for i, (g, w) in enumerate(zip(got, want)) if g != w]
            self.assertEqual((len(got), bad), (1024, []))

    def test_unsupported_cas_forms_fail(self):
        # The 64-bit form, bits 50..51 set, an odd Rb and RZ: no compiled form has them, so translation stops rather than guess
        with tempfile.TemporaryDirectory(prefix='dm-global-cas-reject-') as tmp:
            work = Path(tmp)
            for name, code in (('b64', shader('cas', size64=True)), ('bits50_1', shader('cas', bits50=1)), ('bits50_2', shader('cas', bits50=2)),
                               ('bits50_3', shader('cas', bits50=3)), ('rb_odd', shader('cas', rb=5)), ('rb_rz', shader('cas', rb=255))):
                with self.subTest(name):
                    result = self.translate(work, name, code)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn('ATOM.CAS at 0x', result.stderr)
                    self.assertFalse((work / (name + '.spv')).exists())


if __name__ == '__main__':
    unittest.main()
