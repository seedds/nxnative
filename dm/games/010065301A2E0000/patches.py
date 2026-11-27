"""Star Ocean 1.1.0: build-gated Unity screen shape and layout."""
from struct import pack
from nso_asm import ADDI, ADRP, B, BL, CMPR, LDP_POST, LDRX, MOVX, SUBI, STP_PRE, STRX


def screen_shape(ctx):
    # SwitchPlatformSubModule.ChangeOperationMode (inlined) and ChangeResolution
    # pass these immediate heights to UnityEngine.Screen.SetResolution.
    for at, stock, reg, offset, reason in (
        (0x2169130, 1080, 1, 0x48, "Unity Screen.SetResolution"),
        (0x2169140, 720, 1, 0x4c, "Unity Screen.SetResolution"),
        (0x2169254, 1080, 1, 0x48, "Unity Screen.SetResolution"),
        (0x216926c, 720, 1, 0x4c, "Unity Screen.SetResolution"),
        # NVN allocates storage using the first two builders, then uses
        # the third to initialize the three colour textures.
        (0x77b79c, 1080, 22, 0x48, "NVN swapchain storage size"),
        (0x77ba28, 1080, 2, 0x48, "NVN swapchain texture size"),
        # The swapchain's depth/stencil attachment sets the actual raster bounds.
        (0x766818, 1080, 3, 0x48, "NVN swapchain depth/stencil size"),
        # The Switch backend also uses these defaults for the initial viewport.
        (0x767b38, 720, 22, 0x4c, "Unity handheld display height"),
        (0x767bd8, 1080, 9, 0x48, "Unity console display height"),
        (0x767bf4, 1080, 8, 0x48, "Unity console display height"),
        (0x6e00ec, 720, 10, 0x4c, "Unity initial handheld screen size"),
    ):
        orig = 0x52800000 | stock << 5 | reg  # mov wReg, #stock
        ctx.site(at, pack('<I', orig).hex())
        start = ctx.here()
        ctx.emit(ADRP(16, start, ctx.state))
        ctx.emit(0xb9400210 | (offset // 4) << 10)  # ldr w16, [x16, #offset]
        ctx.emit(0x34000070)  # cbz w16, stock height
        ctx.emit(0x2a1003e0 | reg)  # mov wReg, w16
        ctx.emit(B(ctx.here(), at + 4))
        ctx.emit(orig)
        ctx.emit(B(ctx.here(), at + 4))
        ctx.put(at, [B(at, start)], reason + ' height follows the display shape')


    # The Switch-specific ScreenManager clamps the requested height to
    # 1080 before saving it at +0xc4. Keep its stock clamp with tall mode off.
    # x16/x17 are scratch; preserve NZCV from the original comparison.
    at, orig = 0x6e04e0, 0x1a88b288  # csel w8, w20, w8, lt
    ctx.site(at, pack('<I', orig).hex())
    start = ctx.here()
    ctx.emit(orig)
    ctx.emit(ADRP(16, ctx.here(), ctx.state))
    ctx.emit(0xb9404a10)  # ldr w16, [x16, #0x48]
    ctx.emit(0x34000050)  # cbz w16, back
    ctx.emit(0x2a1403e8)  # mov w8, w20
    ctx.emit(B(ctx.here(), at + 4))
    ctx.put(at, [B(at, start)], 'Unity Switch ScreenManager: keep requested taller height')


    # CinemachineBrain pushes each frame's unmodified lens to the Unity camera.
    # Keep the horizontal angle fixed: vertical→horizontal at 16:9, then
    # horizontal→vertical at the tall ratio. Orthographic extents scale by k.
    # Run the original lens setter with its original pointer and method info.
    at, setter = 0x1981e10, 0x20be3f0
    ctx.site(at, pack('<I', BL(at, setter)).hex())
    start = ctx.here()
    ctx.emit(ADRP(16, start, ctx.state))
    ctx.emit(0xb9404a11)  # ldr w17, [x16, #0x48]
    ctx.emit(0x34000191)  # cbz w17, tail setter (+12)
    ctx.emit(0xa9be7bf0)  # stp x16, x30, [sp, #-32]!
    ctx.emit(0xa90107e0)  # stp x0, x1, [sp, #16]
    ctx.emit(0xbd407601)  # ldr s1, [x16, #0x74] (16:9)
    ctx.emit(0xaa1f03e0)  # mov x0, xzr
    ctx.emit(BL(ctx.here(), 0x20c0550))  # VerticalToHorizontalFieldOfView
    ctx.emit(0xf94003f0)  # ldr x16, [sp]
    ctx.emit(0xbd405a01)  # ldr s1, [x16, #0x58] (tall ratio)
    ctx.emit(0xaa1f03e0)  # mov x0, xzr
    ctx.emit(BL(ctx.here(), 0x20c04e0))  # HorizontalToVerticalFieldOfView
    ctx.emit(0xa94107e0)  # ldp x0, x1, [sp, #16]
    ctx.emit(0xa8c27bf0)  # ldp x16, x30, [sp], #32
    ctx.emit(B(ctx.here(), setter))
    ctx.put(at, [BL(at, start)], 'Cinemachine: keep horizontal FOV on the tall screen')

    at, setter = 0x1981e00, 0x20be5a0
    ctx.site(at, pack('<I', BL(at, setter)).hex())
    start = ctx.here()
    ctx.emit(ADRP(16, start, ctx.state))
    ctx.emit(0xb9404a11)  # ldr w17, [x16, #0x48]
    ctx.emit(0x34000071)  # cbz w17, tail setter
    ctx.emit(0xbd405e01)  # ldr s1, [x16, #0x5c] (vertical expansion)
    ctx.emit(0x1e210800)  # fmul s0, s0, s1
    ctx.emit(B(ctx.here(), setter))
    ctx.put(at, [BL(at, start)], 'Cinemachine: keep orthographic horizontal extent')

    # The shared ui_fade and ui_fade_under RawImage prefabs have centered
    # anchors and a fixed 1920x1080 RectTransform. Size the actual rectangle
    # when UIFadeComponent.StartFade activates it, after its stock position
    # setter. This leaves its width, pivot, draw order and alpha alone.
    at, position, size = 0x2028948, 0x2506bf0, 0x25067b0
    ctx.site(at, pack('<I', B(at, position)).hex())
    start = ctx.here()
    ctx.emit(ADRP(16, start, ctx.state))
    ctx.emit(0xb9404a11)  # ldr w17, [x16, #0x48]
    ctx.emit(0x34000171)  # cbz w17, tail set_localPosition
    ctx.emit(0xa9bf7be0)  # stp x0, x30, [sp, #-16]!
    ctx.emit(BL(ctx.here(), position))
    ctx.emit(0xa8c17be0)  # ldp x0, x30, [sp], #16
    ctx.emit(ADRP(16, ctx.here(), ctx.state))
    ctx.emit(0xb9404a11)  # ldr w17, [x16, #0x48]
    ctx.emit(0xaa1f03e1)  # mov x1, xzr (IL2CPP method info)
    ctx.emit(0x52a89e08)  # mov w8, #0x44f00000 (1920.0f)
    ctx.emit(0x1e270100)  # fmov s0, w8
    ctx.emit(0x1e230221)  # ucvtf s1, w17
    ctx.emit(B(ctx.here(), size))
    ctx.emit(B(ctx.here(), position))
    ctx.put(at, [B(at, start)], 'UIFadeComponent: size the fade rectangle to the taller screen')


    edge_layout(ctx)
    ctx.capabilities.add('SCREEN_ASPECT')


def cbz(reg, pc, target, wide=False):
    delta = target - pc
    assert delta % 4 == 0 and -(1 << 20) <= delta < (1 << 20)
    return (0xb4000000 if wide else 0x34000000) | ((delta // 4 & 0x7ffff) << 5) | reg


def title_owned_chrome(windows):
    # These are dedicated UITitleScene presenters, never reused by Camp.
    # The chrome is separate from the scroll views, actor art and inputs.
    for selector, presenter in windows:
        root = f'{selector}/{presenter}/'
        backdrop = root + 'ui_camp_menu_background_presenter/'
        yield backdrop + 'ui_camp_menu_background_01', 'height'
        yield backdrop + 'ui_camp_menu_background_02', 'height'
        yield root + 'ui_camp_header_image_presenter', 'height'
        yield root + 'FooterImage', 'bottom'


def edge_layout(ctx):
    # Fixed-size design rects hide the extra rows from descendants' anchors.
    # Use only inspected scene paths, not every 1920x1080 rect. Where an edge
    # group mixes edge and center-relative children, translate it as a unit.
    transform, find = 0x24fcc50, 0x2509c10
    size_get, size_set = 0x25066e0, 0x25067b0
    pos_get, pos_set = 0x2506540, 0x2506610
    string_ctor = 0x1a1ca60  # System.String.Ctor(char*, int startIndex, int length)

    def emit_adjustment(path, mode):
        literal = ctx.here()
        if path:
            raw = path.encode('utf-16le')
            for j in range(0, len(raw), 4):
                ctx.emit(int.from_bytes(raw[j:j+4], 'little'))
        start = ctx.here()
        code = []

        def word(w): code.append(w)
        def pc(): return start + len(code) * 4
        def call(to): word(BL(pc(), to))

        # x0 = known scene component, or a child resolved from its transform.
        word(STP_PRE(19, 30, 31, -16))
        word(MOVX(1, 31))
        call(transform)
        word(MOVX(19, 0))
        exits = [len(code)]
        word(0)  # cbz x0, done
        if path:
            word(ADRP(0, pc(), literal))
            word(ADDI(0, 0, literal & 0xfff))
            word(MOVX(1, 31))
            word(0x52800002 | len(path) << 5)  # mov w2, #length
            word(MOVX(3, 31))
            call(string_ctor)
            word(MOVX(1, 0))
            word(MOVX(0, 19))
            word(MOVX(2, 31))
            call(find)
            word(MOVX(19, 0))
            exits.append(len(code))
            word(0)  # cbz x0, done
        if mode == 'remember_title':
            word(ADRP(16, pc(), ctx.state))
            word(STRX(19, 16, 0xb8 // 8))
            done = len(code)
            word(LDP_POST(19, 30, 31, 16))
            word(0xd65f03c0)  # ret
            for j in exits:
                code[j] = cbz(0, start + j * 4, start + done * 4, wide=True)
            for w in code:
                ctx.emit(w)
            return start
        word(MOVX(1, 31))
        rotated = mode == 'rotated_height'
        stretch = mode in ('stretch_height', 'restore_stretch_height')
        restore_chrome = mode in ('restore_stretch_height', 'restore_title_header')
        size_mode = stretch or restore_chrome or mode in ('height', 'rotated_height', 'restore_height')
        call(size_get if size_mode else pos_get)
        unchanged = None
        if mode in ('height', 'rotated_height', 'stretch_height'):
            word(0x52800008 if stretch else 0x52a890e8)  # w8 = bits of authored 0 or 1080.0f
            word(0x1e270102)  # fmov s2, w8
            word(0x1e222000 if rotated else 0x1e222020)  # fcmp s0/s1, s2
            unchanged = len(code)
            word(0)  # b.ne done, a different authored height is not ours
            word(ADRP(16, pc(), ctx.state))
            word(0xb9404a11)  # ldr w17, [x16, #0x48]
            if stretch:
                word(SUBI(17, 17, 1080) ^ (1 << 31))  # sub w17, w17, #1080
            word(0x1e230220 if rotated else 0x1e230221)  # ucvtf s0/s1, w17
        elif mode == 'restore_height' or restore_chrome:
            if restore_chrome:
                word(ADRP(16, pc(), ctx.state))
                word(0xb9404a11)  # ldr w17, [x16, #0x48]
                if stretch:
                    word(SUBI(17, 17, 1080) ^ (1 << 31))  # sub w17, w17, #1080
                word(0x1e230222)  # ucvtf s2, w17
                word(0x1e222020)  # fcmp s1,s2
                unchanged = len(code)
                word(0)  # b.ne done; untouched Camp settings keep their values
            word(0x52800008 if stretch else 0x52a890e8)  # w8 = bits of authored 0 or 1080.0f
            word(0x1e270101)  # fmov s1, w8
        else:
            word(0x52808708)  # mov w8, #1080
            word(ADRP(16, pc(), ctx.state))
            word(0xb9404a11)  # ldr w17, [x16, #0x48]
            word(0x4b110108 if mode == 'bottom' else 0x4b080228)  # stock-tall or tall-stock
            word(0x1e220103)  # scvtf s3, w8
            word(0x1e2c1002)  # fmov s2, #0.5
            word(0x1f020461)  # fmadd s1, s3, s2, s1
        word(MOVX(0, 19))
        word(MOVX(1, 31))
        call(size_set if size_mode else pos_set)
        done = len(code)
        word(LDP_POST(19, 30, 31, 16))
        word(0xd65f03c0)  # ret
        for j in exits:
            code[j] = cbz(0, start + j * 4, start + done * 4, wide=True)
        if unchanged is not None:
            code[unchanged] = 0x54000001 | (done - unchanged) << 5  # b.ne done
        for w in code:
            ctx.emit(w)
        return start

    for at, awake, adjustments, label in (
        (0x2615a60, None, (('', 'bottom'),), 'dialogue portraits'),
        (0x2613a74, 0x26be590, (('', 'bottom'),), 'dialogue and battle operation hints'),
        # The crafting reveal has its own bottom-anchored operation bar,
        # not the event footer. Keep the centered result card and animation.
        (0x238aba4, 0x26be590, (
            ('ui_event_operation_presenter', 'bottom'),
        ), 'Camp crafting result skip hint'),
        (0x21eacb0, 0x275c420, (('ui_field_layout_presenter', 'height'),), 'field HUD'),
        (0x1f57464, 0x275c420, (('UIFooterSelector', 'height'),), 'shared footer'),
        # The black, sprite-less quad is separate from the authored
        # BackgroundCenter artwork and the centered title and menu.
        (0x1f5c178, 0x23023b0, (
            ('UIGameOverSelector/ui_game_over_presenter/Background', 'height'),
        ), 'Game Over black backdrop'),
        # The gradation sprite is rotated 90 degrees, so its local width
        # determines its screen height. Keep the text/art sibling unchanged.
        (0x203c59c, 0x257dec0, (
            ('ui_tutorial_popup_presenter/BackgroundParent/ui_tutorial_background_01', 'height'),
            ('ui_tutorial_popup_presenter/BackgroundParent/ui_tutorial_background_01 (1)', 'height'),
            ('ui_tutorial_popup_presenter/BackgroundParent/Gradation', 'rotated_height'),
        ), 'tutorial dimming backdrop'),
        # Move the screen-space status and command as a pair. The drop-down
        # lives on the window canvas and animates horizontally, so extend
        # its design area rather than moving the selector's rest position.
        (0x23caf44, 0x275c420, (
            ('UIBattleStatusSelector/ui_battle_status_presenter', 'top'),
            ('ui_battle_controller_layout_presenter/ui_battle_operation_presenter', 'top'),
            # Pause combines a vertical edge gradient rotated -90 degrees
            # around its lower edge, and a stock-height side gradient.
            # Reanchor the former and extend the latter without touching
            # either falloff axis, the pause card, or the command menu.
            ('ui_battle_pause_background_presenter/Background (1)', 'height'),
            ('ui_battle_pause_background_presenter/Background', 'bottom'),
            # Survival and linked encounters share this upper-left banner.
            # Move its title, round counter, effects, and backing as a unit.
            ('ui_battle_controller_layout_presenter/ui_chain_battle_presenter', 'top'),
        ), 'battle party status, tactics, pause gradient and chain banner'),
        (0x21b4a44, 0x23023b0, (
            ('ui_battle_operation_selector', 'height'),
        ), 'battle tactics drop-down'),
        # The reward overlay has its own two 900-wide side gradients.
        # Resize their heights, preserving the horizontal falloff and HUD.
        (0x229e730, None, (
            ('ui_battle_result_presenter/Background/Left', 'height'),
            ('ui_battle_result_presenter/Background/Right', 'height'),
        ), 'battle results side gradients'),
        # Fishing result has one 900-wide left-edge gradient next to
        # its separate foreground LayoutParent. Preserve that layout.
        (0x21dfe70, None, (
            ('ui_field_fishing_result_presenter/Background', 'height'),
        ), 'fishing result side gradient'),
        # The picture viewer's backing and grid are separate from the
        # illustration and its interactive zoom/pan layout.
        (0x293e61c, 0x257dec0, (
            ('ui_camp_item_picture_presenter/Background', 'height'),
            ('ui_camp_item_picture_presenter/Grid', 'height'),
        ), 'Camp item picture backing and grid'),
        # UIDialogWindow owns these six stock-size mattes. Only the
        # dimmer grows: leave the card, its local backing and animation alone.
        # Other windows also have similarly named nodes, some deliberately
        # shorter, so do not scan the whole UI for BackgroundBlack01.
        (0x26042fc, 0x23023b0, tuple(
            (f'{presenter}/ui_dialog_background_presenter/BackgroundBlack01', 'height')
            for presenter in (
                'UIDialogPresenter',
                'ui_auto_save_announce_dialog_presenter',
                'ui_dialog_description_presenter',
                'ui_take_over_dialog_presenter',
                'ui_progress_bar_dialog_presenter',
                'ui_demo_dialog_presenter',
            )
        ), 'shared system dialog mattes'),
        # These UISystemWindow dialogs are separate from UIDialogWindow.
        # Extend the full-screen tint only, leaving the card and actions alone.
        (0x25e4c74, 0x23023b0, (
            ('ui_item_discard_selector/ui_item_discard_presenter/'
             'ui_dialog_background_presenter/BackgroundBlack01', 'height'),
            ('UIAssistDialogSelector/ui_assist_dialog_presenter/'
             'ui_dialog_background_presenter/BackgroundBlack01', 'height'),
            ('UIOverflowItemSelector/ui_overflow_item_presenter/'
             'ui_dialog_background_presenter/BackgroundBlack01', 'height'),
            ('UIEquipWizardSelector/ui_equip_wizard_presenter/'
             'ui_dialog_background_presenter/BackgroundBlack01', 'height'),
        ), 'inventory, equipment and assist dialog mattes'),
        # Arena registration has a window-wide backdrop separate from the
        # authored character/contest cards and their modal prompt tints.
        (0x2333074, 0x23023b0, (
            ('ui_background_presenter/Background', 'height'),
        ), 'Coliseum registration matte'),
        # Only decorative layers move; the betting grid and the paddock
        # keep their stock anchors and aspect ratio.
        (0x232e27c, 0x23023b0, (
            ('ui_bunny_race_bet_selector/ui_layout_presenter/Background', 'height'),
            ('ui_bunny_race_bet_selector/ui_layout_presenter/Background/Background (1)', 'height'),
            ('ui_bunny_race_bet_selector/ui_layout_presenter/HeaderBackground', 'top'),
            ('ui_bunny_race_bet_selector/FooterImage', 'bottom'),
        ), 'Bunny Race betting backdrop and edge chrome'),
        # The ingredient list owns separate blur/tint and edge decorations.
        # Do not change the minigame controls, timer, or recipe columns.
        (0x233ac94, 0x257db40, (
            ('CaptureBackground/BackgroundParent/ui_camp_menu_background_01', 'height'),
            ('CaptureBackground/BackgroundParent/ui_camp_menu_background_02', 'height'),
            ('HeaderBackground', 'top'),
            ('FooterImage', 'bottom'),
        ), 'Cooking Master ingredient picker backdrop and edge chrome'),
        # Camp equipment effects belongs to its own presenter, not UIDialogWindow.
        (0x2409a50, None, (
            ('ui_dialog_background_presenter/BackgroundBlack01', 'height'),
        ), 'Camp equipment effects dialog matte'),
        (0x265bb64, 0x23023b0, (
            # OnEndNewGame activates this separate black parent while the
            # new-game/DLC popup is open. Its Image child stretches with it.
            ('BlackOutBackground', 'height'),
            ('ui_title_presenter/BackgroundBlack', 'height'),
            # Original Staff and Original English Staff share one instance.
            *title_owned_chrome((
                ('ui_title_original_staff_selector', 'ui_title_original_staff_presenter'),
                ('ui_title_license_selector', 'ui_title_license_presenter'),
                ('ui_title_standing_picture_selector', 'ui_title_standing_picture_presenter'),
                ('ui_title_voice_gallery_selector', 'ui_title_voice_gallery_presenter'),
            )),
            *title_owned_chrome((
                ('ui_title_new_game_selector', 'ui_title_new_game_presenter'),
            )),
            # Both hero-selection stages share these bottom-pivoted portraits.
            # Follow the tall bottom edge without scaling their original art.
            *((f'ui_title_new_game_selector/ui_title_new_game_presenter/'
               f'ui_title_select_hero_presenter/StandingPicture{hero}', 'bottom')
              for hero in ('Claude', 'Rena')),
        ), 'title scene backing, submenu decor and New Game portraits'),
        # Current Location shares the world-map overlay chrome, not its map
        # viewport. The full-width Background is the first child named
        # Background; the later duplicate is the separate narrow list tint.
        (0x22c2a70, None, (
            ('ui_world_map_fast_travel_selector/Background', 'height'),
            ('ui_world_map_fast_travel_selector/ui_camp_header_image_presenter/HeaderImage', 'top'),
            ('ui_world_map_fast_travel_selector/FooterImage', 'bottom'),
            ('ui_world_map_fast_travel_selector/ui_world_map_parent_presenter/ui_world_map_icon_guide_presenter', 'bottom'),
        ), 'Current Location map backdrop and edge anchors'),
        (0x2156514, 0x23023b0, (
            ('BackgroundParent/ui_camp_menu_background_01', 'height'),
            # The left-side gradient is a separate, narrower tint layer.
            ('BackgroundParent/ui_camp_menu_background_02', 'height'),
            ('ui_camp_header_image_presenter/HeaderImage', 'top'),
            ('ui_camp_footer_parent_presenter/FooterImage', 'bottom'),
            ('ui_camp_header_parent_presenter', 'height'),
        ), 'Camp tint and decorative edge anchors'),
        # The left-side gradient is a separate 940-wide child of the tint.
        # Buy and Sell share these same Shop chrome layers. The footer
        # controller already anchors its hints; leave its text and list alone.
        (0x258ad8c, 0x23023b0, (
            ('ui_shop_layout_presenter/Background', 'height'),
            ('ui_shop_layout_presenter/Background/Background (1)', 'height'),
            ('ui_shop_layout_presenter/HeaderBackground', 'top'),
            ('FooterImage', 'bottom'),
        ), 'Shop backdrop and patterned edge chrome'),
        # Conversation Log shares no Shop assets. Expand only the flat
        # translucent backing and move the two patterned strips intact.
        (0x275daf4, 0x2580c30, (
            ('ui_conversation_log_presenter/Background', 'height'),
            ('ui_conversation_log_presenter/HeaderBackground', 'top'),
            ('ui_conversation_log_presenter/FooterBackground', 'bottom'),
        ), 'Conversation Log tint and patterned edge chrome'),
        # Same decorations in separate windows in UIShopScene. Leave
        # all list/details presenters at their authored bounds.
        (0x24ec208, 0x23023b0, (
            ('ui_quest_selector/Background', 'height'),
            ('ui_quest_selector/Background/Background (1)', 'height'),
            ('ui_quest_selector/HeaderBackground', 'top'),
            ('ui_quest_selector/FooterImage', 'bottom'),
        ), 'Quest backdrop and patterned edge chrome'),
        (0x1f56038, 0x23023b0, (
            ('Background', 'height'),
            ('Background/Background (1)', 'height'),
            ('HeaderBackground', 'top'),
            ('FooterImage', 'bottom'),
        ), 'Fish Collector backdrop and patterned edge chrome'),
        # Title and Camp enter the same dedicated collection selector. The
        # blue tint, left gradient and decorative strips are outside its grid.
        (0x26074a8, 0x2578010, (
            ('ui_common_background_parent_presenter/ui_camp_menu_background_presenter/'
             'ui_camp_menu_background_01', 'height'),
            ('ui_common_background_parent_presenter/ui_camp_menu_background_presenter/'
             'ui_camp_menu_background_02', 'height'),
            ('ui_common_background_parent_presenter/HeaderFooterParent/'
             'ui_camp_header_image_presenter/HeaderImage', 'top'),
            ('FooterImage', 'bottom'),
        ), 'Ending Collection page backdrop and edge chrome'),
        # Restrict the fishing change to its modal, not the field HUD.
        (0x21dd1b0, None, (
            ('ui_field_fishing_bait_presenter/ui_dialog_background_presenter/'
             'BackgroundBlack01', 'height'),
        ), 'fishing bait dialog matte'),
        # This selector remains behind the nested confirmation. Extend its
        # own matte instead of changing the shared confirmation or item list.
        (0x21e8e40, None, (
            ('ui_field_pick_pocket_presenter/ui_select_choice_presenter/Background', 'height'),
        ), 'Pickpocketing selector matte'),
        # These two conversation selectors own distinct decorative tints:
        # the interactive topic/chapter list and the resulting article.
        (0x276d64c, 0x257dec0, (
            ('ui_select_choice_presenter/Background', 'height'),
        ), 'conversation choice matte'),
        (0x276c560, 0x257dec0, (
            ('ui_event_information_presenter/Background', 'height'),
        ), 'event information article matte'),
        # Keep the replay card independent of the gallery backing.
        (0x2605f40, None, (
            ('ui_ending_collection_dialog_presenter/ui_dialog_background_presenter/'
             'BackgroundBlack01', 'height'),
        ), 'Ending replay confirmation matte'),
    ):
        # The Start entry has no direct caller; preserve its incoming
        # component and MethodInfo. Other sites are ordinary Awake calls.
        if awake is None:
            original = {
                0x22c2a70: 0xa9bc7bfd,  # World Map Start saves FP/LR.
                0x229e730: 0xa9bd7bfd,  # Battle Result Start saves FP/LR.
                0x2615a60: 0xf9400008,  # Dialogue Start loads the class pointer.
                0x21dd1b0: 0xa9be7bfd,  # Fishing Bait Start saves FP/LR.
                0x21dfe70: 0xf9404800,  # Fishing Result Start loads the presenter.
                0x21e8e40: 0xa9be7bfd,  # Pickpocketing selector Start saves FP/LR.
                0x2605f40: 0xa9be7bfd,  # Ending replay dialog Start saves FP/LR.
                0x2409a50: 0xf9407800,  # UIEquipEffectListPresenter.Start.
            }[at]
        else:
            original = BL(at, awake)
        ctx.site(at, pack('<I', original).hex())
        helpers = [emit_adjustment(*rule) for rule in adjustments]
        start = ctx.here()
        ctx.emit(ADRP(16, start, ctx.state))
        ctx.emit(0xb9404a11)  # ldr w17, [x16, #0x48]
        stock = start + (8 + 2 * len(helpers)) * 4
        ctx.emit(cbz(17, ctx.here(), stock))
        ctx.emit(STP_PRE(19, 30, 31, -16))
        if awake is None:
            ctx.emit(STP_PRE(0, 1, 31, -16))
        ctx.emit(MOVX(19, 0))
        if awake is not None:
            ctx.emit(BL(ctx.here(), awake))
        for helper in helpers:
            ctx.emit(MOVX(0, 19))
            ctx.emit(BL(ctx.here(), helper))
        if awake is None:
            ctx.emit(LDP_POST(0, 1, 31, 16))
        ctx.emit(LDP_POST(19, 30, 31, 16))
        if awake is not None:
            ctx.emit(0xd65f03c0)  # ret
        assert ctx.here() == stock
        if awake is None:
            ctx.emit(original)
            ctx.emit(B(ctx.here(), at + 4))
        else:
            ctx.emit(B(ctx.here(), awake))
        ctx.put(at, [(B if awake is None else BL)(at, start)], 'tall HUD: ' + label)

    camp_backdrop(ctx, emit_adjustment('', 'height'), emit_adjustment('', 'restore_height'))
    title_backdrops(ctx, emit_adjustment)
    title_menu_chrome(ctx, emit_adjustment)


def fin_backdrop(ctx, simple):
    # Track only the Fin selector's own star Image, obtained at its async
    # sprite setter before that setter dirties the stock mesh.
    load, set_sprite, close = 0x1f4dcc8, 0x17290a0, 0x1f4e1b0
    slot = 0xf8 // 8
    ctx.site(load, pack('<I', B(load, set_sprite)).hex())
    at = ctx.here()
    ctx.put(load, [B(load, at)], 'Fin: track the star field Image on sprite assignment')
    ctx.emit(ADRP(16, ctx.here(), ctx.state))
    ctx.emit(0xb9404a11)  # ldr w17, [x16,#0x48] (tall height)
    ctx.emit(cbz(17, ctx.here(), ctx.here() + 8))
    ctx.emit(STRX(0, 16, slot))
    ctx.emit(B(ctx.here(), set_sprite))

    first = STP_PRE(29, 30, 31, -0x20)
    ctx.site(close, pack('<I', first).hex())
    at = ctx.here()
    ctx.put(close, [B(close, at)], 'Fin: clear the star field Image on close')
    ctx.emit(ADRP(16, ctx.here(), ctx.state))
    ctx.emit(STRX(31, 16, slot))
    ctx.emit(first)
    ctx.emit(B(ctx.here(), close + 4))

    # Simple Image: vertices 0/3 are bottom and 1/2 top. Keep the
    # original quad and triangles, use the same inner vertices (no
    # overlap), and append four outer vertices with reflected UV0.y.
    # Compute the UV slope from the actual generated sprite corners,
    # including the sprite's effective atlas subrect and mesh height.
    at = ctx.here()
    ctx.emit(STP_PRE(19, 30, 31, -16))
    ctx.emit(MOVX(19, 1))
    ctx.emit(SUBI(31, 31, 0x80))  # UIVertex plus four floats at 0x70..
    ctx.emit(BL(ctx.here(), simple))
    def scalar(load, reg, offset):
        return (0xbd4003e0 if load else 0xbd0003e0) | (offset // 4) << 10 | reg
    def fp(op, dest, left, right):
        return op | right << 16 | left << 5 | dest
    add, sub, mul, div = 0x1e202800, 0x1e203800, 0x1e200800, 0x1e201800
    def populate(vertex):
        ctx.emit(MOVX(0, 19))
        ctx.emit(ADDI(1, 31, 0))
        ctx.emit(0x52800002 | vertex << 5)  # mov w2, #vertex
        ctx.emit(MOVX(3, 31))
        ctx.emit(BL(ctx.here(), 0x16d5cf0))  # PopulateUIVertex
    populate(0)
    for dst, src in ((0x70, 4), (0x74, 0x30)):
        ctx.emit(scalar(True, 0, src))
        ctx.emit(scalar(False, 0, dst))
    populate(1)
    for cached, vertex, dest in ((0x70, 4, 0), (0x74, 0x30, 1)):
        ctx.emit(scalar(True, 2, vertex))
        ctx.emit(scalar(True, 3, cached))
        ctx.emit(fp(sub, dest, 2, 3))
    ctx.emit(fp(div, 1, 1, 0))  # slope = (topUV-bottomUV)/(topY-bottomY)
    ctx.emit(ADRP(16, ctx.here(), ctx.state))
    ctx.emit(0xb9404a11)  # tall design height
    ctx.emit(0x52808708)  # mov w8, #1080
    ctx.emit(0x4b080231)  # sub w17, w17, w8
    ctx.emit(0x1e230220)  # ucvtf s0, w17
    ctx.emit(0x1e2c1002)  # fmov s2, #0.5
    ctx.emit(fp(mul, 0, 0, 2))  # added height per edge
    ctx.emit(fp(mul, 1, 1, 0))
    ctx.emit(scalar(False, 0, 0x70))
    ctx.emit(scalar(False, 1, 0x74))
    for original, top in ((1, True), (2, True), (0, False), (3, False)):
        populate(original)
        for member, delta, op in ((4, 0x70, add if top else sub),
                                  (0x30, 0x74, sub if top else add)):
            ctx.emit(scalar(True, 0, member))
            ctx.emit(scalar(True, 1, delta))
            ctx.emit(fp(op, 0, 0, 1))
            ctx.emit(scalar(False, 0, member))
        ctx.emit(MOVX(0, 19))
        ctx.emit(ADDI(1, 31, 0))
        ctx.emit(MOVX(2, 31))
        ctx.emit(BL(ctx.here(), 0x16d68b0))  # AddVert(UIVertex)
    for triangle in ((1, 4, 5), (5, 2, 1), (6, 0, 3), (3, 7, 6)):
        ctx.emit(MOVX(0, 19))
        for reg, index in enumerate(triangle, 1):
            ctx.emit(0x52800000 | index << 5 | reg)
        ctx.emit(MOVX(4, 31))
        ctx.emit(BL(ctx.here(), 0x16d6950))  # AddTriangle
    ctx.emit(ADDI(31, 31, 0x80))
    ctx.emit(LDP_POST(19, 30, 31, 16))
    ctx.emit(0xd65f03c0)  # ret
    return at


def title_backdrops(ctx, emit_adjustment):
    # Keep the original title Background sprite and its animation: it
    # contains the planet as well as stars. Add one reflected star-only
    # strip above the original quad instead of scaling or moving that quad.
    remember = emit_adjustment('ui_title_presenter/Background', 'remember_title')
    open_title, close_title = 0x265bd90, 0x265c720
    ctx.site(open_title, 'e80f1bfc')  # str d8, [sp, #-0x50]!
    ctx.site(close_title, pack('<I', STP_PRE(29, 30, 31, -0x30)).hex())
    at = ctx.here()
    ctx.put(open_title, [B(open_title, at)], 'Title: track only the title background transform')
    ctx.emit(ADRP(16, ctx.here(), ctx.state))
    ctx.emit(0xb9404a11)  # ldr w17, [x16,#0x48]
    ctx.emit(cbz(17, ctx.here(), ctx.here() + 24))
    ctx.emit(STP_PRE(0, 1, 31, -16))
    ctx.emit(STP_PRE(29, 30, 31, -16))
    ctx.emit(BL(ctx.here(), remember))
    ctx.emit(LDP_POST(29, 30, 31, 16))
    ctx.emit(LDP_POST(0, 1, 31, 16))
    ctx.emit(0xfc1b0fe8)
    ctx.emit(B(ctx.here(), open_title + 4))
    at = ctx.here()
    ctx.put(close_title, [B(close_title, at)], 'Title: clear the title background reference on close')
    ctx.emit(ADRP(16, ctx.here(), ctx.state))
    ctx.emit(STRX(31, 16, 0xb8 // 8))
    ctx.emit(STP_PRE(29, 30, 31, -0x30))
    ctx.emit(B(ctx.here(), close_title + 4))

    # Image.OnPopulateMesh tail-call for simple sprites. The title image
    # uses exactly four vertices. Keep them and their triangles unchanged;
    # Clone upper corners (1,2) to vertices 4–7 and mirror UV0.y.
    # Cover the atlas-edge texels with a four-unit overlap.
    simple, mesh_at = 0x17367f0, 0x17367e8
    fin_mesh = fin_backdrop(ctx, simple)
    ctx.site(mesh_at, pack('<I', B(mesh_at, simple)).hex())
    at = ctx.here()
    ctx.put(mesh_at, [B(mesh_at, at)], 'Tall UI: extend the tracked title and Fin backdrops')
    code = []
    put = code.append
    def pc(): return at + 4 * len(code)
    def call(target): put(BL(pc(), target))
    put(ADRP(16, pc(), ctx.state))
    put(0xb9404a11)  # no mesh extension at stock height
    stock_height = len(code); put(0)
    put(LDRX(17, 16, 0xf8 // 8))  # exact Fin star field Image
    put(CMPR(17, 0))
    put(0x54000041)  # b.ne title
    put(B(pc(), fin_mesh))
    put(LDRX(17, 16, 0xb8 // 8))
    z = len(code); put(0)
    put(LDRX(8, 0, 0x48 // 8))  # Graphic.m_RectTransform
    put(CMPR(17, 8))
    mismatch = len(code); put(0)
    put(STP_PRE(19, 30, 31, -16))
    put(MOVX(19, 1))
    put(SUBI(31, 31, 0x70))  # one UIVertex (0x6c bytes)
    call(simple)
    for outer in (False, True):
        for vertex in (1, 2):
            put(MOVX(0, 19))
            put(ADDI(1, 31, 0))
            put(0x52800002 | vertex << 5)  # mov w2, #vertex
            put(MOVX(3, 31))
            call(0x16d5cf0)  # PopulateUIVertex
            if outer:
                put(ADRP(16, pc(), ctx.state))
                put(0xb9404a11)  # ldr w17, [x16,#0x48]
                put(0x52808708)  # mov w8, #1080
                put(0x4b080231)  # sub w17, w17, w8
                put(0x1e230220)  # ucvtf s0, w17
            put(0x1e221002)  # fmov s2, #4.0
            put(0xbd4007e3)  # ldr s3, [sp,#4] (position.y)
            put(0x1e223863)  # fsub s3, s3, s2
            if outer:
                put(0x1e202863)  # fadd s3, s3, s0
                put(0x1e222800)  # fadd s0, s0, s2
            else:
                put(0x1e204040)  # fmov s0, s2
            put(0xbd0007e3)  # str s3, [sp,#4]
            put(0x52a890e8)  # mov w8, #1080.0f bits
            put(0x1e270101)  # fmov s1, w8
            put(0x1e211802)  # fdiv s2, s0, s1
            put(0xbd4033e3)  # ldr s3, [sp,#0x30] (UV0.y)
            put(0x1e223863)  # fsub s3, s3, s2
            put(0xbd0033e3)  # str s3, [sp,#0x30]
            put(MOVX(0, 19))
            put(ADDI(1, 31, 0))
            put(MOVX(2, 31))
            call(0x16d68b0)  # AddVert(UIVertex)
    for triangle in ((4, 6, 7), (7, 5, 4)):
        put(MOVX(0, 19))
        for reg, idx in enumerate(triangle, 1):
            put(0x52800000 | idx << 5 | reg)
        put(MOVX(4, 31))
        call(0x16d6950)  # AddTriangle
    put(ADDI(31, 31, 0x70))
    put(LDP_POST(19, 30, 31, 16))
    put(0xd65f03c0)  # ret
    stock = len(code)
    put(B(pc(), simple))
    code[stock_height] = cbz(17, at + stock_height * 4, at + stock * 4)
    code[z] = cbz(17, at + z * 4, at + stock * 4, wide=True)
    code[mismatch] = 0x54000001 | (stock - mismatch) << 5  # b.ne stock
    for w in code:
        ctx.emit(w)

    # Save/Load shares its canvas with Camp. Title, independent field capture,
    # and the credits Clear Save all need tall chrome; leave Camp alone. Restore
    # the authored height when the tracked window closes.
    prefix = 'ui_common_background_parent_presenter'
    paths = (
        prefix,
        prefix + '/ui_camp_menu_background_presenter/ui_camp_menu_background_01',
        prefix + '/ui_camp_menu_background_presenter/ui_camp_menu_background_02',
        prefix + '/HeaderFooterParent/ui_camp_header_image_presenter',
    )
    for site, original, closing in ((0x24f3024, 0x23013e0, False), (0x24f3344, 0x2301660, True)):
        ctx.site(site, pack('<I', BL(site, original)).hex())
        helpers = [emit_adjustment(path, 'restore_height' if closing else 'height') for path in paths]
        at = ctx.here()
        code = []
        put = code.append
        put(ADRP(16, pc(), ctx.state))
        if closing:
            put(LDRX(17, 16, 0xc0 // 8))
            put(CMPR(17, 0))
            branches = [(len(code), 'ne', 0)]; put(0)
            put(STRX(31, 16, 0xc0 // 8))
        else:
            put(0xb9404a11)  # tall design height, or zero for stock
            branches = [(len(code), 'z', 0)]; put(0)
            put(0x39432811)  # IsCamp
            branches.append((len(code), 'nz', 0)); put(0)
            put(LDRX(17, 0, 0x98 // 8))  # UISaveLoadSelector
            put(cbz(17, pc(), pc() + 16, wide=True))
            put(0xb940d231)  # ldr w17, [x17,#0xd0] (SaveLoadState)
            put(0x71000a3f)  # cmp w17, #2 (Clear Save from CreditScene)
            clear_branch = len(code); put(0)
            put(0x39432011)  # IsCaptureBackground
            put(cbz(17, pc(), pc() + 12) ^ (1 << 24))
            put(LDRX(17, 16, 0xb8 // 8))  # otherwise require active Title
            branches.append((len(code), 'z', 1)); put(0)
            code[clear_branch] = 0x54000000 | (len(code) - clear_branch) << 5  # b.eq chrome
            put(STRX(0, 16, 0xc0 // 8))
        put(STP_PRE(19, 30, 31, -16))
        put(STP_PRE(0, 1, 31, -16))
        put(MOVX(19, 0))
        for helper in helpers:
            put(MOVX(0, 19))
            call(helper)
        put(LDP_POST(0, 1, 31, 16))
        put(LDP_POST(19, 30, 31, 16))
        stock = len(code)
        put(B(pc(), original))
        for j, kind, wide in branches:
            if kind == 'ne':
                code[j] = 0x54000001 | (stock - j) << 5
            else:
                code[j] = cbz(17, at + j * 4, at + stock * 4, wide=bool(wide))
                if kind == 'nz':
                    code[j] ^= 1 << 24  # CBNZ
        for w in code:
            ctx.emit(w)
        ctx.put(site, [BL(site, at)], 'Save/Load: ' + ('restore shared chrome' if closing else 'extend title, field, or Clear Save chrome'))


def title_menu_chrome(ctx, emit_adjustment):
    # UIConfigScene gives Settings and Achievements separate copies of this
    # decorative group. The group stretches to its 1080-high parent with zero
    # sizeDelta; adding the extra rows moves its bottom-anchored FooterImage.
    # The nested HeaderImage has a fixed-height parent, so extend that too.
    chrome = 'ui_common_background_parent_presenter/HeaderFooterParent'
    for closing, entries in (
        (False, ((0x24a1040, 0x2301260, BL, 'Settings'),
                 (0x228ca94, 0x23013e0, BL, 'Achievements'))),
        (True, ((0x24a11a0, 0x2301560, BL, 'Settings'),
                (0x228cb90, 0x2301660, B, 'Achievements'))),
    ):
        helpers = [
            emit_adjustment(chrome, 'restore_stretch_height' if closing else 'stretch_height'),
            emit_adjustment(chrome + '/ui_camp_header_image_presenter',
                            'restore_title_header' if closing else 'height'),
        ]
        for site, original, branch, label in entries:
            ctx.site(site, pack('<I', branch(site, original)).hex())
            start = ctx.here()
            stock = start + (12 if closing else 14) * 4
            ctx.emit(ADRP(16, ctx.here(), ctx.state))
            ctx.emit(0xb9404a11)  # ldr w17, [x16, #0x48], nonzero only if tall
            ctx.emit(cbz(17, ctx.here(), stock))
            if not closing:
                ctx.emit(LDRX(17, 16, 0xb8 // 8))  # active title backdrop
                ctx.emit(cbz(17, ctx.here(), stock, wide=True))
            ctx.emit(STP_PRE(19, 30, 31, -16))
            ctx.emit(STP_PRE(0, 1, 31, -16))
            ctx.emit(MOVX(19, 0))
            for helper in helpers:
                ctx.emit(MOVX(0, 19))
                ctx.emit(BL(ctx.here(), helper))
            ctx.emit(LDP_POST(0, 1, 31, 16))
            ctx.emit(LDP_POST(19, 30, 31, 16))
            assert ctx.here() == stock
            ctx.emit(B(ctx.here(), original))
            ctx.put(site, [branch(site, start)],
                    'Title ' + label + ': ' + ('restore' if closing else 'extend') + ' decorative chrome')


def camp_backdrop(ctx, resize_background, restore_background):
    # These windows pass a 160x90 target to the same blur task, which
    # crops the tall camera to 16:9. Use a separate clone for each owner;
    # the stock textures and non-field Save/Load modes are unchanged.
    clone, destroy = 0x2501d30, 0x2502190
    release, height, create = 0x20da2c0, 0x20d9680, 0x20da270
    set_texture, set_task = 0x1953ef0, 0x2596450
    original_close = 0x2301560
    camp_texture = 0xb0
    # Set/OnClosed call sites, capture image field, and owner state slot.
    tail_owners = (
        (0x258b27c, 0x258b81c, 0xc0, 0xc8, 'Shop'),
        (0x24ec454, 0x24ec6e0, 0xc8, 0xd0, 'Quest'),
        (0x1f562f0, 0x1f56634, 0xc0, 0xd8, 'Fish Collector'),
    )
    field_slot = 0xe0
    bunny_slot = 0xe8
    cooking_slot = 0xf0
    field_open = 0x24f30cc  # UISaveLoadWindow.Open, only called with IsCaptureBackground
    open_owners = ((0x21576b8, camp_texture, 'Camp'),) + tuple(
        (opening, slot, owner) for opening, _, _, slot, owner in tail_owners
    ) + ((field_open, field_slot, 'Field Save/Load'),
         (0x2328ce0, bunny_slot, 'Bunny Race betting'),
         (0x233b020, cooking_slot, 'Cooking Master ingredients'))

    for at, texture_slot, owner in open_owners:
        ctx.site(at, pack('<I', BL(at, set_task)).hex())
        start = ctx.here()
        code, labels, jumps = [], {}, []

        def pc(): return start + len(code) * 4
        def emit(w): code.append(w)
        def call(to): emit(BL(pc(), to))
        def mark(label): labels[label] = pc()
        def branch(label, encoder):
            jumps.append((len(code), label, encoder))
            emit(0)

        emit(ADRP(16, pc(), ctx.state))
        emit(0xb9404a11)  # ldr w17, [x16,#0x48]
        branch('stock', lambda frm, to: cbz(17, frm, to))
        if at == field_open:
            emit(0x39432a71)  # ldrb w17,[x19,#0xca] (UISaveLoadWindow.IsCamp)
            branch('stock', lambda frm, to: cbz(17, frm, to) ^ (1 << 24))
        branch('stock', lambda frm, to: cbz(1, frm, to, wide=True))
        branch('stock', lambda frm, to: cbz(2, frm, to, wide=True))
        emit(STP_PRE(19, 30, 31, -16))
        emit(STP_PRE(2, 3, 31, -16))
        emit(STP_PRE(0, 1, 31, -16))
        emit(LDRX(19, 16, texture_slot // 8))
        branch('create', lambda frm, to: cbz(19, frm, to, wide=True))
        branch('resize', B)
        mark('create')
        emit(MOVX(0, 2))
        call(clone)
        branch('set', lambda frm, to: cbz(0, frm, to, wide=True))
        emit(MOVX(19, 0))
        emit(MOVX(1, 31))
        call(release)
        emit(ADRP(16, pc(), ctx.state))
        emit(0xb9404a11)  # ldr w17, [x16,#0x48]
        emit(0x52800188)  # mov w8,#12 (1920 / 160)
        emit(0x1ac80a21)  # udiv w1,w17,w8 (e.g. 1344 / 12 = 112)
        emit(MOVX(0, 19))
        emit(MOVX(2, 31))
        call(height)
        emit(MOVX(0, 19))
        emit(MOVX(1, 31))
        call(create)
        branch('failed', lambda frm, to: cbz(0, frm, to))
        emit(ADRP(16, pc(), ctx.state))
        emit(STRX(19, 16, texture_slot // 8))
        mark('resize')
        emit(STRX(19, 31, 2))  # replace saved x2=target, keep original task and RawImage
        emit(LDRX(0, 31, 1))
        call(resize_background)
        branch('set', B)
        mark('failed')
        emit(MOVX(0, 19))
        emit(MOVX(1, 31))
        call(destroy)
        mark('set')
        emit(LDP_POST(0, 1, 31, 16))
        emit(LDP_POST(2, 3, 31, 16))
        emit(LDP_POST(19, 30, 31, 16))
        mark('stock')
        emit(B(pc(), set_task))
        for idx, label, encoder in jumps:
            code[idx] = encoder(start + 4 * idx, labels[label])
        for w in code:
            ctx.emit(w)
        ctx.put(at, [BL(at, start)], f'{owner}: capture into a private tall RenderTexture')


    # The task samples the full camera only for an owned tall clone.
    at, original = 0x22c48b8, 0x1e2202a0  # scvtf s0,w21
    ctx.site(at, pack('<I', original).hex())
    start = ctx.here()
    full = start + (3 + 3 * len(open_owners) + 1) * 4
    stock = full + 6 * 4
    ctx.emit(ADRP(16, start, ctx.state))
    ctx.emit(LDRX(17, 23, 0xe8 // 8))  # task's destination texture
    ctx.emit(cbz(17, ctx.here(), stock, wide=True))
    for _, slot, _ in open_owners:
        ctx.emit(LDRX(15, 16, slot // 8))
        ctx.emit(CMPR(17, 15))
        ctx.emit(0x54000000 | ((full - ctx.here()) // 4) << 5)  # b.eq full
    ctx.emit(B(ctx.here(), stock))
    assert ctx.here() == full
    ctx.emit(LDRX(0, 23, 0x100 // 8))
    ctx.emit(0x1e2e1000)  # fmov s0,#1.0
    ctx.emit(0x1e2e1001)  # fmov s1,#1.0
    ctx.emit(0x2f00e402)  # movi d2,#0
    ctx.emit(0x2f00e403)  # movi d3,#0
    ctx.emit(B(ctx.here(), 0x22c4944))
    assert ctx.here() == stock
    ctx.emit(original)
    ctx.emit(B(ctx.here(), at + 4))
    ctx.put(at, [B(at, start)], 'Owned tall targets: preserve the full camera image in the blur capture')

    at = 0x215baa8  # UICampWindow.OnClosed calls its superclass
    ctx.site(at, pack('<I', BL(at, original_close)).hex())
    start = ctx.here()
    ctx.emit(ADRP(16, start, ctx.state))
    ctx.emit(LDRX(17, 16, camp_texture // 8))
    ctx.emit(cbz(17, ctx.here(), start + 19 * 4, wide=True))
    ctx.emit(STP_PRE(19, 30, 31, -16))
    ctx.emit(MOVX(19, 17))
    ctx.emit(STRX(31, 16, camp_texture // 8))
    ctx.emit(BL(ctx.here(), original_close))
    # x19 was the window in the calling function, saved in our frame.
    ctx.emit(LDRX(0, 31))
    ctx.emit(LDRX(0, 0, 0x238 // 8))
    ctx.emit(MOVX(1, 31))
    ctx.emit(MOVX(2, 31))
    ctx.emit(BL(ctx.here(), set_texture))
    # Leave a stock rectangle if the next capture cannot create its clone.
    ctx.emit(LDRX(0, 31))
    ctx.emit(LDRX(0, 0, 0x238 // 8))
    ctx.emit(BL(ctx.here(), restore_background))
    ctx.emit(MOVX(0, 19))
    ctx.emit(MOVX(1, 31))
    # Tail-call Destroy, returning to the original Camp OnClosed code.
    # Place stock continuation after the tall return.
    ctx.emit(LDP_POST(19, 30, 31, 16))
    ctx.emit(B(ctx.here(), destroy))
    ctx.emit(B(ctx.here(), original_close))
    ctx.put(at, [BL(at, start)], 'Camp: retire its private capture when the menu closes')

    # Each OnClosed tail-calls the base with its window still in x0.
    # Detach and retire only that window's capture and restore the stock
    # rectangle, including when the next open falls back to the stock texture.
    for _, at, raw_field, slot, owner in tail_owners:
        ctx.site(at, pack('<I', B(at, original_close)).hex())
        start = ctx.here()
        ctx.emit(ADRP(16, start, ctx.state))
        ctx.emit(LDRX(17, 16, slot // 8))
        stock = start + 20 * 4
        ctx.emit(cbz(17, ctx.here(), stock, wide=True))
        ctx.emit(STP_PRE(19, 30, 31, -16))
        ctx.emit(STP_PRE(0, 1, 31, -16))
        ctx.emit(MOVX(19, 17))
        ctx.emit(STRX(31, 16, slot // 8))
        ctx.emit(LDRX(0, 0, raw_field // 8))
        ctx.emit(MOVX(1, 31))
        ctx.emit(MOVX(2, 31))
        ctx.emit(BL(ctx.here(), set_texture))
        ctx.emit(LDRX(0, 31))
        ctx.emit(LDRX(0, 0, raw_field // 8))
        ctx.emit(BL(ctx.here(), restore_background))
        ctx.emit(MOVX(0, 19))
        ctx.emit(MOVX(1, 31))
        ctx.emit(BL(ctx.here(), destroy))
        ctx.emit(LDP_POST(0, 1, 31, 16))
        ctx.emit(LDP_POST(19, 30, 31, 16))
        ctx.emit(B(ctx.here(), original_close))
        assert ctx.here() == stock
        ctx.emit(B(ctx.here(), original_close))
        ctx.put(at, [B(at, start)], f'{owner}: retire its private capture when the menu closes')

    # Run after native close/hide calls, replay the next original load,
    # and release only the corresponding selector's private capture.
    for at, original, raw_field, slot, owner in (
        (0x24f3348, LDRX(8, 20), 0xa0, field_slot, 'Field Save/Load'),
        (0x2329ea0, LDRX(0, 19, 0xf0 // 8), 0x128, bunny_slot, 'Bunny Race Hide'),
        (0x232a06c, LDRX(0, 19, 0xf0 // 8), 0x128, bunny_slot, 'Bunny Race ForceHide'),
        (0x233b58c, LDRX(8, 19), 0xc0, cooking_slot, 'Cooking Master Hide'),
        (0x233b6d4, LDRX(8, 19), 0xc0, cooking_slot, 'Cooking Master ForceHide'),
    ):
        ctx.site(at, pack('<I', original).hex())
        start = ctx.here()
        ctx.emit(ADRP(16, start, ctx.state))
        ctx.emit(LDRX(17, 16, slot // 8))
        ctx.emit(cbz(17, ctx.here(), start + 16 * 4, wide=True))
        ctx.emit(STP_PRE(21, 30, 31, -16))
        ctx.emit(MOVX(21, 17))
        ctx.emit(STRX(31, 16, slot // 8))
        ctx.emit(LDRX(0, 19, raw_field // 8))
        ctx.emit(MOVX(1, 31))
        ctx.emit(MOVX(2, 31))
        ctx.emit(BL(ctx.here(), set_texture))
        ctx.emit(LDRX(0, 19, raw_field // 8))
        ctx.emit(BL(ctx.here(), restore_background))
        ctx.emit(MOVX(0, 21))
        ctx.emit(MOVX(1, 31))
        ctx.emit(BL(ctx.here(), destroy))
        ctx.emit(LDP_POST(21, 30, 31, 16))
        ctx.emit(original)
        ctx.emit(B(ctx.here(), at + 4))
        ctx.put(at, [B(at, start)], f'{owner}: retire its private capture')


FRAME_STEP = {'d214ba64d902aeda4197b57debabd01b': screen_shape}

BINDING = [{
    'what': 'native tall-screen Unity render targets and world camera',
    'now': 'build', 'goal': 'game', 'purpose': 'rendering',
    'why': 'the exact Switch resolution, allocation, and Cinemachine call sites are specific to this game build',
    'blocked': 'not function-matched; a new game version must be checked independently',
}]
