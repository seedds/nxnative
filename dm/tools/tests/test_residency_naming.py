"""Residency by command buffer (nvn_mtl.m res_name, PR #2715): the NVN layer attaches no residency set to a queue, and every command buffer it commits names the host's sets itself.
A command buffer that ended or was made outside the two helpers would read memory outside its residency, a GPU fault, so this checks the sources: no addResidencySet, endCommandBuffer
only in cb_end (and the recorder's stand-in, which records it for cb_end on the encode thread), commandBuffer on g_q only in cl_cb. dm/gfx/tests/t_res_cb.m checks the paths at run
time. No compiler, GPU or game files needed."""
from pathlib import Path
import re
import unittest

REPO = Path(__file__).resolve().parents[3]
NVN = REPO / 'dm/gfx/nvn'


def code(p):   # the file without its // and /* */ comments, line numbers kept
    text = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), p.read_text(errors='replace'), flags=re.S)
    return [re.sub(r'//.*', '', line) for line in text.splitlines()]


def hits(pattern):   # (file, line number, line) for every code line of the NVN layer that matches
    out = []
    for p in sorted(NVN.iterdir()):
        if p.suffix in ('.m', '.h', '.inc', '.c'):
            out += [(p.name, n, line.strip()) for n, line in enumerate(code(p), 1) if re.search(pattern, line)]
    return out


class ResidencyNaming(unittest.TestCase):
    def test_no_set_on_a_queue(self):
        self.assertEqual(hits(r'\baddResidencySet\s*:'), [])

    def test_command_buffers_end_in_cb_end(self):
        found = hits(r'\bendCommandBuffer\b')
        allowed = [h for h in found if h[0] == 'nvn_mtl.m' and h[2].startswith('[(id<MTL4CommandBuffer>)cb endCommandBuffer]')
                   or h[0] == 'nvn_rec.h' and h[2].startswith('- (void)endCommandBuffer')]
        self.assertEqual(len(allowed), 2, found)   # the helper's own end and the recorder's stand-in
        self.assertEqual([h for h in found if h not in allowed], [])

    def test_classic_command_buffers_come_from_cl_cb(self):
        found = hits(r'\[\s*g_q\s+commandBuffer\s*\]')
        self.assertEqual([h[0] for h in found], ['nvn_mtl.m'], found)
        self.assertIn('static id<MTLCommandBuffer> cl_cb(void)', found[0][2])

    def test_the_scan_reaches_the_layer(self):   # an empty scan would pass the checks above
        self.assertTrue(hits(r'\bres_name\s*\('))
        self.assertTrue(hits(r'\bcb_end\s*\(g_cb\b'))


if __name__ == '__main__':
    unittest.main()
