"""Raw measurement reports must not be mistaken for regression passes."""
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location('gfx_phone_runner', Path(__file__).resolve().parents[1] / 'tools/test_gfx_on_iphone.py')
assert SPEC and SPEC.loader
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


class MeasurementReports(unittest.TestCase):
    def report(self):
        inputs = list(range(95))
        return inputs, {'runID': 'new', 'status': 'completed', 'complete': True,
                        'inputVersion': runner.INPUT_VERSION, 'inputSHA256': runner.INPUT_SHA256,
                        'inputs': inputs, 'outputs': [0xffffffff] * 95,
                        'device': {'gpu': 'test GPU', 'systemVersion': 'test OS'}}

    def test_captured_words_do_not_assert_rounding(self):
        inputs, report = self.report()
        runner.validate_measurement(report, 'new', inputs)
        self.assertNotIn('passed', report)
        self.assertNotIn('checks', report)

    def test_stale_failed_and_malformed_measurements_rejected(self):
        inputs, report = self.report()
        for field, value in [('runID', 'old'), ('status', 'error'), ('complete', False),
                             ('inputSHA256', 'wrong'), ('inputs', list(reversed(inputs))),
                             ('inputs', [False, *inputs[1:]]), ('outputs', [0] * 94),
                             ('outputs', [False] * 95), ('outputs', [1 << 32] * 95),
                             ('device', {'gpu': 'test GPU'})]:
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                runner.validate_measurement({**report, field: value}, 'new', inputs)


if __name__ == '__main__':
    unittest.main()
