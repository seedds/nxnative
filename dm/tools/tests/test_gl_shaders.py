"""gl_shaders.py: what NVIDIA's GLSL compiler takes and glslang does not (DOOM 3's GLSL 1.50: built-ins redeclared as plain varyings, the compatibility colour pair, `sampler` as a
name), a named uniform block's table row, and main's '#version' strings that are only a prefix. The shaders here are written for the test, in the form DOOM 3's renderer makes them."""
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(REPO / 'dm/tools'), str(REPO / 'dm/gfx/tools')]
import gl_shaders   # noqa: E402

PREFIX = ('#version 150\n#define PC\n'
          'vec4 tex2D( sampler2D sampler, vec2 texcoord ) { return texture( sampler, texcoord.xy ); }\n'
          'float saturate( float v ) { return clamp( v, 0.0, 1.0 ); }\n')
VERT = PREFIX + ('uniform vec4 _va_[4];\nuniform matrices_ubo {vec4 matrices [ 408 ] ; } ;\n'
                 'in vec4 in_Position;\nin vec4 in_Color;\n'
                 'out vec4 gl_Position;\nout vec4 vofi_TexCoord0;\nout vec4 gl_FrontColor;\n'
                 'void main() {\n\tint j = int ( in_Color . x * 255.1 * 3 ) ;\n'
                 '\tgl_Position = vec4 ( dot ( in_Position , matrices [ j ] ) , dot ( in_Position , _va_[1] ) , dot ( in_Position , _va_[2] ) , dot ( in_Position , _va_[3] ) ) ;\n'
                 '\tvofi_TexCoord0 = in_Position ;\n\tgl_FrontColor = saturate ( _va_[0] . x ) * in_Color ;\n}\n')
FRAG = PREFIX + ('uniform vec4 _fa_[1];\nuniform sampler2D samp0;\n'
                 'in vec4 gl_FragCoord;\nin vec4 vofi_TexCoord0;\nin vec4 gl_Color;\n'
                 'out vec4 gl_FragColor;\nout float gl_FragDepth;\n'
                 'void main() {\n\tgl_FragColor = tex2D ( samp0 , vofi_TexCoord0 . xy + gl_FragCoord . xy ) * gl_Color * _fa_[0] ;\n\tgl_FragDepth = 0.5 ;\n}\n')


class Fixups(unittest.TestCase):
    def test_builtins_declared_as_varyings(self):
        v = gl_shaders.fixups(VERT, 'vert')
        self.assertNotIn('out vec4 gl_Position;', v)
        self.assertIn('gl_Position = vec4', v)   # the built-in itself stays
        self.assertIn('out vec4 dm_Color;', v); self.assertNotIn('gl_FrontColor', v)
        f = gl_shaders.fixups(FRAG, 'frag')
        for decl in ('in vec4 gl_FragCoord;', 'out float gl_FragDepth;', 'in vec4 gl_Color;', 'out vec4 gl_FragColor;'): self.assertNotIn(decl, f)
        self.assertIn('in vec4 dm_Color;', f); self.assertIn('out vec4 dm_FragColor;', f)
        self.assertIn('dm_FragColor = tex2D', f); self.assertIn('gl_FragCoord . xy', f); self.assertIn('gl_FragDepth = 0.5', f)

    def test_sampler_as_a_name(self):
        f = gl_shaders.fixups(FRAG, 'frag')
        self.assertIn('sampler2D dm_sampler', f); self.assertIn('texture( dm_sampler', f)
        self.assertIn('uniform sampler2D samp0;', f)   # the sampler types are other words

    def test_undeclared_compatibility_names_stay(self):   # only a declaration as a plain varying is NVIDIA's own reading; a compatibility shader's built-in is left to glslang
        t = '#version 150\nvoid main() { gl_FrontColor = vec4(1.0); }\n'
        self.assertEqual(gl_shaders.fixups(t, 'vert'), t)


@unittest.skipUnless(shutil.which('glslangValidator'), 'glslangValidator is not installed')
class Translate(unittest.TestCase):
    def test_doom3_style_pair(self):
        with tempfile.TemporaryDirectory() as tmp:
            v = gl_shaders.translate(VERT.encode(), 'vert', 'v' * 16, tmp)
            f = gl_shaders.translate(FRAG.encode(), 'frag', 'f' * 16, tmp)
            self.assertIsInstance(v, tuple, v); self.assertIsInstance(f, tuple, f)
            rows = v[1]
            self.assertIn('n:matrices_ubo=1:6528', rows)   # 408 vec4s, the default block in slot 0
            self.assertTrue(any(r.startswith('b:0:') for r in rows))
            self.assertIn('o:dm_FragColor=0', f[1])
            # the colour pair is one varying, matched by name between the stages
            self.assertIn('[[user(dm_Color)]]', Path(v[0]).read_text()); self.assertIn('[[user(dm_Color)]]', Path(f[0]).read_text())


class MainStrings(unittest.TestCase):
    def test_a_prefix_is_not_a_shader(self):
        self.assertIsNone(gl_shaders.GL_ENTRY.search(PREFIX.encode()))
        self.assertIsNotNone(gl_shaders.GL_ENTRY.search(VERT.encode()))


if __name__ == '__main__':
    unittest.main()
