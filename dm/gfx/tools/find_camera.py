#!/usr/bin/env python3
"""Find a game's camera uniform block in DM_NVN_CBDUMP files, for a profile's fsr1_upscale row (fsr1_up.inc; docs/reference/rendering.md,
"A game's FSR 1 drawn by MetalFX or FSR 4").

  find_camera.py [--all-stages] <dump folder> F<frame> [F<next frame>]

Only the vertex stage's uniform blocks (F<frame>_p<pass>_st0_cb<slot>_<address>.bin), where the host looks; --all-stages reads every
stage's. Vertex-stream, index and 3D-upload dumps are never read.

Lists, per uniform block (by its address), the offsets of
  proj  a perspective projection: 4x4 row-major, last row 0, 0, -1, 0, first row's x scale > 0
  vp    a view-projection: last row's xyz a unit vector (the camera's forward), the depth row parallel to it, the x and y rows orthogonal to it and each other
  size  two whole floats the size of a render target (both 64-8192)
With a second, later frame: the offsets in its blocks that hold the first frame's view-projection (vp_prev: the game keeps last frame's camera).
Shadow cascades and other views have orthographic or other-size blocks; the main camera is usually the first perspective block of the frame.
"""
import glob
import math
import os
import re
import struct
import sys
from collections import OrderedDict


def floats(path):
    b = open(path, 'rb').read()
    return struct.unpack(f'<{len(b) // 4}f', b[:len(b) // 4 * 4])


def rows(f, off):
    return [f[off // 4 + i * 4: off // 4 + i * 4 + 4] for i in range(4)]


def finite(m): return all(math.isfinite(x) for r in m for x in r)
def dot(a, b): return sum(x * y for x, y in zip(a, b))
def norm(a): return math.sqrt(dot(a, a))


def is_proj(m):
    return finite(m) and m[3] == (0.0, 0.0, -1.0, 0.0) and m[0][0] > 0 and m[1][1] != 0


def is_vp(m):
    if not finite(m): return False
    r0, r1, r2, r3 = (r[:3] for r in m)
    n0, n1, n2, n3 = map(norm, (r0, r1, r2, r3))
    if not 0.98 < n3 < 1.02 or min(n0, n1) < 0.3 or max(n0, n1) > 20 or n2 < 1e-4: return False
    if abs(dot(r0, r3)) / n0 > 0.02 or abs(dot(r1, r3)) / n1 > 0.02 or abs(dot(r0, r1)) / (n0 * n1) > 0.02: return False
    return abs(abs(dot(r2, r3)) / (n2 * n3) - 1) < 1e-3


def is_size(f, off):
    i = off // 4
    if i + 1 >= len(f): return False
    w, h = f[i], f[i + 1]
    return all(math.isfinite(v) and v == int(v) and 64 <= v <= 8192 for v in (w, h))


def pass_no(path):
    m = re.search(r'_p(\d+)_', path)
    return int(m[1]) if m else 0


UBO = re.compile(r'_p\d+_st(\d)_cb\d+_[0-9a-f]+\.bin$')


def blocks(folder, frame, all_stages=False):
    """{address: (a file of it, floats)} of one frame's uniform-block dumps, in pass order"""
    out = OrderedDict()
    for p in sorted(glob.glob(os.path.join(folder, f'{frame}_*.bin')), key=pass_no):
        m = UBO.search(p)
        if not m or (m[1] != '0' and not all_stages): continue
        a = os.path.basename(p).rsplit('_', 1)[1][:-4]
        if a not in out: out[a] = (os.path.basename(p), floats(p))
    return out


def scan(f):
    n = len(f) * 4
    proj = [o for o in range(0, n - 63, 16) if is_proj(rows(f, o))]
    vp = [o for o in range(0, n - 63, 16) if is_vp(rows(f, o)) and o not in proj]
    size = [o for o in range(0, n - 7, 16) if is_size(f, o)]
    return proj, vp, size


def main(argv):
    all_stages = '--all-stages' in argv
    argv = [a for a in argv if a != '--all-stages']
    if len(argv) not in (2, 3): sys.exit(__doc__)
    folder, frame = argv[0], argv[1]
    cur = blocks(folder, frame, all_stages)
    nxt = blocks(folder, argv[2], all_stages) if len(argv) == 3 else {}
    found = 0
    for a, (name, f) in cur.items():
        proj, vp, size = scan(f)
        if not proj or not vp: continue
        found += 1
        sz = ', '.join(f'+{o:#x}={int(f[o // 4])}x{int(f[o // 4 + 1])}' for o in size[:4])
        print(f'{a} ({name}, {len(f) * 4:#x} bytes): proj {" ".join(f"+{o:#x}" for o in proj)}  vp {" ".join(f"+{o:#x}" for o in vp)}  sizes {sz or "-"}')
        for o in vp:
            m = rows(f, o)
            prev = []
            for b, (nname, g) in nxt.items():
                for p in range(0, len(g) * 4 - 63, 16):
                    if rows(g, p) == m: prev.append(f'{b}+{p:#x}')
            if prev: print(f'    vp +{o:#x} of {frame} is in {argv[2]} at {", ".join(prev[:4])}' + (' (the same offset: this frame\'s current camera)' if any(x.endswith(f'+{o:#x}') for x in prev) else ''))
    if not found: print(f'no block of {frame} in {folder} holds both a perspective projection and a view-projection')


if __name__ == '__main__':
    main(sys.argv[1:])
