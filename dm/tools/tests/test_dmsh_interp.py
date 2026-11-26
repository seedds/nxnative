"""Regression coverage for the Maxwell raster-interface SPIR-V pass."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'gfx/tools'))
from dmsh_interp import UnsupportedInterface, interpolation_modes, lower_indexed_fragment_inputs, lower_or_keep

FRAGMENT = '''
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %input %position %out
OpExecutionMode %main OriginUpperLeft
OpDecorate %input Location 0
OpDecorate %out Location 0
OpDecorate %position BuiltIn FragCoord
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%vec4 = OpTypeVector %float 4
%uint = OpTypeInt 32 0
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%three = OpConstant %uint 3
%bool = OpTypeBool
%array = OpTypeArray %vec4 %three
%ia = OpTypePointer Input %array
%iv = OpTypePointer Input %vec4
%is = OpTypePointer Input %float
%os = OpTypePointer Output %float
%input = OpVariable %ia Input
%position = OpVariable %iv Input
%out = OpVariable %os Output
%main = OpFunction %void None %fn
%label = OpLabel
%p = OpLoad %vec4 %position
%x = OpCompositeExtract %float %p 0
%coord = OpConvertFToU %uint %x
%choice = OpIEqual %bool %coord %zero
%index = OpSelect %uint %choice %one %zero
%dynamic = OpAccessChain %is %input %index %zero
%smooth = OpLoad %float %dynamic
%direct = OpAccessChain %is %input %one %zero
%flat = OpLoad %float %direct
%sum = OpFAdd %float %smooth %flat
OpStore %out %sum
OpReturn
OpFunctionEnd
'''


def sph(*modes):
    return bytes(24) + bytes(modes) + bytes(56 - len(modes))


class InterpolationTests(unittest.TestCase):
    def test_component_modes(self):
        self.assertEqual(interpolation_modes(sph(0xaa, 0x01, 0xff), 3), [2, 1, 3])
        with self.assertRaisesRegex(UnsupportedInterface, 'mixed interpolation within location 1'):
            interpolation_modes(sph(0xaa, 0x09, 0xff), 3)

    @unittest.skipUnless(all(shutil.which(tool) for tool in ('spirv-as', 'spirv-val', 'spirv-dis')),
                         'SPIR-V Tools required')
    def test_dynamic_input_and_flat_constant(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, before, after = (root / n for n in ('test.spvasm', 'before.spv', 'after.spv'))
            source.write_text(FRAGMENT)
            subprocess.run(['spirv-as', '--target-env', 'vulkan1.1', source, '-o', before], check=True)
            original = before.read_bytes()
            self.assertEqual(lower_indexed_fragment_inputs(original, sph(0xaa, 0xaa, 0xaa)), (original, ()))
            # Real translator modules can already contain a vector input pointer.
            corrected, changed = lower_indexed_fragment_inputs(original, sph(0xaa, 0x01, 0xff))
            self.assertEqual(changed, (1, 2))
            after.write_bytes(corrected)
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', after], check=True)
            assembly = subprocess.check_output(['spirv-dis', after], text=True)
            self.assertRegex(assembly, r'OpDecorate (%\w+) Location 1\s+OpDecorate \1 Flat')
            self.assertRegex(assembly, r'OpDecorate (%\w+) Location 2\s+OpDecorate \1 NoPerspective')
            self.assertIn('OpAccessChain %_ptr_Private_float', assembly)
            self.assertIn('OpIEqual', assembly)
            self.assertIn('OpFAdd', assembly)

            # A constant location zero previously made the entire array flat.
            source.write_text(FRAGMENT.replace('OpDecorate %input Location 0',
                                               'OpDecorate %input Location 0\nOpDecorate %input Flat'))
            subprocess.run(['spirv-as', '--target-env', 'vulkan1.1', source, '-o', before], check=True)
            corrected, changed = lower_indexed_fragment_inputs(before.read_bytes(), sph(0x01, 0xaa, 0xff))
            self.assertEqual(changed, (1, 2))
            after.write_bytes(corrected)
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', after], check=True)
            assembly = subprocess.check_output(['spirv-dis', after], text=True)
            self.assertRegex(assembly, r'OpDecorate (%\w+) Location 0\s+OpDecorate \1 Flat')
            self.assertEqual(assembly.count(' Flat'), 1)

    @unittest.skipUnless(all(shutil.which(tool) for tool in ('spirv-as', 'spirv-val', 'spirv-dis')),
                         'SPIR-V Tools required')
    def test_maxwell_location_limit_with_clip_input(self):
        fragment = FRAGMENT.replace('OpCapability Shader', 'OpCapability Shader\nOpCapability ClipDistance')
        fragment = fragment.replace('%position %out', '%position %out %clip')
        fragment = fragment.replace('OpDecorate %position BuiltIn FragCoord',
                                    'OpDecorate %position BuiltIn FragCoord\nOpDecorate %clip BuiltIn ClipDistance')
        fragment = fragment.replace('%three = OpConstant %uint 3', '%three = OpConstant %uint 32')
        fragment = fragment.replace('%input = OpVariable %ia Input',
                                    '%clip_array = OpTypeArray %float %one\n'
                                    '%clip_ptr = OpTypePointer Input %clip_array\n'
                                    '%clip = OpVariable %clip_ptr Input\n%input = OpVariable %ia Input')
        fragment = fragment.replace('%dynamic = OpAccessChain', '%dynamic = OpInBoundsAccessChain')
        fragment = fragment.replace('OpStore %out %sum',
                                    '%clip_value_ptr = OpAccessChain %is %clip %zero\n'
                                    '%clip_value = OpLoad %float %clip_value_ptr\n'
                                    '%with_clip = OpFAdd %float %sum %clip_value\n'
                                    'OpStore %out %with_clip')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, before, after = (root / name for name in ('test.spvasm', 'before.spv', 'after.spv'))
            source.write_text(fragment)
            subprocess.run(['spirv-as', '--target-env', 'vulkan1.1', source, '-o', before], check=True)
            modes = [0xaa] * 32
            modes[1], modes[31] = 0x01, 0xff
            module, changed = lower_indexed_fragment_inputs(before.read_bytes(), sph(*modes))
            self.assertEqual(changed, (1, 31))
            after.write_bytes(module)
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', after], check=True)
            assembly = subprocess.check_output(['spirv-dis', after], text=True)
            self.assertIn('BuiltIn ClipDistance', assembly)
            self.assertIn('OpInBoundsAccessChain %_ptr_Private_float', assembly)
            self.assertRegex(assembly, r'OpDecorate (%\w+) Location 31\s+OpDecorate \1 NoPerspective')

    @unittest.skipUnless(shutil.which('spirv-as'), 'SPIR-V Tools required')
    def test_nonindexed_input_is_unchanged_with_mixed_header(self):
        fragment = FRAGMENT.replace('%input = OpVariable %ia Input', '%input = OpVariable %iv Input')
        fragment = fragment.replace('%input %index %zero', '%input %index')
        fragment = fragment.replace('%input %one %zero', '%input %one')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, before = root / 'test.spvasm', root / 'before.spv'
            source.write_text(fragment)
            subprocess.run(['spirv-as', '--target-env', 'vulkan1.1', source, '-o', before], check=True)
            original = before.read_bytes()
            self.assertEqual(lower_indexed_fragment_inputs(original, sph(0x09)), (original, ()))

    @unittest.skipUnless(all(shutil.which(tool) for tool in ('spirv-as', 'spirv-val')),
                         'SPIR-V Tools required')
    def test_stock_accepts_grouped_interface_with_mixed_header(self):
        cross = Path(__file__).resolve().parents[2] / 'gfx/tools/spirv_cross_bin/spirv-cross'
        if not cross.is_file():
            self.skipTest('repository SPIRV-Cross required')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, before = root / 'test.spvasm', root / 'before.spv'
            source.write_text(FRAGMENT)
            subprocess.run(['spirv-as', '--target-env', 'vulkan1.1', source, '-o', before], check=True)
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', before], check=True)
            stock = subprocess.check_output([cross, before, '--msl'], text=True)
            self.assertIn('[[user(locn1)]]', stock)
            with self.assertRaisesRegex(UnsupportedInterface, 'mixed interpolation within location 1'):
                lower_indexed_fragment_inputs(before.read_bytes(), sph(0xaa, 0x09, 0xaa))
            original = before.read_bytes()
            module, changes, reason = lower_or_keep(original, sph(0xaa, 0x09, 0xaa))
            self.assertIs(module, original)
            self.assertEqual(changes, ())
            self.assertIn('mixed interpolation within location 1', reason)
            self.assertEqual(subprocess.check_output([cross, before, '--msl'], text=True), stock)
            with self.assertRaisesRegex(ValueError, 'invalid Maxwell interpolation map'):
                lower_or_keep(original, b'')
            with self.assertRaisesRegex(ValueError, 'invalid SPIR-V instruction'):
                lower_or_keep(original + bytes(4), sph(0xaa, 0x09, 0xaa))


if __name__ == '__main__':
    unittest.main()
