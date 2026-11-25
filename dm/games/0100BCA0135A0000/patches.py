# Taiko no Tatsujin: Rhythm Festival (0100BCA0135A0000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those
# of a module's NSO build ID (FRAME_STEP as described there, "Registered patches"). The game has no frame step stubs: FRAME_STEP only gives its main the state page that the screen
# shape stubs read (DM_SCREEN_ASPECT, as for Animal Crossing: New Horizons); with DM_SCREEN_ASPECT unset or 0 the page stays zero and every stub runs the original instructions.
# The US release (01006B0016AD6000) loads this module and registers its build with its own table (BUILDS).
#
# Layout: constants (state page, camera fields, tbz bits), per-build addresses (BUILDS), the tables of the native screen size, the orthographic cameras and the edge-anchored UI
# (top_lift.c's rules), one function per concern for the sites (screen_shape calls them in the order of the patch area), FRAME_STEP / BINDING, and at the end the words of the
# two C functions, which gen_native.py writes from top_lift.c and bg_cover.c (`python3 gen_native.py --check` compiles them again and compares)
import struct
from nso_asm import ADDI, ADRP, B, BL

JP, US = 'd907a35c2f7f0ed627c1a930cbe0803e', '7150d78c454e7c55b3deba8b86386c8e'   # NSO build IDs: JP 7.0.0 (v2097152), US 6.2.0 (v2031616)

# ---- constants ----
# The state page (ctx.state, loader.c): the output's heights and the mask of the sites that DM_SCREEN_ASPECT_SITES switches (default all ones)
ST_HEIGHT, ST_HANDHELD, ST_SITES = 0x48, 0x4c, 0x54   # the output's height (1200 at 1.6; 0 = 16:9), the handheld's (800), the sites mask
# The sites mask's bits: one per hook, so a site can be switched off (DM_SCREEN_ASPECT_SITES=<mask>) to find which one moved what
SITE_ORTHO_CALLERS = (1, 2, 3)   # the three SetOrtho callers that render: their view is moved by the helper bar camera's shift (the first caller, bit 0, has none)
SITE_SCENE = SITE_BAND = 4       # scene controllers' and loading screens' edge UI; gameplay's top band
SITE_UI = 5                      # song select footer, popups, the story's dimming sprite
SITE_COVER = 6                   # bg_cover
# Unity's Camera object (offsets in the instance)
CAM_CULLING_MASK, CAM_ORTHO_SIZE, CAM_ASPECT = 0x414, 0x438, 0x4c8
SETRESOLUTION = 0x3551c0         # Screen.SetResolution's clamp (JP 7.0.0 offsets of Unity's native code; a build adds BUILDS' d)
RET = 0xD65F03C0


def _movz(rd, imm): return 0x52800000 | imm << 5 | rd
def _cmpi(rn, imm): return 0x7100001F | imm << 10 | rn << 5
def _ldrw(rt, rn, off): return 0xB9400000 | (off // 4) << 10 | rn << 5 | rt
def _ldrs(rt, rn, off): return 0xBD400000 | (off // 4) << 10 | rn << 5 | rt   # ldr s<rt>, [x<rn>, #off]
def _cbz(rt, frm, to): return 0x34000000 | ((to - frm) >> 2 & 0x7FFFF) << 5 | rt
def _cbnz(rt, frm, to): return 0x35000000 | ((to - frm) >> 2 & 0x7FFFF) << 5 | rt
def _tbz(bit, rt, frm, to): return 0x36000000 | bit << 19 | ((to - frm) >> 2 & 0x3FFF) << 5 | rt   # tbz w<rt>, #bit, to
def _hex(w): return struct.pack('<I', w).hex()   # a site's original word, as site() takes it


# ---- per build ----
# d = a build's native (Unity) code's offset from JP 7.0.0's, `fns` the eight IL2CPP methods of top_lift's table (Component.get_transform, Transform.get_childCount,
# Transform.GetChild, Object.get_name, Transform.get_localPosition, Transform.set_localPosition, Transform.get_localScale, Transform.set_localScale; JP 7.0.0's from Il2CppDumper),
# `hooks`, band / story / refresh the other IL2CPP sites. Unity's native code is identical in the builds, shifted by one constant; the game's IL2CPP code is laid out differently
# in each (US 6.2.0: no shift fits), its addresses found by matching function bodies. Every site checks its original word, so a wrong table
# stops the rewrite
def _hooks(scene, trans, footer, popup, page):   # one build's hook sites (site, original word, the page an original adrp loads or 0, the register holding the controller, rules table (_RULES), sites bit, why); x0..x18 are dead at each, x30 saved in the frame
    return (
        (scene, 0x52800028, 0, 19, 'scene', SITE_SCENE, 'SceneControllerBase.OnInitialize: the edge-anchored UI moved to the taller screen\'s edges (bit 4)'),   # mov w8, #1 (state 1)
        (trans, 0xB001D1B4 if page == 0x5946000 else 0x9001D074, page, 19, 'scene', SITE_SCENE, 'TransitionSceneControllerBase.OnInitialize: the loading screens\' edge-anchored UI (bit 4)'),   # adrp x20, <page>
        (footer, 0xAA1303E0, 0, 19, 'footer', SITE_UI, 'FooterController.OnInitialize: the song select footer fitted to the unwidened helper bar camera (bit 5)'),   # mov x0, x19
        (popup, 0xF9400288, 0, 20, 'popup', SITE_UI, 'PresenterWindowControllerBase.Initialized: the popups\' full-width footer moved to the taller screen\'s bottom (bit 5)'),   # ldr x8, [x20]
    )


BUILDS = {
    JP: dict(d=0, fns=(0x28cb7c0, 0x241bef0, 0x241c2f0, 0x28df080, 0x24079b0, 0x2407ae0, 0x241abe0, 0x241ac90),
             hooks=_hooks(0x1e42b1c, 0x1f11e24, 0x1f2fb54, 0x21231ec, 0x5946000), band=0x1d3bfc0, story=0x1b55ee0, refresh=0x1b5605c),   # 7.0.0 (v2097152)
    US: dict(d=-0x7BF0, fns=(0x2853150, 0x23ca220, 0x23ca620, 0x2866950, 0x23b5ce0, 0x23b5e10, 0x23c8f10, 0x23c8fc0),
             hooks=_hooks(0x1ee9e2c, 0x1e756d4, 0x35379d4, 0x1fcd0dc, 0x5881000), band=0x1ce7e40, story=0x1b2c140, refresh=0x1b2c2bc),   # US 6.2.0 (v2031616)
}


# ---- native screen size ----
# A screen taller than 16:9 keeps the picture's width (1920) and takes the height of the screen's ratio (ST_HEIGHT, 1200 at 1.6), the handheld size 1280 x 2/3 of it (ST_HANDHELD, 800).
# Unity 2020.3.26f1 takes its screen size from the operation mode (GetScreenSize, main+0x908528: 1280x720, docked 1920x1080) and sizes its back buffer and swapchain to 1920x1080; the
# game itself calls Screen.SetResolution(1920, 1080), whose clamp keeps the height at 1080 or less. Each site is a movz of a height into a register the next instructions only read:
# its stub loads the height from the state page into that register, or the original value at 0
_HEIGHTS = (
    # (site, register, original height, state offset, reason)
    (0x908540, 22, 720, ST_HANDHELD, 'Unity GetScreenSize: the handheld height (its default)'),
    (0x9085e4, 9, 1080, ST_HEIGHT, 'Unity GetScreenSize: the docked height'),
    (0x9085fc, 8, 1080, ST_HEIGHT, 'Unity GetScreenSize: the docked height (boost mode)'),
    (0x907440, 3, 1080, ST_HEIGHT, 'graphics device init: the back buffer\'s height'),
    (0x91e5a0, 21, 1080, ST_HEIGHT, 'swapchain colour textures: their height'),
    (0x91e820, 2, 1080, ST_HEIGHT, 'swapchain depth texture: its height'),
)

# ---- orthographic cameras ----
# Unity keeps orthographicSize (the half-height, CAM_ORTHO_SIZE) and shows size x aspect (CAM_ASPECT) to each side, so a taller screen loses its sides (the 2D scenes are all
# orthographic, size 540). The four callers of SetOrtho (main+0x5fb348: the projection matrix, its near / far variant and two others) load the size with ldr s3, [xN, #0x438]; each calls
# a stub instead that returns size x max(1, (16/9) / aspect) while the output's height is set: the whole 16:9 width with more above and below. A 16:9 camera, or one rendering into a
# texture of 16:9 or wider, is unchanged. The stub uses s6 / s7 / w8, dead at the four; all four callers save x30.
# The controls helper bar (Back / Confirm) is a Screen Space - Camera canvas drawn by its own OperationCamera, whose culling mask is layer 23 alone (0x800000; in every scene of
# data.unity3d that camera draws only the bar's canvas). The canvas is laid out in the screen's pixels (1920x1200) and scaled to fill orthographicSize's height (1080 world units,
# 0.9), and the bar sits 54 canvas units (0.1 of 540) above its bottom edge. For that camera the view is not widened: the canvas fills the screen 1:1, and the view moves up by
# size x (1 - 1/k) so its bottom is the bar's (Mac, Taiko Mode menu: widened and moved by size x (k - 1), the bar was 0.9 of the screen's width and still 54 units high). The first
# stub leaves that shift in s7 (0 for every other camera), and a second one, called in place of the instruction before bl SetOrtho (after left / right are computed), adds it to
# bottom (s2) and top (s3) for the three callers that render, not for the camera's own projection matrix (the first, Camera +0x11c): the canvas is placed at the view's centre
# through that one (ViewportToWorldPoint), so moving it moved the canvas along and the bar stayed 60 units high (Mac, DM_SCREEN_ASPECT_SITES trials: 0xf high, 0xe at the bottom
# edge). Bits SITE_ORTHO_CALLERS of the sites mask switch those three
_ORTHO_CALLERS = (
    # (the ldr of the size, its base register, the instruction before bl SetOrtho (None: not patched), its word: fmul s1, s3, s1 / mov x0, x20 / mov x0, x19)
    (0x7d2d6c, 20, None, None),
    (0x7d2dfc, 0, 0x7d2e10, 0x1E210861),
    (0x7c8238, 19, 0x7c8254, 0xAA1403E0),
    (0x7d7c30, 21, 0x7d7c48, 0xAA1303E0),
)

# ---- the edge-anchored UI: the rule tables of top_lift.c ----
# top_lift.c (one position-independent function, its words at the end of this file) runs from the hooks of _hooks, each with its table: SceneControllerBase's
# ISceneController.OnInitialize (main+0x1e42a30) once per controller, where the state +0x20 is found 0 and set to 1 (main+0x1e42b1c); TransitionSceneControllerBase's
# (main+0x1f11e00, the loading screens; it never calls the first) after its virtual OnInitialize; FooterController.OnInitialize (main+0x1f2fb20, once per footer);
# PresenterWindowControllerBase.Initialized (main+0x21231b0, each time a popup is set up) after its virtual OnInitialized; LetterBoxCamera.Start (the story scenes);
# EnsoPartsBase.OnInitialize (gameplay's top band)
SCALE, COVER, GROW, BAND, FIT, GE, LE, NEAR = 0x40, 0x41, 0x42, 0x43, 0x44, 1, 2, 3   # the moves beside an int (that many extra half-heights) and the guards, as in top_lift.c
# A rule: (path, move in extra half-heights or SCALE, COVER, GROW, BAND, FIT, guard on the original local y, g): see top_lift.c. Positions are local (a RectTransform under a
# plain Transform: its anchoredPosition), from the scene assets. The guards keep a name that is also used lower on other screens where it is

# the scene controllers' rules (and the loading screens')
_SCENE_RULES = (
    # the hub (MainMenu, level70): its cameras are ortho 1080, twice the world units. The Drum Lvl panel, the coin counter, the Drum Lvl bubble and the place name ("Omiko
    # City", on the map, top left) at the top; the message box at the bottom (not textwindow_Pos itself: it holds the TextCamera)
    ('MainMenu:denden', 2, GE, 440), ('MainMenu:player_coin', 2, 0, 0), ('MainMenu:denden_fukidashi', 2, 0, 0), ('MainMenu:textwindow_Pos/window', -2, 0, 0),
    ('MainMenu:MainContents/entrance_focus_group/modepanel/main_menu_panel', 2, 0, 0),
    # the headers: the Entrance screens' title bar (panel_title, Mission's panel_title_02, Matching's TitleBG at 540), title text (Text_title ~40 screens, the shops'
    # TitleText at ~468) and Drum Lvl panel (517); the options' and menus' titles (title also names the title screen at 0 and the minigame pause window's at 412)
    *((n, 1, GE, 440) for n in ('panel_title', 'panel_title_02', 'TitleBG', 'Text_title', 'TitleText', 'denden', 'title', 'title_text', 'Title_Text')),
    # the results screens' song title band (song_info at 0, its children at ~470), the rank match result's timer, the page numbers (100, 114), difficulty select's rule
    # plate, the star counter (132, 145), the title screen's version, the deck edit comment (92; others <= 0), the toy war timer (107), Ninja race's course icon,
    # progress bar (other enso scenes have them at 140 / 0) and its top cover band
    ('song_info', 1, 0, 0), ('bgfilter', 1, 0, 0),
    *((n, 1, GE, 440) for n in ('Timer', 'page_number', 'BattleRuleText', 'StarSignNum', 'versionText', 'comment_box', 'result_timer', 'icon_course', 'progress_bar')),
    # My Room's Memories (level78): its Drum Lvl panel and coin counter are in the board window (sugoroku_window at -160; 678 / 606 in it)
    ('Memories:sugoroku_window/denden', 1, GE, 440), ('Memories:sugoroku_window/player_coin', 1, GE, 440),
    # song select: the tab bar (Library / Taiko Music Pass / Separately Sold Songs, Theme at 460), the title plate under it (TitleBase: its root, at 0; its child Pos is
    # animated), the Filter Songs / sort panels (Session's top bar), Multi's genre / difficulty bars and the trophy progress under the bar
    ('TitleBase', 1, 0, 0),
    *(('SongSelectCommon/' + n, 1, 0, 0) for n in ('Theme', 'submenu_panels', 'sort_genre', 'sort_diff', 'Progress')),
    # the online match loading screen's (116) title and rule at the top of its common_view
    *(('EnsoOnlineTransition:common_view/' + n, 1, GE, 440) for n in ('text_title', 'text_rule')),
    # at the bottom: the loading screens' "loading" icon (-460; in the genre ones, 57..63, under loading_XX_<genre>/loading_common), the Entrance screens' navigation
    # bubble on the helper bar (-386; 86 and 150 have it at y > 0)
    ('loading_icon', -1, LE, -400), ('*/loading_common/loading_icon', -1, LE, -400), ('entrance_navi', -1, LE, -300),
    # the genre loading screens' Joy-Con notice (loading_caution at -620, off-screen at 16:9; its clips pin loading_caution and Up_Pos, not Up_Pos's children)
    *(('*/loading_common/loading_caution/Up_Pos/' + n, -1, NEAR, 0) for n in ('01_Pos', '02_Pos', 'Text (TMP)')),
    # gameplay's stage below the lanes (EnsoSingleBackground, levels 1..6, 127, 135...: its controller GameObject is Root, no other controller has that name): the 1920x540
    # pictures (bg and the fever bg_fever_a_0X at -270 / -272, a collab's at -270, bg_fever_a_03 at -305) grow by k about the middle so they reach the bottom edge, the
    # strip they stand on (bg_dai, collabo_*_bg_dai at -516) and the dancers on it (EnsoSingle's dancer_mgr at 0) move down to it, as at 16:9
    ('Root:*', GROW, NEAR, -270), ('Root:*', GROW, NEAR, -305), ('Root:*', -1, NEAR, -516), ('EnsoSingle:dancer_mgr', -1, NEAR, 0),
)
# the song select footer (levels 95, 106, 129, 141, 153: the only footers with a child Root, a plain Transform of sprites at -494 under the helper bar camera, whose view
# is not widened but moved up by 540 x (1 - 1/k)): scaled by 1/k about the camera's centre (position and scale), which undoes the unwidened view's magnification and puts
# the bar's bottom on the screen's (assumes the camera shift, sites bits 1..3). The canvas footers (every other screen) have no Root and are already right.
# The results screens' Improvement Support bubble (guide_training, the mascot by -/+: a sprite and a world TextMeshPro under the footer canvas's pattern_10 at -480, on
# a layer the widened main camera draws, not the helper bar's) is put back at its 16:9 size and height above the bar
_FOOTER_RULES = (('Root', SCALE, 0, 0), ('Canvas/pattern_10/guide_training', FIT, 0, -480))
# the popups (the Presenter windows of resources.assets, drawn by the global Presenter cameras, widened as any other): the full-width footers (operation_presenter at -480:
# Controller Settings' style_setting_window, download_progress; -495) and the Controller Settings help band that sits on it (under playstyle_select_<n>P). The other
# popups' footers are inside their window (-435, -386...). A popup can be set up again when it is shown again: the guards match only the original y
_POPUP_RULES = (('operation_presenter', -1, NEAR, -480), ('operation_presenter', -1, NEAR, -495), ('*/text_help', -1, NEAR, -375), ('*/help_window', -1, NEAR, -375))
# the story scenes (Utage's Adventure prefab of resources.assets): its SpriteCamera (a LetterBoxCamera, ortho 540, widened as any other) draws the dimming sprite in front
# of the scene behind, a White 1920x1080 child named sprite, which leaves the extra rows above and below undimmed: scaled by k to cover
_STORY_RULES = (('sprite', COVER, 0, 0),)
# gameplay's top band (the DonBG scenes 7..18, 118, 136, 119, 137; DonBGController on bg at +402 or, for double play's second player, -402 / -400): the pattern (276..280 high
# about bg's y, under bg/Transform/<a scroll node>) stretched in y to the screen's edge, its inner edge kept on the lane. The decorations on it stay. Free training's (149)
# has no DonBGController and stays
_BAND_RULES = tuple(('bg:Transform/*/' + n, BAND, 0, 138) for n in ('bg_Pos', 'Normalbg', 'Feverbg', 'Bg', 'Bg_fever', 'effect_Pos'))
_RULES = dict(scene=_SCENE_RULES, footer=_FOOTER_RULES, popup=_POPUP_RULES, story=_STORY_RULES, band=_BAND_RULES)   # by the name _hooks uses; the tables' order in the patch area


def _rules_words(rules):   # top_lift.c's rule table: int8 move, uint8 guard, int16 g, the path, NUL; an empty path ends it
    b = b''.join(struct.pack('<bBh', m, g, v) + n.encode() + b'\0' for n, m, g, v in rules) + b'\0' * 5
    b += b'\0' * (-len(b) % 4)
    return struct.unpack(f'<{len(b) // 4}I', b)


def _words(blob): return [int(blob[i:i + 8], 16) for i in range(0, len(blob), 8)]


# ---- the sites, one function per concern; each takes the rewriter's ctx (here, emit, site, put, state) ----
def _load_state(ctx, reg, off):   # reg = the state page's word at off
    ctx.emit(ADRP(reg, ctx.here(), ctx.state)); ctx.emit(_ldrw(reg, reg, off))


def _call_rules(ctx, code, table):   # top_lift(x0 the controller or this, x1 main's base, x2 the table, x3 the state page)
    ctx.emit(ADRP(1, ctx.here(), 0))   # main's base
    ctx.emit(ADRP(2, ctx.here(), table)); ctx.emit(ADDI(2, 2, table & 0xFFF))
    ctx.emit(ADRP(3, ctx.here(), ctx.state))
    ctx.emit(BL(ctx.here(), code))


def _screen_heights(ctx, d):
    for at, rd, orig, off, why in _HEIGHTS:
        at += d
        ctx.site(at, _hex(_movz(rd, orig)))
        start = ctx.here(); _load_state(ctx, rd, off)
        ctx.emit(_cbnz(rd, ctx.here(), ctx.here() + 8)); ctx.emit(_movz(rd, orig))   # 0 = 16:9: the original height
        ctx.emit(B(ctx.here(), at + 4))
        ctx.put(at, [B(at, start)], why)


def _set_resolution(ctx, d):
    # Screen.SetResolution (main+0x3551c0..dc) clamps the size it stores (+0xc0 / +0xc4) to 1920x1080: with the output's height set the width stays capped at 1920 and the height
    # follows it, width x the output's height / 1920 (the game's 1920x1080 → 1920x1200, 1280x720 → 1280x800), both kept in w21 / w20 for the "standard size" compares after it.
    # w8 / w9 are dead there (a call follows)
    at = SETRESOLUTION + d
    orig = [_cmpi(21, 1920), 0x5280F008, 0x1A88B2A8, _cmpi(20, 1080), 0xB900C268, 0x52808708, 0x1A88B288, 0xB900C668]
    clamp = (0x5280F008,   # mov w8, #1920
             0x6B0802BF,   # cmp w21, w8
             0x1A88B2B5,   # csel w21, w21, w8, lt
             0xB900C275,   # str w21, [x19, #0xc0]
             0x1B097EB4,   # mul w20, w21, w9
             0x1AC80A94,   # udiv w20, w20, w8
             0xB900C674)   # str w20, [x19, #0xc4]
    ctx.site(at, struct.pack('<8I', *orig).hex())
    start = ctx.here(); _load_state(ctx, 9, ST_HEIGHT)
    ctx.emit(_cbz(9, ctx.here(), ctx.here() + 4 * (len(clamp) + 2)))   # 0 = 16:9: the original clamp, after the 7 words and the branch below
    for w in clamp: ctx.emit(w)
    ctx.emit(B(ctx.here(), at + 32))
    for w in orig: ctx.emit(w)
    ctx.emit(B(ctx.here(), at + 32))
    ctx.put(at, [B(at, start)], 'Screen.SetResolution: the height follows the width in the screen\'s shape')
    # Its "standard size" compares (1280 x 720 and 1920 x 1080 clear the custom-size flag +0xc9) take the heights of the screen's shape, so the docked / handheld switch still
    # resizes. w8 (main+0x355204) and w9 (main+0x35521c) are written right after
    for at, rd, orig, off in ((0x355204 + d, 8, 720, ST_HANDHELD), (0x35521c + d, 9, 1080, ST_HEIGHT)):
        ctx.site(at, _hex(_cmpi(20, orig)))
        start = ctx.here(); _load_state(ctx, rd, off)
        ctx.emit(_cbnz(rd, ctx.here(), ctx.here() + 8)); ctx.emit(_movz(rd, orig))
        ctx.emit(0x6B00029F | rd << 16)   # cmp w20, w<rd>
        ctx.emit(B(ctx.here(), at + 4))
        ctx.put(at, [B(at, start)], f'Screen.SetResolution: {1280 if orig == 720 else 1920} x the screen\'s height is a standard size')


def _ortho(ctx, d):
    for bit, (at, rn, pre, pre_word) in enumerate(_ORTHO_CALLERS):
        at += d; ld = _ldrs(3, rn, CAM_ORTHO_SIZE)
        ctx.site(at, _hex(ld))
        stub = (_ldrs(6, rn, CAM_ASPECT),   # ldr s6, [xN, #0x4c8]   aspect
                0x5291C708, 0x72A7FC68,     # mov w8, #0x3fe38e39    16/9
                0x1E270107,                 # fmov s7, w8
                0x1E2618E7,                 # fdiv s7, s7, s6
                0x1E2E1006,                 # fmov s6, #1.0
                0x1E2668E7,                 # fmaxnm s7, s7, s6      k
                _ldrs(6, rn, CAM_ORTHO_SIZE),   # ldr s6, [xN, #0x438]   size
                0x1E2708C3,                 # fmul s3, s6, s7        size x k
                _ldrw(8, rn, CAM_CULLING_MASK),   # ldr w8, [xN, #0x414]   culling mask
                0x7160011F,                 # cmp w8, #0x800000      layer 23 alone: the OperationCamera
                0x540000A1,                 # b.ne no shift
                0x1E2718C7,                 # fdiv s7, s6, s7
                0x1E2738C7,                 # fsub s7, s6, s7        the shift, size x (1 - 1/k)
                0x1E2040C3,                 # fmov s3, s6            not widened
                RET,
                0x2F00E407)                 # no shift: movi d7, #0
        start = ctx.here(); ctx.emit(ld); ctx.emit(0x2F00E407)   # movi d7, #0   no shift
        _load_state(ctx, 8, ST_HEIGHT)
        ctx.emit(_cbz(8, ctx.here(), ctx.here() + 4 * len(stub)))   # 0 = 16:9: the size as it is, no shift (the movi d7 at the stub's end)
        for w in stub: ctx.emit(w)
        ctx.emit(RET)
        ctx.put(at, [BL(at, start)], f'SetOrtho caller: the orthographic size widened to keep 16:9\'s sides (x{rn})')
        if bit not in SITE_ORTHO_CALLERS: continue
        pre += d
        ctx.site(pre, _hex(pre_word))
        start = ctx.here(); ctx.emit(pre_word)
        _load_state(ctx, 8, ST_SITES)   # the conversion sites (DM_SCREEN_ASPECT_SITES): bit <caller> moves this caller's view
        ctx.emit(_tbz(bit, 8, ctx.here(), ctx.here() + 12))   # tbz w8, #bit, ret
        ctx.emit(0x1E272842); ctx.emit(0x1E272863)   # fadd s2, s2, s7; fadd s3, s3, s7
        ctx.emit(RET)
        ctx.put(pre, [BL(pre, start)], f'SetOrtho caller: bottom and top moved by the helper bar camera\'s shift (x{rn})')


def _top_lift_hooks(ctx, build):
    # The edge-anchored UI (top_lift.c): moved by the extra half-height when its controller initializes. The IL2CPP sites come from the build's table (not the native shift d).
    # Returns the function's address and its rule tables' (by name)
    code = ctx.here()
    for w in _words(_TOP_LIFT): ctx.emit(w)
    tables = {}
    for name, rules in _RULES.items():
        tables[name] = ctx.here()
        for w in build['fns'] + _rules_words(rules): ctx.emit(w)
    for at, orig, page, reg, rules, bit, why in build['hooks']:
        ctx.site(at, _hex(orig))
        start = ctx.here(); _load_state(ctx, 8, ST_SITES)
        ctx.emit(_tbz(bit, 8, ctx.here(), ctx.here() + 28))   # tbz w8, #bit, orig (+7)
        ctx.emit(0xAA0003E0 | reg << 16)   # mov x0, x<reg>   the controller
        _call_rules(ctx, code, tables[rules])
        ctx.emit(ADRP(orig & 31, ctx.here(), page) if page else orig)   # orig (an adrp is rebuilt for the stub's pc)
        ctx.emit(B(ctx.here(), at + 4))
        ctx.put(at, [B(at, start)], why)
    return code, tables


def _story_and_band(ctx, build, code, tables):
    # LetterBoxCamera.RefreshCamera (main+0x1b56040) sets its camera's viewport to the 1920x1080 picture's shape inside the screen, inset by its padding (+0x44 / +0x48), so the
    # story scenes' cameras drew nothing in the extra rows. With the output's height set the padding it reads is 0: the viewport is the whole screen, and the size stub
    # above widens it as any other camera (its orthographicSize stays the picture's 540). x8 is written after it. No bit of the sites mask
    at = build['refresh']
    ctx.site(at, _hex(0x2D489804))   # ldp s4, s6, [x0, #0x44]
    start = ctx.here(); ctx.emit(0x2D489804)
    _load_state(ctx, 8, ST_HEIGHT)
    ctx.emit(_cbz(8, ctx.here(), ctx.here() + 12))
    ctx.emit(0x2F00E404); ctx.emit(0x2F00E406)   # movi d4, #0; movi d6, #0
    ctx.emit(B(ctx.here(), at + 4))
    ctx.put(at, [B(at, start)], 'LetterBoxCamera.RefreshCamera: no letterbox on the taller screen (the story scenes fill it)')
    # LetterBoxCamera.Start (main+0x1b55ee0: mov w8, #1; strb w8, [x0, #0x68]; ret, no frame): the story's dimming sprite. Its stub saves x30 and this
    at = build['story']
    ctx.site(at, _hex(0x52800028))
    start = ctx.here()
    ctx.emit(0xA9BE7BFD)   # stp x29, x30, [sp, #-0x20]!
    ctx.emit(0xF9000BE0)   # str x0, [sp, #0x10]   this
    _load_state(ctx, 8, ST_SITES)
    ctx.emit(_tbz(SITE_UI, 8, ctx.here(), ctx.here() + 24))   # tbz w8, #5, restore (+6)
    _call_rules(ctx, code, tables['story'])
    ctx.emit(0xF9400BE0)   # restore: ldr x0, [sp, #0x10]
    ctx.emit(0xA8C27BFD)   # ldp x29, x30, [sp], #0x20
    ctx.emit(0x52800028)   # orig: mov w8, #1
    ctx.emit(B(ctx.here(), at + 4))
    ctx.put(at, [B(at, start)], 'LetterBoxCamera.Start: the story scenes\' dimming sprite scaled to cover the taller screen (bit 5)')
    # EnsoPartsBase.OnInitialize (main+0x1d3bfc0: ret, the virtual one Awake calls, overridden by neither DonBGController nor most parts): this is the part
    at = build['band']
    ctx.site(at, _hex(RET))
    start = ctx.here()
    ctx.emit(0xA9BF7BFD)   # stp x29, x30, [sp, #-0x10]!
    _load_state(ctx, 8, ST_SITES)
    ctx.emit(_tbz(SITE_BAND, 8, ctx.here(), ctx.here() + 24))   # tbz w8, #4, restore (+6)
    _call_rules(ctx, code, tables['band'])
    ctx.emit(0xA8C17BFD)   # restore: ldp x29, x30, [sp], #0x10
    ctx.emit(RET)          # orig: ret
    ctx.put(at, [B(at, start)], 'EnsoPartsBase.OnInitialize: gameplay\'s top band stretched to the taller screen\'s edge (bit 4)')


def _bg_cover_hooks(ctx, d):
    # The full-screen backgrounds (bg_cover.c, compiled as top_lift.c): a sprite of one 16:9 screen covers the taller one, scaled in its render node once Unity has filled it.
    # Unity's two sprite extraction functions (main+0x862f60, main+0x863470: the SpriteRenderer's render nodes) right after the shared node fill (bl main+0x82c8e0) returns:
    # x25 the node, x23 the renderer, the caller-saved registers dead (the next instruction reloads x8), x30 saved in the frame. The sprite's own bounds functions
    # (main+0x866760 / 0x866834) run only for a renderer in one draw mode (+0x204 == 1) and none of the backgrounds do: the node keeps the world bounds copied from the
    # TransformInfo, which bg_cover scales with the matrix
    cover = ctx.here()
    for w in _words(_BG_COVER): ctx.emit(w)
    for at in (0x8630f4 + d, 0x8635d8 + d):
        ctx.site(at, _hex(0xF940BEE8))   # ldr x8, [x23, #0x178]
        start = ctx.here(); _load_state(ctx, 8, ST_SITES)
        ctx.emit(_tbz(SITE_COVER, 8, ctx.here(), ctx.here() + 16))   # tbz w8, #6, orig (+4)
        ctx.emit(0xAA1903E0)   # mov x0, x25   the node
        ctx.emit(ADRP(1, ctx.here(), ctx.state))
        ctx.emit(BL(ctx.here(), cover))
        ctx.emit(0xF940BEE8)   # orig
        ctx.emit(B(ctx.here(), at + 4))
        ctx.put(at, [B(at, start)], 'sprite render node: a 16:9 screen sized sprite scaled to cover the taller screen (bit 6)')


def screen_shape(ctx, build):   # build: BUILDS[NSO build ID]. The order is the patch area's layout
    d = build['d']   # this build's native offsets minus JP 7.0.0's (the same Unity, only the game's IL2CPP code differs)
    _screen_heights(ctx, d)
    _set_resolution(ctx, d)
    _ortho(ctx, d)
    code, tables = _top_lift_hooks(ctx, build)
    _story_and_band(ctx, build, code, tables)
    _bg_cover_hooks(ctx, d)


FRAME_STEP = {
    JP: lambda ctx: screen_shape(ctx, BUILDS[JP]),   # 7.0.0 (v2097152) main. The US profile (01006B0016AD6000/patches.py) registers its build from this module's table
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open
BINDING = [
    {'what': "screen shape: Unity's screen and swapchain size, Screen.SetResolution's clamp and the orthographic cameras' width (DM_SCREEN_ASPECT)", 'now': 'build', 'goal': 'game',
     'purpose': 'rendering', 'why': "the output size is the host's, but where Unity takes its sizes is this build's code",
     'blocked': 'the stubs have no function-matched form yet'},
]

# ---- the C functions' words: written by gen_native.py from top_lift.c and bg_cover.c, do not edit ----
_TOP_LIFT = (   # top_lift.c: 744 words
    'd10783ff6d143bef6d1533ed6d162beb6d1723e9a9186ffca91967faa91a5ff8'
    'a91b57f6a91c4ff4a91d7bfd910743fdb94048737110e67f540059cbaa0203f7'
    'aa0103f9aa0003f6b94000488b080028d2800001d63f0100f90023e0b40058a0'
    'b94012e88b080328f94023f4aa1403e0d2800001d63f01004ea11c28b9400ee8'
    '8b080328aa1603e0d2800001d63f0100aa0003f6b94006e88b080328aa1403e0'
    'd2800001d63f0100b9003fe07100041f5400560b528000015110e2681e03fd0e'
    '1e23026052a890e81e2701011e20182c910082e8f9000be8910092e9910052c8'
    'a90227e81e2e10001e2c180b910052e8f9001be8910072e8f9003be81e202108'
    '1e2c10001e3c10011e204c20528e148872a7efa81e27010f5288f5c872a7f028'
    '1e2009c0bd000fe01e27010d910263f8bd001febf9002bf61400000e1e2c1800'
    'f9403be81e2c1821b94001088b080328f94033e0d2800001d63f0100b9405fe1'
    '11000421b9403fe86b08003f54005020b9400ae88b080328f94023e0b9005fe1'
    'd2800002d63f0100f90033e0b4fffe80b9400ee88b080328f94033e0d2800001'
    'd63f0100f90037e0b4fffda0f94017e83940010a34fffd4af94037e891005108'
    'f90027e8f94017f4f9400be814000008aa1603f7f9402bf6f9404be891000508'
    '8b1a029438405e8a34fffbaaf90043e8d280001a11000689aa0903e88b1a0289'
    '3940052b9100075a1100050935ffff6b8b1a028beb0b029fd1000575aa1403e9'
    'f9004beb540005225280000bd280000938696a8c7100e99f5400012091000529'
    '8b09028cd100058c1100056beb15019f54ffff03aa1403e91400001bb4000136'
    '7100053f540000e17100a95f54000280b94012ca6b09015f54fffb0114000007'
    'b4fffad6b94012ca6b09015f54fffa617100053f5400014b2a0b03eaaa1403eb'
    'f94013ec7840258d3840156e6b0e01bf54fff941f100054a54ffff618b090289'
    '91000529f9404bebaa0903eaeb0b013f54000162aa0903eb3940016a7100bd5f'
    '540000c09100056aeb15017faa0a03eb54ffff4314000002aa0b03eaf9404beb'
    'eb0a017f54000180cb0901487100051f540002e13940012a7100a95f54000460'
    'f94037eab940114a6b08015f54fff561140000154b0903ea8b1a028b8b0a016a'
    '7100055f54002d40f94037ebb940116b4b09028c4b0b018b2b1a017f54fff3e1'
    '7100055f54002d6a14000172f94037eab940114a6b08015f54fff3017100051f'
    '5400014b92407908aa0903eaf94027eb7840256c3840154d6b0d019f54fff1e1'
    'f100050854ffff615280000bf94033e8f81283a8f9005fe98b1a0288f9003fe8'
    'b9009bfff9404bfcaa1703f69102e3f714000007510006cbaa1c03e8910263f8'
    'f9404bfcaa0803f637ffef4bf86b5ae9eb1c013f54000122394001287100bd1f'
    '540009e091000528eb15013faa0803e954ffff4314000002aa0903e8eb08039f'
    '540009409100050caa0c03f3eb15011f54000222f9403fe9cb08012851000509'
    'aa0c03f3f240052954000100aa0c03f33940026a7100bd5f540000e091000673'
    'f100052954ffff61d1000908f1000d1f54000e02f90047ecb86b5b1bb94006c8'
    '8b080328aa1603fcd10363a9f86b5938aa1803e0d2800001aa0b03f6d63f0100'
    '6b00037f54fff98aaa1c03f7b9400b888b08032811000769910263eab8365949'
    'aa1803e0aa1b03e1d2800002d63f010071001adfaa1603eb54000dc8aa0003fb'
    '910263f8f9404bfcb4000ce0aa0b03f8aa1703f6b9400ee88b080328aa1b03e0'
    'd2800001d63f0100f94047edcb0d02687100051f54000c81b4000c60394001a9'
    '7100a93f9102e3f754000de0b94010096b08013f54000c6014000072aa0903e8'
    'eb08039f54fff701d10363a8aa0b03f7f86b591bf94043f8394003087101051f'
    '54000261b9401ac88b080328aa1b03e0d2800001d63f01001e2f20201e2dc420'
    '910263f8540005051e2c1800f9403be81e2c1821b94001088b080328aa1b03e0'
    'd2800001d63f01001400001fb94012c88b080328aa1b03e0d2800001d63f0100'
    '39400b0839400f0953185d2913003d292a080133394007081e22026a7100051f'
    '910263f8540000611e2a2020540001cb7100091f540000611e2a202054000148'
    '71000d1f54000841510042681e2201031e232020110042681e2201031e23a422'
    '54000769510006eb9102e3f717ffff6f8b1a0288d100050891000e73385fd269'
    '7100bd3f54000980385fe2697100bd3f54000960385ff2697100bd3f54000940'
    '394002697100bd3f54fff06091001269eb08027faa0903f354fffe21d1000d33'
    '17ffff7daa1703f69102e3f717ffff57aa1703f69102e3f7910263f8f9404bfc'
    '17ffff529102e3f7b40002c0b94010096b08013f540002617100051f5400014b'
    '9100500992407908aa0d03ea7840252b3840154c6b0c017f54000141f1000508'
    '54ffff611100070bd10363a8f82b591bf82b5aed910263f8b82b5b1f17ffff3b'
    'aa1803eb910263f817ffff38f94043e8394001087101091f540003ac7101011f'
    '540000607101091f540009417101011f1e2b0d881e2808001e280821b94016c8'
    '8b080328aa1b03e0d2800001d63f0100b9401ac88b080328aa1b03e0d2800001'
    'd63f01001e2009001e210901f9403be817ffff89d1000e7317ffff3fd1000a73'
    '17ffff3dd100067317ffff3b71010d1f540006807101111f540005c1b9401ac8'
    '8b080328aa1b03e0d28000014ea21c4b4ea01c084eac1d894eae1dcc4ea11c2e'
    'd63f01004ea01c054ea81d004ea11c284eae1dc14eac1d8e4ea91d2c4eab1d63'
    'bd401feb528e148872a7efa81e2701041e2421005288f5c872a7f0281e270104'
    '1e24c50054fff1051e2009601e2a28211e2e38211f21a961f9401be8b9400108'
    '8b080328aa1b03e04ea21c4a4ea31c62d28000014ea51ca9d63f01001e290960'
    '1e280961f9403be84eaa1d4217ffff5213001d081e2201031f0305c114000022'
    'b9401ac88b080328aa1b03e0d28000014ea21c484ea01c094ea11c2ad63f0100'
    '528e148872a7efa81e2701041e2420205288f5c872a7f0281e2701041e24c420'
    '54ffec25531f7a681e2201041e2419c41e2e10051e2528841e210881f9403be8'
    'b94001088b080328aa1b03e0d2800001d63f01004ea91d204ea81d02bd400fe1'
    '1e2a2821f9401be817ffff2b3940012a7100a95f540001e0f94037eab940114a'
    '4b09028b4b0a016a2b1a015f54ffc6614b090108f94027ea7840254b3840152c'
    '6b0c017f54ffc5a1f100050854ffff61f94043f5394002a87101051f54000141'
    'b9401ae88b080328f94033e0d2800001d63f01001e2f20201e2dc42054ffc3e5'
    '17fffdf7b94012e88b080328f94033e0d2800001d63f010039400aa839400ea9'
    '53185d2913003d292a080133394006a81e2202687100051f540000611e282020'
    '54ffc1cb7100091f540000611e28202054ffc14871000d1f5400010151004268'
    '1e2201031e232020110042681e2201031e23a42254ffc028f94043e839400108'
    '71010d1f540001ec54000681b9401ae88b080328f94033e0d28000014ea21c48'
    '4ea01c094ea11c2ad63f01001e2f202054ffbe4d1e2d202054ffbe0514000043'
    '7101111f54000781b9401ae88b080328f94033e0d28000014ea21c494ea01c0b'
    '4ea11c2ad63f01004eab1d64bd401feb1e2f202054ffbc2d1e2d202054ffbbe5'
    '4ea21c434ea91d221e2409641e2829451e2e38a51f25a165f9401be8b9400108'
    '8b080328f94033e04ea01c094ea41c804ea11c2a4ea51ca14ea31c68d2800001'
    'd63f01001e2909601e2a0961f9403be84ea81d0217fffda57101011f54000060'
    '7101091f540002817101011f1e2b0d881e2808001e280821b94016e88b080328'
    'f94033f3aa1303e0d2800001d63f0100b9401ae88b080328aa1303e0d2800001'
    'd63f01001e2009001e210901f9403be817fffd8e13001d081e2201031f0305c1'
    'f9401be817fffd89531f7a681e2201041e2419c41e2e10051e2528841e210881'
    'f9403be8b94001088b080328f94033e0d2800001d63f01004ea91d204ea81d02'
    'bd400fe11e2a2821f9401be817fffd77a95d7bfda95c4ff4a95b57f6a95a5ff8'
    'a95967faa9586ffc6d5723e96d562beb6d5533ed6d543bef910783ffd65f03c0'
)
_BG_COVER = (   # bg_cover.c: 76 words
    'b94048287110e51f5400092bbd4004001e20c00152824de972a750691e270122'
    '1e2220205400084cbd4010011e20c0221e2701231e232040540007ac2d549002'
    '1e222842bd4000031e2140651e2020681e234ca51e2508451e242884bd401402'
    '1e2140461e2020481e224cc61e2608845290000972a89da91e2701261e2620a0'
    '5290000972a890891e2701251e2554885298000972a892891e2701251e255480'
    '5400046cbd409c041e2408441e20c0841e2210051e252080540003ac1e230104'
    '52a890e81e2701051e25188452824de872a750681e2701051e2528841e230883'
    '1e2008802d000003bd4008001e200880bd0008001e2108801e2208812d020400'
    'bd4018001e200880bd001800fd401800fd4040010ea0d4210f841020fd004000'
    'fc48c0000f849000fc08c000d65f03c0'
)
