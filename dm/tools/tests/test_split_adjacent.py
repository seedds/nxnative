"""Unreferenced NVN control/code neighbours, using synthetic shader bytes."""
import contextlib
import io
import struct
import unittest

from dm.tools.tests.test_split_gzip import fz, ss


def compute(exit_at: int | None = 8):
    ctl = bytearray(fz.VS[0]); code = bytearray(0x100)
    struct.pack_into('<I', ctl, 0x704, len(code))
    struct.pack_into('<3I', ctl, 0x748, 64, 1, 1)
    struct.pack_into('<Q', code, 0, 1)
    if exit_at is not None: struct.pack_into('<Q', code, exit_at, 0xe30000000007000f)
    return bytes(ctl), bytes(code)


class Adjacent(unittest.TestCase):
    def test_main_without_shader_relocations_keeps_compute(self):
        ctl, code = compute()
        with contextlib.redirect_stdout(io.StringIO()) as log:
            found = ss.raw_shaders(bytes(0x2000) + ctl + code, 'main')
        self.assertEqual([v[1:] for v in found], [('cs', ctl, code)])
        self.assertEqual(log.getvalue(), '')

    def test_separate_code_copied_by_address(self):
        ctl, code = fz.VS
        ctl = bytearray(ctl)
        source = len(ctl)
        ctl.extend(b'!!NVvp5.0')
        struct.pack_into('<I', ctl, 0x14, source)
        struct.pack_into('<I', ctl, 0x20, len(ctl))
        d = bytearray(0x1000 + len(code))
        d[0x1000:] = code
        # ADRP x1, +4096 / ADD x1, x1, #0 / MOV w2, #size / BL.
        seq = (0xb0000001, 0x91000021, 0x52800002 | len(code) << 5, 0x94000000)
        struct.pack_into('<4I', d, 0x80, *seq)
        self.assertEqual(ss.referenced_shader_copy(d, bytes(ctl), [0x70]), ('vs', bytes(ctl), code))
        struct.pack_into('<I', d, 0x8c, 0xaa0503e1)   # MOV x1, x5 overwrites the source before the call
        struct.pack_into('<I', d, 0x90, 0x94000000)
        self.assertIsNone(ss.referenced_shader_copy(d, bytes(ctl), [0x70]))

    def test_graphics_before_and_after(self):
        ctl, code = fz.VS
        for data, at in ((ctl + code, 0), (code + ctl, len(code))):
            self.assertEqual(ss.adjacent_shader(data, at)[1:], ('vs', ctl, code))

    def test_code_copy_takes_precedence_over_adjacent_code(self):
        ctl, code = fz.VS
        other_code = code[:-1] + bytes([code[-1] ^ 1])
        control = bytearray(ctl + b'!!NVvp5.0')
        struct.pack_into('<I', control, 0x14, len(ctl))
        struct.pack_into('<I', control, 0x20, len(control))
        image = bytearray(0x2000) + control + other_code
        image[0x1000:0x1000 + len(code)] = code
        # ADRP/ADD x8 finds the control. ADRP/ADD x1, MOV w2, and BL point at the separate code.
        struct.pack_into('<2I', image, 0x70, 0xd0000008, 0x91000108)
        struct.pack_into('<4I', image, 0x80, 0xb0000001, 0x91000021, 0x52800002 | len(code) << 5, 0x94000000)
        self.assertEqual([record[3] for record in ss.raw_shaders(bytes(image), 'main')], [code])
        struct.pack_into('<I', image, 0x78, 0x94000000)   # a separate call between the control use and the possible copy
        self.assertEqual([record[3] for record in ss.raw_shaders(bytes(image), 'main')], [other_code])

    def test_unique_headerless_compute(self):
        ctl, code = compute()
        self.assertEqual(ss.adjacent_shader(ctl + code, 0)[1:], ('cs', ctl, code))

    def test_unaligned_control_then_headed_compute(self):
        ctl, code = compute()
        code = ss.CS_MAGIC + code[4:]
        data = bytes(0x104) + ctl + code
        self.assertEqual(ss.adjacent_shader(data, 0x104)[1:], ('cs', ctl, code))

    def test_compute_exit_breaks_tie(self):
        ctl, code = compute()
        _, noexit = compute(None)
        for data, offset in ((noexit + ctl + code, len(code)), (code + ctl + noexit, len(code))):
            self.assertEqual(ss.adjacent_shader(data, offset)[1:], ('cs', ctl, code))

    def test_two_exits_or_no_exits_are_ambiguous(self):
        ctl, code = compute()
        self.assertIsNone(ss.adjacent_shader(code + ctl + code, len(code)))
        _, noexit = compute(None)
        self.assertIsNone(ss.adjacent_shader(noexit + ctl + noexit, len(code)))

    def test_scheduling_word_is_not_an_exit(self):
        ctl, code = compute(0)
        _, noexit = compute(None)
        self.assertIsNone(ss.adjacent_shader(code + ctl + noexit, len(code)))

    def test_zero_padding_is_not_compute_code(self):
        ctl, code = compute()
        self.assertIsNone(ss.adjacent_shader(ctl + bytes(len(code)), 0))

    def test_exit_in_invalid_scheduling_stream_is_not_code(self):
        ctl, code = compute(); metadata = bytearray(code)
        struct.pack_into('<Q', metadata, 32, 1 << 63)
        self.assertEqual(ss.adjacent_shader(code + ctl + metadata, len(code))[1:], ('cs', ctl, code))

    def test_truncated_ranges_and_embedded_control(self):
        ctl, code = compute()
        self.assertIsNone(ss.adjacent_shader(ctl + code[:-1], 0))
        self.assertIsNone(ss.adjacent_shader(ctl[:0x720], 0))
        self.assertIsNone(ss.adjacent_shader(ctl + ss.CTL_MAGIC + code[4:], 0))
        self.assertIsNone(ss.adjacent_shader(code + ctl, -1))

    def test_data_file_has_no_module_header(self):
        ctl, code = compute()
        found = ss.ttdat_programs(ctl + code, 'shaders')
        self.assertEqual([v[1:] for v in found], [('cs', ctl, code)])


if __name__ == '__main__': unittest.main()
