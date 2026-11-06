# dm/gfx/tools/find_camera.py on synthetic DM_NVN_CBDUMP files: a camera block laid out as Tears of the Kingdom's (render size +0x120, projection +0x70, view-projection +0x30,
# last frame's +0x180) is found with its offsets, last frame's matrix is found in the next frame's block, and an orthographic block (a shadow cascade) is not taken
from pathlib import Path
import math
import struct
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / 'tools/find_camera.py'


def mul(a, b): return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def camera(yaw, near=0.1, sx=3.1, sy=5.5):
    """projection (row-major, GL, infinite far) and view-projection of a camera at (10, 2, 5) turned by yaw"""
    c, s = math.cos(yaw), math.sin(yaw)
    rot = [[c, 0, -s], [0, 1, 0], [s, 0, c]]
    eye = (10.0, 2.0, 5.0)
    view = [r + [-sum(r[k] * eye[k] for k in range(3))] for r in rot] + [[0, 0, 0, 1]]
    proj = [[sx, 0, 0, 0], [0, sy, 0, 0], [0, 0, -1, -2 * near], [0, 0, -1, 0]]
    return proj, mul(proj, view)


def block(vp, vp_prev, proj, w=1600, h=896):
    f = [0.0] * (0x940 // 4)
    def put(off, m):
        for i, r in enumerate(m): f[off // 4 + i * 4: off // 4 + i * 4 + 4] = r
    put(0x30, vp); put(0x70, proj); put(0x180, vp_prev); put(0x1c0, proj)
    f[0x120 // 4], f[0x124 // 4] = float(w), float(h)
    return struct.pack(f'<{len(f)}f', *f)


class FindCamera(unittest.TestCase):
    def run_tool(self, files, *frames):
        with tempfile.TemporaryDirectory() as d:
            for name, data in files.items(): Path(d, name).write_bytes(data)
            r = subprocess.run([sys.executable, str(TOOL), d, *frames], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout

    def test_camera_and_last_frame(self):
        p, vp0 = camera(0.0); _, vp1 = camera(0.05)
        # float32 round trip: what the dump holds
        vp0 = [list(struct.unpack('<4f', struct.pack('<4f', *r))) for r in vp0]
        ortho = [[0.01, 0, 0, 0], [0, 0.01, 0, 0], [0, 0, -0.001, 0], [0, 0, 0, 1]]
        files = {'F10_p1_st0_cb1_aaaa000.bin': block(vp0, vp0, p), 'F10_p2_st0_cb1_bbbb000.bin': block(ortho, ortho, ortho),
                 'F11_p1_st0_cb1_aaaa100.bin': block(vp1, vp0, p)}
        out = self.run_tool(files, 'F10', 'F11')
        self.assertIn('aaaa000 (F10_p1_st0_cb1_aaaa000.bin, 0x940 bytes): proj +0x70 +0x1c0  vp +0x30 +0x180  sizes +0x120=1600x896', out)
        self.assertIn('vp +0x30 of F10 is in F11 at aaaa100+0x180', out)
        self.assertNotIn('bbbb000', out)

    def test_vertex_stage_only(self):   # a fragment-stage block and a vertex-stream dump with the same floats are not the host's: only --all-stages reads the first
        p, vp = camera(0.0)
        data = block(vp, vp, p)
        files = {'F5_p1_st1_cb1_dddd000.bin': data, 'F5_p1_vb0_eeee000.bin': data}
        self.assertIn('no block of F5', self.run_tool(files, 'F5'))
        out = self.run_tool(files, '--all-stages', 'F5')
        self.assertIn('dddd000', out); self.assertNotIn('eeee000', out)

    def test_none(self):
        ortho = [[0.01, 0, 0, 0], [0, 0.01, 0, 0], [0, 0, -0.001, 0], [0, 0, 0, 1]]
        self.assertIn('no block of F1', self.run_tool({'F1_p1_st0_cb0_cccc000.bin': block(ortho, ortho, ortho)}, 'F1'))


if __name__ == '__main__':
    unittest.main()
