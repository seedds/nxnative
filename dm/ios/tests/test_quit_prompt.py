"""The iOS quit prompt (dm/ios/Sources/quit_prompt.h), built and run on the Mac.

quit_prompt/quit_prompt_check.m runs the touch steps of the quit state machine (a tap on the quit control opens the prompt, Cancel or a
tap outside closes it, Quit is final, a controller button already held when it opens does not count), and lays the prompt out on the
iPhone, iPad and iPhone Duo screens in the six languages of the launchers: the card stays on the screen, the two pills keep one fixed
width whatever the language and every label fits at full size, and a tap lands where it should. Core Animation and Core Text only, so
no simulator is needed. QP_CAPTURE_DIR=<dir> also draws the prompt to PNGs.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

IOS = Path(__file__).resolve().parents[1]
HERE = Path(__file__).resolve().parent


@unittest.skipUnless(sys.platform == "darwin" and shutil.which("clang"), "needs clang and the Mac's Core Animation")
class QuitPromptTests(unittest.TestCase):
    def test_states_layout_and_hits(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "quit_prompt_check"
            subprocess.run(["clang", "-x", "objective-c", "-fobjc-arc", "-Wall", "-Werror", "-Wno-unused-function",
                            "-I", str(IOS / "Sources"), "-I", str(IOS.parent / "host"),
                            str(HERE / "quit_prompt" / "quit_prompt_check.m"),
                            "-framework", "Foundation", "-framework", "AppKit", "-framework", "QuartzCore", "-framework", "CoreText",
                            "-o", str(exe)], check=True)
            run = subprocess.run([str(exe)], capture_output=True, text=True, env=os.environ)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("ok:", run.stdout)


if __name__ == "__main__":
    unittest.main()
