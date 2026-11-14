"""gl_shaders.py's text fix-ups before glslang: a function closed with `};` at file scope (Mega Man Legacy Collection's run-time shaders, which NVIDIA's compiler takes and glslang's
ES profile rejects as an 'extraneous semicolon') loses the semicolon; a struct's and an interface block's `};` keep it. No glslang or game files needed."""
from pathlib import Path
import sys
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import gl_shaders  # noqa: E402

# the shape of Mega Man Legacy Collection's captured vertex shader ca0e684c5cd15d29 (CRLF, as captured), with a struct, an interface block and a helper function added
SRC = ('#version 320 es\r\nprecision highp float;\r\nstruct S { float a; };\r\nuniform B { vec4 c; };\r\nin vec3 position;\r\n'
       'float f(float x) { if (x > 0.0) { return x; } return 0.0; };\r\n'
       'void main() \r\n{\r\n\tgl_Position = vec4(position * f(1.0), 1.0);\r\n};\r\n')


class FunctionSemicolons(unittest.TestCase):
    def test_dropped_after_functions_only(self):
        out = gl_shaders.fn_semicolons(SRC)
        self.assertIn('struct S { float a; };', out)
        self.assertIn('uniform B { vec4 c; };', out)
        self.assertIn('return 0.0; }\r\n', out)
        self.assertTrue(out.endswith('1.0);\r\n}\r\n'))
        self.assertEqual(out.count(';'), SRC.count(';') - 2)

    def test_idempotent_and_through_fixups(self):
        out = gl_shaders.fn_semicolons(SRC)
        self.assertEqual(gl_shaders.fn_semicolons(out), out)
        self.assertEqual(gl_shaders.fixups(SRC), out)


if __name__ == '__main__':
    unittest.main()
