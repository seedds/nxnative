"""issue_device_check.py: the Device and OS field of a bug report against README's Requirements. Only clear cases are `unsupported`; no game files needed."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
import issue_device_check as idc

UNSUPPORTED = [
    'Mac mini Intel Core i5, macOS 26.1', 'MacBook Pro 2019 i7, macOS 15', 'iMac x86_64', 'Intel MacBook Air', 'macbook pro core i9',
    'MacBook Pro M2 Max, macOS 15.2', 'Mac Studio M1 Ultra, macOS Sequoia 15.1', 'MacBook Air M3 on macOS Sonoma', 'mac mini m4 macOS 14',
    'iPhone 14 Pro, iOS 26', 'iPhone 15, iOS 26', 'iPhone 15 Plus iOS 26.1', 'iPhone SE (3rd generation)', 'iPhone XR', 'iPhone XS Max', 'iPhone X', 'iPhone 8',
    'IPHONE 13 mini', 'iphone 12', 'iPhone 16 Pro, iOS 18.2', 'iPhone 17, iOS 18', 'iPhone with an A16, iOS 26', 'iPhone A15 Bionic',
    'iPad 10th gen A14', 'iPad mini 6 (A15)', 'iPad Pro M1, iPadOS 18.4', 'iPad Air iPadOS 17', 'iOS 18.3', 'my phone runs iOS 17.5.1',
]
NOT_UNSUPPORTED = [
    ('MacBook Pro M2 Max, macOS 26.1', 'ok'), ('Mac mini M1 macOS 26', 'ok'), ('macbook air m3, macOS Tahoe 26.0.1', 'ok'), ('Mac Studio M4 Max', 'ok'),
    ('iPhone 15 Pro, iOS 26', 'ok'), ('iPhone 15 Pro Max, iOS 26.1', 'ok'), ('IPHONE 15PRO', 'ok'), ('iPhone 16e', 'ok'), ('iPhone 16', 'ok'), ('iPhone 17 Pro Max iOS 26.2', 'ok'),
    ('iPhone Air', 'ok'), ('iPhone 16 Pro (A18 Pro) running iOS 26', 'ok'),
    ('iPad Pro M4, iPadOS 26.1', 'ok'), ('iPad Air M2', 'ok'), ('iPad Pro 11-inch (M1), iPadOS 26', 'ok'), ('iPad mini 7 (A17 Pro)', 'ok'),
    ('M1 MacBook, version 26', 'ok'), ('iOS 26.1', 'ok'),
    ('Mac with an M1, Intel build via Rosetta', 'unknown'), ('iPhone 16 and an iPad 10th gen A14', 'unknown'), ('iPhone 14 Pro (A17 Pro?)', 'unknown'),
    ('iPhone 16, was iOS 18, now iOS 26', 'unknown'), ('iPhone 14 and iPhone 16', 'unknown'), ('iPhone 16 Pro, MacBook Pro M2, macOS 15', 'unknown'),
    ('a MacBook', 'unknown'), ('iPad 10th gen', 'unknown'), ('iPhone', 'unknown'), ('Windows PC with Intel i7', 'unknown'), ('', 'unknown'), ('_No response_', 'unknown'),
]


def body(device):
    return '### What happened\n\nIt crashed.\n\n### Game, title ID and version\n\nX\n\n### Device and OS\n\n%s\n\n### Settings and environment overrides\n\nnone\n' % device


def status(device):
    return idc.classify(idc.section(body(device)))[0]


class DeviceCheck(unittest.TestCase):
    def test_unsupported(self):
        for d in UNSUPPORTED:
            with self.subTest(d):
                self.assertEqual(status(d), 'unsupported', idc.classify(d))

    def test_not_flagged(self):
        for d, want in NOT_UNSUPPORTED:
            with self.subTest(d):
                self.assertEqual(status(d), want, idc.classify(d.lower()))

    def test_reasons(self):
        self.assertIn('Intel', idc.classify('Mac mini Intel i5')[1])
        self.assertIn('iPhone 15 or 15 Plus, which has the A16 chip', idc.classify('iphone 15, ios 26')[1])
        self.assertIn('macOS below 26', idc.classify('M2 Mac, macOS 15')[1])

    def test_sections(self):
        self.assertEqual(idc.section('### What happened\n\nx\n'), None)
        self.assertEqual(idc.classify(idc.section('no form at all, iPhone 8'))[0], 'unknown')
        self.assertEqual(idc.section('### Device and OS\r\n\r\niPhone 8\r\n\r\n### Logs\r\n\r\nx'), 'iPhone 8')
        self.assertEqual(idc.section('### Device and OS\n\n_No response_\n'), None)

    def test_comment(self):
        c = idc.comment('iPhone `8` @maintainer ' + 'x' * 200, 'reason')
        self.assertTrue(c.startswith(idc.MARKER + '\n'))
        self.assertIn(idc.REQUIREMENTS, c)
        self.assertNotIn('`8`', c)
        self.assertLess(len(c), 900)

    def test_cli(self):
        with tempfile.TemporaryDirectory() as d:
            ev, cf = Path(d) / 'event.json', Path(d) / 'comment.md'
            ev.write_text(json.dumps({'issue': {'body': body('iPhone 11, iOS 26')}}))
            with contextlib.redirect_stdout(io.StringIO()):
                idc.main(['--event', str(ev), '--comment-file', str(cf)])
            self.assertTrue(cf.read_text().startswith(idc.MARKER))
            cf.unlink()
            ev.write_text(json.dumps({'issue': {'body': body('iPhone 16')}}))
            with contextlib.redirect_stdout(io.StringIO()):
                idc.main(['--event', str(ev), '--comment-file', str(cf)])
            self.assertFalse(cf.exists())


if __name__ == '__main__':
    unittest.main()
