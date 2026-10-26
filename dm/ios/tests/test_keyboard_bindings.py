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

if __name__ == "__main__":
    unittest.main()
