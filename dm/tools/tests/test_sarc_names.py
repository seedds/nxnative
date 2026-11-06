"""SARC names stay within the name table, including on malformed archives."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'fuzz'))
import fuzz_shader_split as fz


class SarcNames(unittest.TestCase):
    def test_named_members_and_payload(self):
        members = [(b'a.bin', b'payload\0with zeros'), (b'b.bin', b'next')]
        self.assertEqual(list(fz.ss.sarc_members(fz.sarc(members))), [(n.decode(), d) for n, d in members])

    def test_unnamed_member(self):
        data = bytearray(fz.sarc([(b'a', b'payload')]))
        struct.pack_into('<I', data, 0x24, 0)
        self.assertEqual(list(fz.ss.sarc_members(bytes(data))), [('#0', b'payload')])

    def test_name_offset_cannot_point_into_payload(self):
        data = bytearray(fz.sarc([(b'a', b'fake_name\0')]))
        names, doff = 0x38, struct.unpack_from('<I', data, 12)[0]
        struct.pack_into('<I', data, 0x24, 0x01000000 | ((doff - names) // 4))
        with self.assertRaisesRegex(SystemExit, 'SARC.*name.*outside'):
            list(fz.ss.sarc_members(bytes(data)))

    def test_terminator_must_precede_payload(self):
        data = bytearray(fz.sarc([(b'a', b'\0payload')]))
        doff = struct.unpack_from('<I', data, 12)[0]
        data[0x38:doff] = b'x' * (doff - 0x38)
        with self.assertRaisesRegex(SystemExit, 'SARC.*name.*unterminated'):
            list(fz.ss.sarc_members(bytes(data)))


if __name__ == '__main__': unittest.main()
