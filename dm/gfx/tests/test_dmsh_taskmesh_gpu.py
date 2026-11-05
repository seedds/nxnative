"""Synthetic Ampere ISBE programs rendered through native Metal task/mesh entries.

No game bytes: all instructions and NVN2/SPH metadata are generated here. The
input and output ISBE allocations deliberately have identical sizes. A task
passes API first through its payload and emits two mesh groups; each mesh uses
its child index, sparse packed attributes, vertex 32 and backwards MAP stores.
A second fixture exports sparse primitive attributes from primitive block 32.
Set DMSH_TEST_XLATE to the rebuilt translator and DMSH_TEST_GPU=1 on a Mac.
"""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_geometry  # noqa: E402
import dmsh_taskmesh as tm  # noqa: E402

ATTRS = (0x70, 0x74, 0x78, 0x7c, 0x80, 0x88, 0xa4)
PRIMITIVE_ATTRS = (0x94, 0xb8)  # generic location 1.y and 3.z
PAYLOAD = 64 * len(ATTRS) * 4


def control(stage, threads, primitive=False):
    ctl = bytearray(0xd8)
    ctl[0x56] = 6 if stage == 'ts' else 7
    struct.pack_into('<3H', ctl, 0x58, threads, PAYLOAD, 0)
    ctl[0x5f] = 3
    struct.pack_into('<2H', ctl, 0x60, 33, 33 if primitive else 2)
    sph = sum(1 << (400 + attr // 4) for attr in ATTRS).to_bytes(0x80, 'little')
    return ctl, sph


def instruction(op, pred=7, fields=()):
    # No pending read/write scoreboard barriers for these sequential fixtures.
    word = op | pred << 12 | 7 << 110 | 7 << 113
    for start, width, value in fields:
        word |= (value & ((1 << width) - 1)) << start
    return word.to_bytes(16, 'little')


def mov(reg, value, pred=7):
    return instruction(0x424, pred, ((16, 8, reg), (24, 8, 255), (32, 32, value), (64, 8, 255), (81, 3, 7), (84, 3, 7), (87, 3, 7)))


def float_bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def isberd(reg, offset, output=False):
    # Mesa NAK SM70 OpIsberd: ATTR.32, byte offset plus RZ.
    return instruction(0x923, fields=((16, 8, reg), (24, 8, 255), (40, 16, offset), (74, 2, 2), (76, 2, 3), (79, 1, int(output))))


def isbewr(reg, offset, *, access=3, size=2):
    # Mesa NAK SM70 OpIsbewr: output ATTR.32 or MAP.{8,16,32}.
    return instruction(0x927, fields=((24, 8, 255), (32, 8, reg), (40, 16, offset), (74, 2, size), (76, 2, access), (79, 1, 1)))


def compare(pred, reg, value, op=2):
    # ISETP.<op>.U32.AND Pn, PT, Rreg, imm32, PT.
    return instruction(0x80c, fields=((24, 8, reg), (32, 32, value), (76, 3, op), (81, 3, pred), (84, 3, 7), (87, 3, 7)))


def exit_(pred=7):
    return instruction(0x94d, pred, ((87, 3, 7),))


def shader(stage, threads, task_payload=True, primitive=False):
    primitive = primitive and stage == 'ms'
    ctl, sph = control(stage, threads, primitive)
    # Only thread zero performs the guest work; the other early returns must
    # still participate in the wrapper's final barrier/export.
    code = instruction(0x919, fields=((16, 8, 10), (72, 8, 0x21)))
    code += compare(0, 10, 0, op=5) + exit_(0)
    code += isberd(1, 0)
    if stage == 'ts':
        code += mov(2, 2) + isbewr(2, 4) + isbewr(1, 16)
    else:
        if task_payload:
            code += isberd(2, 16)
        else:
            code += isberd(2, 0)
        code += compare(1, 2, 7)  # first supplied to the object/direct mesh
        code += compare(2, 1, 1)  # child group 1 belongs on the right
        positions = {0: (-0.95, -0.8), 32: (-0.95, -0.8), 1: (-0.05, -0.8), 2: (-0.5, 0.8),
                     3: (-0.2, 0.82), 4: (0.2, 0.82), 5: (0.0, 0.98)}
        for vertex, (x, y) in positions.items():
            values = [x, y, -0.5, 1.0, 0.0, 0.5, 0.75]
            for rank, value in enumerate(values):
                code += mov(3, float_bits(value))
                if rank == 0 and vertex in (32, 1, 2):
                    code += mov(3, float_bits(x + 1.0), pred=2)
                if rank == 4:
                    code += mov(3, float_bits(1.0), pred=1)
                # Deliberately calculate the packing independently of tm.skew_word.
                word = (vertex // 32) * (len(ATTRS) * 32) + rank * 32 + vertex % 32
                code += isbewr(3, word * 4)
        # Counter encoded by a backwards MAP.32 store from byte address 3.
        # A wrongly little-endian counter would emit the sentinel triangle too.
        code += mov(3, 33 if primitive else 1) + isbewr(3, 3, access=0, size=2)
        # Primitive fixture: primitives 0..31 are degenerate [0,0,0]. Only
        # primitive 32 is visible, so its attributes must use the next block.
        map_base = 4 + (32 * 3 if primitive else 0)
        code += mov(3, 0x2001) + isbewr(3, map_base + 1, access=0, size=1)
        code += mov(3, 2) + isbewr(3, map_base + 2, access=0, size=0)
        if primitive:
            for prim, values in ((0, (0.125, 0.5)), (32, (0.25, 0.75))):
                for rank, value in enumerate(values):
                    word = 64 * len(ATTRS) + (prim // 32) * 32 * len(PRIMITIVE_ATTRS) + rank * 32 + prim % 32
                    code += mov(3, float_bits(value)) + isbewr(3, word * 4)
        else:
            code += mov(3, 0x030405) + isbewr(3, 9, access=0, size=2)
            # Restore the first triangle's last index overwritten by MAP.32.
            code += mov(3, 2) + isbewr(3, 6, access=0, size=0)
    binary = sph + code + exit_()
    full = binary
    if primitive:
        secondary = (0x010c1081 | sum(1 << (400 + attr // 4) for attr in PRIMITIVE_ATTRS)).to_bytes(128, 'little')
        offset = (len(binary) + 127) & ~127
        struct.pack_into('<2I', ctl, 0x64, offset, len(secondary))
        full = binary + bytes(offset - len(binary)) + secondary
    return tm.metadata(ctl, full), binary


FRAGMENT = '''
fragment float4 pixel(dm_vertex v [[stage_in]]) {
    return float4(v.attr0.x, v.attr0.z, v.attr2.y, 1.0f);
}
'''
PRIMITIVE_FRAGMENT = '''
struct dm_fragment {
    float4 attr0 [[user(locn0)]];
    float4 attr1 [[user(locn1), flat]];
    float4 attr3 [[user(locn3), flat]];
};
fragment float4 pixel(dm_fragment v [[stage_in]]) {
    return float4(dot(v.attr1, float4(1)), dot(v.attr3, float4(1)), v.attr0.x, 1.0f);
}
'''


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'needs the rebuilt translator')
class TaskMeshGpu(unittest.TestCase):
    def compile_source(self, path, stage, threads, task_payload=True, primitive=False):
        md, binary = shader(stage, threads, task_payload, primitive)
        sizes = md['isbe_sizes']
        assert isinstance(sizes, list)
        if primitive and stage == 'ms':
            self.assertEqual(md['primitive_attrs'], list(PRIMITIVE_ATTRS))
            self.assertEqual(md['primitive_base_words'], 448)
            self.assertEqual(sizes, [448, 576, 103])
        else:
            self.assertEqual(sizes[0], sizes[1])
        (path / 'k.bin').write_bytes(binary)
        job = dict(key='k', stage=stage, arch='sm86', code=str(path / 'k.bin'), tex={}, img={}, attr={}, fout={},
                   local=[threads, 1, 1], isbe_sizes=md['isbe_sizes'], shared=0, debug=True)
        (path / 'job.json').write_text(json.dumps([job]))
        result = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(path / 'job.json'), str(path)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        spv = path / 'k.spv'
        dmsh_geometry.flatten_sets(spv)
        if shutil.which('spirv-val'):
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(spv)], check=True)
        result = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), '--msl', '--msl-version', '20300', '--msl-argument-buffers', str(spv)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        # The two same-sized declarations must retain independent semantic names.
        self.assertIn(f'spvUnsafeArray<uint, {sizes[0]}> dm_isbe_input;', result.stdout)
        self.assertIn(f'spvUnsafeArray<uint, {sizes[1]}> dm_isbe_output;', result.stdout)
        source = tm.wrap(result.stdout, md)
        fragment = PRIMITIVE_FRAGMENT if primitive else FRAGMENT
        (path / 'k.metal').write_text(source + (fragment if stage == 'ms' else ''))
        return path / 'k.metal'

    def test_isbe_regions_survive_equal_allocation_sizes(self):
        with tempfile.TemporaryDirectory() as temp:
            for stage in ('ts', 'ms'):
                path = Path(temp) / stage; path.mkdir()
                self.compile_source(path, stage, 32)

    def test_primitive_suffix_uses_secondary_header(self):
        with tempfile.TemporaryDirectory() as temp:
            self.compile_source(Path(temp), 'ms', 32, primitive=True)

    @unittest.skipUnless(os.environ.get('DMSH_TEST_GPU') == '1' and shutil.which('xcrun'), 'set DMSH_TEST_GPU=1 on a Mac')
    def test_native_object_and_mesh_pixels(self):
        self.native_pixels(False)

    @unittest.skipUnless(os.environ.get('DMSH_TEST_GPU') == '1' and shutil.which('xcrun'), 'set DMSH_TEST_GPU=1 on a Mac')
    def test_native_primitive_attribute_pixels(self):
        self.native_pixels(True)

    def native_pixels(self, primitive):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            runner = root / 'runner'
            subprocess.run(['clang', '-fobjc-arc', '-O1', str(Path(__file__).with_name('taskmesh_gpu.m')), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(runner)], check=True)
            for threads in (8, 32):
                libs = []
                for name, stage, payload in (('task', 'ts', True), ('mesh', 'ms', True), ('direct', 'ms', False)):
                    path = root / f'{name}{threads}'; path.mkdir()
                    source = self.compile_source(path, stage, threads, payload, primitive)
                    subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-std=metal3.1', '-c', str(source), '-o', str(path / 'k.air')], check=True)
                    lib = path / 'k.metallib'
                    subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(path / 'k.air'), '-o', str(lib)], check=True)
                    libs.append(str(lib))
                result = subprocess.run([str(runner), *libs, str(threads), str(PAYLOAD), str(int(primitive))], capture_output=True, text=True)
                if result.returncode == 77:
                    self.skipTest(result.stdout + result.stderr)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                print(result.stdout.strip())


if __name__ == '__main__':
    unittest.main()
