"""The Drinkbox shader archive has adjacent, potentially unaligned code and control."""
import os
import struct
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
with patch.dict(os.environ, {'DM_GAME': '010020D01AD24000'}):
    import shader_split as sh


def archive():
    code = bytearray(0x100)
    code[:4] = sh.GFX_MAGIC
    struct.pack_into('<I', code, 0x30, 0x461)
    control = bytearray(0x838)
    control[:4] = sh.CTL_MAGIC
    struct.pack_into('<I', control, 0x20, len(control))
    struct.pack_into('<I', control, 0x704, len(code))
    return b'SHADERS\0\1' + b'name\0\0' + code + control, code, control


class DrinkboxShaders(unittest.TestCase):
    def test_reads_unaligned_adjacent_record(self):
        data, code, control = archive()
        self.assertEqual(sh.drinkbox_shaders(data, 'shaders.dat.nx'),
                         [('shaders.dat.nx#SHADERS@0xf', 'vs', control, code)])

    def test_rejects_inconsistent_record(self):
        data, _, _ = archive()
        with self.assertRaises(SystemExit):
            sh.drinkbox_shaders(data[:-1], 'shaders.dat.nx')
