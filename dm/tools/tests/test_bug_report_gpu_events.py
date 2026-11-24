"""scripts/bug_report.py and the reports macOS writes for a GPU fault of the host (issue #2777, also #2909).

macOS writes one `gpuEvent-<process>-<date>.ips` per GPU hang or page fault, in /Library/Logs/DiagnosticReports (and, on some
systems, the user's folder): docs/reference/rendering.md names them, dm/gfx/tests/t_gpu_fault_kinds.m reads them from both
folders. They hold the fault's kind (firmware-detected lockup, BIF0 page fault), its address and the busy stage, which is what
the hang issues ask reporters for. bug_report.py takes only files whose NAME STARTS with the app's executable or `dmhost`,
from the user's folder alone, so a report zip never has them and reporters attach them by hand (#2777: "which bug_report.py
doesn't collect").

Synthetic data only: every file here is made in a temporary folder.
"""
import importlib.util
import os
import plistlib
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('bug_report', REPO / 'scripts/bug_report.py')
assert spec and spec.loader
br = importlib.util.module_from_spec(spec); spec.loader.exec_module(br)

TID = '0100AAAA0000B000'
GPU_EVENT = '{"bug_type":"284","timestamp":"2026-11-15 13:55:50.00 +0000","name":"gpuEvent"}\n{"process_name":"dmhost","analysis":{"restart_reason_desc":"firmware-detected lockup","signature":577}}\n'


class GpuEventReports(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.home = self.root / 'home'
        (self.home / 'Library/Logs/nxnative').mkdir(parents=True)
        (self.home / 'Library/Logs/DiagnosticReports').mkdir(parents=True)
        self.log = self.home / f'Library/Logs/nxnative/{TID}.log'
        self.log.write_text('[RED] command buffer error: Caused GPU Hang Error (00000003:kIOGPUCommandBufferCallbackErrorHang)\n')
        started = time.time() - 600; os.utime(self.log, (started, started)); os.utime(self.log, None)   # a run that began 10 minutes ago: an earlier time also moves the file's birth time back
        self.system = self.root / 'system/Library/Logs/DiagnosticReports'; self.system.mkdir(parents=True)
        self.app = self.root / 'Game.app'; (self.app / 'Contents').mkdir(parents=True)
        (self.app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'DMTitleID': TID, 'CFBundleDisplayName': 'Fake Game', 'CFBundleExecutable': 'nxnative'}))
        for p in (mock.patch.object(br, 'read_prefs', return_value={}),
                  mock.patch.object(br, 'device', return_value={'Mac': 'Mac99,1', 'Chip': 'Apple M9', 'macOS': '27.0', 'Xcode': 'Xcode 27.1', 'Metal compiler': 'metal 1'})):
            p.start(); self.addCleanup(p.stop)

    def put(self, folder, name, age_s=60, text=GPU_EVENT):
        p = folder / name; p.write_text(text); t = time.time() - age_s; os.utime(p, (t, t)); return p

    def crash_names(self, bundle): return {n for n in bundle.files if n.startswith('crash/')}

    def test_user_folder(self):
        d = self.home / 'Library/Logs/DiagnosticReports'
        self.put(d, 'gpuEvent-dmhost-2026-11-15-135550.ips')
        self.put(d, 'gpuEvent-Safari-2026-11-15-135551.ips')        # another process's GPU fault: not the game's
        self.put(d, 'dmhost-2026-11-15-135600.ips', text='crash\n')   # the host's own crash report, taken today
        b, _ = br.collect(br.resolve(str(self.app)), home=str(self.home))
        names = self.crash_names(b)
        self.assertIn('crash/dmhost-2026-11-15-135600.ips', names)
        self.assertNotIn('crash/gpuEvent-Safari-2026-11-15-135551.ips', names)
        self.assertIn('crash/gpuEvent-dmhost-2026-11-15-135550.ips', names, 'the GPU fault report macOS wrote for the host is not in the bundle')

    def test_system_folder(self):
        # where macOS writes them on the Macs of #2777 and #2909: /Library/Logs/DiagnosticReports, given to collect() as a folder so that a test can stand in for it
        self.put(self.system, 'gpuEvent-dmhost-2026-11-15-135550.ips')
        self.put(self.system, 'gpuEvent-dmhost-2026-10-01-000000.ips', age_s=30 * 86400)   # older than the 7 days the crash reports keep to
        self.put(self.system, 'gpuEvent-WindowServer-2026-11-15-135552.ips')
        b, report = br.collect(br.resolve(str(self.app)), home=str(self.home), system_reports=str(self.system))
        self.assertEqual(self.crash_names(b), {'crash/gpuEvent-dmhost-2026-11-15-135550.ips'})
        self.assertIn('firmware-detected lockup', b.files['crash/gpuEvent-dmhost-2026-11-15-135550.ips'])

    def test_another_games_run_is_left_out(self):
        # every packed app's host is dmhost: a report written outside this game's runs is another game's, and it must not crowd out or pass for this one's
        self.put(self.system, 'gpuEvent-dmhost-2026-11-15-135550.ips')
        self.put(self.system, 'gpuEvent-dmhost-2026-11-15-120000.ips', age_s=2 * 3600)
        b, report = br.collect(br.resolve(str(self.app)), home=str(self.home), system_reports=str(self.system))
        self.assertEqual(self.crash_names(b), {'crash/gpuEvent-dmhost-2026-11-15-135550.ips'})
        self.assertIn('1 from outside this game', report)

    def test_no_mac_log_is_marked(self):
        self.log.unlink()
        self.put(self.system, 'gpuEvent-dmhost-2026-11-15-120000.ips', age_s=2 * 3600)
        b, report = br.collect(br.resolve(str(self.app)), home=str(self.home), system_reports=str(self.system))
        self.assertEqual(self.crash_names(b), {'crash/gpuEvent-dmhost-2026-11-15-120000.ips'})
        self.assertIn('not matched to this game', report)


if __name__ == '__main__':
    unittest.main()
