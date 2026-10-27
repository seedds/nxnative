#!/usr/bin/env python3
"""Exercise the actual frontend keyboard function against the original bindings."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

class KeyboardBindings(unittest.TestCase):
    def test_original_keys_and_native_publication(self):
        source = (ROOT / "dm/ios/Sources/DmGamepad.m").read_text()
        start = source.index("static uint64_t keyboard_button(")
        end = source.index("\nstatic void read_key(", start)
        code = source[start:end]
        harness = """#import <GameController/GameController.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "dm_input.h"
""" + code + """
int main(void) {
    unsetenv("DM_KEY_ENTER_A");
    assert(keyboard_button(GCKeyCodeKeyJ) == DM_BTN_A);
    assert(keyboard_button(GCKeyCodeKeyK) == DM_BTN_B);
    assert(keyboard_button(GCKeyCodeKeyU) == DM_BTN_X);
    assert(keyboard_button(GCKeyCodeKeyI) == DM_BTN_Y);
    assert(keyboard_button(GCKeyCodeKeyL) == 0);
    assert(keyboard_button(GCKeyCodeKeyW) == 0);
    assert(keyboard_button(GCKeyCodeKeyQ) == DM_BTN_L);
    assert(keyboard_button(GCKeyCodeKeyE) == DM_BTN_R);
    assert(keyboard_button(GCKeyCodeKeyZ) == DM_BTN_ZL);
    assert(keyboard_button(GCKeyCodeKeyC) == DM_BTN_ZR);
    assert(keyboard_button(GCKeyCodeReturnOrEnter) == DM_BTN_PLUS);
    setenv("DM_KEY_ENTER_A", "1", 1);
    assert(keyboard_button(GCKeyCodeReturnOrEnter) == DM_BTN_A);
    assert(keyboard_button(GCKeyCodeTab) == DM_BTN_MINUS);
    assert(keyboard_button(GCKeyCodeLeftArrow) == DM_BTN_LEFT);
    assert(keyboard_button(GCKeyCodeUpArrow) == DM_BTN_UP);
    assert(keyboard_button(GCKeyCodeRightArrow) == DM_BTN_RIGHT);
    assert(keyboard_button(GCKeyCodeDownArrow) == DM_BTN_DOWN);
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as temp:
            unit, exe = Path(temp)/"keyboard.m", Path(temp)/"keyboard"
            unit.write_text(harness)
            subprocess.run(["xcrun", "clang", "-x", "objective-c", "-I", str(ROOT/"dm/host"), str(unit), "-framework", "GameController", "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
        publish = source[source.index("static void publish("):source.index("// ---- Shaking")]
        self.assertNotIn("dm_skill_buttons_apply", publish)
        self.assertNotIn("dm_za_skill_input_active", publish)
        self.assertIn("g_touch_native = g_touch_face | g_touch_btn;", publish)
        self.assertIn("uint64_t keyboard = g_key_btn;", publish)
        self.assertIn("g_key_rx = g_key_ry = 0;", source)

    def test_touch_dpad_is_available_in_simple_and_full_layouts(self):
        source = (ROOT / "dm/ios/Sources/DmGamepad.m").read_text()
        self.assertIn("static BOOL fullOnly(int b) { return b == B_LS || b == B_RS; }", source)
        began = source[source.index("- (void)touchesBegan:"):source.index("- (void)touchesMoved:")]
        self.assertIn("kind = C_DPAD", began)
        self.assertNotIn("if (_full) { CGFloat d = dist(p, self.dpadCenter)", began)

    def test_game_profiles_force_complete_touch_surface_beside_external_input(self):
        source = (ROOT / "dm/ios/Sources/DmGamepad.m").read_text()
        for title_id in ("0100F43008C44000", "0100152000022000"):
            with self.subTest(title_id=title_id):
                profile = (ROOT / f"dm/games/{title_id}/game.tsv").read_text()
                environment = (ROOT / f"dm/games/{title_id}/ios.env").read_text()
                self.assertIn("pad_layout\tfull", profile)
                self.assertIn("DM_PAD_FORCE_FULL=1", environment)
                self.assertIn("DM_PAD_KEEP_TOUCH=1", environment)

        self.assertIn('static BOOL pad_force_full(void)', source)
        self.assertIn('if (pad_force_full()) l = 1;', source)
        self.assertIn('int l = pad_force_full() ? 1', source)
        self.assertIn('BOOL any = !keepTouch &&', source)
        mouse = source[source.index("- (void)mouseMotionX:"):source.index("- (void)inputDevicesChanged")]
        self.assertIn('if (!pad_keep_touch() &&', mouse)

        # The complete visible surface still publishes the original native
        # Switch bits; no battle-specific face remapping is introduced.
        self.assertIn("g_touch_native = g_touch_face | g_touch_btn;", source)
        self.assertIn("{B_ZL, B_L, B_ZR, B_R, B_MINUS, B_PLUS, B_LS, B_RS}", source)

if __name__ == "__main__":
    unittest.main()
