"""Exercise generated profile data through the production prefix/draw selectors."""
from pathlib import Path
import copy
import os
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games'))
import game


class AspectProfile(unittest.TestCase):
    def compile_selector(self, profile, assertions):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'game_profile.h').write_text(game.header(profile))
            source = root / 'selector.c'
            source.write_text('''#include "game_profile.h"
#include "nvn_aspect.h"
#include <assert.h>
static const char prefixes[][9] = DM_GAME_ASPECT_UI_PREFIXES;
static int layout(const char *key) {
    return nvn_aspect_ui_prefix(key, prefixes, DM_GAME_ASPECT_UI_PREFIX_COUNT);
}
int main(void) {
''' + assertions + '\nreturn 0;\n}\n')
            exe = root / 'selector'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(REPO / 'dm/gfx/nvn'), str(source), '-lm', '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


    def test_default_profile_does_not_classify_float_canvas(self):
        profile = copy.copy(game.load('0100F2C0115B6000'))
        profile.rows = [(k, v) for k, v in profile.rows if k != 'aspect_ui_prefixes']
        self.compile_selector(profile, '''
    assert(!layout("7f2113e2d2db603f_b"));
    assert(!layout(""));
    assert(!nvn_aspect_ui_shader(1, layout("7f2113e2d2db603f_b")));
    assert(nvn_aspect_ui_draw(1, 6, 0) && nvn_aspect_ui_shader(0, layout(NULL)));
''')


    def test_generator_rejects_non_prefix_data(self):
        base = game.load('0100F2C0115B6000')
        for value in ('7f2113e', '7f2113e20', '7f2113eg'):
            profile = copy.copy(base)
            profile.rows = [(k, v) for k, v in base.rows if k != 'aspect_ui_prefixes']
            profile.rows.append(('aspect_ui_prefixes', [value]))
            with self.subTest(value=value), self.assertRaises(SystemExit):
                game.header(profile)


if __name__ == '__main__':
    unittest.main()
