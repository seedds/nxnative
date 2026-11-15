"""Read-only HUD observations: exact build, native instruction and cadence guards."""
import importlib.util
from pathlib import Path
import struct
import sys
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'dm/tools'))
spec = importlib.util.spec_from_file_location('mp4_hud_diag', ROOT / 'dm/games/010019A01E2F2000/hud_diag.py')
hud = importlib.util.module_from_spec(spec); spec.loader.exec_module(hud)


class HUDObservations(unittest.TestCase):
    def setUp(self):
        self.image = bytearray(0x138ee20)
        for _, off, expected, *_ in hud.SITES:
            self.image[off:off+4] = bytes.fromhex(expected)
        self.module = types.SimpleNamespace(build_id=bytes.fromhex(hud.BUILD), text_end=len(self.image), image=self.image)
        self.patched = bytearray(self.image)
        self.fixups = {'BUILD_ID': hud.BUILD[:32], 'TEXT_END': hex(len(self.image))}

    def test_original_and_rewritten_builds_and_each_instruction_are_checked(self):
        hud.validate(self.module, self.patched, self.fixups)
        self.module.build_id = bytes(32)
        with self.assertRaisesRegex(ValueError, 'verified'): hud.validate(self.module)
        self.module.build_id = bytes.fromhex(hud.BUILD)
        for fixups in ({}, self.fixups | {'BUILD_ID': '00'*16}, self.fixups | {'TEXT_END': '0x10'}):
            with self.assertRaisesRegex(ValueError, 'layout'): hud.validate(self.module, self.patched, fixups)
        for _, off, *_ in hud.SITES:
            for image in (self.image, self.patched):
                image[off] ^= 1
                with self.assertRaisesRegex(ValueError, 'instruction mismatch'): hud.validate(self.module, self.patched, self.fixups)
                image[off] ^= 1

    def test_probe_environment_only_enables_bounded_observation(self):
        env = dict(line.split('=', 1) for line in hud.environment(self.module).splitlines())
        self.assertTrue(all(k.startswith('DM_PROBE') for k in env))
        self.assertEqual(env['DM_PROBE_LIMIT'], '64')
        self.assertEqual(env['DM_PROBE_INTERVAL_FRAMES'], '600')
        self.assertEqual(len(env['DM_PROBE'].split(',')), 5)
        self.assertIn('main:fc2e74@x20+8^s0', env['DM_PROBE'])

    def test_menu_observation_adds_only_bounded_inputs_and_preserves_normal_mode(self):
        env = dict(line.split('=', 1) for line in hud.environment(self.module, menu=True).splitlines())
        self.assertEqual(env['DM_PROBE_INTERVAL_FRAMES'], '120')
        self.assertEqual(env['DM_PROBE_LIMIT'], '64')
        self.assertEqual(env['DM_NVN_KEYDUMP'], '41368ee8d070c30a:6:120')
        self.assertIn('main:12dfa4@x26+54', env['DM_PROBE'])
        self.assertTrue(all(k.startswith('DM_PROBE') or k == 'DM_NVN_KEYDUMP' for k in env))
        self.assertNotIn('DM_NVN_KEYDUMP', hud.environment(self.module))
        self.assertIn('DM_PROBE_INTERVAL_FRAMES=600', hud.environment(self.module))

    def test_panel_trace_keeps_coverage_branch_and_has_no_gpu_capture(self):
        env=dict(line.split('=',1) for line in hud.environment(self.module,panels=True).splitlines())
        self.assertEqual(env['DM_PROBE_INTERVAL_FRAMES'],'120')
        self.assertEqual(len(env['DM_PROBE'].split(',')),4)
        self.assertNotIn('916c0c',env['DM_PROBE'])
        self.assertNotIn('DM_NVN_KEYDUMP',env)
        self.assertIn('main:12df9c@x26+48',env['DM_PROBE'])

    def test_sites_are_non_pc_relative_and_do_not_overlap_runtime_writes(self):
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        for _, off, expected, *_ in hud.SITES:
            ins, = md.disasm(bytes.fromhex(expected), off)
            self.assertIn(ins.mnemonic, ('ldrb', 'str', 'add', 'ldr'))
            self.assertNotIn(off, (0x916c70, 0x33170c, 0x336a8c, 0xa1d2d4))

    def test_duplicate_and_invalid_sites_are_refused(self):
        site = hud.SITES[0]
        for sites in (hud.SITES+(site,), ((site[0], -4, *site[2:]),), ((site[0], 1, *site[2:]),),
                      ((site[0], site[1], site[2], 31, site[4], site[5]),)):
            with mock.patch.object(hud, 'SITES', sites), self.assertRaisesRegex(ValueError, 'invalid'):
                hud.environment(self.module)

    def test_summary_reports_context_fov_and_bounds_without_claiming_a_fix(self):
        context = struct.pack('<4I', 3, 0, 0, 0).hex()
        bounds = struct.pack('<4f', 8.888889, -8.888889, -5, 5).hex()
        log = (f'[dm] PROBE main+0xfc2e74 #1 [5s] F=600 [x20+0x8]={context} s0=65(42820000)\n'
               f'[dm] PROBE main+0x916c0c #1 [5s] F=600 [x8+0xc]={bounds}\n')
        log += '[dm] ignored\n[kd] F620 pass 9 vs=9272509a2b90fa72_b fs=41368ee8d070c30a_b prim 4 vertices 6 instances 1 target yes\n'
        summary = hud.summarize(log)
        self.assertIn('contexts=[3]', summary); self.assertIn("'65'", summary)
        self.assertIn('orthographic-ui-bounds: 1', summary)
        self.assertIn('composition-camera: 0', summary)
        self.assertIn('Map quad input snapshots: 1', summary)
        self.assertIn('does not change HUD size', summary)
        self.assertIn('Missing/capped observations are unknown', summary)


if __name__ == '__main__': unittest.main()
