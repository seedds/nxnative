"""Desktop GLSL 1.20 as MojoShader's glsl120 profile writes it (FNA's OpenGL driver: Timespinner), rewritten by gl_shaders.py, without game data."""
from pathlib import Path
import re
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import gl_shaders as gl

VERT = b'''#version 120
uniform vec4 vs_uniforms_vec4[4];
uniform float vpFlip;
#define vs_c0 vs_uniforms_vec4[0]
attribute vec4 vs_v0;
attribute vec4 vs_v1;
#define vs_oPos gl_Position
#define vs_oD0 gl_FrontColor
#define vs_oT0 gl_TexCoord[0]

void main()
{
\tvs_oPos = vs_v1 * vs_c0.x;
\tvs_oD0 = vs_v0;
\tvs_oT0.xy = vs_v1.xy;
\tgl_Position.y = gl_Position.y * vpFlip;
}
'''
FRAG = b'''#version 120
vec4 ps_r0;
uniform sampler2D ps_s0;
#define ps_v0 gl_Color
#define ps_t0 gl_TexCoord[0]
#define ps_oC0 gl_FragColor

void main()
{
\tps_r0 = texture2D(ps_s0, ps_t0.xy);
\tps_oC0 = ps_r0 * ps_v0;
}
'''


class LegacyIo(unittest.TestCase):
    def test_built_in_varyings_get_one_name_in_both_stages(self):
        v, f = gl.legacy_glsl(VERT.decode(), 'vert'), gl.legacy_glsl(FRAG.decode(), 'frag')
        for text, io in ((v, 'out'), (f, 'in')):
            self.assertIn(f'{io} vec4 dm_Color;', text)
            self.assertIn(f'{io} vec4 dm_TexCoord0;', text)
            self.assertNotRegex(text, r'\bgl_(FrontColor|Color|TexCoord)\b')
        self.assertIn('gl_Position', v)   # ES 3.10 has it

    def test_only_used_built_ins_are_declared(self):
        text = gl.legacy_glsl('void main() { gl_FragData[1] = gl_TexCoord[2] + gl_SecondaryColor; }', 'frag')
        self.assertEqual(re.findall(r'^(?:layout\(location = \d\) )?(?:in|out) \w+ \w+;', text, re.M),
                         ['in vec4 dm_SecondaryColor;', 'in vec4 dm_TexCoord2;', 'layout(location = 1) out vec4 dm_FragData1;'])

    @unittest.skipUnless(shutil.which('glslangValidator') and Path(gl.SPIRV_CROSS).exists(), 'needs glslang and the in-repo spirv-cross')
    def test_varyings_link_by_name(self):
        with tempfile.TemporaryDirectory() as tmp:
            names = []
            for src, stage in ((VERT, 'vert'), (FRAG, 'frag')):
                r = gl.translate(src, stage, stage, tmp)
                self.assertIsInstance(r, tuple, r)
                names.append(sorted(re.findall(r'\[\[user\((\w+)\)\]\]', Path(r[0]).read_text())))
            self.assertEqual(names, [['dm_Color', 'dm_TexCoord0']] * 2)


if __name__ == '__main__': unittest.main()
