"""The 1.1.0 Camp left gradient must use the tall height, not move or scale."""
import struct
import unittest
from unittest.mock import patch

from test_star_ocean_map import BL, Fixture, PATCHES, START


class CampGradient(unittest.TestCase):
    def test_both_camp_tints_follow_only_the_tall_height(self):
        ctx = Fixture()
        with patch.object(PATCHES, 'camp_backdrop'), patch.object(PATCHES, 'title_backdrops'), patch.object(PATCHES, 'title_menu_chrome'):
            PATCHES.edge_layout(ctx)
        hook, base = 0x2156514, 0x23023b0
        self.assertEqual(ctx.checks[hook], struct.pack('<I', BL(hook, base)).hex())
        stub = hook + (ctx.patches[hook][0] & 0x3ffffff) * 4
        self.assertEqual(ctx.word(stub + 8), PATCHES.cbz(17, stub + 8, stub + 18 * 4))
        for index, name in enumerate(('ui_camp_menu_background_01', 'ui_camp_menu_background_02')):
            call = stub + (7 + index * 2) * 4
            rel = ctx.word(call) & 0x3ffffff
            helper = call + (rel - (1 << 26)) * 4
            self.assertEqual(ctx.word(call), BL(call, helper))
            prefix = b''.join(struct.pack('<I', w) for w in ctx.output[:(helper - START) // 4])
            path = ('BackgroundParent/' + name).encode('utf-16le')
            self.assertTrue(prefix.rstrip(b'\0').endswith(path.rstrip(b'\0')))
            addresses = range(helper, helper + 31 * 4, 4)
            self.assertTrue(any(ctx.word(a) == 0x1e230221 for a in addresses))  # sizeDelta.y
            self.assertTrue(any(ctx.word(a) == BL(a, 0x25067b0) for a in addresses))  # set_sizeDelta
