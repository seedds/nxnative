"""dm/tools/ctl_scan.py with a fake host: a thread that answers `memdump` and `peek` lines of a DM_CTL file in a log, over synthetic memory."""
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import threading
import time
import unittest

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('ctl_scan', REPO / 'dm/tools/ctl_scan.py')
assert spec is not None and spec.loader is not None
ctl_scan = importlib.util.module_from_spec(spec); spec.loader.exec_module(ctl_scan)

MAIN, ALIAS, STACK, HEAP = 0x10000000, 0x310000000, 0x310002000, 0x330000000
LOG_HEAD = (f'[dm] main: base {MAIN:#x} size 0x1000\n'
            f'[dm] guest AS 0x300000000 +0x40004000  heap {HEAP:#x} alias {ALIAS:#x} stack {STACK:#x}\n')


class FakeHost(threading.Thread):
    """Reads the lines appended to the ctl file and answers as hle_hid.c's ctl_main does (memdump: a file and a log line; peek: four words per log line)"""
    def __init__(self, ctl, log, memory):
        super().__init__(daemon=True)
        self.ctl, self.log, self.memory, self.stop, self.done = ctl, log, memory, threading.Event(), 0

    def read(self, addr, n):
        for base, data in self.memory.items():
            if base <= addr < base + len(data):
                return bytes(data[addr - base:addr - base + n])
        return b''

    def run(self):
        while not self.stop.is_set():
            lines = self.ctl.read_text().splitlines()
            for line in lines[self.done:]:
                words = line.split()
                if words and words[0] == 'memdump':
                    addr, n, path = int(words[1], 16), int(words[2]), words[3]
                    data = self.read(addr, n)
                    Path(path).write_bytes(data)
                    self.say(f'[dm] DM_CTL: memdump {addr:#x}: {len(data)} of {n} bytes -> {path}')
                elif words and words[0] == 'peek':
                    addr = int(words[1], 16)
                    data = self.read(addr, 16)
                    if len(data) < 16: self.say(f'[dm] DM_CTL: peek {addr:#x}: not readable')
                    else:
                        w = struct.unpack('<4I', data); f = struct.unpack('<4f', data)
                        self.say(f'[dm] DM_CTL: peek {addr:#x}: {f[0]:g} {f[1]:g} {f[2]:g} {f[3]:g} ({w[0]:08x} {w[1]:08x} {w[2]:08x} {w[3]:08x})')
            self.done = len(lines)
            time.sleep(0.005)

    def say(self, text):
        with open(self.log, 'a') as f: f.write(text + '\n')


class Scan(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        self.ctl, self.log, self.state = root / 'ctl.txt', root / 'game.log', root / 'state.json'
        self.ctl.write_text(''); self.log.write_text(LOG_HEAD)
        heap = bytearray(0x4000)
        struct.pack_into('<f', heap, 0x100, 1015.01)       # a float
        struct.pack_into('<d', heap, 0x200, 1015.005)      # a double
        struct.pack_into('<i', heap, 0x300, 1015)          # an int32
        struct.pack_into('<q', heap, 0x408, 7_000_000_000)  # an int64, 8 aligned
        struct.pack_into('<f', heap, 0x500, 1015.04)       # outside +-0.01
        self.memory = {HEAP: heap, MAIN: bytearray(0x1000), ALIAS: bytearray(0x2000)}
        self.fake = FakeHost(self.ctl, self.log, self.memory)
        self.fake.start()
        self.addCleanup(self.fake.join); self.addCleanup(self.fake.stop.set)
        self.host = ctl_scan.Host(self.ctl, self.log, poll=0.005)

    def scan(self, kind, value, decimals=2):
        lo, hi = ctl_scan.value_range(kind, value, decimals)
        return ctl_scan.scan(self.host, kind, lo, hi, self.state)

    def test_each_type_finds_its_value_and_only_that(self):
        found = self.scan('f32', '1015.01')
        self.assertEqual([a for a, _ in found], [HEAP + 0x100])
        self.assertAlmostEqual(found[0][1], 1015.01, places=3)
        self.assertEqual([a for a, _ in self.scan('f64', '1015.01')], [HEAP + 0x200])
        self.assertEqual([a for a, _ in self.scan('i32', '1015')], [HEAP + 0x300])
        self.assertEqual(self.scan('i64', '7000000000'), [(HEAP + 0x408, 7_000_000_000)])

    def test_a_common_value_stops_at_the_limit(self):
        lo, hi = ctl_scan.value_range('i32', '0', 2)
        found = ctl_scan.scan(self.host, 'i32', lo, hi, self.state, limit=50)
        self.assertEqual(len(found), 50)
        self.assertEqual(len(json.loads(self.state.read_text())['candidates']), 50)

    def test_a_wider_range_takes_in_the_neighbour(self):
        self.assertEqual([a for a, _ in self.scan('f32', '1015.01', decimals=1)], [HEAP + 0x100, HEAP + 0x500])

    def test_filter_keeps_what_follows_the_change(self):
        self.scan('f32', '1015.01')
        struct.pack_into('<f', self.memory[HEAP], 0x100, 1018.06)
        lo, hi = ctl_scan.value_range('f32', '1018.06', 2)
        keep = ctl_scan.filter_candidates(self.host, self.state, lo, hi)
        self.assertEqual([a for a, _ in keep], [HEAP + 0x100])
        self.assertEqual(json.loads(self.state.read_text())['candidates'][0][0], HEAP + 0x100)
        lo, hi = ctl_scan.value_range('f32', '5', 2)
        self.assertEqual(ctl_scan.filter_candidates(self.host, self.state, lo, hi), [])

    def test_filter_drops_what_is_no_longer_readable(self):
        self.scan('i32', '1015')
        self.memory[HEAP] = bytearray(0x100)   # the page is gone
        self.fake.memory = self.memory
        self.assertEqual(ctl_scan.filter_candidates(self.host, self.state, '1015', '1015'), [])

    def test_a_new_run_makes_the_candidates_stale(self):
        self.scan('i32', '1015')
        with open(self.log, 'a') as f: f.write('[dm] guest AS 0x400000000 +0x40004000  heap 0x430000000 alias 0x410000000 stack 0x410002000\n')
        with self.assertRaises(SystemExit):
            ctl_scan.filter_candidates(self.host, self.state, '1015', '1015')

    def test_show_names_the_region_and_cheat_writes_every_candidate(self):
        self.scan('i32', '1015')
        state = json.loads(self.state.read_text())
        regions = {k: tuple(v) for k, v in state['regions'].items()}
        self.assertEqual(ctl_scan.describe(HEAP + 0x300, regions), 'heap + 0x300')
        self.assertEqual(ctl_scan.describe(MAIN + 4, regions), 'main + 0x4')
        self.assertEqual(ctl_scan.describe(0x1234, regions), '0x1234')


class Helpers(unittest.TestCase):
    def test_memory_map_takes_the_last_run_and_sizes_the_regions(self):
        text = '[dm] main: base 0x1000 size 0x10\n[dm] guest AS 0x100 +0x400  heap 0x200 alias 0x110 stack 0x120\n' + LOG_HEAD
        regions, run = ctl_scan.memory_map(text)
        self.assertEqual(regions['main'], (MAIN, 0x1000))
        self.assertEqual(regions['alias'], (ALIAS, 0x2000))
        self.assertEqual(regions['heap'], (HEAP, 0x300000000 + 0x40004000 - HEAP))
        self.assertIn(f'heap {HEAP:#x}', run)
        with self.assertRaises(SystemExit): ctl_scan.memory_map('nothing here\n')

    def test_value_range(self):
        self.assertEqual(ctl_scan.value_range('i32', '1015', 2), ('1015', '1015'))
        lo, hi = ctl_scan.value_range('f64', '1015.01', 2)
        self.assertAlmostEqual(float(lo), 1015.00, places=6); self.assertAlmostEqual(float(hi), 1015.02, places=6)
        lo, hi = ctl_scan.value_range('f32', '3', 0)
        self.assertEqual((float(lo), float(hi)), (2.0, 4.0))

    def test_cheat_words_match_cheats_known_to_work(self):
        # a 4-byte store in the heap, and an 8-byte one with a 33-bit offset (the two forms the Donkey Kong Bananza note lists)
        self.assertEqual(ctl_scan.cheat_words('i32', 'heap', 0x5A2B964C, 5000), '04100000 5A2B964C 00001388')
        self.assertEqual(ctl_scan.cheat_words('f64', 'heap', 0x1CB37FAF8, 999999), '08100001 CB37FAF8 412E847E 00000000')
        self.assertEqual(ctl_scan.cheat_words('f32', 'alias', 0x10, 1.0), '04200000 00000010 3F800000')
        self.assertEqual(ctl_scan.cheat_words('i32', 'main', 0x1234, -1), '04000000 00001234 FFFFFFFF')
        self.assertEqual(ctl_scan.cheat_words('i64', 'main', 0, 5_000_000_000), '08000000 00000000 00000001 2A05F200')

    def test_the_c_scanner_reports_aligned_values_in_range_with_their_address(self):
        helper = ctl_scan.scanner()
        with tempfile.TemporaryDirectory() as tmp:
            dump = Path(tmp) / 'dump'
            data = bytearray(64)
            struct.pack_into('<i', data, 8, 42); struct.pack_into('<i', data, 13, 42)   # the second is not 4-aligned
            struct.pack_into('<i', data, 20, 99)
            dump.write_bytes(data)
            out = subprocess.run([str(helper), str(dump), 'a000', 'i32', '40', '50'], capture_output=True, text=True, check=True).stdout
            self.assertEqual(out.split(), ['a008', '42'])
            out = subprocess.run([str(helper), str(dump), 'a000', 'i32', '0', '100', '2'], capture_output=True, text=True, check=True).stdout
            self.assertEqual(len(out.splitlines()), 2)   # the limit
            self.assertNotEqual(subprocess.run([str(helper), str(dump), 'a000', 'nope', '0', '1'], capture_output=True).returncode, 0)


if __name__ == '__main__':
    unittest.main()
