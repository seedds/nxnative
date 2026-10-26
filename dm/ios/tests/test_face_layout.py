#!/usr/bin/env python3
"""Check real virtual face geometry and held-touch transition policy."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class FaceLayout(unittest.TestCase):
    def test_geometry_and_held_owner_transitions(self):
        harness = r'''
#include <assert.h>
#include "DmFaceLayout.h"
static void check(dm_face_layout *layout, const int expected[4][2]) {
    for (unsigned button = 0; button < 4; ++button) {
        dm_face_offset p = dm_face_layout_offset(layout, button);
        assert(p.x == expected[button][0] && p.y == expected[button][1]);
        for (unsigned other = 0; other < button; ++other) {
            dm_face_offset q = dm_face_layout_offset(layout, other);
            assert(p.x != q.x || p.y != q.y);
        }
    }
}
int main(void) {
    const int normal[4][2] = {{0,1}, {1,0}, {-1,0}, {0,-1}};
    const int skill[4][2] = {{1,0}, {0,1}, {0,-1}, {-1,0}};
    dm_face_layout layout = {0};
    check(&layout, normal);
    // Showing a selector while any finger owns the diamond cannot move it,
    // including a finger which currently presses no bits between two keys.
    assert(!dm_face_layout_update(&layout, 1, 1));
    check(&layout, normal);
    assert(dm_face_layout_update(&layout, 1, 0));
    check(&layout, skill);
    assert(!dm_face_layout_update(&layout, 1, 0));
    // Closing while held also waits for release. Intermediate state changes
    // do not queue a stale layout; the latest observed selector wins.
    assert(!dm_face_layout_update(&layout, 0, 1));
    check(&layout, skill);
    assert(!dm_face_layout_update(&layout, 1, 1));
    assert(!dm_face_layout_update(&layout, 1, 0));
    check(&layout, skill);
    assert(dm_face_layout_update(&layout, 0, 0));
    check(&layout, normal);
    assert(!dm_face_layout_update(&layout, 0, 0));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            unit, executable = Path(directory) / 'layout.c', Path(directory) / 'layout'
            unit.write_text(harness)
            subprocess.run(['xcrun', 'clang', '-Wall', '-Werror', '-I',
                            str(ROOT / 'dm/ios/Sources'), str(unit), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_shared_render_hit_test_geometry_and_native_inputs(self):
        source = (ROOT / 'dm/ios/Sources/DmGamepad.m').read_text()
        update = source[source.index('- (void)updateSkillFaceLayout {'):source.index('- (CGPoint)stickHome')]
        self.assertIn('_faceOf.count != 0', update)
        self.assertIn('dm_za_skill_layout_active()', update)
        self.assertIn('NSThread.isMainThread', update)
        self.assertIn('[self layoutIfNeeded]', update)
        hit = source[source.index('- (uint64_t)faceAt:'):source.index('- (uint64_t)faceAt:') + 450]
        self.assertIn('[self facePos:b]', hit)
        layout = source[source.index('- (void)layoutSubviews {'):source.index('- (void)safeAreaInsetsDidChange')]
        self.assertIn('[self facePos:b]', layout)
        publish = source[source.index('static void publish('):source.index('// ---- Shaking')]
        self.assertNotIn('dm_za_skill_', publish)
        self.assertNotIn('dm_skill_buttons_apply', publish)
        self.assertIn('g_touch_native = g_touch_face | g_touch_btn;', publish)
        self.assertIn('uint64_t keyboard = g_key_btn;', publish)
        host = (ROOT / 'dm/host/hle_za_timing.c').read_text()
        self.assertIn('getenv("DM_ZA_SKILL_LAYOUT")', host)
        self.assertNotIn('DM_ZA_SKILL_DIRECTIONS', host)


if __name__ == '__main__':
    unittest.main()
