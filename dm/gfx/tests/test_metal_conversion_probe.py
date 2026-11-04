import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # the probe is a sibling module: the CI runs each test file from the repository root
import metal_conversion_probe as probe


class ProbeTests(unittest.TestCase):
    def test_original_inputs(self):
        data = probe.serialize(probe.input_cases())
        self.assertEqual(len(data), 95 * 4)
        self.assertEqual(hashlib.sha256(data).hexdigest(), probe.INPUT_SHA256)
        self.assertEqual(hashlib.sha256(data).hexdigest(), '90d94428e0a6ccb1b4e36a0e99f5238496daa04cb099c938ef4b8ad56151bbfb')

    def test_serialization_and_channels(self):
        words = [0, 0xffffffff, 0x12345678]
        self.assertEqual(probe.deserialize(probe.serialize(words)), words)
        with self.assertRaises(ValueError):
            probe.deserialize(b'123')
        channels = probe.analyze({0: []}, [0x12345678])[0]['channels']
        self.assertEqual([c['observed'] for c in channels], [0x678, (0x12345678 >> 11) & 0x7ff, 0x12345678 >> 22])

    def test_even_tie_and_neighbors(self):
        self.assertEqual(probe.candidates(0x35bfffff, 6), (1, 1, False))
        self.assertEqual(probe.candidates(0x35c00000, 6), (1, 2, True))
        self.assertEqual(probe.candidates(0x35c00001, 6), (1, 2, False))
        self.assertEqual(probe.candidates(0x35000000, 6), (0, 0, True))
        with self.assertRaises(ValueError):
            probe.candidates(0xbf800000, 6)

    def test_incomplete_and_wrong_run_rejected(self):
        cases = {0: []}
        report = {'status': 'completed', 'run_id': 'a', 'case_count': 1, 'cases': probe.analyze(cases, [0])}
        probe.validate_report(report, 'a', cases, [0])
        for bad in ({**report, 'status': 'failed'}, {**report, 'run_id': 'old'}, {**report, 'cases': []}):
            with self.assertRaises(ValueError):
                probe.validate_report(bad, 'a', cases, [0])
        with self.assertRaises(ValueError):
            probe.validate_report(report, 'a', cases, [])
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run([sys.executable, str(Path(probe.__file__)), '--output-dir', directory], check=False, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(list(Path(directory).iterdir()), [])


if __name__ == '__main__':
    unittest.main()
