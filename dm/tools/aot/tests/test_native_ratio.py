#!/usr/bin/env python3
"""native_ratio.py without its tools (no clang, no nxaot): the port of int_ops.c applied to the committed source, the CRC-32 table, the
parsing of nxaot_host's and the native harness's lines, the statistics, the ratio table's row and the program list.

  python3 -m unittest dm/tools/aot/tests/test_native_ratio.py
"""
import importlib.util
import os
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('native_ratio', os.path.join(HERE, 'native_ratio.py'))
nr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nr)


class NativeRatio(unittest.TestCase):
    # Every replacement of the port matches the committed int_ops.c exactly once, and no A64 assembly is left
    def test_int_ops_port(self):
        with open(os.path.join(nr.CORPUS, 'src', 'int_ops.c')) as f:
            text = f.read()
        ported = nr.port_source(text, nr.INT_OPS_PORT)
        self.assertNotIn('__asm__("', ported)
        self.assertNotIn('<arm_acle.h>', ported)
        self.assertIn('#include "nxaot_native_acle.h"', ported)
        self.assertIn('r = b ? a / b : 0;', ported)
        with self.assertRaises(nr.RatioError):
            nr.port_source(text + text, nr.INT_OPS_PORT)  # a replacement found twice

    # The table is CRC-32's (zlib's polynomial, reflected), and the header defines what int_ops.c calls
    def test_crc_table_and_header(self):
        table = nr.crc32_table()
        crc = 0xFFFFFFFF
        for b in b'12345678':
            crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8)
        self.assertEqual(crc ^ 0xFFFFFFFF, zlib.crc32(b'12345678'))
        self.assertEqual(crc ^ 0xFFFFFFFF, 0x9AE0DAAF)  # int_ops.c's crc32.check
        header = nr.native_acle()
        for name in ('__crc32b', '__crc32h', '__crc32w', '__crc32d', '__crc32cb', '__crc32ch', '__crc32cw', '__crc32cd', '__rbitll',
                     '__rev16ll'):
            self.assertIn(f' {name}(', header)

    # nxaot_host's summary line (6 decimals) and the native harness's (9)
    def test_parse_fastest(self):
        host = ('nxaot_host: x.nro: exited after 0.071234 s (fastest of 20: 0.069876 s), 45 lines, 0 dispatcher misses, 451 line-table '
                'entries, 0 threads; nxaot llvm 18.1.3\n')
        self.assertEqual(nr.parse_fastest(host), (20, 0.069876))
        native = 'native: ./int_ops: exited 0 after 0.047924 s (fastest of 10: 0.043800015 s), 45 lines\n'
        self.assertEqual(nr.parse_fastest(native), (10, 0.043800015))
        with self.assertRaises(nr.RatioError):
            nr.parse_fastest('nothing here')

    def test_labels_and_programs(self):
        self.assertEqual(nr.split_label('int_ops-gcc'), ('int_ops', 'gcc'))
        self.assertEqual(nr.split_label('switch-clang'), ('switch', 'clang'))
        self.assertEqual(nr.split_label('bench'), ('bench', None))
        for label in nr.DEFAULT_PROGRAMS:
            self.assertIn(nr.split_label(label)[0], nr.PROGRAMS)
        self.assertFalse(set(nr.PROGRAMS) & set(nr.NOT_NATIVE))
        # every corpus program is either measured or has its reason
        progs = {f[:-4] for f in os.listdir(os.path.join(nr.CORPUS, 'expected')) if f.endswith('.out')}
        self.assertEqual(progs - set(nr.PROGRAMS) - set(nr.NOT_NATIVE), set())

    # The speeds: a variant's samples within SPEED_GAP of its fastest, and the others
    def test_split_speeds(self):
        self.assertEqual(nr.split_speeds([59e-6, 24.5e-6, 58e-6, 25e-6]), ([24.5e-6, 25e-6], [59e-6, 58e-6]))
        self.assertEqual(nr.split_speeds([4.2, 4.3, 4.25]), ([4.2, 4.3, 4.25], []))

    # The statistics: a variant's time is the median of its fast group, the slow group's beside it; ratios compare one speed with itself
    def test_summary_and_row(self):
        stats = nr.summarize({'native': [0.010, 0.004, 0.011, 0.0042], 'W': [0.016, 0.007, 0.015, 0.0068], 'C': [0.040, 0.044]})
        self.assertAlmostEqual(stats['native']['fast'], 0.0041)
        self.assertAlmostEqual(stats['native']['slow'], 0.0105)
        self.assertAlmostEqual(stats['W']['fast'], 0.0069)
        self.assertEqual(stats['W']['n'], {'fast': 2, 'slow': 2})
        self.assertIsNone(stats['C']['slow'])
        row = nr.ratio_row('int_ops-gcc', stats, {'native': 1000, 'W': 4000, 'C': 9000}, ['W', 'C', 'H'])
        self.assertEqual(row, '| int_ops-gcc | 4.100 ms | 6.900 ms (1.68x / 1.48x) | 42.000 ms (10.24x / -) | - | 1,000 | 4.0 / 9.0 / - |')
        head = nr.table_header(['W', 'C'])
        self.assertEqual(head[0].count('|') - 1, len(head[1].split('|')) - 2)


if __name__ == '__main__':
    unittest.main()
