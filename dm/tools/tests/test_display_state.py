"""Exercise real startup callback allocation/metadata without proprietary game files."""
import io
from pathlib import Path
import types
import unittest

SOURCE = (Path(__file__).resolve().parents[1] / 'patch_nso.py').read_text()
CALLBACKS = SOURCE[SOURCE.index("FRAME_STEP = PATCHES['FRAME_STEP']"):SOURCE.index('# ---- Spin waits')]
METADATA = '\n'.join(line.strip() for line in SOURCE.splitlines()
                     if line.startswith('    if FS_STATE_OFF:') or line.startswith('    if DISPLAY_STATE_OFF: f.write'))


class DisplayState(unittest.TestCase):
    def test_callback_combinations(self):
        for far in (False, True):
            for frame_step in (False, True):
                for display in (False, True):
                    for name in ('main', 'sdk'):
                        with self.subTest(far=far, frame_step=frame_step, display=display, name=name):
                            seen = {}
                            def fs(ctx):
                                seen['fps'] = ctx.state
                                ctx.render_scale = 1
                            def ds(ctx):
                                seen['display'] = ctx.state
                            env = dict(PATCHES={'FRAME_STEP': {'build': fs} if frame_step else {},
                                                'DISPLAY': {'build': ds} if display else {}},
                                       BUILD_ID='build', NAME=name, KEY_PAGE_OFF=0x4000,
                                       FAR=far, PATCH_OFF=0x10000, IMAGE_SIZE=0x8000,
                                       IMG=bytearray(0x10000), types=types,
                                       use_area=lambda _: None, here=lambda: 0, emit=lambda _: None)
                            exec(CALLBACKS, env)
                            f = io.StringIO()
                            env['f'] = f
                            exec(METADATA, env)
                            active_fs = frame_step and name == 'main'
                            active_display = display and name == 'main'
                            self.assertEqual(env['FS_STATE_OFF'], 0x8000 if active_fs else 0)
                            self.assertEqual(env['DISPLAY_STATE_OFF'], 0x8100 if active_display else 0)
                            self.assertEqual(env['RENDER_SCALE'], int(active_fs))
                            self.assertEqual(seen, dict(([('fps', 0x8000)] if active_fs else []) +
                                                        ([('display', 0x8100)] if active_display else [])))
                            self.assertEqual(f.getvalue(), ('# FPS_STATE_OFF\t0x8000\n' if active_fs else '') +
                                             ('# DISPLAY_STATE_OFF\t0x8100\n' if active_display else ''))
                            self.assertEqual(env['IMAGE_SIZE'], 0xc000 if far and (active_fs or active_display) else 0x8000)


if __name__ == '__main__':
    unittest.main()
