"""Test named Vulkan detection, shader framing and RenderDragon SPIR-V repair without game assets."""
from pathlib import Path
import hashlib
import struct
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games')); import profile_guess as pg
sys.path.insert(0, str(REPO / 'dm/gfx/tools')); import vk_shaders as vk


def spirv(capability):
    return struct.pack('<7I', vk.MAGIC, 0x10000, 0, 1, 0, (2 << 16) | 17, capability)


def bgfx(code):
    uniform = b'\x04test' + struct.pack('<BBHH', 0, 1, 0, 1)
    return b'VSH\x05' + struct.pack('<IH', 0, 1) + uniform + struct.pack('<I', len(code)) + code + b'\0'


def bone_shader(uint='late', alias=False):
    """The signed input shares its pointer type with another input and has two loads with signed consumers."""
    unsigned = '%uint = OpTypeInt 32 0\n' if uint != 'absent' else ''
    return '''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %bone %other %position
OpName %bone "a_indices"
OpName %other "other"
OpDecorate %bone Location 1
OpDecorate %other Location 2
OpDecorate %position BuiltIn Position
%void = OpTypeVoid
%fn = OpTypeFunction %void
%int = OpTypeInt 32 1
''' + (unsigned if uint == 'early' else '') + '''%float = OpTypeFloat 32
%vec4 = OpTypeVector %float 4
%zero = OpConstant %float 0
%one = OpConstant %float 1
%input = OpTypePointer Input %int
%output = OpTypePointer Output %vec4
%bone = OpVariable %input Input
%other = OpVariable %input Input
%position = OpVariable %output Output
''' + (unsigned if uint == 'late' else '%uint_input = OpTypePointer Input %uint\n' if uint == 'early' else '') + '''%main = OpFunction %void None %fn
%entry = OpLabel
''' + ('%alias = OpCopyObject %input %bone\n' if alias else '') + f'''%a = OpLoad %int {"%alias" if alias else "%bone"}
%b = OpLoad %int %bone
%c = OpLoad %int %other
%sum = OpIAdd %int %a %b
%result = OpISub %int %sum %c
%x = OpConvertSToF %float %result
%pos = OpCompositeConstruct %vec4 %x %zero %zero %one
OpStore %position %pos
OpReturn
OpFunctionEnd
'''


class RenderDragon(unittest.TestCase):
    def test_payload_has_shader_header_and_exact_instruction_span(self):
        good = spirv(1)
        bad = bgfx(good + b'\0' * 4)
        stray = struct.pack('<I', len(good)) + good
        self.assertEqual([code for _, code in vk.bgfx_modules(stray + bad + bgfx(good))], [good])

    def test_material_sources_and_deduplication(self):
        with tempfile.TemporaryDirectory() as d:
            directory = Path(d)
            material = directory / 'shader.material.bin'
            material.write_bytes(vk.MATERIAL_MAGIC + bgfx(spirv(1)) + bgfx(spirv(2)))
            empty = directory / 'uniforms.material.bin'
            empty.write_bytes(vk.MATERIAL_MAGIC + b'uniforms only')
            loose = directory / 'duplicate.spv'
            loose.write_bytes(spirv(2))
            self.assertEqual(set(map(Path, vk.spirv_files(d))), {loose, material})
            modules = list(vk.spirv_sources([d]))
            self.assertEqual([data for _, _, data in modules], [spirv(2), spirv(1)])
            self.assertTrue(modules[1][1].startswith('/shader.material.bin@'))

    def test_spirv_unsigned_fetch_preserves_other_inputs_and_signed_consumers(self):
        with tempfile.TemporaryDirectory() as d:
            original, fixed = Path(d) / 'original.spv', Path(d) / 'fixed.spv'
            for uint in ('absent', 'late', 'early'):
                with self.subTest(uint=uint):
                    subprocess.run(['spirv-as', '--target-env', 'vulkan1.0', '-o', str(original), '-'], input=bone_shader(uint), text=True, check=True)
                    subprocess.run(['spirv-val', '--target-env', 'vulkan1.0', str(original)], check=True)
                    data = original.read_bytes()
                    words = list(struct.unpack(f'<{len(data) // 4}I', data))
                    result = vk.unsigned_bone_indices(words)
                    fixed.write_bytes(struct.pack(f'<{len(result)}I', *result))
                    subprocess.run(['spirv-val', '--target-env', 'vulkan1.0', str(fixed)], check=True)
                    src = subprocess.run(['spirv-dis', str(fixed)], capture_output=True, text=True, check=True).stdout
                    self.assertEqual(src.count('OpLoad %uint %a_indices'), 2)
                    self.assertEqual(src.count('OpBitcast %int'), 2)
                    self.assertIn('OpLoad %int %other', src)
                    self.assertIn('OpIAdd %int', src)
                    self.assertIn('OpISub %int', src)
                    self.assertEqual(vk.unsigned_bone_indices(result), result)

    def test_material_repair_keeps_original_key_and_one_function(self):
        with tempfile.TemporaryDirectory() as d:
            original = Path(d) / 'shader.spv'
            subprocess.run(['spirv-as', '--target-env', 'vulkan1.0', '-o', str(original), '-'], input=bone_shader(), text=True, check=True)
            data = original.read_bytes(); key = hashlib.sha256(data).hexdigest()[:16]
            row = vk.one((key, '/Actor.material.bin@0x20', data), d)
            self.assertEqual(row[:3], (key, 'vert', []))
            self.assertEqual(len(row[3]), 1)
            src = Path(row[3][0]).read_text()
            self.assertIn(f'vk_{key}(', src)
            self.assertIn('uint a_indices [[attribute(1)]]', src)
            self.assertIn('int other [[attribute(2)]]', src)
            self.assertNotIn(f'vk_{key}_u', src)
            vk.one((key, '/ordinary.vert', data), d)
            self.assertIn('int a_indices [[attribute(1)]]', Path(row[3][0]).read_text())

    def test_changed_input_pointer_contract_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            original = Path(d) / 'shader.spv'
            for asm in (bone_shader(alias=True), bone_shader(alias=True).replace('OpLoad %int %bone', 'OpLoad %int %alias')):
                subprocess.run(['spirv-as', '--target-env', 'vulkan1.0', '-o', str(original), '-'], input=asm, text=True, check=True)
                data = original.read_bytes()
                with self.assertRaisesRegex(SystemExit, 'use other than OpLoad'):
                    vk.unsigned_bone_indices(list(struct.unpack(f'<{len(data) // 4}I', data)))

    def test_unread_or_builtin_input_is_unchanged(self):
        with tempfile.TemporaryDirectory() as d:
            original = Path(d) / 'shader.spv'
            for asm in (bone_shader().replace('OpLoad %int %bone', 'OpLoad %int %other'),
                        bone_shader().replace('OpDecorate %bone Location 1', 'OpDecorate %bone BuiltIn VertexIndex')):
                subprocess.run(['spirv-as', '--target-env', 'vulkan1.0', '-o', str(original), '-'], input=asm, text=True, check=True)
                subprocess.run(['spirv-val', '--target-env', 'vulkan1.0', str(original)], check=True)
                data = original.read_bytes(); words = list(struct.unpack(f'<{len(data) // 4}I', data))
                self.assertEqual(vk.unsigned_bone_indices(words), words)

    def test_named_vulkan_has_instance_and_device_evidence(self):
        imports = {pg.RO_LOOKUP}
        names = b'\0vkGetInstanceProcAddr\0vkCreateInstance\0vkCreateDevice\0'
        self.assertTrue(pg.named_vulkan(names, imports))
        self.assertFalse(pg.named_vulkan(names, set()))
        self.assertFalse(pg.named_vulkan(b'\0vkGetInstanceProcAddr\0', imports))
        self.assertFalse(pg.named_vulkan(names, imports | {'vkGetInstanceProcAddr'}))
        src = pg.shader_sources(imports, 0, [], 1, 0, vk_named=True)
        self.assertEqual(src['graphics'][0], 'vulkan')
        self.assertFalse(src['vk_capture'][0])
        self.assertEqual(pg.shader_sources(imports | {'nvnBootstrapLoader'}, 0, [], 0, 0, vk_named=True)['graphics'][0], 'nvn')
