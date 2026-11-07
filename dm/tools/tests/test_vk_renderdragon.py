"""Minecraft (RenderDragon, Mojang's bgfx fork): a Vulkan game whose main imports no Vulkan function and looks them up with nn::ro::LookupSymbol, and whose SPIR-V is inside
compiled materials (renderer/materials/*.material.bin) and main as bgfx shader binaries. Synthetic bytes, no game files."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games')); import profile_guess as pg
sys.path.insert(0, str(REPO / 'dm/gfx/tools')); import vk_shaders as vk


def spirv(n_caps):   # a module: header (magic, version 1.0, generator, bound, schema 0), then n_caps OpCapability Shader
    return struct.pack('<5I', vk.MAGIC, 0x10000, 0, 1, 0) + b''.join(struct.pack('<2I', (2 << 16) | 17, 1) for _ in range(n_caps))


def bgfx(kind, code):   # a bgfx shader binary, format 5: magic, hash, no uniforms, the code's size and the code, a trailing zero
    return kind + b'\x05' + b'\xaa\xbb\xcc\xdd' + struct.pack('<H', 0) + struct.pack('<I', len(code)) + code + b'\0'


class RenderDragon(unittest.TestCase):
    def test_material_modules(self):
        a, b = spirv(1), spirv(3)
        stray = struct.pack('<I', 64) + struct.pack('<I', vk.MAGIC) + b'\0' * 12   # a magic whose size prefix does not cover a whole module: not taken
        data = vk.RD_MAGIC + b'name' + stray + bgfx(b'VSH', a) + bgfx(b'FSH', b) + bgfx(b'FSH', a)
        got = list(vk.material_modules(data))
        self.assertEqual([m for _, m in got], [a, b, a])
        self.assertTrue(all(data[o:o + len(m)] == m for o, m in got))
        self.assertFalse(vk.whole_module(a + b'\0' * 4, 0, len(a) + 4))   # the instructions end before the size

    def test_sources_in_a_folder(self):
        with tempfile.TemporaryDirectory() as d:
            mats = Path(d) / 'renderer/materials'; mats.mkdir(parents=True)
            (mats / 'A.material.bin').write_bytes(vk.RD_MAGIC + bgfx(b'VSH', spirv(1)) + bgfx(b'FSH', spirv(2)))
            (mats / 'BRDF.material.bin').write_bytes(vk.RD_MAGIC + b'no passes')   # samplers and uniforms only (Minecraft's PBR/BRDF)
            (Path(d) / 'loose.spv').write_bytes(spirv(4))
            src = list(vk.spirv_sources(d))
            self.assertEqual(sorted((Path(p).name, at is None) for p, at, _ in src), [('A.material.bin', False), ('A.material.bin', False), ('loose.spv', True)])
            self.assertEqual(sorted(Path(p).name for p in vk.spirv_files(d)), ['A.material.bin', 'loose.spv'])

    def test_graphics_from_looked_up_names(self):
        ro = {pg.RO_LOOKUP}
        img = b'\0vkGetInstanceProcAddr\0vkCreateDevice\0vkCmdDraw\0other\0'
        self.assertEqual(pg.vk_lookups(img, ro), 3)
        self.assertEqual(pg.vk_lookups(img, set()), 0)   # no nn::ro::LookupSymbol
        self.assertEqual(pg.vk_lookups(img, ro | {'vkGetInstanceProcAddr'}), 0)   # imports Vulkan: counted as imports (Balatro: SDL's loader alone)
        self.assertEqual(pg.vk_lookups(b'\0vkCreateDevice\0', ro), 0)   # no vkGetInstanceProcAddr to start from
        s = pg.shader_sources(ro, 0, [], 145, 0, vk_lookups=141)
        self.assertEqual(s['graphics'][0], 'vulkan'); self.assertFalse(s['vk_capture'][0])
        self.assertEqual(pg.shader_sources(ro | {'nvnBootstrapLoader'}, 0, [], 0, 0, vk_lookups=141)['graphics'][0], 'nvn')


if __name__ == '__main__': unittest.main()
