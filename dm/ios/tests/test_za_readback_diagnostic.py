#!/usr/bin/env python3
"""Keep production Z-A GPU readback diagnostics strictly opt-in."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "gfx/nvn/nvn_mtl_legacy.inc"
DISPATCH = ROOT / "gfx/nvn/nvn_mtl.m"


def body(source, signature):
    at = 0
    while True:
        start = source.index(signature, at)
        opening = source.index("{", start)
        semicolon = source.find(";", start, opening)
        if semicolon < 0:
            break
        at = semicolon + 1
    depth = 1
    for index in range(opening + 1, len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if not depth:
            return source[opening + 1:index]
    raise AssertionError("unbalanced function")


class ZAReadbackDiagnosticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = SOURCE.read_text()

    def test_only_exact_environment_value_one_enables_readback(self):
        enabled = body(self.source, "static int za_diag_enabled(void)")
        self.assertIn('getenv("DM_NVN_ZA_DIAG")', enabled)
        self.assertIn('enabled = value && !strcmp(value, "1");', enabled)
        self.assertNotIn("atoi", enabled)
        self.assertNotIn("!= NULL", enabled)

    def test_every_diagnostic_entry_is_guarded_before_state_or_gpu_work(self):
        for signature in ("static void za_diag_arm(", "static void za_diag_use(",
                          "static void za_diag_op(", "static void za_diag_pass_end(void)"):
            with self.subTest(signature=signature):
                self.assertTrue(body(self.source, signature).lstrip().startswith(
                    "if (!za_diag_enabled()) return;"))

    def test_default_path_reaches_no_readback_allocation(self):
        end = body(self.source, "static void za_diag_pass_end(void)")
        self.assertLess(end.index("if (!za_diag_enabled()) return;"), end.index("newBufferWithLength"))
        self.assertNotIn("captured automatically", self.source)
        profile = (ROOT / "games/0100F43008C44000/ios.env").read_text()
        self.assertNotIn("DM_NVN_ZA_DIAG", profile)

    def test_host_tracks_inline_texture_implementation_dependencies(self):
        makefile = (ROOT / "host/Makefile").read_text()
        self.assertIn("../gfx/nvn/*.inc", makefile)
        self.assertIn("../gfx/tex/*.inc", makefile)

    def test_ios_dispatches_to_the_legacy_renderer(self):
        dispatch = DISPATCH.read_text()
        self.assertIn("#if TARGET_OS_IPHONE", dispatch)
        self.assertIn('#include "nvn_mtl_legacy.inc"', dispatch)


if __name__ == "__main__":
    unittest.main()
