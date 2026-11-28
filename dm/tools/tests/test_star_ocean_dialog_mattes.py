"""Check the build-gated Star Ocean modal backdrop hook without game data."""
import struct
import unittest
from unittest.mock import patch

from test_star_ocean_map import BL, B, Fixture, PATCHES, START


class DialogMattes(unittest.TestCase):
    def test_only_fullscreen_mattes_expand_and_stock_bypasses_helpers(self):
        ctx = Fixture()
        with patch.object(PATCHES, 'camp_backdrop'), patch.object(PATCHES, 'title_backdrops'):
            PATCHES.edge_layout(ctx)
        hook, awake = 0x26042fc, 0x23023b0
        self.assertEqual(ctx.checks[hook], struct.pack('<I', BL(hook, awake)).hex())
        (inst,) = ctx.patches[hook]
        stub = hook + (inst & 0x3ffffff) * 4
        self.assertEqual(inst, BL(hook, stub))
        self.assertEqual(ctx.word(stub + 4), 0xb9404a11)
        stock = stub + 20 * 4
        self.assertEqual(ctx.word(stub + 8), PATCHES.cbz(17, stub + 8, stock))
        self.assertEqual(ctx.word(stub + 20), BL(stub + 20, awake))
        self.assertEqual(ctx.word(stock), B(stock, awake))
        for i, presenter in enumerate((
            'UIDialogPresenter',
            'ui_auto_save_announce_dialog_presenter',
            'ui_dialog_description_presenter',
            'ui_take_over_dialog_presenter',
            'ui_progress_bar_dialog_presenter',
            'ui_demo_dialog_presenter',
        )):
            call = stub + 28 + i * 8
            rel = ctx.word(call) & 0x3ffffff
            helper = call + (rel - (1 << 26) if rel & (1 << 25) else rel) * 4
            self.assertEqual(ctx.word(call), BL(call, helper))
            path = (presenter + '/ui_dialog_background_presenter/BackgroundBlack01').encode('utf-16le')
            begin = helper - (len(path) + 3) // 4 * 4
            data = b''.join(struct.pack('<I', ctx.word(addr)) for addr in range(begin, helper, 4))
            self.assertEqual(data[:len(path)], path)
            self.assertGreaterEqual(begin, START)
            # Set height only; never alter the card and controls' positions.
            code = range(helper, helper + 29 * 4, 4)
            self.assertIn(0x52a890e8, [ctx.word(a) for a in code])  # authored 1080.0f check
            self.assertIn(0x1e230221, [ctx.word(a) for a in code])  # only s1 (height) changes
            self.assertTrue(any(ctx.word(a) == BL(a, 0x25067b0) for a in code))
            self.assertFalse(any(ctx.word(a) == BL(a, 0x2506610) for a in code))

    def test_title_new_game_extends_its_blackout_backing(self):
        ctx = Fixture()
        with patch.object(PATCHES, 'camp_backdrop'), patch.object(PATCHES, 'title_backdrops'):
            PATCHES.edge_layout(ctx)
        hook, awake = 0x265bb64, 0x23023b0
        self.assertEqual(ctx.checks[hook], struct.pack('<I', BL(hook, awake)).hex())
        (inst,) = ctx.patches[hook]
        stub = hook + (inst & 0x3ffffff) * 4
        inst = ctx.word(stub + 8)
        stock = stub + 8 + ((inst >> 5) & 0x7ffff) * 4
        self.assertEqual(ctx.word(stub + 8), PATCHES.cbz(17, stub + 8, stock))
        self.assertEqual(ctx.word(stub + 20), BL(stub + 20, awake))
        self.assertEqual(ctx.word(stock), B(stock, awake))
        call = stub + 28
        rel = ctx.word(call) & 0x3ffffff
        helper = call + (rel - (1 << 26) if rel & (1 << 25) else rel) * 4
        literal = 'BlackOutBackground'.encode('utf-16le')
        start = helper - (len(literal) + 3) // 4 * 4
        encoded = b''.join(struct.pack('<I', ctx.word(a)) for a in range(start, helper, 4))
        self.assertEqual(encoded[:len(literal)], literal)
        code = range(helper, helper + 29 * 4, 4)
        self.assertIn(0x1e230221, [ctx.word(a) for a in code])
        self.assertTrue(any(ctx.word(a) == BL(a, 0x25067b0) for a in code))
        self.assertFalse(any(ctx.word(a) == BL(a, 0x2506610) for a in code))

    def test_game_menu_awake_hooks_adjust_only_decor(self):
        ctx = Fixture()
        with patch.object(PATCHES, 'camp_backdrop'), patch.object(PATCHES, 'title_backdrops'):
            PATCHES.edge_layout(ctx)
        for hook, awake, paths in (
            (0x238aba4, 0x26be590, (
                ('ui_event_operation_presenter', 0x4b110108, 0x2506610),
            )),
            (0x276d64c, 0x257dec0, (
                ('ui_select_choice_presenter/Background', 0x1e230221, 0x25067b0),
            )),
            (0x276c560, 0x257dec0, (
                ('ui_event_information_presenter/Background', 0x1e230221, 0x25067b0),
            )),
            (0x2333074, 0x23023b0, (
                ('ui_background_presenter/Background', 0x1e230221, 0x25067b0),
            )),
            (0x232e27c, 0x23023b0, (
                ('ui_bunny_race_bet_selector/ui_layout_presenter/Background',
                 0x1e230221, 0x25067b0),
                ('ui_bunny_race_bet_selector/ui_layout_presenter/Background/Background (1)',
                 0x1e230221, 0x25067b0),
                ('ui_bunny_race_bet_selector/ui_layout_presenter/HeaderBackground',
                 0x4b080228, 0x2506610),
                ('ui_bunny_race_bet_selector/FooterImage', 0x4b110108, 0x2506610),
            )),
            (0x233ac94, 0x257db40, (
                ('CaptureBackground/BackgroundParent/ui_camp_menu_background_01',
                 0x1e230221, 0x25067b0),
                ('CaptureBackground/BackgroundParent/ui_camp_menu_background_02',
                 0x1e230221, 0x25067b0),
                ('HeaderBackground', 0x4b080228, 0x2506610),
                ('FooterImage', 0x4b110108, 0x2506610),
            )),
            (0x258ad8c, 0x23023b0, (
                ('ui_shop_layout_presenter/Background', 0x1e230221, 0x25067b0),
                ('ui_shop_layout_presenter/Background/Background (1)', 0x1e230221, 0x25067b0),
                ('ui_shop_layout_presenter/HeaderBackground', 0x4b080228, 0x2506610),
                ('FooterImage', 0x4b110108, 0x2506610),
            )),
            (0x275daf4, 0x2580c30, (
                ('ui_conversation_log_presenter/Background', 0x1e230221, 0x25067b0),
                ('ui_conversation_log_presenter/HeaderBackground', 0x4b080228, 0x2506610),
                ('ui_conversation_log_presenter/FooterBackground', 0x4b110108, 0x2506610),
            )),
            (0x24ec208, 0x23023b0, (
                ('ui_quest_selector/Background', 0x1e230221, 0x25067b0),
                ('ui_quest_selector/Background/Background (1)', 0x1e230221, 0x25067b0),
                ('ui_quest_selector/HeaderBackground', 0x4b080228, 0x2506610),
                ('ui_quest_selector/FooterImage', 0x4b110108, 0x2506610),
            )),
            (0x1f56038, 0x23023b0, (
                ('Background', 0x1e230221, 0x25067b0),
                ('Background/Background (1)', 0x1e230221, 0x25067b0),
                ('HeaderBackground', 0x4b080228, 0x2506610),
                ('FooterImage', 0x4b110108, 0x2506610),
            )),
        ):
            with self.subTest(hook=hex(hook)):
                self.assertEqual(ctx.checks[hook], struct.pack('<I', BL(hook, awake)).hex())
                (inst,) = ctx.patches[hook]
                stub = hook + (inst & 0x3ffffff) * 4
                stock = stub + (8 + 2 * len(paths)) * 4
                self.assertEqual(inst, BL(hook, stub))
                self.assertEqual(ctx.word(stub + 8), PATCHES.cbz(17, stub + 8, stock))
                self.assertEqual(ctx.word(stub + 20), BL(stub + 20, awake))
                self.assertEqual(ctx.word(stock), B(stock, awake))
                for i, (path, operation, setter) in enumerate(paths):
                    call = stub + 28 + i * 8
                    rel = ctx.word(call) & 0x3ffffff
                    helper = call + (rel - (1 << 26) if rel & (1 << 25) else rel) * 4
                    literal = path.encode('utf-16le')
                    start = helper - (len(literal) + 3) // 4 * 4
                    encoded = b''.join(struct.pack('<I', ctx.word(a)) for a in range(start, helper, 4))
                    self.assertEqual(encoded[:len(literal)], literal)
                    code = range(helper, helper + 29 * 4, 4)
                    self.assertIn(operation, [ctx.word(a) for a in code])
                    self.assertTrue(any(ctx.word(a) == BL(a, setter) for a in code))

    def test_each_blur_uses_its_own_tall_target_and_releases_it(self):
        ctx = Fixture()
        ctx.capabilities = set()
        resize, restore = START - 0x100, START - 0x200
        PATCHES.camp_backdrop(ctx, resize, restore)

        def target(at, inst):
            delta = inst & 0x3ffffff
            return at + (delta - (1 << 26) if delta & (1 << 25) else delta) * 4

        slots = ((0x21576b8, 0xb0), (0x258b27c, 0xc8),
                 (0x24ec454, 0xd0), (0x1f562f0, 0xd8), (0x24f30cc, 0xe0), (0x2328ce0, 0xe8), (0x233b020, 0xf0))
        for hook, slot in slots:
            with self.subTest(hook=hex(hook)):
                self.assertEqual(ctx.checks[hook], struct.pack('<I', BL(hook, 0x2596450)).hex())
                stub = target(hook, ctx.patches[hook][0])
                self.assertEqual(ctx.word(stub + 8) & 0xff00001f, 0x34000011)
                stock = stub + 8 + ((ctx.word(stub + 8) >> 5) & 0x7ffff) * 4
                self.assertEqual(ctx.word(stock), B(stock, 0x2596450))
                body = range(stub, stock, 4)
                self.assertIn(PATCHES.LDRX(19, 16, slot // 8), [ctx.word(a) for a in body])
                self.assertIn(PATCHES.STRX(19, 16, slot // 8), [ctx.word(a) for a in body])
                self.assertTrue(any(ctx.word(a) == BL(a, resize) for a in body))

        render = target(0x22c48b8, ctx.patches[0x22c48b8][0])
        full = render + (4 + 3 * len(slots)) * 4
        stock = full + 24
        for i, (_, slot) in enumerate(slots):
            at = render + (3 + 3 * i) * 4
            self.assertEqual(ctx.word(at), PATCHES.LDRX(15, 16, slot // 8))
            self.assertEqual(ctx.word(at + 4), PATCHES.CMPR(17, 15))
            self.assertEqual(ctx.word(at + 8), 0x54000000 | ((full - at - 8) // 4) << 5)
        self.assertEqual(ctx.word(full - 4), B(full - 4, stock))
        self.assertEqual(ctx.word(full + 20), B(full + 20, 0x22c4944))
        self.assertEqual(ctx.word(stock), 0x1e2202a0)

        for close, slot, field in ((0x258b81c, 0xc8, 0xc0), (0x24ec6e0, 0xd0, 0xc8),
                                   (0x1f56634, 0xd8, 0xc0)):
            with self.subTest(close=hex(close)):
                self.assertEqual(ctx.checks[close], struct.pack('<I', B(close, 0x2301560)).hex())
                stub = target(close, ctx.patches[close][0])
                body = range(stub, stub + 20 * 4, 4)
                words = [ctx.word(a) for a in body]
                self.assertIn(PATCHES.STRX(31, 16, slot // 8), words)
                self.assertEqual(words.count(PATCHES.LDRX(0, 0, field // 8)), 2)
                self.assertTrue(any(ctx.word(a) == BL(a, restore) for a in body))
                self.assertTrue(any(ctx.word(a) == BL(a, 0x2502190) for a in body))
                self.assertEqual(ctx.word(stub + 80), B(stub + 80, 0x2301560))

    def test_fin_reflects_only_the_star_image_without_altering_its_quad(self):
        def target(at, instruction):
            imm = instruction & 0x3ffffff
            return at + (imm - (1 << 26) if imm & (1 << 25) else imm) * 4

        ctx = Fixture()
        PATCHES.title_backdrops(ctx, lambda *args: START - 0x100)
        load, setter, close = 0x1f4dcc8, 0x17290a0, 0x1f4e1b0
        mesh, simple = 0x17367e8, 0x17367f0
        slot = 0xf8 // 8
        self.assertEqual(ctx.checks[load], struct.pack('<I', B(load, setter)).hex())
        self.assertEqual(ctx.checks[close], 'fd7bbea9')
        self.assertEqual(ctx.checks[mesh], struct.pack('<I', B(mesh, simple)).hex())
        capture = target(load, ctx.patches[load][0])
        self.assertEqual(ctx.word(capture + 8), PATCHES.cbz(17, capture + 8, capture + 16))
        self.assertEqual(ctx.word(capture + 12), PATCHES.STRX(0, 16, slot))
        self.assertEqual(ctx.word(capture + 16), B(capture + 16, setter))
        cleanup = target(close, ctx.patches[close][0])
        self.assertEqual(ctx.word(cleanup + 4), PATCHES.STRX(31, 16, slot))
        self.assertEqual(ctx.word(cleanup + 12), B(cleanup + 12, close + 4))
        dispatch = target(mesh, ctx.patches[mesh][0])
        branch = ctx.word(dispatch + 8)
        stock = dispatch + 8 + ((branch >> 5) & 0x7ffff) * 4
        self.assertEqual(branch, PATCHES.cbz(17, dispatch + 8, stock))
        self.assertEqual(ctx.word(stock), B(stock, simple))
        self.assertEqual(ctx.word(dispatch + 12), PATCHES.LDRX(17, 16, slot))
        self.assertEqual(ctx.word(dispatch + 16), PATCHES.CMPR(17, 0))
        self.assertEqual(ctx.word(dispatch + 20), 0x54000041)  # unmatched goes to Title
        fin = target(dispatch + 24, ctx.word(dispatch + 24))
        self.assertEqual(ctx.word(fin + 12), BL(fin + 12, simple))
        body = range(fin, dispatch, 4)
        self.assertIn(0x1e201821, [ctx.word(a) for a in body])  # actual corner UV/height slope
        self.assertIn(0x1e2c1002, [ctx.word(a) for a in body])  # half the extra height
        self.assertEqual(sum(ctx.word(a) == BL(a, 0x16d68b0) for a in body), 4)
        tris = [tuple((ctx.word(a + off) >> 5) & 0xffff for off in (-16, -12, -8))
                for a in body if ctx.word(a) == BL(a, 0x16d6950)]
        self.assertEqual(tris, [(1, 4, 5), (5, 2, 1), (6, 0, 3), (3, 7, 6)])

    def test_clear_save_uses_chrome_without_title_or_field_capture(self):
        ctx = Fixture()
        PATCHES.title_backdrops(ctx, lambda *args: START - 0x100)
        hook, base_open, chrome = 0x24f3024, 0x23013e0, 56
        self.assertEqual(ctx.checks[hook], struct.pack('<I', BL(hook, base_open)).hex())
        stub = hook + (ctx.patches[hook][0] & 0x3ffffff) * 4
        self.assertEqual(ctx.word(stub + 4), 0xb9404a11)  # zero height on stock
        self.assertEqual(ctx.word(stub + 12), 0x39432811)  # IsCamp
        self.assertEqual(ctx.word(stub + 20), PATCHES.LDRX(17, 0, 0x98 // 8))
        self.assertEqual(ctx.word(stub + 24), PATCHES.cbz(17, stub + 24, stub + 40, wide=True))
        self.assertEqual(ctx.word(stub + 28), 0xb940d231)  # selector SaveLoadState
        self.assertEqual(ctx.word(stub + 32), 0x71000a3f)  # exactly Clear Save (2)
        self.assertEqual(ctx.word(stub + 36), 0x54000000 | ((chrome - 36) // 4) << 5)
        self.assertEqual(ctx.word(stub + 40), 0x39432011)  # IsCaptureBackground
        self.assertEqual(ctx.word(stub + 44),
                         PATCHES.cbz(17, stub + 44, stub + chrome) ^ (1 << 24))
        self.assertEqual(ctx.word(stub + 48), PATCHES.LDRX(17, 16, 0xb8 // 8))  # Title
        self.assertEqual(ctx.word(stub + chrome), PATCHES.STRX(0, 16, 0xc0 // 8))
        stock = stub + 112
        self.assertEqual(ctx.word(stub + 8), PATCHES.cbz(17, stub + 8, stock))
        self.assertEqual(ctx.word(stub + 16), PATCHES.cbz(17, stub + 16, stock) ^ (1 << 24))
        self.assertEqual(ctx.word(stub + 52), PATCHES.cbz(17, stub + 52, stock, wide=True))
        self.assertEqual(ctx.word(stock), B(stock, base_open))

    def test_field_save_capture_stock_camp_and_cleanup(self):
        ctx = Fixture()
        ctx.capabilities = set()
        resize, restore = START - 0x100, START - 0x200
        PATCHES.camp_backdrop(ctx, resize, restore)
        hook, close = 0x24f30cc, 0x24f3348
        stub = hook + (ctx.patches[hook][0] & 0x3ffffff) * 4
        self.assertEqual(ctx.word(stub + 12), 0x39432a71)  # IsCamp
        stock = stub + 16 + ((ctx.word(stub + 16) >> 5) & 0x7ffff) * 4
        self.assertEqual(ctx.word(stub + 16), PATCHES.cbz(17, stub + 16, stock) ^ (1 << 24))
        self.assertEqual(ctx.word(stock), B(stock, 0x2596450))
        original = PATCHES.LDRX(8, 20)
        self.assertEqual(ctx.checks[close], struct.pack('<I', original).hex())
        stub = close + (ctx.patches[close][0] & 0x3ffffff) * 4
        self.assertEqual(ctx.word(stub + 4), PATCHES.LDRX(17, 16, 0xe0 // 8))
        self.assertEqual(ctx.word(stub + 8), PATCHES.cbz(17, stub + 8, stub + 64, wide=True))
        self.assertEqual(ctx.word(stub + 20), PATCHES.STRX(31, 16, 0xe0 // 8))
        self.assertEqual(ctx.word(stub + 24), PATCHES.LDRX(0, 19, 0xa0 // 8))
        self.assertEqual(ctx.word(stub + 44), BL(stub + 44, restore))
        self.assertEqual(ctx.word(stub + 64), original)
        self.assertEqual(ctx.word(stub + 68), B(stub + 68, close + 4))

    def test_minigame_selectors_release_capture_on_both_hide_paths(self):
        ctx = Fixture()
        ctx.capabilities = set()
        resize, restore = START - 0x100, START - 0x200
        PATCHES.camp_backdrop(ctx, resize, restore)
        for close, original, slot, raw in (
            (0x2329ea0, PATCHES.LDRX(0, 19, 0xf0 // 8), 0xe8, 0x128),
            (0x232a06c, PATCHES.LDRX(0, 19, 0xf0 // 8), 0xe8, 0x128),
            (0x233b58c, PATCHES.LDRX(8, 19), 0xf0, 0xc0),
            (0x233b6d4, PATCHES.LDRX(8, 19), 0xf0, 0xc0),
        ):
            self.assertEqual(ctx.checks[close], struct.pack('<I', original).hex())
            stub = close + (ctx.patches[close][0] & 0x3ffffff) * 4
            self.assertEqual(ctx.word(stub + 4), PATCHES.LDRX(17, 16, slot // 8))
            self.assertEqual(ctx.word(stub + 8), PATCHES.cbz(17, stub + 8, stub + 64, wide=True))
            self.assertEqual(ctx.word(stub + 20), PATCHES.STRX(31, 16, slot // 8))
            self.assertEqual(ctx.word(stub + 24), PATCHES.LDRX(0, 19, raw // 8))
            self.assertEqual(ctx.word(stub + 44), BL(stub + 44, restore))
            self.assertEqual(ctx.word(stub + 64), original)
            self.assertEqual(ctx.word(stub + 68), B(stub + 68, close + 4))

    def test_battle_controller_keeps_pause_gradients_and_chain_banner_scoped(self):
        ctx = Fixture()
        with patch.object(PATCHES, 'camp_backdrop'), patch.object(PATCHES, 'title_backdrops'):
            PATCHES.edge_layout(ctx)
        hook, awake = 0x23caf44, 0x275c420
        self.assertEqual(ctx.checks[hook], struct.pack('<I', BL(hook, awake)).hex())
        (inst,) = ctx.patches[hook]
        stub = hook + (inst & 0x3ffffff) * 4
        self.assertEqual(inst, BL(hook, stub))
        stock = stub + 72
        self.assertEqual(ctx.word(stub + 8), PATCHES.cbz(17, stub + 8, stock))
        self.assertEqual(ctx.word(stub + 20), BL(stub + 20, awake))
        self.assertEqual(ctx.word(stock - 4), 0xd65f03c0)
        self.assertEqual(ctx.word(stock), B(stock, awake))
        for i, (path, setter, other) in enumerate((
            ('ui_battle_pause_background_presenter/Background (1)', 0x25067b0, 0x2506610),
            ('ui_battle_pause_background_presenter/Background', 0x2506610, 0x25067b0),
            ('ui_battle_controller_layout_presenter/ui_chain_battle_presenter', 0x2506610, 0x25067b0),
        )):
            call = stub + 44 + 8 * i
            rel = ctx.word(call) & 0x3ffffff
            helper = call + (rel - (1 << 26) if rel & (1 << 25) else rel) * 4
            self.assertEqual(ctx.word(call), BL(call, helper))
            literal = path.encode('utf-16le')
            begin = helper - (len(literal) + 3) // 4 * 4
            raw = b''.join(struct.pack('<I', ctx.word(addr)) for addr in range(begin, helper, 4))
            self.assertEqual(raw[:len(literal)], literal)
            words = [ctx.word(addr) for addr in range(helper, helper + 30 * 4, 4)]
            if path.endswith('ui_chain_battle_presenter'):
                self.assertIn(0x4b080228, words)  # upward half-height gain
            self.assertTrue(any(w == BL(helper + j * 4, setter) for j, w in enumerate(words)))
            self.assertFalse(any(w == BL(helper + j * 4, other) for j, w in enumerate(words)))

    def test_camp_item_picture_extends_only_its_backing_and_grid(self):
        ctx = Fixture()
        with patch.object(PATCHES, 'camp_backdrop'), patch.object(PATCHES, 'title_backdrops'):
            PATCHES.edge_layout(ctx)
        hook, awake = 0x293e61c, 0x257dec0
        self.assertEqual(ctx.checks[hook], '29fef097')
        (inst,) = ctx.patches[hook]
        stub = hook + (inst & 0x3ffffff) * 4
        self.assertEqual(inst, BL(hook, stub))
        stock = stub + 48
        self.assertEqual(ctx.word(stub + 8), PATCHES.cbz(17, stub + 8, stock))
        self.assertEqual(ctx.word(stub + 20), BL(stub + 20, awake))
        self.assertEqual(ctx.word(stock - 4), 0xd65f03c0)
        self.assertEqual(ctx.word(stock), B(stock, awake))
        for i, name in enumerate(('Background', 'Grid')):
            call = stub + 28 + i * 8
            rel = ctx.word(call) & 0x3ffffff
            helper = call + (rel - (1 << 26) if rel & (1 << 25) else rel) * 4
            self.assertEqual(ctx.word(call), BL(call, helper))
            path = ('ui_camp_item_picture_presenter/' + name).encode('utf-16le')
            begin = helper - (len(path) + 3) // 4 * 4
            data = b''.join(struct.pack('<I', ctx.word(addr)) for addr in range(begin, helper, 4))
            self.assertEqual(data[:len(path)], path)
            body = range(helper, helper + 29 * 4, 4)
            self.assertTrue(any(ctx.word(a) == BL(a, 0x25067b0) for a in body))
            self.assertFalse(any(ctx.word(a) == BL(a, 0x2506610) for a in body))

    def test_fishing_result_extends_only_its_left_gradient(self):
        ctx = Fixture()
        with patch.object(PATCHES, 'camp_backdrop'), patch.object(PATCHES, 'title_backdrops'):
            PATCHES.edge_layout(ctx)
        site = 0x21dfe70
        original = PATCHES.LDRX(0, 0, 0x90 // 8)
        self.assertEqual(ctx.checks[site], struct.pack('<I', original).hex())
        stub = site + (ctx.patches[site][0] & 0x3ffffff) * 4
        stock = stub + 40
        self.assertEqual(ctx.word(stub + 8), PATCHES.cbz(17, stub + 8, stock))
        self.assertEqual(ctx.word(stock), original)
        self.assertEqual(ctx.word(stock + 4), B(stock + 4, site + 4))
        addr = stub + 28
        rel = ctx.word(addr) & 0x3ffffff
        helper = addr + (rel - (1 << 26) if rel & (1 << 25) else rel) * 4
        literal = 'ui_field_fishing_result_presenter/Background'.encode('utf-16le')
        start = helper - (len(literal) + 3) // 4 * 4
        data = b''.join(struct.pack('<I', ctx.word(a)) for a in range(start, helper, 4))
        self.assertEqual(data[:len(literal)], literal)
        body = range(helper, helper + 29 * 4, 4)
        self.assertTrue(any(ctx.word(a) == BL(a, 0x25067b0) for a in body))
        self.assertFalse(any(ctx.word(a) == BL(a, 0x2506610) for a in body))

    def test_battle_result_extends_only_the_side_gradients(self):
        ctx = Fixture()
        with patch.object(PATCHES, 'camp_backdrop'), patch.object(PATCHES, 'title_backdrops'):
            PATCHES.edge_layout(ctx)
        at = 0x229e730
        stock_word = 0xa9bd7bfd
        self.assertEqual(ctx.checks[at], struct.pack('<I', stock_word).hex())
        stub = at + (ctx.patches[at][0] & 0x3ffffff) * 4
        stock = stub + 12 * 4
        self.assertEqual(ctx.word(stub + 8), PATCHES.cbz(17, stub + 8, stock))
        self.assertEqual(ctx.word(stock), stock_word)
        self.assertEqual(ctx.word(stock + 4), B(stock + 4, at + 4))
        for i, side in enumerate(('Left', 'Right')):
            call = stub + (7 + i * 2) * 4
            delta = ctx.word(call) & 0x3ffffff
            dest = call + (delta - (1 << 26) if delta & (1 << 25) else delta) * 4
            path = ('ui_battle_result_presenter/Background/' + side).encode('utf-16le')
            begin = dest - (len(path) + 3) // 4 * 4
            raw = b''.join(struct.pack('<I', ctx.word(a)) for a in range(begin, dest, 4))
            self.assertEqual(raw[:len(path)], path)
            addrs = range(dest, dest + 29 * 4, 4)
            self.assertIn(0x52a890e8, [ctx.word(a) for a in addrs])
            self.assertIn(0x1e230221, [ctx.word(a) for a in addrs])
            self.assertTrue(any(ctx.word(a) == BL(a, 0x25067b0) for a in addrs))

    def test_equipment_effects_adjust_only_its_matte_and_replay_start(self):
        ctx = Fixture()
        with patch.object(PATCHES, 'camp_backdrop'), patch.object(PATCHES, 'title_backdrops'):
            PATCHES.edge_layout(ctx)
        at = 0x2409a50
        original = 0xf9407800
        self.assertEqual(ctx.checks[at], struct.pack('<I', original).hex())
        stub = at + (ctx.patches[at][0] & 0x3ffffff) * 4
        stock = stub + 10 * 4
        self.assertEqual(ctx.word(stub + 8), PATCHES.cbz(17, stub + 8, stock))
        self.assertEqual(ctx.word(stock), original)
        self.assertEqual(ctx.word(stock + 4), B(stock + 4, at + 4))
        call = stub + 28
        delta = ctx.word(call) & 0x3ffffff
        dest = call + (delta - (1 << 26) if delta & (1 << 25) else delta) * 4
        literal = 'ui_dialog_background_presenter/BackgroundBlack01'.encode('utf-16le')
        begin = dest - (len(literal) + 3) // 4 * 4
        raw = b''.join(struct.pack('<I', ctx.word(a)) for a in range(begin, dest, 4))
        self.assertEqual(raw[:len(literal)], literal)
        code = range(dest, dest + 29 * 4, 4)
        self.assertIn(0x52a890e8, [ctx.word(a) for a in code])
        self.assertIn(0x1e230221, [ctx.word(a) for a in code])
        self.assertTrue(any(ctx.word(a) == BL(a, 0x25067b0) for a in code))

    def test_ending_page_adjusts_only_tints_and_decorations(self):
        ctx = Fixture()
        with patch.object(PATCHES, 'camp_backdrop'), patch.object(PATCHES, 'title_backdrops'):
            PATCHES.edge_layout(ctx)
        hook, base = 0x26074a8, 0x2578010
        self.assertEqual(ctx.checks[hook], struct.pack('<I', BL(hook, base)).hex())
        (inst,) = ctx.patches[hook]
        stub = hook + (inst & 0x3ffffff) * 4
        stock = stub + 16 * 4
        self.assertEqual(inst, BL(hook, stub))
        self.assertEqual(ctx.word(stub + 4), 0xb9404a11)
        self.assertEqual(ctx.word(stub + 8), PATCHES.cbz(17, stub + 8, stock))
        self.assertEqual(ctx.word(stub + 20), BL(stub + 20, base))
        self.assertEqual(ctx.word(stock), B(stock, base))
        backdrop = 'ui_common_background_parent_presenter/'
        for i, (path, change, setter) in enumerate((
            (backdrop + 'ui_camp_menu_background_presenter/ui_camp_menu_background_01',
             0x1e230221, 0x25067b0),
            (backdrop + 'ui_camp_menu_background_presenter/ui_camp_menu_background_02',
             0x1e230221, 0x25067b0),
            (backdrop + 'HeaderFooterParent/ui_camp_header_image_presenter/HeaderImage',
             0x4b080228, 0x2506610),
            ('FooterImage', 0x4b110108, 0x2506610),
        )):
            call = stub + 28 + i * 8
            rel = ctx.word(call) & 0x3ffffff
            helper = call + (rel - (1 << 26) if rel & (1 << 25) else rel) * 4
            literal = path.encode('utf-16le')
            begin = helper - (len(literal) + 3) // 4 * 4
            data = b''.join(struct.pack('<I', ctx.word(a)) for a in range(begin, helper, 4))
            self.assertEqual(data[:len(literal)], literal)
            code = range(helper, helper + 30 * 4, 4)
            self.assertIn(change, [ctx.word(a) for a in code])
            self.assertTrue(any(ctx.word(a) == BL(a, setter) for a in code))

    def test_replay_fishing_and_pickpocket_expand_only_the_dimmer(self):
        ctx = Fixture()
        with patch.object(PATCHES, 'camp_backdrop'), patch.object(PATCHES, 'title_backdrops'):
            PATCHES.edge_layout(ctx)
        for at, path in (
            (0x2605f40, 'ui_ending_collection_dialog_presenter/'
                        'ui_dialog_background_presenter/BackgroundBlack01'),
            (0x21dd1b0, 'ui_field_fishing_bait_presenter/'
                        'ui_dialog_background_presenter/BackgroundBlack01'),
            (0x21e8e40, 'ui_field_pick_pocket_presenter/'
                        'ui_select_choice_presenter/Background'),
        ):
            with self.subTest(hook=hex(at)):
                original = 0xa9be7bfd
                self.assertEqual(ctx.checks[at], struct.pack('<I', original).hex())
                (branch,) = ctx.patches[at]
                stub = at + (branch & 0x3ffffff) * 4
                self.assertEqual(branch, B(at, stub))
                stock = stub + 40
                self.assertEqual(ctx.word(stub + 8), PATCHES.cbz(17, stub + 8, stock))
                self.assertEqual(ctx.word(stock), original)
                self.assertEqual(ctx.word(stock + 4), B(stock + 4, at + 4))
                call = stub + 28
                rel = ctx.word(call) & 0x3ffffff
                helper = call + (rel - (1 << 26) if rel & (1 << 25) else rel) * 4
                self.assertEqual(ctx.word(call), BL(call, helper))
                encoded = path.encode('utf-16le')
                start = helper - (len(encoded) + 3) // 4 * 4
                literal = b''.join(struct.pack('<I', ctx.word(a)) for a in range(start, helper, 4))
                self.assertEqual(literal[:len(encoded)], encoded)
                code = range(helper, helper + 29 * 4, 4)
                self.assertIn(0x52a890e8, [ctx.word(a) for a in code])
                self.assertIn(0x1e230221, [ctx.word(a) for a in code])
                self.assertTrue(any(ctx.word(a) == BL(a, 0x25067b0) for a in code))
                self.assertFalse(any(ctx.word(a) == BL(a, 0x2506610) for a in code))

    def test_system_inventory_and_assist_extend_only_the_full_screen_mat(self):
        ctx = Fixture()
        with patch.object(PATCHES, 'camp_backdrop'), patch.object(PATCHES, 'title_backdrops'):
            PATCHES.edge_layout(ctx)
        hook, awake = 0x25e4c74, 0x23023b0
        self.assertEqual(ctx.checks[hook], struct.pack('<I', BL(hook, awake)).hex())
        stub = hook + (ctx.patches[hook][0] & 0x3ffffff) * 4
        stock = stub + 16 * 4
        self.assertEqual(ctx.word(stub + 8), PATCHES.cbz(17, stub + 8, stock))
        self.assertEqual(ctx.word(stock), B(stock, awake))
        for i, parent in enumerate((
            'ui_item_discard_selector/ui_item_discard_presenter',
            'UIAssistDialogSelector/ui_assist_dialog_presenter',
            'UIOverflowItemSelector/ui_overflow_item_presenter',
            'UIEquipWizardSelector/ui_equip_wizard_presenter',
        )):
            call = stub + (7 + i * 2) * 4
            delta = ctx.word(call) & 0x3ffffff
            helper = call + (delta - (1 << 26) if delta & (1 << 25) else delta) * 4
            path = (parent + '/ui_dialog_background_presenter/BackgroundBlack01').encode('utf-16le')
            begin = helper - (len(path) + 3) // 4 * 4
            raw = b''.join(struct.pack('<I', ctx.word(a)) for a in range(begin, helper, 4))
            self.assertEqual(raw[:len(path)], path)
            code = range(helper, helper + 29 * 4, 4)
            self.assertIn(0x1e230221, [ctx.word(a) for a in code])
            self.assertTrue(any(ctx.word(a) == BL(a, 0x25067b0) for a in code))
            self.assertFalse(any(ctx.word(a) == BL(a, 0x2506610) for a in code))
