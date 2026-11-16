#!/usr/bin/env python3
"""Synthetic test for Resident Evil's signed iOS frame-rate text variants."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
GEN_EMBED = ROOT / "dm/ios/tools/gen_embed.py"
BUILD_ID = "6bec9b23b09df46c315aa6f4bfacf91c"
SITES = (
    (0x799900, "0e000014", "c5010054", "0e000014"),
    (0x775ac4, "2f000014", "e8050034", "2f000014"),
    (0x0ce5dc, "085ea8d2", "083ea8d2", "084ea8d2"),
    (0x0ce5e8, "085ee8f2", "083ee8f2", "084ee8f2"),
)


class ResidentEvilFpsEmbedTests(unittest.TestCase):
    def test_generates_native_30_and_derived_60_signed_text(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            patched = root / "patched"
            generated = root / "Gen"
            patched.mkdir()
            generated.mkdir()

            size = 0x7d0000
            image = bytearray(size)
            for off, high, _native, _sixty in SITES:
                image[off:off + 4] = bytes.fromhex(high)
            (patched / "main.patched.bin").write_bytes(image)
            (patched / "main.fixups.tsv").write_text(
                "# TEXT_END 7a0000\n"
                "# PATCH_OFF 7c0000\n"
                "# KEY_PAGE_OFF 7a0000\n"
                "# KEY_PAGE_SIZE 4000\n"
                f"# BUILD_ID {BUILD_ID}\n",
                encoding="utf-8",
            )

            asm = generated / "embed.S"
            subprocess.run(
                [sys.executable, str(GEN_EMBED), "--re-fps", str(patched), str(asm), "main"],
                check=True,
                capture_output=True,
                text=True,
            )
            source = asm.read_text(encoding="utf-8")
            self.assertIn("__TEXT,__dmmain30x0", source)
            self.assertIn("__TEXT,__dmmain60x0", source)

            expected = {
                30: [native for _off, _high, native, _sixty in SITES],
                60: [sixty for _off, _high, _native, sixty in SITES],
            }
            for fps, values in expected.items():
                variant = (generated / f"base_main_fps{fps}_x0.bin").read_bytes()
                for (off, _high, _native, _sixty), value in zip(SITES, values):
                    self.assertEqual(variant[off:off + 4].hex(), value)

            # The source image remains the 120 fps variant; gen_embed only adds signed alternatives.
            original = (patched / "main.patched.bin").read_bytes()
            for off, high, _native, _sixty in SITES:
                self.assertEqual(original[off:off + 4].hex(), high)

    def test_refuses_an_unexpected_base_instruction(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            patched = root / "patched"
            generated = root / "Gen"
            patched.mkdir()
            generated.mkdir()
            image = bytearray(0x7d0000)
            for off, high, _native, _sixty in SITES:
                image[off:off + 4] = bytes.fromhex(high)
            image[SITES[0][0]] ^= 0xff
            (patched / "main.patched.bin").write_bytes(image)
            (patched / "main.fixups.tsv").write_text(
                "# TEXT_END 7a0000\n"
                "# PATCH_OFF 7c0000\n"
                "# KEY_PAGE_OFF 7a0000\n"
                "# KEY_PAGE_SIZE 4000\n"
                f"# BUILD_ID {BUILD_ID}\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [sys.executable, str(GEN_EMBED), "--re-fps", str(patched), str(generated / "embed.S"), "main"],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unexpected main instruction", result.stderr)


if __name__ == "__main__":
    unittest.main()
