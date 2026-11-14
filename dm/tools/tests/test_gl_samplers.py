"""gl_shaders.py lists only the samplers its MSL reads. A sampler the shader declares and never samples is in SPIRV-Cross's reflection with the binding of one that is used
(Mega Man Legacy Collection's sprite shader: T0 and T1, both binding 0, only T0 sampled); listed, the host bound the unused one's texture over the real one. Needs glslang and the
in-repo spirv-cross (a Mac binary): skipped where they do not run. No game files needed."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import gl_shaders  # noqa: E402

SPRITE = (b'#version 320 es\nprecision highp float;\nin vec4 Color;\nin vec2 Texcoord;\nout vec4 outColor;\nuniform sampler2D T0;\nuniform sampler2D T1;\n'
          b'void main()\n{\n\toutColor = texture(T0, Texcoord) * Color;\n}\n')
BOTH = SPRITE.replace(b'* Color;', b'* texture(T1, Texcoord) * Color;')


def _tools_run():
    if not shutil.which('glslangValidator'): return False
    try: return subprocess.run([gl_shaders.SPIRV_CROSS, '--revision'], capture_output=True).returncode == 0
    except OSError: return False


@unittest.skipUnless(_tools_run(), 'needs glslangValidator and the in-repo spirv-cross')
class UnusedSamplers(unittest.TestCase):
    def samplers(self, src):
        with tempfile.TemporaryDirectory() as tmp:
            r = gl_shaders.translate(src, 'frag', 'k', tmp)
        self.assertIsInstance(r, tuple, r)
        return sorted(x for x in r[1] if x.startswith('t:'))

    def test_a_sampler_the_code_never_reads_is_not_listed(self):
        self.assertEqual(self.samplers(SPRITE), ['t:T0=0:sampler2D'])

    def test_samplers_that_are_read_stay(self):
        self.assertEqual([x.split('=')[0] for x in self.samplers(BOTH)], ['t:T0', 't:T1'])


if __name__ == '__main__':
    unittest.main()
