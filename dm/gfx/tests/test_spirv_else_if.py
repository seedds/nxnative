#!/usr/bin/env python3
"""else-if chains in the SPIR-V back end.

A synthetic sm86 pixel program reads c[3][0] and writes 1.0, 2.0 or 3.0 to the red channel of target 0:

    if (x > 1) r = 1; else if (x > 0) r = 2; else r = 3;

In normal mode (XLATE_DEBUG=0) the structured program makes the second test an ElseIf block (an Else that holds
nothing but an If). The SPIR-V generator used to take only an Else as the false target of an If, so the false edge
went to the merge block and the ElseIf and Else bodies had no way in. Debug mode (the build default) adds a comment
node per instruction, so there the Else also holds the branch's comment and no ElseIf forms: it is the control. This
checks that every block of the translated module can be reached in both modes, that both validate, and that the MSL
keeps all three results. The program is built here from its encoding; no game data.

The translator is built from this tree's own sources (spirv_else_if/ElseIf.csproj, as goto_conditions does), with
dmsh_xlate's flags for a pixel shader, so the test checks the SpirvGenerator.cs of the change under test. Needs the
.NET SDK 10 or later, spirv-val and SPIRV-Cross; skipped without them. DMSH_TEST_DOTNET names the dotnet to use,
DMSH_TEST_RYU_SRC another Ryujinx.Graphics.Shader source folder (-p:RyuSrc).
"""
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent / 'tools'
try:   # the chain's own SPIRV-Cross options, so the MSL here is the MSL the chain makes
    sys.path.insert(0, str(TOOLS))
    import dmsh_chain  # noqa: E402
    CROSS_MSL = list(dmsh_chain.CROSS_MSL)
except (ImportError, AttributeError):
    CROSS_MSL = ['--msl', '--msl-version', '30200', '--msl-argument-buffers', '--msl-texture-buffer-native']
RZ, PT = 255, 7
CONTROL = 0x000fea0000000000 << 64   # stall 5, yield, no barriers


def inst(op, guard=PT, ginv=0, rd=RZ, ra=RZ, imm=None, fields=()):
    v = op | (guard << 12) | (ginv << 15) | (rd << 16) | (ra << 24)
    if imm is not None:
        v |= (imm & 0xffffffff) << 32
    for bit, width, val in fields:
        v |= (val & ((1 << width) - 1)) << bit
    return v | CONTROL


def f32(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def mov_imm(rd, value):
    return inst(0x802, rd=rd, imm=value, fields=((72, 4, 0xf),))


def mov_cbuf(rd, bank, offset):
    return inst(0xa02, rd=rd, fields=((38, 16, offset // 4), (54, 5, bank), (72, 4, 0xf)))


def isetp_gt(pd, ra, imm):   # ISETP.GT.AND Pd, PT, Ra, imm, PT (signed)
    return inst(0x80c, ra=ra, imm=imm, fields=((73, 1, 1), (76, 3, 4), (81, 3, pd), (84, 3, PT), (87, 3, PT)))


def bra(here, target, guard=PT, ginv=0):
    return inst(0x947, guard=guard, ginv=ginv, fields=((34, 48, (target - here - 16) // 4), (87, 3, PT)))


def program():
    """0x80-byte header (pixel shader, render target 0 RGBA) and the code"""
    header = bytearray(0x80)
    struct.pack_into('<I', header, 0, 2 | (3 << 5) | (5 << 10))
    struct.pack_into('<I', header, 18 * 4, 0xf)
    code = [
        mov_imm(1, f32(0.0)), mov_imm(2, f32(0.0)), mov_imm(3, f32(1.0)),
        mov_cbuf(4, 3, 0),                     # R4 = c[3][0]
        isetp_gt(0, 4, 1),                     # P0 = R4 > 1
        isetp_gt(1, 4, 0),                     # P1 = R4 > 0
        bra(0x60, 0x90, guard=0, ginv=1),      # @!P0 BRA 0x90
        mov_imm(0, f32(1.0)),                  # R0 = 1
        bra(0x80, 0xd0),                       # BRA 0xd0
        bra(0x90, 0xc0, guard=1),              # 0x90: @P1 BRA 0xc0 (on P1 itself: in debug mode the else block then
                                               # holds nothing but the If, which becomes an ElseIf)
        mov_imm(0, f32(3.0)),                  # R0 = 3
        bra(0xb0, 0xd0),                       # BRA 0xd0
        mov_imm(0, f32(2.0)),                  # 0xc0: R0 = 2
        inst(0x94d, fields=((87, 3, PT),)),    # 0xd0: EXIT
        bra(0xe0, 0xe0),
    ]
    return bytes(header) + b''.join(struct.pack('<QQ', c & ((1 << 64) - 1), c >> 64) for c in code)


def unreachable_blocks(spv):
    """labels that no branch, merge, switch or phi names, other than a function's first label"""
    words = struct.unpack(f'<{len(spv) // 4}I', spv)
    labels, refs, first = [], set(), True
    i = 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xffff
        operands = words[i + 1:i + count]
        if op == 54:                       # OpFunction
            first = True
        elif op == 248:                    # OpLabel
            if not first:
                labels.append(operands[0])
            first = False
        elif op == 249:                    # OpBranch
            refs.add(operands[0])
        elif op == 250:                    # OpBranchConditional
            refs.update(operands[1:3])
        elif op == 247:                    # OpSelectionMerge
            refs.add(operands[0])
        elif op == 246:                    # OpLoopMerge
            refs.update(operands[0:2])
        elif op == 251:                    # OpSwitch
            refs.add(operands[1])
            refs.update(operands[3::2])
        elif op == 245:                    # OpPhi
            refs.update(operands[3::2])
        i += count
    return [x for x in labels if x not in refs]


def find_spirv_cross():
    found = shutil.which('spirv-cross')
    if found:
        return found
    bundled = TOOLS / 'spirv_cross_bin' / 'spirv-cross'
    if platform.system() == 'Darwin' and bundled.is_file():
        return str(bundled)
    return None


class ElseIfChains(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dotnet = os.environ.get('DMSH_TEST_DOTNET') or shutil.which('dotnet')
        cls.spirv_val = shutil.which('spirv-val')
        cls.spirv_cross = find_spirv_cross()
        missing = [name for name, tool in (('dotnet', cls.dotnet), ('spirv-val', cls.spirv_val),
                                           ('spirv-cross', cls.spirv_cross)) if not tool]
        if missing:
            raise unittest.SkipTest('needs ' + ', '.join(missing))
        version = subprocess.run([cls.dotnet, '--version'], capture_output=True, text=True).stdout.strip()
        if not version or int(version.split('.')[0]) < 10:
            raise unittest.SkipTest(f'needs the .NET SDK 10 or later (have {version or "none"})')
        cls.tmp = tempfile.TemporaryDirectory(prefix='spirv_else_if_')
        work = Path(cls.tmp.name)
        # as test_dmsh_goto_conditions.py builds: no build servers, every obj / bin (the referenced projects' too) under
        # the temporary directory, none left in the checkout
        command = [cls.dotnet, 'build', str(HERE / 'spirv_else_if/ElseIf.csproj'), '-c', 'Release',
                   '--disable-build-servers', '-p:UseSharedCompilation=false',
                   '--artifacts-path', str(work / 'artifacts'), '-o', str(work / 'bin')]
        if os.environ.get('DMSH_TEST_RYU_SRC'):
            command.append('-p:RyuSrc=' + os.environ['DMSH_TEST_RYU_SRC'])
        build = subprocess.run(command, capture_output=True, text=True)
        if build.returncode:
            cls.tmp.cleanup()
            raise AssertionError('ElseIf.csproj did not build:\n' + build.stdout[-4000:] + build.stderr[-2000:])
        (work / 'else_if.bin').write_bytes(program())
        run = subprocess.run([cls.dotnet, str(work / 'bin' / 'ElseIf.dll'), str(work / 'else_if.bin'), str(work / 'out')],
                             capture_output=True, text=True, timeout=300)
        cls.log = run.stdout + run.stderr
        cls.spv = {mode: (work / 'out' / f'else_if_{mode}.spv') for mode in ('debug', 'normal')}

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_every_block_is_reachable(self):
        for mode, path in self.spv.items():
            with self.subTest(mode=mode):
                self.assertTrue(path.is_file(), self.log)
                self.assertEqual(unreachable_blocks(path.read_bytes()), [], f'{mode} mode')

    def test_modules_validate(self):
        for mode, path in self.spv.items():
            with self.subTest(mode=mode):
                self.assertTrue(path.is_file(), self.log)
                r = subprocess.run([self.spirv_val, '--target-env', 'vulkan1.2', str(path)], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_msl_keeps_all_three_results(self):
        for mode, path in self.spv.items():
            with self.subTest(mode=mode):
                self.assertTrue(path.is_file(), self.log)
                msl = subprocess.run([self.spirv_cross, *CROSS_MSL, str(path)], capture_output=True,
                                     text=True, check=True).stdout
                # The program moves the three results as integer bit patterns; SPIRV-Cross drops unreachable blocks,
                # so a branch without a way in loses its constant from the MSL.
                missing = [value for value in (1.0, 2.0, 3.0)
                           if not any(f in msl for f in (str(f32(value)), hex(f32(value)), f'0x{f32(value):08X}'))]
                self.assertEqual(missing, [], f'{mode} mode: the branches writing {missing} are missing from the MSL')


if __name__ == '__main__':
    unittest.main()
