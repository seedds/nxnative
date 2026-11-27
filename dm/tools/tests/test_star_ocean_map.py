"""Check the Star Ocean 1.1.0 Current Location guest UI rewrite, without game data."""
import importlib.util
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from nso_asm import B, BL  # noqa: E402

SPEC = importlib.util.spec_from_file_location(
    'star_ocean_patches', TOOLS.parent / 'games/010065301A2E0000/patches.py')
assert SPEC is not None and SPEC.loader is not None
PATCHES = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCHES)

HOOK, ORIGINAL, START = 0x22c2a70, 0xa9bc7bfd, 0x5da0000

class Fixture:
    state = 0x5cb0000

    def __init__(self):
        self.output = []
        self.checks = {}
        self.patches = {}
        self.capabilities = set()

    def site(self, address, expected):
        assert address not in self.checks, f'duplicate patch site {address:#x}'
        self.checks[address] = expected

    def here(self):
        return START + 4 * len(self.output)

    def emit(self, word):
        self.output.append(word)

    def put(self, address, words, _reason):
        assert address not in self.patches, f'duplicate patch write {address:#x}'
        self.patches[address] = words

    def word(self, address):
        return self.output[(address - START) // 4]


class ScreenShape(unittest.TestCase):
    def test_exact_build_exports_only_the_standard_picture_shape(self):
        ctx = Fixture()
        self.assertEqual(set(PATCHES.FRAME_STEP), {'d214ba64d902aeda4197b57debabd01b'})
        next(iter(PATCHES.FRAME_STEP.values()))(ctx)
        self.assertEqual(ctx.capabilities, {'SCREEN_ASPECT'})
        self.assertEqual(set(ctx.checks), set(ctx.patches))
        self.assertEqual(len(ctx.patches), 74)
        self.assertTrue({0x2169130, 0x77b79c, 0x77ba28, 0x766818,
                         0x1981e10, 0x1981e00, 0x2028948} <= ctx.patches.keys())
        # No optional Status asset mesh or silhouette capture sites.
        self.assertTrue({0x214d49c, 0x20a59bc, 0x20b59f4}.isdisjoint(ctx.patches))

    def test_height_hook_uses_stock_immediate_when_shape_is_off(self):
        ctx = Fixture()
        PATCHES.screen_shape(ctx)
        hook, stock = 0x2169130, 0x52800000 | (1080 << 5) | 1
        jump = ctx.patches[hook][0]
        stub = hook + (jump & 0x3ffffff) * 4
        self.assertEqual(ctx.checks[hook], struct.pack('<I', stock).hex())
        self.assertEqual(ctx.word(stub + 4), 0xb9404a10)  # output height
        self.assertEqual(ctx.word(stub + 8), PATCHES.cbz(16, stub + 8, stub + 20))
        self.assertEqual(ctx.word(stub + 20), stock)
        self.assertEqual(ctx.word(stub + 24), B(stub + 24, hook + 4))


class CurrentLocationMap(unittest.TestCase):
    def test_six_decoration_helpers_and_stock_prologue(self):
        ctx = Fixture()
        with patch.object(PATCHES, 'camp_backdrop'), patch.object(PATCHES, 'title_backdrops'):
            PATCHES.edge_layout(ctx)
        self.assertEqual(ctx.checks[HOOK], struct.pack('<I', ORIGINAL).hex())
        (inst,) = ctx.patches[HOOK]
        stub = HOOK + (inst & 0x3ffffff) * 4
        self.assertEqual(inst, B(HOOK, stub))
        self.assertEqual(ctx.word(stub + 4), 0xb9404a11)  # render height
        stock = stub + 20 * 4
        self.assertEqual(ctx.word(stub + 8), PATCHES.cbz(17, stub + 8, stock))
        self.assertEqual(ctx.word(stock), ORIGINAL)
        self.assertEqual(ctx.word(stock + 4), B(stock + 4, HOOK + 4))
        for i, (path, adjust, method) in enumerate((
            ('Background', 0x1e230221, 0x25067b0),  # set sizeDelta.y to render height
            ('HeaderImage', 0x4b080228, 0x2506610),  # move up half added height
            ('FooterImage', 0x4b110108, 0x2506610),  # move down half added height
            ('ui_world_map_icon_guide_presenter', 0x4b110108, 0x2506610),
        )):
            call = stub + (7 + 2 * i) * 4
            rel = ctx.word(call) & 0x3ffffff
            dest = call + (rel - (1 << 26) if rel & (1 << 25) else rel) * 4
            self.assertEqual(ctx.word(call), BL(call, dest))
            # The literal precedes its helper. The helper should use only the
            # specified geometry setter, never scale or modify map content.
            prefix = b''.join(struct.pack('<I', w) for w in ctx.output[:(dest - START)//4])
            self.assertTrue(prefix.rstrip(b'\0').endswith(path.encode('utf-16le').rstrip(b'\0')))
            addresses = range(dest, dest + 34 * 4, 4)
            self.assertTrue(any(ctx.word(a) == adjust for a in addresses))
            self.assertTrue(any(ctx.word(a) == BL(a, method) for a in addresses))
        # The same-named list tint is child 2, not the full-width
        # Background found by name. Only its own child 0 is the gradient.
        for index, children in enumerate(((2,), (2, 0)), 4):
            call = stub + (7 + 2 * index) * 4
            branch = ctx.word(call)
            rel = branch & 0x3ffffff
            helper = call + (rel - (1 << 26) if rel & (1 << 25) else rel) * 4
            self.assertEqual(branch, BL(call, helper))
            code = range(helper, helper + (31 + 11 * len(children)) * 4, 4)
            count_calls = [a for a in code if ctx.word(a) == BL(a, 0x2509a00)]
            self.assertEqual(len(count_calls), len(children))
            for a, child in zip(count_calls, children):
                self.assertEqual(ctx.word(a + 4), 0x7100001f | ((child + 1) << 10))
                self.assertEqual(ctx.word(a + 8) & 0xff00001f, 0x54000003)
                self.assertEqual(ctx.word(a + 16), 0x52800001 | (child << 5))
                self.assertEqual(ctx.word(a + 24), BL(a + 24, 0x2509f20))
            self.assertIn(0x52a890e8, [ctx.word(a) for a in code])  # stock-height guard
            self.assertTrue(any(ctx.word(a) == BL(a, 0x25067b0) for a in code))
            self.assertFalse(any(ctx.word(a) == BL(a, 0x2506610) for a in code))
        for render_height, shift in ((1080, 0), (1200, 60), (1344, 132)):
            self.assertEqual((render_height - 1080) // 2, shift)
            self.assertEqual(1080 + 2 * shift, render_height)


class TitleOwnedChrome(unittest.TestCase):
    def test_title_chrome_and_new_game_portrait_anchors_ride_the_awake_hook(self):
        ctx = Fixture()
        with patch.object(PATCHES, 'camp_backdrop'), patch.object(PATCHES, 'title_backdrops'):
            PATCHES.edge_layout(ctx)
        at = 0x265bb64
        self.assertEqual(ctx.checks[at], struct.pack('<I', BL(at, 0x23023b0)).hex())
        (branch,) = ctx.patches[at]
        stub = at + (branch & 0x3ffffff) * 4
        raw = b''.join(struct.pack('<I', w) for w in ctx.output[:(stub - START) // 4])
        for selector, presenter in (
            ('original_staff', 'original_staff'), ('license', 'license'),
            ('standing_picture', 'standing_picture'), ('voice_gallery', 'voice_gallery'),
            ('new_game', 'new_game'),
        ):
            root = f'ui_title_{selector}_selector/ui_title_{presenter}_presenter/'
            for decoration in (
                'ui_camp_menu_background_presenter/ui_camp_menu_background_01',
                'ui_camp_menu_background_presenter/ui_camp_menu_background_02',
                'ui_camp_header_image_presenter', 'FooterImage',
            ):
                self.assertIn((root + decoration).encode('utf-16le'), raw)
        self.assertNotIn('scroll_view'.encode('utf-16le'), raw)
        # The stock path must be an untouched tail call to WindowComponent.Awake.
        inst = ctx.word(stub + 8)
        stock = stub + 8 + ((inst >> 5) & 0x7ffff) * 4
        self.assertEqual(inst, PATCHES.cbz(17, stub + 8, stock))
        self.assertEqual(ctx.word(stock), B(stock, 0x23023b0))
        for i, hero in enumerate(('Claude', 'Rena')):
            with self.subTest(hero=hero):
                call = stock - 20 + i * 8
                rel = ctx.word(call) & 0x3ffffff
                helper = call + (rel - (1 << 26) if rel & (1 << 25) else rel) * 4
                self.assertEqual(ctx.word(call), BL(call, helper))
                path = ('ui_title_new_game_selector/ui_title_new_game_presenter/'
                        f'ui_title_select_hero_presenter/StandingPicture{hero}').encode('utf-16le')
                literal = helper - (len(path) + 3) // 4 * 4
                self.assertEqual(raw[literal - START:literal - START + len(path)], path)
                code = range(helper, helper + 29 * 4, 4)
                self.assertIn(0x4b110108, [ctx.word(a) for a in code])
                self.assertTrue(any(ctx.word(a) == BL(a, 0x2506610) for a in code))
                self.assertFalse(any(ctx.word(a) == BL(a, 0x25067b0) for a in code))


class TitleMenuChrome(unittest.TestCase):
    def test_title_only_open_and_stock_passthrough_for_both_windows(self):
        ctx = Fixture()
        calls = []

        def helper(path, mode):
            calls.append((path, mode))
            address = ctx.here()
            ctx.emit(0xd65f03c0)
            return address

        PATCHES.title_menu_chrome(ctx, helper)
        chrome = 'ui_common_background_parent_presenter/HeaderFooterParent'
        header = chrome + '/ui_camp_header_image_presenter'
        self.assertEqual(calls, [
            (chrome, 'stretch_height'), (header, 'height'),
            (chrome, 'restore_stretch_height'), (header, 'restore_title_header'),
        ])

        for addr, original, branch, count, title_only in (
            (0x24a1040, 0x2301260, BL, 14, True),
            (0x228ca94, 0x23013e0, BL, 14, True),
            (0x24a11a0, 0x2301560, BL, 12, False),
            (0x228cb90, 0x2301660, B, 12, False),
        ):
            self.assertEqual(ctx.checks[addr], struct.pack('<I', branch(addr, original)).hex())
            (inst,) = ctx.patches[addr]
            entry = addr + (inst & 0x3ffffff) * 4
            tail = entry + count * 4
            self.assertEqual(inst, branch(addr, entry))
            self.assertEqual(ctx.word(entry + 4), 0xb9404a11)
            self.assertEqual(ctx.word(entry + 8), PATCHES.cbz(17, entry + 8, tail))
            if title_only:
                self.assertEqual(ctx.word(entry + 12), PATCHES.LDRX(17, 16, 0xb8 // 8))
                self.assertEqual(ctx.word(entry + 16), PATCHES.cbz(17, entry + 16, tail, wide=True))
            self.assertEqual(ctx.word(tail), B(tail, original))

    def test_stretched_group_uses_extra_height_and_restores_zero_delta(self):
        ctx = Fixture()
        with patch.object(PATCHES, 'camp_backdrop'), patch.object(PATCHES, 'title_backdrops'):
            PATCHES.edge_layout(ctx)
        marker = PATCHES.SUBI(17, 17, 1080) ^ (1 << 31)
        self.assertEqual(marker, 0x5110e231)  # sub w17, w17, #1080
        self.assertEqual(ctx.output.count(marker), 2)
        at = ctx.output.index(marker)
        self.assertEqual(ctx.output[at-1], 0xb9404a11)  # render height
        self.assertEqual(ctx.output[at+1], 0x1e230221)  # convert extra rows to sizeDelta.y
        closing = ctx.output.index(marker, at + 1)
        self.assertEqual(ctx.output[closing+1:closing+3], [0x1e230222, 0x1e222020])
        self.assertEqual(ctx.output[closing+3] & 0xff00001f, 0x54000001)  # b.ne bypass restore
