"""Scan observations are build-gated, instruction-checked and never infer gameplay success."""
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'dm/tools'))
spec = importlib.util.spec_from_file_location('mp4_scan_diag', ROOT / 'dm/games/010019A01E2F2000/scan_diag.py')
assert spec and spec.loader
scan = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scan)


class ScanDiagnostics(unittest.TestCase):
    def setUp(self):
        self.build, self.probes = next(iter(scan.BUILDS.items()))
        all_probes = (*self.probes, *scan.ACQUISITION_BUILDS[self.build], *scan.QUERY_BUILDS[self.build])
        end = max(p[1] for p in all_probes) + 4
        image = bytearray(end)
        for _, off, original, *_ in all_probes:
            image[off:off + 4] = bytes.fromhex(original)
        self.module = types.SimpleNamespace(build_id=bytes.fromhex(self.build), text_end=end, image=image)
        self.fixups = {'BUILD_ID': self.build[:32], 'TEXT_END': hex(end)}

    def test_original_and_rewritten_validated(self):
        self.assertEqual(scan.validate(self.module, bytes(self.module.image), self.fixups), self.probes)

    def test_full_identity_including_tail_required(self):
        self.module.build_id = self.module.build_id[:16] + bytes(16)
        with self.assertRaisesRegex(ValueError, 'verified main'):
            scan.environment(self.module)

    def test_changed_original_and_rewritten_rejected(self):
        rewritten = bytearray(self.module.image)
        rewritten[self.probes[0][1]] ^= 1
        with self.assertRaisesRegex(ValueError, 'rewritten instruction'):
            scan.validate(self.module, rewritten, self.fixups)
        self.module.image = rewritten
        with self.assertRaisesRegex(ValueError, 'original instruction'):
            scan.environment(self.module)

    def test_wrong_rewritten_layout_and_build_rejected(self):
        for fixups in (None, self.fixups | {'TEXT_END': '4'}, self.fixups | {'BUILD_ID': '00' * 16}):
            with self.assertRaisesRegex(ValueError, 'build/layout'):
                scan.validate(self.module, self.module.image, fixups)

    def test_invalid_offsets_and_duplicate_sites_rejected(self):
        row = self.probes[0]
        for off in (-4, row[1] + 1, self.module.text_end):
            bad = (row[0], off, *row[2:])
            with mock.patch.object(scan, 'BUILDS', {self.build: (bad,)}):
                with self.assertRaisesRegex(ValueError, 'invalid site'):
                    scan.environment(self.module)
        with mock.patch.object(scan, 'BUILDS', {self.build: (row, row)}):
            with self.assertRaisesRegex(ValueError, 'duplicate'):
                scan.environment(self.module)

    def test_bounded_read_only_configuration(self):
        env = dict(line.split('=', 1) for line in scan.environment(self.module).splitlines())
        self.assertLess(len(env['DM_PROBE']), 512)
        self.assertEqual(len(env['DM_PROBE'].split(',')), 11)
        self.assertEqual(env['DM_PROBE_CHANGES'], '4')
        self.assertEqual(env['DM_PROBE_LIMIT'], '512')
        self.assertIn('main:699d4@x0+2b0^s0', env['DM_PROBE'])
        self.assertIn('main:5d9450', env['DM_PROBE'].split(','))  # every BeginScan call, even if arguments repeat
        self.assertEqual(env['DM_PROBE_EVERY'], '0')  # rare BeginScan calls must not be sampled away
        self.assertFalse(any('RESOLUTION' in key or 'VIS_SCALE' in key for key in env))

    def test_acquisition_sites_and_cadence_are_checked(self):
        env = dict(line.split('=', 1) for line in scan.environment(self.module, acquisition=True).splitlines())
        self.assertEqual(len(scan.validate(self.module, self.module.image, self.fixups, acquisition=True)), 16)
        self.assertEqual(len(env['DM_PROBE'].split(',')), 16)
        self.assertLess(len(env['DM_PROBE']), 512)
        self.assertEqual(env['DM_PROBE_INTERVAL_FRAMES'], '30')
        self.assertEqual(env['DM_PROBE_EVERY'], '0')
        self.assertEqual(env['DM_PROBE_CHANGES'], '0')
        self.assertEqual(env['DM_PROBE_FLOATS'], '1')
        self.assertIn('main:5d9450', env['DM_PROBE'].split(','))
        bad = bytearray(self.module.image); bad[0x4034a0] ^= 1
        with self.assertRaisesRegex(ValueError, 'rewritten instruction'):
            scan.validate(self.module, bad, self.fixups, acquisition=True)
        self.module.build_id = bytes(32)
        with self.assertRaisesRegex(ValueError, 'verified main'):
            scan.environment(self.module, acquisition=True)

    def test_acquisition_summary_decodes_flag_and_full_pointer(self):
        log = ('[dm] PROBE main+0x402934 #20 [1.0s] F=60 T=1 [x8+0x130]=00000000010000000000000000000000\n'
               '[dm] PROBE main+0x402938 #20 [1.0s] F=60 T=1 [x8+0x190]=88776655443322110000000000000000\n')
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            scan.summarize(log, acquisition=True)
        self.assertIn('flag134=01', out.getvalue())
        self.assertIn('ptr190=1122334455667788', out.getvalue())
        self.assertIn('project-bounds: no printed hit', out.getvalue())

    def test_probe_sites_do_not_overlap_runtime_patch_branches(self):
        profile = scan.PROFILE
        probe_sites = {p[1] for table in (profile['SCAN_DIAGNOSTICS'], profile['SCAN_ACQUISITION_DIAGNOSTICS'], profile['SCAN_QUERY_DIAGNOSTICS'])
                       for probes in table.values() for p in probes}
        self.assertTrue(probe_sites.isdisjoint({profile['scene_site'], profile['SCAN_SOURCE_SITE']}))

    def test_query_probes_validate_and_decode_source_size(self):
        env = scan.environment(self.module, query=True)
        self.assertEqual(len(scan.validate(self.module, self.module.image, self.fixups, query=True)), 16)
        self.assertIn('main:f74b68@x0+0', env)
        self.assertIn('DM_PROBE_INTERVAL_FRAMES=31', env)
        self.assertIn('main:336a6c@x28+14', env)
        self.assertNotIn('main:336a8c', env)
        self.assertLess(len(dict(line.split('=', 1) for line in env.splitlines())['DM_PROBE']), 512)
        bad = bytearray(self.module.image); bad[0x336a6c] ^= 1
        with self.assertRaisesRegex(ValueError, 'rewritten instruction'):
            scan.validate(self.module, bad, self.fixups, query=True)
        with self.assertRaisesRegex(ValueError, 'choose only one'):
            scan.environment(self.module, acquisition=True, query=True)
        log = '[dm] PROBE main+0x336a8c #1 [1.0s] F=60 T=1 [x28+0x14]=800c0000080700000000000000000000\n'
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            scan.summarize(log, query=True)
        self.assertIn('3200x1800', out.getvalue())
        self.assertIn('Observed source sizes outside the native scan-query bounds before adaptation: 3200x1800', out.getvalue())
        valid = log.replace('800c000008070000', '00050000d0020000')
        accepted = io.StringIO()
        with contextlib.redirect_stdout(accepted):
            scan.summarize(valid, query=True)
        self.assertNotIn('Observed source sizes rejected', accepted.getvalue())
        out = io.StringIO()
        slot = '[dm] PROBE main+0xd325e8 #1 [1.0s] F=60 T=1 [x8+0x1270]=0200000000000000ffffffff00000000\n'
        with contextlib.redirect_stdout(out):
            scan.summarize(slot, query=True)
        self.assertIn('ring=2, selected=-1', out.getvalue())


    def test_summary_preserves_unknowns_and_reports_scalar(self):
        log = ('[dm] PROBE main+0xcb2cb8 #1 [1.0s] F=60 T=1 x0=0 [x0+0x2b0]=unreadable\n'
               '[dm] PROBE main+0x699d4 #1 [2.0s] F=120 T=1 x0=0x1234 s0=0.5(3f000000) s1=999(4479c000) [x0+0x2b0]=01000000000000000000000000000000\n')
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            scan.summarize(log)
        self.assertIn('target-resolved: 1 observations, f60..f60; first-word values unreadable', out.getvalue())
        self.assertIn('scalar min/max 0.5/0.5', out.getvalue())
        self.assertIn('begin-request: no printed hit', out.getvalue())
        self.assertIn('do not establish that a scan completed', out.getvalue())


if __name__ == '__main__':
    unittest.main()
