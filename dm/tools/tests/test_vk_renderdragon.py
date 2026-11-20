"""Test named Vulkan detection and shader framing without game assets."""
from pathlib import Path
import struct
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

    def test_unsigned_variant_preserves_signed_uses(self):
        shader = 'struct V { int a_indices [[attribute(1)]]; }; vertex float4 f(V in [[stage_in]]) { return bones[in.a_indices]; }'
        result = vk.unsigned_bone_indices(shader)
        assert result is not None
        location, variant = result
        self.assertEqual(location, '1')
        self.assertIn('uint a_indices [[attribute(1)]]', variant)
        self.assertIn('bones[int(in.a_indices)]', variant)
        self.assertIsNone(vk.unsigned_bone_indices(shader.replace('a_indices', 'other')))

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
