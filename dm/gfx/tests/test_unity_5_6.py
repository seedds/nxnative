"""Unity 5.6 Shader resources with stripped type trees."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import unityfs


def empty_shader(tree):
    """Build an all-zero Shader tree with one item in each collection."""
    data = bytearray()

    def align():
        data.extend(bytes(-len(data) % 4))

    def emit(node):
        if node.type in unityfs.PRIM and not node.kids:
            data.extend(bytes(struct.calcsize('<' + unityfs.PRIM[node.type])))
        elif node.type == 'string':
            data.extend(struct.pack('<i', 0))
            if node.kids and node.kids[0].flags & 0x4000:
                align()
        elif node.kids and node.kids[0].type == 'Array':
            array = node.kids[0]
            data.extend(struct.pack('<i', 1))
            emit(array.kids[1])
            if array.flags & 0x4000:
                align()
        else:
            for child in node.kids:
                emit(child)
        if node.flags & 0x4000:
            align()

    emit(tree)
    return bytes(data)


class Unity56Tests(unittest.TestCase):
    def test_stripped_shader_layout_reads_all_fields(self):
        type_hash = bytes.fromhex('a70f7abc6586fb35b3a0641aa81e9375')
        tree = unityfs.known_tree(unityfs.CLASS_SHADER, type_hash)
        payload = empty_shader(tree)
        reader = unityfs.Reader(payload, True)
        shader = unityfs._read(reader, tree)

        self.assertEqual(reader.p, len(payload))
        self.assertEqual(shader['m_Name'], '')
        self.assertEqual(shader['platforms'], [0])
        subshader = shader['m_ParsedForm']['m_SubShaders'][0]
        shader_pass = subshader['m_Passes'][0]
        self.assertEqual(shader_pass['m_NameIndices'], [{'first': '', 'second': 0}])
        self.assertEqual(shader_pass['m_State']['rtBlend7']['colMask'], {'val': 0.0, 'name': ''})
        self.assertEqual(shader_pass['progVertex']['m_SubPrograms'][0]['m_GpuProgramType'], 0)


if __name__ == '__main__':
    unittest.main()
