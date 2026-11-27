"""Execute Ocarina of Time's screen-shape stub for the movie's viewport on ARM64, without game files: the title screen's background and the opening are a 16:9
movie drawn into the whole frame, so on a taller screen the stub keeps its viewport 16:9 about the centre, and leaves it alone at 16:9 or with the shape off.

Optional dependency: unicorn. On macOS its JIT needs execution outside the sandbox.
"""
import importlib
import importlib.util
import math
from pathlib import Path
import struct
import sys
import types
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
try:
    uc = importlib.import_module('unicorn')
    arm = importlib.import_module('unicorn.arm64_const')
except ImportError:
    uc = arm = None

spec = importlib.util.spec_from_file_location('ocarina_patches', Path(__file__).resolve().parents[2] / 'games/0400486024DEC000/patches.py')
assert spec is not None and spec.loader is not None
patch = importlib.util.module_from_spec(spec); spec.loader.exec_module(patch)
OTHERS = patch.MATCH_FRAME_STEP['main'][0]['others']
FUNCTIONS = {name: 0x100000 + i * 0x10000 for i, name in enumerate(OTHERS)}


def record_hooks(others):
    sites, capabilities, cursor = {}, set(), [0x8000000]

    def emit(_word):
        cursor[0] += 4

    ctx = types.SimpleNamespace(here=lambda: cursor[0], emit=emit, site=sites.__setitem__, put=lambda off, words, why: None,
                                state=0x9000000, base=0, others=others, capabilities=capabilities)
    patch.frame_step(ctx)
    return sites, capabilities


class Placement(unittest.TestCase):
    def test_movie_draw_site(self):
        # the movie's draw is one of the screen shape's functions, and its stub replaces the instruction after the viewport is built (ldp x9, x8, [x19, #0x18] at +0x64)
        self.assertEqual(OTHERS['movie draw']['size'], 0x158)
        sites, capabilities = record_hooks(FUNCTIONS)
        self.assertEqual(sites[FUNCTIONS['movie draw'] + 0x64], '69a241a9')
        self.assertEqual(len(sites), 23)   # frame step, DRS, two render-scale hooks, and 19 aspect hooks
        self.assertEqual(capabilities, {'DYNAMIC_RESOLUTION', 'FRAME_STEP_120', 'SCREEN_ASPECT'})

    def test_any_missing_aspect_match_leaves_all_aspect_hooks_out(self):
        render_scale = FUNCTIONS['render scale']
        other_sites = {0xe4, FUNCTIONS['dynamic resolution'], render_scale + 0xce4, render_scale + 0xd48}
        for name in patch.screen_shape_functions:
            with self.subTest(name=name):
                sites, capabilities = record_hooks(FUNCTIONS | {name: None})
                self.assertEqual(set(sites), other_sites)
                self.assertEqual(capabilities, {'DYNAMIC_RESOLUTION', 'FRAME_STEP_120'})

    def test_aspect_does_not_need_dynamic_resolution(self):
        sites, _ = record_hooks(FUNCTIONS)
        for name in ('dynamic resolution', 'dynamic resolution update'):
            with self.subTest(name=name):
                without_drs, capabilities = record_hooks(FUNCTIONS | {name: None})
                self.assertEqual(set(without_drs), set(sites) - {FUNCTIONS['dynamic resolution']})
                self.assertEqual(capabilities, {'FRAME_STEP_120', 'SCREEN_ASPECT'})


class Arm64ScreenShape(unittest.TestCase):
    base, area, state, frame = 0x4000000, 0x2000000, 0x9000000, 0xa000000
    movie = 0x3b5c710

    def setUp(self):
        assert uc is not None and arm is not None
        self.arm = arm
        self.u = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        for a, n in ((self.base, 0x4000000), (self.state, 0x1000), (self.frame, 0x1000)):
            self.u.mem_map(a, n)
        self.cursor = self.area
        ctx = types.SimpleNamespace(here=lambda: self.cursor, emit=self.emit, site=lambda off, orig: None, put=self.put, state=self.state - self.base,
                                    others=FUNCTIONS | {'movie draw': self.movie}, capabilities=set())
        patch.screen_shape(ctx)

    def emit(self, w):
        self.u.mem_write(self.base + self.cursor, struct.pack('<I', w)); self.cursor += 4

    def put(self, off, words, why):
        self.u.mem_write(self.base + off, struct.pack('<' + 'I' * len(words), *words))


@unittest.skipIf(uc is None, 'optional ARM64 execution dependency: unicorn')
class MovieViewport(Arm64ScreenShape):
    def run_viewport(self, oh, rect):
        self.u.mem_write(self.state + 0x48, struct.pack('<I', oh))
        self.u.mem_write(self.frame + 0x100 - 0x28 + 8, struct.pack('<4f', *rect))
        self.u.reg_write(self.arm.UC_ARM64_REG_X29, self.frame + 0x100)
        self.u.reg_write(self.arm.UC_ARM64_REG_X19, self.frame + 0x800)
        end = self.base + self.movie + 0x68
        self.u.emu_start(end - 4, end, count=100)
        self.assertEqual(self.u.reg_read(self.arm.UC_ARM64_REG_PC), end)
        return struct.unpack('<4f', self.u.mem_read(self.frame + 0x100 - 0x28 + 8, 16))

    def test_taller_screens_keep_the_movie_16_9(self):
        self.assertEqual(self.run_viewport(1440, (0, 0, 2560, 1920)), (0, 240, 2560, 1680))   # 4:3: bars of 240 above and below
        self.assertEqual(self.run_viewport(1200, (0, 0, 2560, 1600)), (0, 80, 2560, 1520))    # 16:10
        self.assertEqual(self.run_viewport(1280, (0, 0, 1920, 1280)), (0, 100, 1920, 1180))   # 3:2 at 1920 wide

    def test_16_9_and_shape_off_stay(self):
        self.assertEqual(self.run_viewport(1440, (0, 0, 2560, 1440)), (0, 0, 2560, 1440))     # a 16:9 frame
        self.assertEqual(self.run_viewport(0, (0, 0, 2560, 1920)), (0, 0, 2560, 1920))        # shape off (oh 0): the game's own


@unittest.skipIf(uc is None, 'optional ARM64 execution dependency: unicorn')
class PaneGuard(Arm64ScreenShape):
    def run_load_window(self, *, name=b'N_SlideList_00', kind=7, authored_height=1984.0, count=7, active=True):
        # A retail-shaped synthetic pane array with the Load list window at index 5.
        layout, res, nodes = self.frame, self.frame + 0x180, self.frame + 0x400
        records, names, record = self.frame + 0x200, self.frame + 0x240, self.frame + 0x980
        pane = nodes + 5 * 0xb0
        title, node_name = self.frame + 0x900, self.frame + 0x930
        for addr, fmt, value in (
            (self.state + 0x48, 'I', 1440 if active else 0), (self.state + 0x5c, 'f', 4 / 3),
            (pane + 0x18, 'Q', layout), (pane + 0xa8, 'I', kind << 22), (pane + 0x5c, 'f', 1984.0),
            (layout + 0x10, 'Q', res), (layout + 0x100, 'Q', nodes), (layout + 0x13c, 'H', count),
            (res, 'I', 0x004c4955), (res + 0x18, 'Q', title), (res + 0x20, 'Q', names), (res + 0x28, 'Q', records), (res + 0x38, 'I', 7),
            (records + 5 * 8, 'Q', record), (names + 5 * 8, 'Q', node_name),
            (record + 0x38, 'I', 7 << 7), (record + 0x30, 'f', authored_height),
        ):
            self.u.mem_write(addr, struct.pack('<' + fmt, value))
        self.u.mem_write(title, b'SystemLoadList_00\0')
        self.u.mem_write(node_name, name + b'\0')
        self.u.reg_write(self.arm.UC_ARM64_REG_X19, pane)
        self.u.reg_write(self.arm.UC_ARM64_REG_SP, self.frame + 0xf00 if active else 0)
        site = self.base + FUNCTIONS['pane global matrix'] + 0x3b0
        self.u.emu_start(site, site + 4, count=1000)
        self.assertEqual(self.u.reg_read(self.arm.UC_ARM64_REG_PC), site + 4)
        return struct.unpack('<f', self.u.mem_read(pane + 0x5c, 4))[0]

    def test_only_the_reviewed_pane_can_change_height(self):
        self.assertEqual(self.run_load_window(name=b'N_Other_00'), 1984.0)
        self.assertEqual(self.run_load_window(kind=1), 1984.0)
        self.assertEqual(self.run_load_window(authored_height=1700.0), 1984.0)
        self.assertEqual(self.run_load_window(count=5), 1984.0)
        self.assertAlmostEqual(self.run_load_window(), 2344.0, places=3)

    def test_shape_off_replays_without_stack_or_call(self):
        self.assertEqual(self.run_load_window(active=False), 1984.0)


@unittest.skipIf(uc is None, 'optional ARM64 execution dependency: unicorn')
class ShortcutCapture(Arm64ScreenShape):
    def run_capture(self, index, name, *, oh=1200, live=5, authored=5, width=3840.0, height=2160.0, count=7, layout_name=b'ShortCutBG_00'):
        layout, res, nodes = self.frame, self.frame + 0x180, self.frame + 0x400
        records, names, record = self.frame + 0x200, self.frame + 0x240, self.frame + 0x980
        pane, title, node_name = nodes + index * 0xb0, self.frame + 0x900, self.frame + 0x930
        for addr, fmt, value in (
            (self.state + 0x48, 'I', oh), (self.state + 0x5c, 'f', oh / 1080.0),
            (pane + 0x18, 'Q', layout), (pane + 0xa8, 'I', live << 22),
            (layout + 0x10, 'Q', res), (layout + 0x100, 'Q', nodes), (layout + 0x13c, 'H', count),
            (res, 'I', 0x004c4955), (res + 0x18, 'Q', title), (res + 0x20, 'Q', names), (res + 0x28, 'Q', records),
            (res + 0x38, 'I', 14 if layout_name == b'ShortCut_01' else 7),
            (records + index * 8, 'Q', record), (names + index * 8, 'Q', node_name),
            (record + 0x38, 'I', authored << 7), (record + 0x2c, 'f', width), (record + 0x30, 'f', height),
        ):
            self.u.mem_write(addr, struct.pack('<' + fmt, value))
        self.u.mem_write(title, layout_name + b'\0')
        self.u.mem_write(node_name, name + b'\0')
        self.u.reg_write(self.arm.UC_ARM64_REG_X19, pane)
        self.u.reg_write(self.arm.UC_ARM64_REG_SP, self.frame + 0xf00 if oh else 0)
        identity = ((1., 0., 0., 0.), (0., 1., 0., 0.), (0., 0., 1., 0.), (0., 0., 0., 1.))
        for i, column in enumerate(identity):
            self.u.reg_write(getattr(self.arm, f'UC_ARM64_REG_Q{i}'), int.from_bytes(struct.pack('<4f', *column), 'little'))
        site = self.base + FUNCTIONS['pane global matrix'] + 0x3b0
        self.u.emu_start(site, site + 4, count=2000)
        self.assertEqual(self.u.reg_read(self.arm.UC_ARM64_REG_PC), site + 4)
        return [struct.unpack('<4f', self.u.reg_read(getattr(self.arm, f'UC_ARM64_REG_Q{i}')).to_bytes(16, 'little')) for i in range(4)]

    def test_both_shortcut_captures_extend_without_shifting(self):
        for index, name in ((2, b'C_CaptureClearBG_00'), (3, b'C_CaptureBG_00')):
            for oh in (0, 1200, 1344):
                with self.subTest(index=index, output_height=oh):
                    x, y, z, translation = self.run_capture(index, name, oh=oh)
                    self.assertEqual((x, z, translation), ((1., 0., 0., 0.), (0., 0., 1., 0.), (0., 0., 0., 1.)))
                    self.assertAlmostEqual(y[1], oh / 1080 if oh else 1.0, places=5)

    def test_shortcut_capture_checks_identity(self):
        for override in ({'live': 1}, {'authored': 1}, {'width': 1920.}, {'height': 1080.}, {'count': 6}):
            with self.subTest(override=override):
                self.assertEqual(self.run_capture(2, b'C_CaptureClearBG_00', **override)[1][1], 1.)
        self.assertEqual(self.run_capture(2, b'C_Other_00')[1][1], 1.)
        self.assertEqual(self.run_capture(4, b'P_CaptureClearBG_00', live=1, authored=1)[1][1], 1.)

    def test_only_the_bottom_gradient_follows_the_new_bottom_edge(self):
        opts = dict(live=1, authored=1, height=1362., count=14, layout_name=b'ShortCut_01')
        for oh in (0, 1200, 1344):
            with self.subTest(output_height=oh):
                _, y, _, t = self.run_capture(1, b'P_Bg_00', oh=oh, **opts)
                self.assertEqual(y, (0., 1., 0., 0.))
                self.assertAlmostEqual(t[1], 1080 - oh if oh else 0.0, places=3)
        self.assertEqual(self.run_capture(5, b'Pa_Guide_02', **opts)[3][1], 0.)


@unittest.skipIf(uc is None, 'optional ARM64 execution dependency: unicorn')
class OptionsBackdrop(Arm64ScreenShape):
    def run_backdrop(self, oh, *, clip_name=b'S_Scissor_00', live_width=2200.0):
        layout, res, nodes = self.frame, self.frame + 0x180, self.frame + 0x400
        records, names = self.frame + 0x200, self.frame + 0x300
        title = self.frame + 0x900
        self.u.mem_write(title, b'SystemOption_00\0')

        def write(addr, fmt, value):
            self.u.mem_write(addr, struct.pack('<' + fmt, value))

        for addr, fmt, value in (
            (self.state + 0x48, 'I', oh), (self.state + 0x5c, 'f', oh / 1080 if oh else 1.0),
            (layout + 0x10, 'Q', res), (layout + 0x100, 'Q', nodes), (layout + 0x13c, 'H', 17),
            (res, 'I', 0x004c4955), (res + 0x18, 'Q', title), (res + 0x20, 'Q', names),
            (res + 0x28, 'Q', records), (res + 0x38, 'I', 17),
        ):
            write(addr, fmt, value)
        for i, (name, kind, width, height) in (
            (4, (clip_name, 4, 2200., 2160.)),
            (5, (b'P_OptionBg_00', 1, 2240., 5000.)),
            (6, (b'N_Unrelated_00', 0, 1920., 1524.)),
        ):
            pane, record, name_addr = nodes + i * 0xb0, self.frame + 0xa00 + (i - 4) * 0x40, title + (i - 3) * 0x30
            self.u.mem_write(name_addr, name + b'\0')
            for addr, fmt, value in (
                (names + i * 8, 'Q', name_addr), (records + i * 8, 'Q', record),
                (record + 0x2c, 'f', width), (record + 0x30, 'f', height), (record + 0x38, 'I', kind << 7),
                (pane + 0x18, 'Q', layout), (pane + 0xa8, 'I', kind << 22),
                (pane + 0x58, 'f', live_width if i == 4 else width), (pane + 0x5c, 'f', height),
            ):
                write(addr, fmt, value)

        def calc(i, y, z=0.0):
            pane = nodes + i * 0xb0
            self.u.reg_write(self.arm.UC_ARM64_REG_X19, pane)
            self.u.reg_write(self.arm.UC_ARM64_REG_SP, self.frame + 0xf00 if oh else 0)
            matrix = ((1., 0., 0., 0.), (0., 1., 0., 0.), (0., 0., 1., 0.), (0., y, z, 1.))
            for j, col in enumerate(matrix):
                self.u.reg_write(getattr(self.arm, f'UC_ARM64_REG_Q{j}'), int.from_bytes(struct.pack('<4f', *col), 'little'))
            pc = self.base + FUNCTIONS['pane global matrix'] + 0x3b0
            self.u.emu_start(pc, pc + 4, count=2000)
            self.assertEqual(self.u.reg_read(self.arm.UC_ARM64_REG_PC), pc + 4)
            for j in range(3):
                self.assertEqual(self.u.reg_read(getattr(self.arm, f'UC_ARM64_REG_Q{j}')), int.from_bytes(struct.pack('<4f', *matrix[j]), 'little'))
            _, y, z, _ = struct.unpack('<4f', self.u.reg_read(self.arm.UC_ARM64_REG_Q3).to_bytes(16, 'little'))
            return y, z, struct.unpack('<f', self.u.mem_read(pane + 0x5c, 4))[0]

        clip_y, mark, h = calc(4, 0.)
        self.assertEqual(calc(4, 0.), (clip_y, mark, h))  # repeated frames start with the authored matrix
        picture_y, _, picture_height = calc(5, 1240. + clip_y, mark)
        self.assertEqual(picture_height, 5000.)
        self.assertEqual(calc(6, -12.)[::2], (-12., 1524.))
        return clip_y, h, picture_y

    def test_options_clip_extends_without_scaling_the_picture(self):
        for oh in (0, 1200, 1344):
            with self.subTest(output_height=oh):
                extra = oh - 1080 if oh else 0
                y, h, picture_y = self.run_backdrop(oh)
                self.assertAlmostEqual(y + h / 2, 1080., places=3)
                self.assertAlmostEqual(y - h / 2, -1080. - extra, places=3)
                self.assertAlmostEqual(picture_y, 1240. - max(0, extra - 180), places=3)

    def test_options_rejects_wrong_name_or_live_width(self):
        self.assertEqual(self.run_backdrop(1344, clip_name=b'Other_00'), (0., 2160., 1240.))
        self.assertEqual(self.run_backdrop(1344, live_width=2199.), (0., 2160., 1240.))


@unittest.skipIf(uc is None, 'optional ARM64 execution dependency: unicorn')
class ProjectionFields(Arm64ScreenShape):
    def set_fovy(self, proj, fovy, aspect):
        # sead's setFovy and its inlined copies write the angle, sin, cos and tan of its half together
        h = fovy / 2
        self.u.mem_write(proj + 0xa0, struct.pack('<5f', fovy, math.sin(h), math.cos(h), math.tan(h), aspect))
        return struct.unpack('<5f', self.u.mem_read(proj + 0xa0, 20))

    def update(self, key, *, oh=1200, tol=0.01, sp=None):
        proj = self.frame + 0x200
        r = 1920.0 / oh if oh else 0.0
        k = (16.0 / 9.0) / r if oh else 0.0
        self.u.mem_write(self.state + 0x48, struct.pack('<I', oh))
        self.u.mem_write(self.state + 0x58, struct.pack('<3f', r, k, tol))
        off, reg, loads = {'perspective proj': (0x000, 'X0', ('S0', 'S1')),
                           'camera update': (0xf6c, 'X2', ('S0', 'S1')),
                           'device proj': (0x098, 'X0', ('S0', 'S3'))}[key]
        self.u.reg_write(getattr(self.arm, 'UC_ARM64_REG_' + reg), proj)
        self.u.reg_write(self.arm.UC_ARM64_REG_SP, self.frame + 0xf00 if sp is None else sp)
        site = self.base + FUNCTIONS[key] + off
        self.u.emu_start(site, site + 4, count=2000)
        self.assertEqual(self.u.reg_read(self.arm.UC_ARM64_REG_PC), site + 4)
        stored = struct.unpack('<5f', self.u.mem_read(proj + 0xa0, 20))
        loaded = tuple(struct.unpack('<f', struct.pack('<I', self.u.reg_read(getattr(self.arm, 'UC_ARM64_REG_' + r)) & 0xffffffff))[0] for r in loads)
        if not math.isnan(stored[4]):
            self.assertEqual(loaded, (stored[3], stored[4]))
        return stored

    def test_vert_plus_updates_all_five_fields_across_sites_ratios_and_fov_ranges(self):
        proj = self.frame + 0x200
        # Covers 16:10 (1200), iPhone Duo 1.43 (1344), 4:3 (1440) and all three Cephes atanf ranges (low, mid, high).
        for oh in (1200, 1344, 1440):
            r = 1920.0 / oh
            k = (16.0 / 9.0) / r
            for deg in (25.0, 50.0, 95.0, 130.0):
                fovy = math.radians(deg)
                exp_tan = math.tan(fovy / 2) * k
                exp_h = math.atan(exp_tan)
                exp = (2 * exp_h, math.sin(exp_h), math.cos(exp_h), exp_tan, r)
                for key in ('perspective proj', 'camera update', 'device proj'):
                    with self.subTest(oh=oh, deg=deg, key=key):
                        self.u.mem_write(self.state + 0x2fc, b'\0' * 0x104)
                        self.set_fovy(proj, fovy, 16.0 / 9.0)
                        stored = self.update(key, oh=oh)
                        for got, want in zip(stored, exp):
                            self.assertAlmostEqual(got, want, places=5)
                        # Horizontal half-tangent tan(fovy'/2) * r matches the original 16:9 framing.
                        self.assertAlmostEqual(math.tan(stored[0] / 2) * stored[4], math.tan(fovy / 2) * (16.0 / 9.0), places=5)
                        # A later update on the same projection leaves all fields intact without using the stack.
                        self.assertEqual(self.update(key, oh=oh, sp=0), stored)

    def test_copied_widened_fovy_does_not_double_widen(self):
        proj = self.frame + 0x200
        self.set_fovy(proj, math.radians(50.0), 16.0 / 9.0)
        widened = self.update('camera update', oh=1440)
        # Simulate another view copying the widened fovy into a fresh 16:9 projection.
        self.set_fovy(proj, widened[0], 16.0 / 9.0)
        second = self.update('device proj', oh=1440)
        self.assertEqual(second[0], widened[0])
        self.assertAlmostEqual(second[3], widened[3], places=5)
        self.assertAlmostEqual(second[4], 1920.0 / 1440, places=6)

    def test_fov_off_sets_aspect_only_hor_minus(self):
        proj = self.frame + 0x200
        for key in ('perspective proj', 'camera update', 'device proj'):
            with self.subTest(key=key):
                game = self.set_fovy(proj, 0.8, 16.0 / 9.0)
                stored = self.update(key, oh=1200, tol=-1.0)
                self.assertEqual(stored[:4], game[:4])
                self.assertAlmostEqual(stored[4], 1.6, places=6)

    def test_shape_off_and_non_16_9_and_nan_bypass_stack(self):
        proj = self.frame + 0x200
        game = self.set_fovy(proj, 0.8, 16.0 / 9.0)
        self.assertEqual(self.update('perspective proj', oh=0, sp=0), game)
        sq = self.set_fovy(proj, 0.8, 1.0)
        self.assertEqual(self.update('perspective proj', oh=1440, sp=0), sq)
        self.set_fovy(proj, 0.8, float('nan'))
        nan_out = self.update('perspective proj', oh=1440, sp=0)
        self.assertTrue(math.isnan(nan_out[4]))

    def test_preserves_caller_registers(self):
        proj = self.frame + 0x200
        for key, base_reg in (('perspective proj', 'X0'), ('camera update', 'X2'), ('device proj', 'X0')):
            with self.subTest(key=key):
                self.u.mem_write(self.state + 0x2fc, b'\0' * 0x104)
                self.set_fovy(proj, 0.8, 16.0 / 9.0)
                gprs = [f'X{i}' for i in range(31) if i not in (16, 17) and f'X{i}' != base_reg]
                sentinels = {r: 0x1122334455660000 + i for i, r in enumerate(gprs)}
                for r, v in sentinels.items():
                    self.u.reg_write(getattr(self.arm, 'UC_ARM64_REG_' + r), v)
                q_sentinels = {f'Q{i}': ((0xaa00 + i) << 64) | (0xbb00 + i) for i in range(6, 32)}
                for r, v in q_sentinels.items():
                    self.u.reg_write(getattr(self.arm, 'UC_ARM64_REG_' + r), v)
                self.update(key, oh=1440)
                for r, v in sentinels.items():
                    self.assertEqual(self.u.reg_read(getattr(self.arm, 'UC_ARM64_REG_' + r)), v, r)
                for r, v in q_sentinels.items():
                    self.assertEqual(self.u.reg_read(getattr(self.arm, 'UC_ARM64_REG_' + r)), v, r)


if __name__ == '__main__':
    unittest.main()
