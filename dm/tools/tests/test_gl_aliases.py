"""dm/gfx/gl/gl_aliases.h (written by dm/gfx/tools/gl_aliases.py from Khronos gl.xml): sorted for hle_gl.m's binary search, and an extension name maps onto another only where the
registry or the reviewed GL ES list says the two are the same function, not wherever a vendor suffix can be stripped."""
from pathlib import Path
import re
import sys
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(REPO / 'dm/gfx/tools')]
import gl_aliases   # noqa: E402


def table():
    return re.findall(r'\{"(\w+)", "(\w+)"\}', (REPO / 'dm/gfx/gl/gl_aliases.h').read_text())


class Header(unittest.TestCase):
    def test_sorted_and_unique(self):   # gl_lookup's binary search compares with strcmp: byte order
        names = [n for n, _ in table()]
        self.assertGreater(len(names), 600)
        self.assertEqual(names, sorted(set(names), key=lambda s: s.encode()))

    def test_the_same_functions(self):
        t = dict(table())
        for n, same in (('glGenFramebuffersEXT', 'glGenFramebuffers'), ('glCheckFramebufferStatusEXT', 'glCheckFramebufferStatus'),   # Fate/hollow ataraxia REMASTERED
                        ('glBindRenderbufferEXT', 'glBindRenderbuffer'), ('glBindFramebufferEXT', 'glBindFramebuffer'),
                        ('glBindBufferARB', 'glBindBuffer'), ('glActiveTextureARB', 'glActiveTexture'), ('glVertexAttribPointerARB', 'glVertexAttribPointer'),   # DOOM 3
                        ('glBindVertexArrayOES', 'glBindVertexArray'), ('glGenFramebuffersOES', 'glGenFramebuffers')):
            self.assertEqual(t.get(n), same, n)

    def test_not_by_suffix(self):   # other arguments or another meaning under the stripped name
        t = dict(table())
        for n in ('glVertexAttribPointerNV', 'glGetProgramivARB', 'glPolygonOffsetEXT', 'glVertexAttribFormatNV', 'glBindVertexArrayAPPLE',
                  'glRenderbufferStorageMultisampleIMG', 'glTextureStorage2DEXT'):
            self.assertNotIn(n, t)

    def test_reviewed_es_list_is_in(self):
        t = dict(table())
        for n, same in gl_aliases.ES_SAME.items(): self.assertEqual(t.get(n), same, n)


if __name__ == '__main__':
    unittest.main()
