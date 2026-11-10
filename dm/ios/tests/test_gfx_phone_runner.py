"""What the iPhone gfx fixture runner decides without a phone: which report it accepts, what passes, what it builds."""
import importlib.util
import json
from pathlib import Path
import unittest

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('test_gfx_on_iphone', REPO / 'dm/ios/tools/test_gfx_on_iphone.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

RUN = '3f0c1d52-6c0e-4b7e-9d0a-5d1f0c9a7e11'


def report(run_id=RUN, complete=True, passed=True, exit_code=0, checks=12, failed=0):
    return {'runID': run_id, 'complete': complete, 'passed': passed,
            'tests': {'clear_ds': {'exitCode': exit_code, 'checks': checks, 'failed': failed}}}


class AcceptReport(unittest.TestCase):
    def test_this_runs_finished_report_is_accepted(self):
        self.assertEqual(runner.accept_report(json.dumps(report()), RUN), report())

    def test_a_report_from_an_earlier_run_is_refused(self):
        # The same bundle ID keeps its Documents folder across installs: the old results.json is still there
        # until the new app writes its own.
        stale = json.dumps(report(run_id='11111111-2222-3333-4444-555555555555'))
        self.assertIsNone(runner.accept_report(stale, RUN))
        self.assertIsNone(runner.accept_report(json.dumps({'complete': True, 'passed': True}), RUN))

    def test_an_unfinished_report_is_refused(self):
        # What the app writes at launch, before the fixture runs.
        self.assertIsNone(runner.accept_report(json.dumps({'runID': RUN, 'complete': False}), RUN))
        self.assertIsNone(runner.accept_report(json.dumps({'runID': RUN}), RUN))
        self.assertIsNone(runner.accept_report(json.dumps(report(complete='yes')), RUN))

    def test_a_malformed_report_is_refused(self):
        whole = json.dumps(report())
        for text in ('', whole[:len(whole) // 2], 'not json', '[]', 'null', json.dumps([report()]), json.dumps(RUN)):
            with self.subTest(text=text):
                self.assertIsNone(runner.accept_report(text, RUN))


class ReportPassed(unittest.TestCase):
    def test_a_clean_run_passes(self):
        self.assertTrue(runner.report_passed(report(), 'clear_ds'))

    def test_a_failed_check_fails(self):
        self.assertFalse(runner.report_passed(report(passed=False, exit_code=1, failed=1), 'clear_ds'))

    def test_the_counts_must_agree_with_the_apps_verdict(self):
        for change in ({'failed': 1}, {'exit_code': 1}, {'checks': 0}, {'passed': False}, {'passed': 1},
                       {'passed': 'true'}, {'checks': '12'}, {'checks': True}, {'failed': None}):
            with self.subTest(change=change):
                self.assertFalse(runner.report_passed(report(**change), 'clear_ds'))

    def test_a_report_without_this_fixtures_result_fails(self):
        self.assertFalse(runner.report_passed({'runID': RUN, 'complete': True, 'passed': True}, 'clear_ds'))
        self.assertFalse(runner.report_passed({**report(), 'tests': []}, 'clear_ds'))
        self.assertFalse(runner.report_passed({**report(), 'tests': {'clear_ds': 12}}, 'clear_ds'))
        self.assertFalse(runner.report_passed({**report(), 'tests': {'other': report()['tests']['clear_ds']}}, 'clear_ds'))


class Names(unittest.TestCase):
    def test_the_fixture_is_a_test_on_main(self):
        path = runner.fixture_path(REPO, 'clear_ds')
        self.assertEqual(path, REPO / 'dm/gfx/tests/t_clear_ds.m')
        self.assertTrue(path.is_file())
        # fixture_entry.m returns these two file-static counters of the fixture to the app.
        self.assertIn('static int checks, failed;', path.read_text())

    def test_the_app_is_the_targets_debug_product(self):
        self.assertEqual(runner.app_path(Path('/out/DerivedData')),
                         Path('/out/DerivedData/Build/Products/Debug-iphoneos/GfxClearVerification.app'))

    def test_the_support_sources_are_the_makefiles(self):
        # support_sources() raises when dm/gfx/tests/Makefile's MTL_T_SRC or GFX no longer reads as it expects.
        sources = runner.support_sources(REPO)
        names = [path.name for path in sources]
        self.assertEqual(names[:4], ['hle_nvn.c', 'crash.c', 'hle_bigstack.c', 't_host_stubs.c'])
        self.assertNotIn('nvn_mtl.m', names)   # the fixture includes it
        self.assertEqual(names.count('hle_nvn.c'), 1)
        self.assertEqual([path for path in sources if not path.is_file()], [])


if __name__ == '__main__': unittest.main()
