"""Stripped Unity 2017 Shader resources, without game data."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import unityfs


def string(value):
    raw = value.encode()
    return struct.pack('<i', len(raw)) + raw + bytes(-len(raw) % 4)


class Unity2017Tests(unittest.TestCase):
    def test_stripped_shader_has_no_texture_map(self):
        # Empty properties and subshaders, then the parsed shader's strings,
        # dependencies and aligned bool. There is no texture map before baked.
        parsed = (struct.pack('<ii', 0, 0) + string('Legacy/Blit') + string('')
                  + string('') + struct.pack('<i', 0) + b'\1\0\0\0')
        arrays = b''.join(struct.pack('<iI', 1, value) for value in (38, 0, 0, 0))
        payload = (string('Legacy') + parsed + arrays + struct.pack('<i', 0)
                   + struct.pack('<iiq', 1, 0, 42) + b'\1\0\0\0')
        type_hash = bytes.fromhex('266d53113fa30d2b858f2768f92eaa14')
        metadata = (b'2017.4.28f1\0' + struct.pack('<i?iiBh', 38, False, 1, 48, 0, -1)
                    + type_hash + struct.pack('<i', 1))
        metadata += bytes(-(20 + len(metadata)) % 4)
        metadata += struct.pack('<qIII', 1, 0, len(payload), 0)
        data = (struct.pack('>IIII', len(metadata), 0x100 + len(payload), 17, 0x100)
                + bytes(4) + metadata).ljust(0x100, b'\0') + payload
        tree = unityfs.known_tree(48, type_hash)
        objects = list(unityfs.serialized_objects(data, {}, {48}, {(48, type_hash): tree}))
        shader = objects[0][2]
        self.assertEqual(shader['m_ParsedForm']['m_Name'], 'Legacy/Blit')
        self.assertEqual(shader['platforms'], [38])
        self.assertEqual(shader['offsets'], [0])
        self.assertEqual(shader['m_Dependencies'], [{'m_FileID': 0, 'm_PathID': 42}])
        self.assertTrue(shader['m_ShaderIsBaked'])
        reader = unityfs.Reader(payload, True)
        unityfs._read(reader, tree)
        self.assertEqual(reader.p, len(payload))

    def test_stripped_2017_2_shader_layout(self):
        type_hash = bytes.fromhex('71556c0fc74d861cf024e0cd349bd987')
        tree = unityfs.known_tree(48, type_hash)

        def count(n):
            return 1 + sum(count(k) for k in n.kids)

        self.assertEqual(count(tree), 1178)
        parsed_node = next(n for n in tree.kids if n.name == 'm_ParsedForm')
        sub = next(n for n in parsed_node.kids if n.name == 'm_SubShaders').kids[0].kids[1]
        pas = next(n for n in sub.kids if n.name == 'm_Passes').kids[0].kids[1]
        prog = next(n for n in pas.kids if n.name == 'progVertex')
        sp = prog.kids[0].kids[0].kids[1]

        tex = next(n for n in sp.kids if n.name == 'm_TextureParams').kids[0].kids[1]
        self.assertNotIn('m_MultiSampled', [n.name for n in tex.kids])
        dim = next(n for n in tex.kids if n.name == 'm_Dim')
        self.assertEqual(dim.flags, 0x4000)
        tex_reader = unityfs.Reader(struct.pack('<iiib', 1, 2, -1, 2) + b'\0\0\0', True)
        tex_val = unityfs._read(tex_reader, tex)
        assert isinstance(tex_val, dict)
        self.assertEqual(tex_val['m_Dim'], 2)
        self.assertEqual(tex_reader.p, 16)

        cb = next(n for n in sp.kids if n.name == 'm_ConstantBuffers').kids[0].kids[1]
        self.assertNotIn('m_StructParams', [n.name for n in cb.kids])
        cb_reader = unityfs.Reader(struct.pack('<iiii', 3, 0, 0, 64), True)
        cb_val = unityfs._read(cb_reader, cb)
        assert isinstance(cb_val, dict)
        self.assertEqual(cb_val['m_NameIndex'], 3)
        self.assertEqual(cb_val['m_Size'], 64)
        self.assertEqual(cb_reader.p, 16)

        parsed = (struct.pack('<ii', 0, 0) + string('Legacy/Blit') + string('')
                  + string('') + struct.pack('<i', 0) + b'\1\0\0\0')
        arrays = b''.join(struct.pack('<iI', 1, value) for value in (38, 0, 0, 0))
        payload = (string('Legacy') + parsed + arrays + struct.pack('<i', 0)
                   + struct.pack('<iiq', 1, 0, 42) + b'\1\0\0\0')
        metadata = (b'2017.2.1p3\0' + struct.pack('<i?iiBh', 38, False, 1, 48, 0, -1)
                    + type_hash + struct.pack('<i', 1))
        metadata += bytes(-(20 + len(metadata)) % 4)
        metadata += struct.pack('<qIII', 1, 0, len(payload), 0)
        data = (struct.pack('>IIII', len(metadata), 0x100 + len(payload), 17, 0x100)
                + bytes(4) + metadata).ljust(0x100, b'\0') + payload
        objects = list(unityfs.serialized_objects(data, {}, {48}, {(48, type_hash): tree}))
        shader = objects[0][2]
        self.assertEqual(shader['m_ParsedForm']['m_Name'], 'Legacy/Blit')
        self.assertEqual(shader['platforms'], [38])
        self.assertEqual(shader['offsets'], [0])
        self.assertEqual(shader['m_Dependencies'], [{'m_FileID': 0, 'm_PathID': 42}])
        self.assertTrue(shader['m_ShaderIsBaked'])
        reader = unityfs.Reader(payload, True)
        unityfs._read(reader, tree)
        self.assertEqual(reader.p, len(payload))


if __name__ == '__main__':
    unittest.main()
