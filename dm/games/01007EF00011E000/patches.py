# The Legend of Zelda: Breath of the Wild (01007EF00011E000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
import struct
from nso_asm import ADDI, ADRP, B, LDRX
CUTS = {
  # The Legend of Zelda: Breath of the Wild 1.9.0 (v1114112) main: 60 fps. The main loop of sead::GameFrameworkNx (main+0x112660c) runs a dynamic interval (framework +0x267 = 1):
  # every frame it computes how many vsyncs the last frame needed (rounded up), clamped to [2, max 3] (the max comes from the interval controller +0xc, constructed at main+0x1126c04); it steps up after 3 overrunning frames in a row,
  # steps down one level every 6 frames until 2, and hands the result through setWaitVsyncInterval (vtable +0x130, main+0x11269b8) to presentation (nvnWindowSetPresentInterval).
  # The game logic time step follows it: ksys::VFR is initialized with a base interval of 2 (constant 2 at main+0x127e740 → VFR::init main+0x151d2d0), and every frame VFR::updateInterval (main+0x151d3d0,
  # called at main+0xcf9ad4 / main+0x1281044) takes the framework's current interval (vtable +0x138); the frame ratio = current interval / 2 -- at interval 1 each frame advances half a 30 fps tick and the game speed stays the same;
  # when it struggles and steps back to 2 / 3 the ratio is 1 / 1.5, again at the same speed. So only the floor changes from 2 to 1: the lower bound of the candidate interval (two places), the step-down target recorded when stepping up, max(candidate, 3) - 1 → max(candidate, 2) - 1,
  # and stepping down stops at < 3 → < 2. On hardware the same logic starting from 1 would also be 60 fps; it falls back to 30 by itself only when its input shows a slow frame (the last two cuts make that input include the GPU time).
  # With the frame step (FRAME_STEP below) the host's step replaces the controller's interval, except while the game's own VFR override is set (and at fps_target 0): the framework then keeps the controller's
  'cd57b23fa4bbad65803d9788c01821ee': [
    (0x112670c, '6041201e', '4041201e', 'lower bound of the candidate interval: fmov s0,s11 (2.0) → fmov s0,s10 (1.0)'),
    (0x1126710, '7f0b0071', '7f070071', 'lower bound of the candidate interval: cmp w27,#2 → cmp w27,#1'),
    (0x112665c, '78008052', '58008052', 'step-down target recorded when stepping up, max(candidate, w24) − 1: mov w24,#3 → mov w24,#2'),
    (0x1126748, '1f0d0071', '1f090071', 'step-down target recorded when stepping up: cmp w8,#3 → cmp w8,#2'),
    (0x1126794, '3f0d0071', '3f090071', 'stepping down stops at < 3 → at < 2: cmp w9,#3 → cmp w9,#2'),
    # The controller's input (framework +0x250, controller main+0x1126cec..) is the main thread's CPU span up to the draw step (about 2.7 ms in the first shrine), so GPU-bound frames stayed at interval 1 and the game ran slow
    # (iPhone 16 Pro Max: 25 ms frames, two thirds of the speed). The game has a GPU-time path that uses max(CPU span, GPU time between the frame's two timestamp reports, framework +0x220, read after the fence wait)
    # but takes it only while [main+0x1e3a638]+0x1d2b8 is set (64 of 6,046 shrine frames); both of its checks become branches into it. The GPU time needs the host's segment timestamps (nvn_mtl.m rep_ticks)
    (0x112691c, 'c00000b4', '03000014', 'interval controller always takes the GPU time path (CPU span timing): cbz x0,#0x1126934 → b #0x1126928'),
    (0x1126974, 'c00000b4', '03000014', 'interval controller always takes the GPU time path (takes max(CPU span, GPU time)): cbz x0,#0x112698c → b #0x1126980'),
  ],
}

# Breath of the Wild: the framework's interval controller (the 60 fps cuts above) hands its interval to setWaitVsyncInterval (main+0x11269b8), which the main loop calls once a frame
# (main+0x11267c0); the framework keeps it at fw+0x244 (getter vtable +0x138, read by both callers of VFR::updateInterval and directly in 43 places) and passes it on to
# nvnWindowSetPresentInterval. The stub at the setter's entry replaces it with the host's step, read once a frame: ksys::VFR::updateInterval (main+0x151d3d0, x0 = the VFR at
# [main+0x1e93890]; called from GameScene::calc and the per-frame delegate main+0x1280f80) takes the framework's interval in the next frame (ratio = step / 2, base interval 2, and the
# int interval that 16 pieces of logic read), so the time step and everything else that reads the interval agree. The step is not written into the VFR call itself: the game presents
# on its own thread, so the host's step can change between the two VFR calls of a frame. The VFR's own override (+0x29 flag, +0x2c interval: 2 at main+0xc3b588 and main+0xc5ebf8,
# 1 at main+0xc609d8, set and cleared by game scenes) wins in the VFR:
# a stub there publishes it at state +0x24 so the host holds frames that long, and the setter stub leaves the framework's interval to the game while it is set. x16 / x17 are free
# at both entries
def frame_step(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    fs_site(0x11269b8, 'fd7bbea9'); fs_site(0x151d3d0, 'fd7bbea9')
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9400211,   # ldr w17,[x16]          target fps (0 = off)
              0x34000191):  # cbz w17,orig (+12)
        emit(w)
    emit(ADRP(17, here(), 0x1e93890)); emit(LDRX(17, 17, 0x890 // 8))   # the VFR
    for w in (0xb4000131,   # cbz x17,orig (+9)
              0x3940a631,   # ldrb w17,[x17,#0x29]   the VFR's override
              0x350000f1,   # cbnz w17,orig (+7)     the framework keeps the game's interval
              0xb9402211,   # ldr w17,[x16,#0x20]    step in vsyncs
              0x340000b1,   # cbz w17,orig (+5)      not decided yet
              0x2a1103e1,   # mov w1,w17
              0xf9401a11,   # ldr x17,[x16,#0x30]
              0x8b214231,   # add x17,x17,w1,uxtw
              0xf9001a11,   # str x17,[x16,#0x30]    vsyncs stepped
              0xa9be7bfd):  # orig: stp x29,x30,[sp,#-0x20]!
        emit(w)
    emit(B(here(), 0x11269bc))
    fs_put(0x11269b8, [B(0x11269b8, start)], 'setWaitVsyncInterval: the host\'s step unless the VFR override is set (state page target fps; 0 = the game\'s interval)')
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9400211,   # ldr w17,[x16]          target fps (0 = off)
              0x340000b1,   # cbz w17,orig (+5)
              0x3940a411,   # ldrb w17,[x0,#0x29]    the VFR's override
              0x34000051,   # cbz w17,pub (+2)       none: 0
              0xb9402c11,   # ldr w17,[x0,#0x2c]
              0xb9002611,   # pub: str w17,[x16,#0x24]  the game's own step: the host holds frames that long
              0xa9be7bfd):  # orig: stp x29,x30,[sp,#-0x20]!
        emit(w)
    emit(B(here(), 0x151d3d4))
    fs_put(0x151d3d0, [B(0x151d3d0, start)], 'VFR::updateInterval: its own override published for the host (state page target fps; 0 = original)')
    # Held buttons: sead::ControllerBase::updateDerivativeParams_ (main+0xeb0ef0) adds s8 to each held button's count, a constant 1.0 (main+0xeb0f78..88), and raises the repeat bit
    # when fmod(count - delay, pulse) < s8: delays and pulses are counted in frames, so at 60 fps menus repeated twice as fast as at 30 (MacNativeGames 866beb9). s8 is now the VFR's
    # ratio (+0x3c: interval / base interval 2), 0.5 at a one-vsync step, 1.0 at two as shipped; the constant stays when there is no VFR yet or the ratio is not positive.
    # x8 is loaded again after the site, s0 before its first read
    fs_site(0xeb0f80, '08f144f9'); fs_site(0xeb0f88, '080140bd')
    start = here(); emit(0xbd400108)   # ldr s8,[x8]            the constant 1.0
    emit(ADRP(8, here(), 0x1e93890)); emit(LDRX(8, 8, 0x890 // 8))   # the VFR
    for w in (0xb40000a8,   # cbz x8,back (+5)
              0xbd403d00,   # ldr s0,[x8,#0x3c]      its ratio
              0x1e202008,   # fcmp s0,#0.0
              0x5400004d,   # b.le back (+2)
              0x1e204008):  # fmov s8,s0
        emit(w)
    emit(B(here(), 0xeb0f8c))   # back
    fs_put(0xeb0f88, [B(0xeb0f88, start)], 'held-button counts step by the VFR ratio (menu repeat in game time at any frame step)')
    # Render scale (state +0x10, float written by the loader: DM_RENDER_SCALE, 1.0 by default; independent of the frame step). The dynamic-resolution method (main+0x12666e4) keeps a
    # scale in 0.8–1.0 rounded to 0.05 steps (+0x1218, main+0x1266818), or the game's forced 0.75 / 0.5 (+0x284 & 0x1800), in s8; from main+0x1266880 it applies s8: each view's
    # scale (+0x28a0 / +0x28a4 = s8 × base width (+0x1214: 1600 docked, 1280 handheld) / output width), from which the scene buffers are sized each frame (main+0x106340c), the
    # shader ratio uking_dynamic_resolution_ratio and the other readers (main+0xc3c3f4, an LOD check at main+0x14126b4). Setting s8 there keeps them all consistent (a multiply in the
    # size function, TOTK's way, would leave the ratio and those readers at the old scale). The apply block runs only when the dynamic path has just recomputed the scale (+0x280 bit 4,
    # set at main+0x1266830) or in a forced mode, else main+0x126686c skips it; with a factor it is taken every frame, as in the dynamic mode, so the factor holds with dynamic
    # resolution off too. Factors above 1.0 are refused by the loader: the per-frame texture pool's size is unknown. x8, s0, s1 and the flags are dead at both sites
    fs_site(0x1266880, '88ae40f9'); fs_site(0x126686c, struct.pack('<I', B(0x126686c, 0x1266950)).hex())
    apply = here(); emit(ADRP(8, here(), FS_STATE_OFF))
    for w in (0xbd401100,   # ldr s0,[x8,#0x10]      render scale
              0x1e202008,   # fcmp s0,#0.0
              0x540000ad,   # b.le orig (+5)         not set
              0x1e2e1001,   # fmov s1,#1.0
              0x1e212000,   # fcmp s0,s1
              0x54000040,   # b.eq orig (+2)         1.0: the game's own scale
              0x1e204008,   # fmov s8,s0             the host's scale
              0xf940ae88):  # orig: ldr x8,[x20,#0x158]
        emit(w)
    emit(B(here(), 0x1266884))
    fs_put(0x1266880, [B(0x1266880, apply)], '3D view scale = the host render scale (1.0 = the game\'s dynamic resolution)'); ctx.render_scale = 1
    start = here(); emit(ADRP(8, here(), FS_STATE_OFF))
    for w in (0xb9401108,   # ldr w8,[x8,#0x10]      render scale (float bits)
              0x34000088,   # cbz w8,skip (+4)       not set
              0x52091908,   # eor w8,w8,#0x3f800000
              0x34000048):  # cbz w8,skip (+2)       1.0
        emit(w)
    emit(B(here(), 0x1266880)); emit(B(here(), 0x1266950))   # apply the host's scale; skip: the original branch
    fs_put(0x126686c, [B(0x126686c, start)], 'scale apply block taken every frame when the host render scale is not 1.0')
    # Screen shape: a screen taller than 16:9 (the iPhone Duo's, about 1.43:1) gets a picture of its own shape instead of a stretched
    # or cut one. The game's sizes are constants: the output size static [main+0x1e43dc0] (1920x1080; set in nnMain and by its getter main+0x1261880) that sizes the
    # framework's window textures, the layout size static [main+0x1e43dd0] (1280x720; nnMain, getter main+0x12618dc), and the display mode switch main+0x1266074, which
    # sets the display size and viewport for docked (1920x1080) and handheld (1280x720, and the 720.0 of its scaled case). Each height becomes the host's when it set one
    # (state +0x48 u32 output height, +0x4c u32 layout height, +0x50 float layout height; loader.c from DM_SCREEN_ASPECT; 0 = the game's own). The widths stay, so the
    # 3D views (1600 wide docked, scaled from the output size) grow in height too. x16 is free at every site: nnMain's sites follow a call through the PLT
    def taller(site, orig, load, apply, why):
        fs_site(site, orig)
        start = here(); emit(struct.unpack('<I', bytes.fromhex(orig))[0])   # the original instruction
        emit(ADRP(16, here(), FS_STATE_OFF))
        for w in (load, 0x34000050, apply):   # ldr w16,[x16,#off]; cbz w16,+8 (0: the game's size); the host's height
            emit(w)
        emit(B(here(), site + 4))
        fs_put(site, [B(site, start)], why)
    OUT, LAY, LAYF = 0xb9404a10, 0xb9404e10, 0xb9405210   # ldr w16,[x16,#0x48] / #0x4c / #0x50
    HI9, HI8, W4, W9 = 0xb3607e09, 0xb3607e08, 0x2a1003e4, 0x2a1003e9   # bfi x9 / x8,x16,#32,#32 (the pair's height); mov w4,w16; mov w9,w16
    for site, orig, load, apply, why in (
            (0xc61ef4, '0987c0f2', OUT, HI9, 'output size static, nnMain: 1920x1080 → the host\'s height'),
            (0xc61f28, '0987c0f2', OUT, HI9, 'output size static, nnMain (second path)'),
            (0x12618c4, '0987c0f2', OUT, HI9, 'output size static, its getter'),
            (0xc61f5c, '085ac0f2', LAY, HI8, 'layout size static, nnMain: 1280x720 → the host\'s layout height'),
            (0x1261920, '095ac0f2', LAY, HI9, 'layout size static, its getter'),
            (0x12660d4, '0887c0f2', OUT, HI8, 'docked display size 1920x1080'),
            (0x12660f0, '04878052', OUT, W4, 'docked viewport height 1080'),
            (0x1266124, '085ac0f2', LAY, HI8, 'handheld display size 1280x720'),
            (0x1266188, '045a8052', LAY, W4, 'handheld viewport height 720'),
            (0x1266134, '8986a852', LAYF, W9, 'scaled display height 720.0')):
        taller(site, orig, load, apply, why)
    # The game's conversions between output pixels and the 1280x720 layout space, and the layout objects built with that size: each takes the layout height too, while
    # its bit of state +0x54 is set (loader.c DM_SCREEN_ASPECT_SITES, all by default), so one build can try them one by one. 720.0 constants: main+0xdf19ac divides the
    # output size by (1280, 720) for the layout scale; the others pair 720.0 with 1280.0 the same way. 1280x720 pairs: layout objects' defaults
    def taller_bit(site, orig, bit, load, apply, why):
        fs_site(site, orig)
        start = here(); emit(struct.unpack('<I', bytes.fromhex(orig))[0])
        emit(ADRP(16, here(), FS_STATE_OFF))
        emit(0xb9405610); emit(0x36000000 | bit << 19 | 5 << 5 | 16)   # ldr w16,[x16,#0x54]; tbz w16,#bit,back (+5)
        emit(ADRP(16, here(), FS_STATE_OFF))
        for w in (load, 0x34000050, apply):
            emit(w)
        emit(B(here(), site + 4))
        fs_put(site, [B(site, start)], why)
    W8, W23, HI10 = 0x2a1003e8, 0x2a1003f7, 0xb3607e0a   # mov w8,w16; mov w23,w16; bfi x10,x16,#32,#32
    for bit, (site, orig, load, apply, why) in enumerate((
            (0xdf19ac, '8886a852', LAYF, W8, '720.0: output size / layout size (the layout scale)'),
            (0xde85dc, '8986a852', LAYF, W9, '720.0 with 1280.0 (screen position in layout space)'),
            (0xdea070, '8886a852', LAYF, W8, '720.0 with 1280.0 (screen position / layout size)'),
            (0xdea678, '8886a852', LAYF, W8, '720.0 with 1280.0'),
            (0xe27b44, '9786a852', LAYF, W23, '720.0 stored with 1280.0'),
            (0xf6e798, '8986a852', LAYF, W9, '720.0 with 1280.0 (screen position)'),
            (0x100817c, '8886a852', LAYF, W8, '720.0 with 1280.0 passed as a layout size'),
            (0x1544644, '8886a852', LAYF, W8, '720.0 with 1280.0'),
            (0x1059730, '085ac0f2', LAY, HI8, '1280x720 layout size of an object'),
            (0x106bb6c, '085ac0f2', LAY, HI8, '1280x720 default sizes of a layer'),
            (0x13f9704, '085ac0f2', LAY, HI8, '1280x720 passed as a size'),
            (0xef1388, '0a5ac0f2', LAY, HI10, '1280x720 in a descriptor'))):
        taller_bit(site, orig, bit, load, apply, why)
    # The field of view: the camera's projection takes its view's ratio, so a taller view keeps the vertical angle and shows less at the sides than at 16:9. To keep the 16:9
    # view's sides and show more above and below, sead::PerspectiveProjection (fovy +0xa0, sin / cos / tan of half of it +0xa4 / +0xa8 / +0xac, aspect +0xb0) takes a fovy
    # of 2 atan(tan(fovy / 2) x k) for a view of the screen's ratio, k = (16/9) / ratio, where setFovy_ (main+0xeb4898), the constructor (main+0xeb48f4) and set()
    # (main+0xeb49a4) compute the half angle. The widened fovy is stored too: what derives a frustum from the fovy and the ratio (the shadow cascades, culling) must see the
    # view as it is drawn (Mac, a Master Mode save: with only sin / cos / tan widened, the ground under the grass went black in patches that turned with the camera; with no
    # widening at all they were gone). A fovy read back from a widened view and set again (main+0xeefe68 copies one view's into another) must not widen twice: the last 64
    # fovys the stubs produced are kept at state +0x100 (+0xfc the next slot), and one of those stays as it is. Views of another ratio (cube maps, shadow maps, 16:9 render
    # targets) keep theirs. State +0x58 float screen ratio (0 = off), +0x5c float k, +0x60 float tolerance. Each site is followed by a call to sinf, so the calls to tanf and
    # atanf clobber nothing the function keeps in caller-saved registers; x19 is the projection at all three. The body (clang -c, `nop` where the ADRPs and BLs go):
    #     adrp x16,state; ldr s1,[x16,#0x58]; fcmp s1,#0; b.eq 9f; <s2 = this view's ratio>; fsub s2,s2,s1; fabs s2,s2; ldr s3,[x16,#0x60]; fcmp s2,s3; b.ge 9f
    #     ldr w9,[x19,#0xa0]; add x10,x16,#0x100; mov w11,#64; 1: ldr w12,[x10],#4; cmp w12,w9; b.eq 9f; subs w11,w11,#1; b.ne 1b
    #     fmov s0,<half>; bl tanf; adrp x16,state; ldr s1,[x16,#0x5c]; fmul s0,s0,s1; bl atanf; fmov <half>,s0; fadd s0,s0,s0; str s0,[x19,#0xa0]
    #     adrp x16,state; ldr w11,[x16,#0xfc]; and w12,w11,#63; add w11,w11,#1; str w11,[x16,#0xfc]; add x10,x16,#0x100; str s0,[x10,w12,uxtw #2]; 9: fmov s0,<half>
    TANF, ATANF = 0x159ff60, 0x15a0070
    FOV_BODY = (None, 0xbd405a01, 0x1e202028, 0x540003e0, 'ratio', 0x1e213842, 0x1e20c042, 0xbd406203, 0x1e232040, 0x5400032a,
                0xb940a269, 0x9104020a, 0x5280080b, 0xb840454c, 0x6b09019f, 0x54000260, 0x7100056b, 0x54ffff81,
                'half_to_s0', 'tanf', None, 0xbd405e01, 0x1e210800, 'atanf', 's0_to_half', 0x1e202800, 0xbd00a260,
                None, 0xb940fe0b, 0x1200156c, 0x1100056b, 0xb900fe0b, 0x9104020a, 0xbc2c5940, 'half_to_s0')
    def fov_stub(site, orig, aspect_word, half_to_s0, s0_to_half):
        fs_site(site, orig)
        start = here()
        for w in FOV_BODY:
            if w is None: emit(ADRP(16, here(), FS_STATE_OFF))
            elif w == 'tanf': emit(B(here(), TANF) | 0x80000000)
            elif w == 'atanf': emit(B(here(), ATANF) | 0x80000000)
            else: emit({'ratio': aspect_word, 'half_to_s0': half_to_s0, 's0_to_half': s0_to_half}.get(w, w))
        emit(B(here(), site + 4))
        fs_put(site, [B(site, start)], 'sead::PerspectiveProjection fovy widened for a view of the screen\'s ratio (more above and below, 16:9 sides), stored widened')
    fov_stub(0xeb48b8, '0041201e', 0xbd40b262, 0x1e204100, 0x1e204008)   # setFovy_: ratio at [x19,#0xb0], half in s8
    fov_stub(0xeb4968, '0041201e', 0xbd40b262, 0x1e204100, 0x1e204008)   # constructor: the same
    fov_stub(0xeb49d0, '2041201e', 0x1e204102, 0x1e204120, 0x1e204009)   # set(): ratio still in s8 (stored after), half in s9
    # The camera's own fovy: the main camera's getter (main+0x10ee074, [camera]+0x74) feeds two horizontal culling tests (main+0x890238, main+0x97cd28: the horizontal angle
    # from the fovy and the viewport's ratio, main+0xd57cc0), and the degrees getter (main+0x10ee0a8) two more; they take the widened fovy too, or what stands at the picture's
    # sides is culled. x16 is free in both (getters with a frame); the degrees one reads its factor through x8 (adrp before the site), made again after the calls
    def cam_fovy(site, orig, x8_page, why):
        fs_site(site, orig)
        back = 17 if x8_page else 16   # the index of the branch back
        start = here(); emit(struct.unpack('<I', bytes.fromhex(orig))[0]); emit(ADRP(16, here(), FS_STATE_OFF))
        for w in (0xbd405a01, 0x1e202028, 0x54000000 | (back - 4) << 5,   # ldr s1,[x16,#0x58]; fcmp s1,#0.0; b.eq back (off)
                  0xbd406201, 0x1e202028, 0x54000004 | (back - 7) << 5,   # ldr s1,[x16,#0x60]; fcmp s1,#0.0; b.mi back (DM_SCREEN_FOV=off: a negative tolerance)
                  0x1e2c1001, 0x1e210800):                                # fmov s1,#0.5; fmul s0,s0,s1
            emit(w)
        emit(B(here(), TANF) | 0x80000000); emit(ADRP(16, here(), FS_STATE_OFF)); emit(0xbd405e01); emit(0x1e210800)   # bl tanf; ldr s1,[x16,#0x5c]; fmul s0,s0,s1
        emit(B(here(), ATANF) | 0x80000000); emit(0x1e202800)                                                         # bl atanf; fadd s0,s0,s0
        if x8_page: emit(ADRP(8, here(), x8_page))
        emit(B(here(), site + 4))
        fs_put(site, [B(site, start)], why)
    cam_fovy(0x10ee094, '007440bd', 0, 'main camera fovy getter: widened like the projection (culling tests)')
    cam_fovy(0x10ee0cc, '007440bd', 0x16a0000, 'main camera fovy getter in degrees: widened like the projection')
    # The HUD's projection still came out 16:9 (its aspect is stored by other code too), so sead::PerspectiveProjection::doUpdateMatrix (main+0xeb4a44) builds the matrix of
    # a 16:9 projection with near 1 and far 10000, the HUD camera's, with the screen's ratio and tan(half angle) x k: the same widened vertical angle, nothing else changed.
    # State +0x74 float 16/9, +0x78 float its tolerance, +0x7c float 10000.0
    # A projection whose fovy the stubs above already widened (one of the last 64 at state +0x100: a copy of the HUD camera's, read back and set again with its 16:9
    # aspect) has its tan widened already: it takes the screen's ratio only, or its vertical angle came out widened twice (Mac, a Master Mode save: the map's footprint
    # trail drawn 0.8 times as tall as the terrain under it, more off the further from the middle). doUpdateMatrix is a leaf: x9-x12 are free
    fs_site(0xeb4a50, '0188552d')
    start = here(); emit(0x2d558801); emit(ADRP(16, here(), FS_STATE_OFF))   # ldp s1,s2,[x0,#0xac] (tan, aspect)
    for w in (0xbd405a10, 0x1e202208, 0x54000320, 0xbd407611, 0x1e313851, 0x1e20c231, 0xbd407a12, 0x1e322220, 0x5400026a,   # ratio on; aspect 16:9
              0x2d534811, 0x1e2e1013, 0x1e332220, 0x540001e1, 0xbd407e13, 0x1e332240, 0x54000181,                          # near 1, far 10000
              0x1e204202,                                                                                                  # aspect = ratio
              0xb940a009, 0x9104020a, 0x5280080b, 0xb840454c, 0x6b09019f, 0x540000a0, 0x7100056b, 0x54ffff81,             # fovy widened already: done
              0xbd405e11, 0x1e310821):                                                                                     # tan x k
        emit(w)
    emit(B(here(), 0xeb4a54))
    fs_put(0xeb4a50, [B(0xeb4a50, start)], 'PerspectiveProjection matrix: the HUD camera (16:9, near 1, far 10000) takes the screen\'s ratio and a widened vertical angle')
    # The HUD at the taller screen's edges and the full-screen panes (duo_anchor.c, two entry points; the layout library: pane parent +0x18, translation +0x30, size
    # +0x50, global matrix +0x70 (ty +0x8c), name +0xb8): duo_calc runs in Pane::Calculate (main+0x276a0) once a pane's global matrix has been computed and before its
    # children's (main+0x27ae0); it moves a HUD widget at the 720 space's top or bottom edge (or one of the HUD's corner clusters) out by the extra half-height with
    # everything below it, and stretches a full-screen background to the taller screen, both in the global matrix, which every way of drawing follows (text boxes and
    # the game's own picture classes draw from it, not through Pane::LoadMtx). duo_size runs at the entry of the two capture panes' Calculate (main+0xf60cc0,
    # main+0xf59b2c), which draw their children or the screen behind them into a texture of their own size, and makes a full-screen capture of children as tall as the
    # screen. Nothing changes inside a full-screen cut-out: the map's tiles, icons and the bounding pane its tiles are drawn through scroll and zoom (Mac, a Master Mode
    # save: the map's scrolling container, at y 1809, had taken the edge rule and its tiles left their icons behind; zoomed in, the bounding pane became full-screen and
    # its stretch stretched the tiles 1.24 times). The words: `clang -target arm64-apple-macos -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-jump-tables
    # -fno-asynchronous-unwind-tables -c duo_anchor.c`, its __text as 32-bit words (no relocations: constants are immediates, parameters at state +0x80..+0x9c),
    # duo_calc at +0, duo_size at +0x25b4
    ANCHOR_WORDS = (
        'bd4000201e20200854000041d65f03c03942e008340013685280326a72a0200a5293b8a972b023894a0901091b0a7d293942e40b34000b6b4a0b01291b0a7d293942e80b'
        '34000aeb4a0b01291b0a7d293942ec0b34000a6b4a0b01291b0a7d293942f00b340009eb4a0b01291b0a7d293942f40b3400096b4a0b01291b0a7d293942f80b340008eb'
        '4a0b01291b0a7d293942fc0b3400086b4a0b01291b0a7d293943000b340007eb4a0b01291b0a7d293943040b3400076b4a0b01291b0a7d293943080b340006eb4a0b0129'
        '1b0a7d2939430c0b3400066b4a0b01291b0a7d293943100b340005eb4a0b01291b0a7d293943140b3400056b4a0b01291b0a7d293943180b340004eb4a0b01291b0a7d29'
        '39431c0b3400046b4a0b01291b0a7d293943200b340003eb4a0b01291b0a7d293943240b3400036b4a0b01291b0a7d293943280b340002eb4a0b01291b0a7d2939432c0b'
        '3400026b4a0b01291b0a7d293943300b340001eb4a0b01291b0a7d293943340b3400016b4a0b01291b0a7d293943380b340000eb4a0b01291b0a7d2939433c0b3400006b'
        '4a0b01291b0a7d29528d5aaa72a0c4ca6b0a013f540006a1bd405002bd4070011e2108421e20c042bd4010231e232040540005cbbd408c021e20c0431e2690041e242060'
        '54000525bd4084031e202068540004c0bd40540452a882c81e2701051e2520805400010b52a883481e2701051e252080540000881e2010051f051004bd00540452a882c8'
        '1e2701051e2010061f06140552a883481e2701071f061c061e2520801e26a48254ffee681e3010051f0510001e2018809101d008fd403c040d4091016e18048191022008'
        'bd4080046e0c04644d4081044f8090216e1c04444f809080140004a1f940000aeb02015f54000681bd405001bd4070021e2208211e20c023bd4010211e212060540005ab'
        'bd405402bd4084011e2108421e20c044bd40142552a895e91e2701261e2620601e25a4885400046abd407c031e20c06352a844091e2701241e242060540003a5bd408c03'
        '1e20c0631e2701241e242060540003051e202028540002c01e2140401e2020481e224c0052a885e81e2701021e2220005290000872a886c81e2701021e22a40254ffe708'
        'bd40042052a886881e2701021e2208001e2140221e2020281e214c411e211800bd005400d65f03c0f9400c09eb06015f54000c81bd405001bd4070021e2208211e20c022'
        'bd4010211e21204054000babbd407c021e20c04252a8440b1e2701631e23204054000ae5bd408c021e20c0421e2701631e23204054000a455280030b52a895ec1e270183'
        'aa0003ec140000037100056b54000820f9400d8cb40007ecf940018deb0201bf54ffff41bd405182bd4071841e2408421e20c0441e21208054fffe8bbd405582bd408585'
        '1e2508421e20c045bd4014221e2320801e22a4a854fffd8bb40006e9f940012beb02017f54000681bd405123bd4071241e2408631e20c0631e212060540005cbbd405524'
        'bd4085251e2508841e20c08452a895eb1e2701651e2520601e22a488540004ab5280030b52a895ec1e270183aa0903ec140000037100056b54000280f9400d8cb400024c'
        'f940018deb0201bf54ffff41bd405184bd4071851e2508841e20c0841e21208054fffe8bbd405585bd4085861e2608a51e20c0a51e2320801e22a4a854fffdab1400000b'
        'bd405401bd4084021e2208231e2288241e2020681e234c8352a880eb1e2701641e2420605400152abd405402bd4084011e2108461e2188421e2020c81e264c42eb06015f'
        'f940140b9100800cfa431144fa4c1160fa440144fa421144fa4511441a9f17ed1e2020281a9f15ad3600064dbd405004bd4070031e2308871e2388841e2020e81e274c90'
        'bd4010251e2522005400024bbd407c041e20c08452a8440e1e2701d11e31208054000185bd408c041e20c0911e2701d21e32222052a880ee1e2701d11e3144485400008b'
        'bd4018311e31204054000b0937003a0d1e25220054003425bd408c041e20c0911e2690121e32222052a882ca1e2701511e3144485290000a72a886ca1e2701511e31a442'
        '540032a834001e48528d8a6a72ac8d2ab940b80b6b0a017f5400110152800a681400018d2957200f12005d0e528d2a8872ad8e886b0801ff540000a152884ca872a008e8'
        '6b0801df54000340528bea0872ada9286b0801ff540000a1528cec2872a00ca86b0801df54000240528bea0872ac29a86b0801ff540000a1528dcd2872a00ac86b0801df'
        '5400014012001de8528bea1072ac28506b1001ff54fff621528d6c6f72a008ef6b0f01df54fff5a1bd4004202d4e0c021e220800bd007000bd4004201e230800bd007400'
        'bd4004202d4f0c021e220800bd007800bd4004201e230800bd007c00bd400420bd4080021e220800bd008000bd4004201e210800bd008400bd4004202d5108011e210800'
        'bd008800bd4004201e220800bd008c00d65f03c0eb05015f54ffc860eb02015f54ffc820eb04015f54ffc7e0eb03015f54ffc7a0eb0c017f54ffc7615280030852a895e9'
        '1e270123aa0003e9140000037100050854001920f9400d29b40018e9f940012aeb02015f54ffff41bd405126bd4071271e2708c61e20c0c61e2520c054fffe8bbd405527'
        'bd4085301e3008e71e20c0e7bd4014301e2320c01e30a4e854fffd8b17fffe20bd4014241e2010051f0510041e2020481e24146054ffc3455ea1b8235e21d8631e233823'
        '52824de872a740681e2701041e242060529374c872a758881e2701041e24c46054ffc1c41e2028001e2140431e2020481e224c621e2218001e2028201e2c10011e212800'
        '1e25c00052a750081e2701011e212800bd005400d65f03c012001d6872181d7f54000c60b84b900b6b0a017f5400204072181d7f54000bc0b84ba00b6b0a017f54001fa0'
        '72181d7f54000b20b84bb00b6b0a017f54001f0072181d7f54000a80b940bc0b6b0a017f54001e6072181d7f540009e0b84bd00b6b0a017f54001dc072181d7f54000940'
        'b84be00b6b0a017f54001d2072181d7f540008a0b84bf00b6b0a017f54001c8072181d7f54000800b940c00b6b0a017f54001be072181d7f54000760b84c100b6b0a017f'
        '54001b4072181d7f540006c0b84c200b6b0a017f54001aa072181d7f54000620b84c300b6b0a017f54001a0072181d7f54000580b940c40b6b0a017f5400196072181d7f'
        '540004e0b84c500b6b0a017f540018c072181d7f54000440b84c600b6b0a017f5400182072181d7f540003a0b84c700b6b0a017f5400178072181d7f54000300b940c80b'
        '6b0a017f540016e072181d7f54000260b84c900b6b0a017f5400164072181d7f540001c0b84ca00b6b0a017f540015a072181d7f54000120b84cb00b6b0a017f54001500'
        '72181d7f54000080b940cc0b6b0a017f540014605280030a52a895eb1e270171aa0003eb140000037100054a540002a0f9400d6bb400026bf940016ceb02019f54ffff41'
        'bd405172bd4071731e330a521e20c2521e25224054fffe8bbd405573bd4085741e340a731e20c273bd4014341e3122401e34a66854fffd8b14000088bd407c111e214232'
        '1e2022281e314e521e2c10131e330a131e322a725290000a72a883aa1e2701531e33224054000f0b34004608528848ca72ac298ab940b80b6b0a017f540001a1528008c8'
        '1400007052a886881e2701031e232040540000a51e2010031f0308001e221800140002a4bd400420140002a212001d6872181d7f54004380b84b900b6b0a017f54000c00'
        '72181d7f540042e0b84ba00b6b0a017f54000b6072181d7f54004240b84bb00b6b0a017f54000ac072181d7f540041a0b940bc0b6b0a017f54000a2072181d7f54004100'
        'b84bd00b6b0a017f5400098072181d7f54004060b84be00b6b0a017f540008e072181d7f54003fc0b84bf00b6b0a017f5400084072181d7f54003f20b940c00b6b0a017f'
        '540007a072181d7f54003e80b84c100b6b0a017f5400070072181d7f54003de0b84c200b6b0a017f5400066072181d7f54003d40b84c300b6b0a017f540005c072181d7f'
        '54003ca0b940c40b6b0a017f5400052072181d7f54003c00b84c500b6b0a017f5400048072181d7f54003b60b84c600b6b0a017f540003e072181d7f54003ac0b84c700b'
        '6b0a017f5400034072181d7f54003a20b940c80b6b0a017f540002a072181d7f54003980b84c900b6b0a017f5400020072181d7f540038e0b84ca00b6b0a017f54000160'
        '72181d7f54003840b84cb00b6b0a017f540000c072181d7f540037a0b940cc0b6b0a017f5400374152a885ea1e2701521e3220405400352a1e2522005400058bbd407c03'
        '1e20c06352a8440a1e2701441e2420601e2701431e23444452a880ea1e2701431e23c4405400042539416c0a53020d4abd408c036f00e4101e2c10041e2408447100055f'
        '1e3c10111e3108511e311e107100095f1e300c901e3028701e2142111e2022081e304e311e31289152a872ca1e2701521e32222052a8774a1e2701521e32a62254000128'
        '1e20c071bd4008321e322220540028a452a8590a1e2701511e3120405400282c2d4608011e2208421f0108221e2a10011e212040fa40c92454ff98a01e20c0e21e20c0c3'
        '52a8590a1e2701441e2420601e2594481a9fb7ea5280030b52a895ec1e27018214000004aa0c03e97100056b540003a0f9400d2cb400036c2d4611231e2408841f031063'
        '1e21206054ff960cf940012deb0201bf54fffe81bd405123bd4071241e2408631e20c0631e25206054fffdcbbd405524bd4085261e2608841e20c084bd4014261e262080'
        '1a9fa7e91e2220601a9fa5292a0a01293707fc6917fffc9bbd408c011e2140221e2020281e214c42bd4008231e2320405400a8cabd400c231e2320407a40a90454001021'
        '3942e00834ff91c8bd407c025280326972a020095293b8aa72b0238a4a0a01081b097d083942e40a34000b6a4a0a01081b097d083942e80a34000aea4a0a01081b097d08'
        '3942ec0a34000a6a4a0a01081b097d083942f00a340009ea4a0a01081b097d083942f40a3400096a4a0a01081b097d083942f80a340008ea4a0a01081b097d083942fc0a'
        '3400086a4a0a01081b097d083943000a340007ea4a0a01081b097d083943040a3400076a4a0a01081b097d083943080a340006ea4a0a01081b097d0839430c0a3400066a'
        '4a0a01081b097d083943100a340005ea4a0a01081b097d083943140a3400056a4a0a01081b097d083943180a340004ea4a0a01081b097d0839431c0a3400046a4a0a0108'
        '1b097d083943200a340003ea4a0a01081b097d083943240a3400036a4a0a01081b097d083943280a340002ea4a0a01081b097d0839432c0a3400026a4a0a01081b097d08'
        '3943300a340001ea4a0a01081b097d083943340a3400016a4a0a01081b097d083943380a340000ea4a0a01081b097d0839433c0a3400006a4a0a01081b097d085280d889'
        '72a00ac96b09011f5400160c5280dda972b811a96b09011f54002ecc5293fe4972b558c96b09011f540037ac529781a972b2b2c96b09011f540042cc529948a972b156e9'
        '6b09011f5400574c5298e40972b116e96b09011f540084c052897d2972b139c9140002155280326972a020095293b8aa72b0238a4a0a01081b097d083942e40a34000b6a'
        '4a0a01081b097d083942e80a34000aea4a0a01081b097d083942ec0a34000a6a4a0a01081b097d083942f00a340009ea4a0a01081b097d083942f40a3400096a4a0a0108'
        '1b097d083942f80a340008ea4a0a01081b097d083942fc0a3400086a4a0a01081b097d083943000a340007ea4a0a01081b097d083943040a3400076a4a0a01081b097d08'
        '3943080a340006ea4a0a01081b097d0839430c0a3400066a4a0a01081b097d083943100a340005ea4a0a01081b097d083943140a3400056a4a0a01081b097d083943180a'
        '340004ea4a0a01081b097d0839431c0a3400046a4a0a01081b097d083943200a340003ea4a0a01081b097d083943240a3400036a4a0a01081b097d083943280a340002ea'
        '4a0a01081b097d0839432c0a3400026a4a0a01081b097d083943300a340001ea4a0a01081b097d083943340a3400016a4a0a01081b097d083943380a340000ea4a0a0108'
        '1b097d0839433c0a3400006a4a0a01081b097d08529fab4972bd76896b09011f5400020d528af54972aab1c96b09011f54000b0c529f81e972a674a96b09011f5400260c'
        '529fab6972bd76896b09011f54008a005280d8a972a00ac914000136529802a972b866c96b09011f54001bed5294d26972bcef496b09011f5400234c529802c972b866c9'
        '6b09011f540088205297034972bc2c69140001271e2228001e2218001e2140821e2022081e24cc421e222a02bd4080041e240804bd0080041e210801bd008401bd408801'
        '1e210801bd0088011e2e10011e2038201e2338411f010c00bd008c00d65f03c05286666972a762496b09011f54001d0c5288320972a322a96b09011f5400240c528fee69'
        '72a1daa96b09011f5400324c52887e2972a10ae96b09011f5400444c5280d8a972a00ac96b09011f54006f205291e34972a08ea96b09011f5400830152b870081e270103'
        '1e2328231e20c0631e2110041e24206054ff6b255298000872b880281e2701031e23204054ff6a8d5288000872b880081400030f528000091400000d529d09c972aefbe9'
        '6b09011f54001c0c528af56972aab1c96b09011f54007f20529cd78972aedbe9140000df5280002952a8868a1e2701451e252040540000a51e2010051f0508001e221800'
        '14000002bd40042037000d6934000f08528d892872aead88b940b8096b08013f54000ca072181d3f54000e20b84b90096b08013f54000c0072181d3f54000d80b84ba009'
        '6b08013f54000b6072181d3f54000ce0b84bb0096b08013f54000ac072181d3f54000c40b940bc096b08013f54000a2072181d3f54000ba0b84bd0096b08013f54000980'
        '72181d3f54000b00b84be0096b08013f540008e072181d3f54000a60b84bf0096b08013f5400084072181d3f540009c0b940c0096b08013f540007a072181d3f54000920'
        'b84c10096b08013f5400070072181d3f54000880b84c20096b08013f5400066072181d3f540007e0b84c30096b08013f540005c072181d3f54000740b940c4096b08013f'
        '5400052072181d3f540006a0b84c50096b08013f5400048072181d3f54000600b84c60096b08013f540003e072181d3f54000560b84c70096b08013f5400034072181d3f'
        '540004c0b940c8096b08013f540002a072181d3f54000420b84c90096b08013f5400020072181d3f54000380b84ca0096b08013f5400016072181d3f540002e0b84cb009'
        '6b08013f540000c072181d3f54000240b940cc096b08013f540001e1fc4740026e0260026e0404626e1c062291022008bd4080036e0c04234d4081034f8090416e1c0483'
        '4f8090603d801c013d802000d65f03c091022008bd4080026e0c04224d4081026e1c04824f8090403d802000d65f03c0529716c972b2df496b09011f54006ce05283e869'
        '72b388696b09011f54006c605280ddc972b811a9140000495282970972baeb096b09011f54000f2c5281ffe972ba8b096b09011f54001cec528b546972b898e96b09011f'
        '54002ccc5280ddc972b811a96b09011f540056a05281102972b831a96b09011f54006a4152a870481e2701031e2328231e20c0631e2110041e24206054ff526552a878a8'
        '1e2701031e23204054ff51ed52a879081400024b529d344972aa47696b09011f54000eac5291624972a821496b09011f54001bec5286fd8972a7c2496b09011f54002b4c'
        '5286668972a762496b09011f54005060528d194972a7c229140001595294d28972bcef496b09011f54006500529504e972bd0f491400000e529f820972a674a96b09011f'
        '5400642052910ec972a7428914000007529d09e972aefbe96b09011f54006340529d6ea972af3be96b09011f540062c017fffdd4529e914972b63e096b09011f5400188c'
        '52941fe972b5f3096b09011f5400296c5293fe6972b558c96b09011f540051a05285450972b596a96b09011f5400616152a8704814000241529e9e4972a5bbc96b09011f'
        '5400198c5289148972a4d0c96b09011f5400280c5288322972a322a96b09011f54005100529c4e4972a3cf696b09011f54005f2152a86fc81e2701031e2328231e20c063'
        '1e2110041e24206054ff47455290000872a873481e2701031e23204054ff46ad5290000872a873a8140001f052866f4972bd93496b09011f5400176c5283f3c972bc1069'
        '6b09011f540026ec5282972972baeb096b09011f54004e805282fbe972bb2b096b09011f54005b4152a86fe81e2701031e2328231e20c0631e2110041e24206054ff4365'
        '52a84c881e2701031e23204054ff42ed52a84f88140001d352934e0972ab87e96b09011f5400152c5284c32972aa68c96b09011f5400262c529d346972aa47696b09011f'
        '54003920528490e972aa48c9140001f35294dae972b516696b09011f5400278c529781c972b2b2c96b09011f54003b605282a98972b4cb696b09011f5400564152b86e08'
        '1e2701031e2328231e20c0641e2110031e232080540001655290000872a87b081e2701041e242040540000cd5290000872a87b681e2701041e2420405400536452b86f48'
        '1e2701041e2428241e20c0841e23208054ff3c655290000872a879e81e2701031e23204054ff3bcd5290000872a87a48140001995290532972a21aa96b09011f5400250c'
        '528fee8972a1daa96b09011f54004460529020e972a1faa96b09011f540050e152a86d681e2701031e2328231e20c0631e2110041e24206054ff39055290000872a87188'
        '1e2701031e23204054ff386d5290000872a871e81400017e528264a972bacb096b09011f5400248c5282000972ba8b096b09011f540042a05282326972baab096b09011f'
        '54004d8152a86fe81e2701031e2328231e20c0631e2110041e24206054ff35a552b84f881e2701031e23204054ff352d52b84c88140001655298f52972a928696b09011f'
        '5400244c5291626972a821496b09011f54003320529a644972a852a9140000af5282c6e972b737c96b09011f5400282c529e916972b63e096b09011f54003f8052933429'
        '72b721a96b09011f5400490152a86f681e2701031e2328231e20c0631e2110041e24206054ff312552a880881e2701031e23204054ff30ad5290000872a880a814000140'
        '52883b2972a699c96b09011f5400280c529e9e6972a5bbc96b09011f54002e80529f820972a674a96b09011f540045c11e36100314000165529eb74972bdd2696b09011f'
        '540028cc52866f6972bd93496b09011f540026c0529e850972bdb26914000156529bbf2972ad3b496b09011f5400298c52934e2972ab87e96b09011f54002420528680e9'
        '72ac22091400010d529948c972b156e96b09011f540039c052997b2972b176e96b09011f540041c152b86e281e2701031e2328231e20c0631e2110041e24206054ff29e5'
        '5288000872a882081e2701031e23204054ff294d5298000872a882281400010552887e4972a10ae96b09011f54002400528f8aa972a144296b09011f54003ee152a86f68'
        '1400006a528b548972b898e96b09011f540022c052956d4972b922296b09011f54003da152a86f681e2701031e2328231e20c0631e2110041e24206054ff25c552b84988'
        '1e2701031e23204054ff254d52b84688140000e65286fda972a7c2496b09011f54002540528d4ba972a7e2296b09011f54003b0152a86d681e2701031e2328231e20c063'
        '1e2110041e24206054ff23255290000872b871881e2701031e23204054ff228d5290000872b87128140000cf5294200972b5f3096b09011f540020e05294526972b61309'
        '140000b9528914a972a4d0c96b09011f54001c605282a86972a506496b09011f540037416f00e4031e2328231e2140641e2020281e234c831e2110041e24206052900008'
        '72a87b481e2701031e2344445290000872a87ba8140001a35283f3e972bc10696b09011f54002e20529d644972bcbca96b09011f5400348152a86f481e2701031e232823'
        '1e20c0631e2110041e24206054ff1ca55290000872a87dc81e2701031e23204054ff1c0d5290000872a87e281400009b5284c34972aa68c96b09011f540016c0528ac309'
        '72aa91c96b09011f540031a152a86b081e2701031e2328231e20c0631e2110041e24206054ff19c552b883281e2701031e23204054ff194d5290000872b882e814000085'
        '5294db0972b516696b09011f540014005293cc0972b538c96b09011f54002ee152a86d681e2701031e2328231e20c0631e2110041e24206054ff17055290000872a87128'
        '1e2701031e23204054ff166d5290000872a871881400006e5290534972a21aa96b09011f54002700529085a972a23aa96b09011f54002c0152a86d681e2701031e232823'
        '1e20c0631e2110041e24206054ff14255290000872b871e81e2701031e23204054ff138d5290000872b8718814000057528264c972bacb096b09011f540025c0528bfa09'
        '72bade296b09011f5400292152a86a881e2701031e2328231e20c0631e2110041e24206054ff11455290000872a87aa81e2701031e23204054ff10ad5290000872a87b08'
        '140000405298f54972a928696b09011f5400242052845e8972aa28c96b09011f5400264152a870481e2701031e2328231e20c0651e20c0431e2110041e2420a054000065'
        '1e2420605400244452a865c81e2701051e2528251e20c0a51e2420a05400012552b864081e2701041e242040540000ad52b863481e2701041e2420405400228452b86f28'
        '1e2701021e2228221e20c0441e2110021e22208054ff0b651e2220605400216417fff8585282c70972b737c96b09011f540002805282c04972b757a96b09011f54002101'
        '52b86f681e2701031e2328231e20c0631e2110041e24206054ff09255290000872b880a81e2701031e23204054ff088d52b880881e270103140000381e3610031400001e'
        '52883b4972a699c96b09011f540003005284cd0972a6c7e96b09011f54001de16f00e4031e2328231e2140641e2020281e234c831e2110041e24206052b850c81e270103'
        '1e23444452b84e88140000da529eb76972bdd2696b09011f54000080529ee9c972bdf2691400001152a86d681e2701031e2328231e20c0631e20c0421e2110041e242060'
        '1e24444054ff0365140000cc529bbf4972ad3b496b09011f54000200529abc8972ae76e96b09011f5400194152a86d681e2701031e2328231e20c0641e2110031e232080'
        '54ff01651e20c0421e2320405400174417fff80752b86f681e2701031e2328231e20c0631e2110041e2420605290000872b880a81e2701031e23444452b88088140000aa'
        '52b86cc81e2701031e2328231e20c0631e2110041e2420605290000872a87ce81e2701031e2344445290000872a87d481400009d52b86ac81400006152b86b8817ffffc8'
        '52a871081e2701031e2328231e20c0641e2110031e23208052b870a81e2701041e24444452b870481e2701041e24c440540011e452a86d881e2701041e2428241e20c084'
        '1e232080540001655290000872b870681e2701031e232040540000cd5290000872b870081e2701031e23204054000fe452a86b8817ffffbb52a86d681e2701031e232823'
        '1e20c0631e2110041e2420605290000872a871281e2701031e2344445290000872a871881400006d52a86f681e2701031e2328231e20c0631e2110041e24206052a867c8'
        '1e2701031e23444452a868881400006252a86fe817ffff8f52a86d681e2701031e2328231e20c0631e2110041e2420605290000872b871e81e2701031e23444452900008'
        '72b871881400005352a86fe81e2701031e2328231e20c0631e2110041e24206052b840881e2701031e2344441e3770031400004952a86f681e2701031e2328231e20c063'
        '1e2110041e24206052a880881e2701031e2344445290000872a880a81400003c52b870c81e2701031e2328231e20c0631e2110041e2420605288000872a882081e270103'
        '1e2344445298000872a882281400002f52a86f681e2701031e2328231e20c0631e2110041e24206052b868881e2701031e23444452b867c81400002452a86d681e270103'
        '1e2328231e20c0631e2110041e2420605290000872a871881e2701031e2344445290000872a871e81400001752a86fe81e2701031e2328231e20c0631e2110041e242060'
        '1e2770031e23444452a840881400000d52b86ba81e2701031e2328231e20c0631e2110041e2420605290000872a879081e2701031e2344445290000872a879681e270103'
        '1e23c44054fee9e51e2140021e2020281e22cc001e202820bd008c00d65f03c0528c226972bef4496b09011f5400028d528b8d0972a830096b09011f540006ad529aeec9'
        '72ae96e96b09011f54000e6d529b812972afc2496b09011f5400116c529aeee972ae96e96b09011f54ff9c605290dfa972af54a914000072529336c972b714896b09011f'
        '5400020c529022a972b544296b09011f540005ec5293042972b00d496b09011f54ffe2a0528fbe0972b504296b09011f54ffe220528ff06972b5242914000077529237a9'
        '72b7eb896b09011f5400076c529336e972b714896b09011f54000fe05289460972b714e96b09011f54000dc0528299a972b749696b09011f54fee1e152a86ca817fffefd'
        '5294866972a1acc96b09011f5400068c528c228972bef4496b09011f54000900529fc50972a03c296b09011f54ffde00529776e972a069e96b09011f54ffd5c017fff6fc'
        '529022c972b544296b09011f540001205290878972b584296b09011f540000a05290b9e972b5a4296b09011f54fede0152a86b881e2701031e2328231e20c0631e211004'
        '1e24206054fedd255290000872b87ba81e2701031e23204054fedc8d5290000872b87b4817fffe9f529237c972b7eb896b09011f540004005291656972ba9e296b09011f'
        '54ff90e0529c6ca972be1c291400002e5294868972a1acc96b09011f540002a05291c60972a42fa96b09011f54000520528a2c8972a750091400000b528b8d2972a83009'
        '6b09011f54ff8ea05284f98972a8b5496b09011f54ffce805297402972a9d2296b09011f54ff8da017fff6be52a86b881e2701031e2328231e20c0631e2110041e242060'
        '54fed6e55288000872b882a81e2701031e23204054fed64d5298000872b8826817fffe6d529b814972afc2496b09011f540000c05292d1c972afed496b09011f54ffd2c0'
        '17fff6a652a865c81e2701031e2328231e20c0631e2110041e24206054fed3e552b86f681e2701031e23204054fed36d52b86ea817fffe5752b86ea817fffe87bd400020'
        '1e20200854000041d65f03c0bd405001bd4010201e20202054ffff84bd400421bd405402bd4014231e3e10041f0110641e2320401e24544054fffe8abd4030041e20c084'
        '52a844081e2701051e25208054fffdcabd4034041e20c0841e2701051e25208054fffd2a5280030852a895e91e270124aa0003e9140000037100050854000280f9400d29'
        'b4000249f940012aeb02015f54ffff41bd405125bd4071261e2608a51e20c0a51e2020a054fffe8bbd405526bd4085271e2708c61e20c0c61e2420a01e23a4c854fffdab'
        '17ffffcf1e220820bd005400d65f03c0'
    )
    DUO_SIZE = 0x25b4   # duo_size's offset in ANCHOR_WORDS
    anchor_code = here(); hx = ''.join(ANCHOR_WORDS)
    for i in range(0, len(hx), 8):
        emit(int(hx[i:i + 8], 16))
    def vt(r, off): emit(ADRP(r, here(), off)); emit(ADDI(r, r, off & 0xfff))   # x<r> = main + off (a vtable)
    # x0-x17 and the caller-saved vector registers are free at main+0x27ae0 (the flags are loaded next; x19 = the pane, x20 / x21 the arguments; x30 saved at entry)
    fs_site(0x27ae0, '68624139')
    start = here(); emit(0xaa1303e0); emit(ADRP(1, here(), FS_STATE_OFF)); emit(0x91020021)   # mov x0,x19; x1 = state + 0x80
    vt(2, 0x1d0e788); vt(3, 0x1d0d7a8); vt(4, 0x1d0cec8); vt(5, 0x1d0de38); vt(6, 0x1d0ea58)   # the cut-out pane's, the capture panes', the bounding pane's and the window's vtables
    emit(B(here(), anchor_code) | 0x80000000); emit(0x39416268); emit(B(here(), 0x27ae4))   # bl duo_calc; ldrb w8,[x19,#0x58]; back
    fs_put(0x27ae0, [B(0x27ae0, start)], 'Pane::Calculate: a HUD widget at the edge moved to the taller screen\'s edge with its children, full-screen backgrounds stretched (duo_anchor.c)')
    # The two capture panes size their texture as their size x the framebuffer / the layout's own size (main+0xf5a0dc), and a capture of the screen behind it copies
    # the rect centred at half the layout's height (main+0xf5a4b8): both read the layout file's 1280x720, so on the taller screen a 720-unit capture copied its middle
    # band into a texture of the whole screen's rows, and one made as tall as the screen copied a rect 88 units too high (Mac, a Master Mode save: the inventory's title
    # bar brighter on the right, its bottom bar black, the weapon wheel's screenshot over the live HUD). Both take the screen's layout height (state +0x50) instead of a
    # shorter one while the screen ratio is on (+0x58). x16 is free at both; s16 / s17 and s20 / s21 are unused there. The other capture class's copy of the same code
    # (main+0xf66118 the texture, main+0xf66334 the copied rect; x16, s20 and s21 unused in it) is the dialog box's blurred backing: with 720 its rect stood 88 below
    # the box, so the copy's lower quarter read past the screen's bottom edge, black through the clamp-to-border sampler (an iPhone Duo: a dark band across an NPC's
    # dialog box from the 720 space's bottom edge down; on the Mac the copy's centre moved from v 0.9587 to 0.8605, the box's own, and the band went)
    for site, orig, words in ((0xf5a0dc, '2211452d', (0xbd405a10, 0x1e202208, 0x540000a0, 0xbd405211, 0x1e312080, 0x5400004a, 0x1e204224)),    # s4 = the height
                              (0xf5a4b8, '6006452d', (0xbd405a14, 0x1e202288, 0x540000a0, 0xbd405215, 0x1e352020, 0x5400004a, 0x1e2042a1)),    # s1 = the height
                              (0xf66118, '812f40bd', (0xbd405a14, 0x1e202288, 0x540000a0, 0xbd405215, 0x1e352020, 0x5400004a, 0x1e2042a1)),    # s1 = the height
                              (0xf66334, '8007452d', (0xbd405a14, 0x1e202288, 0x540000a0, 0xbd405215, 0x1e352020, 0x5400004a, 0x1e2042a1))):   # s1 = the height
        fs_site(site, orig)
        start = here()
        emit(struct.unpack('<I', bytes.fromhex(orig))[0]); emit(ADRP(16, here(), FS_STATE_OFF))
        for w in words:   # ldr s,[x16,#0x58]; fcmp s,#0.0; b.eq back; ldr s',[x16,#0x50]; fcmp height,s'; b.ge back; fmov height,s'
            emit(w)
        emit(B(here(), site + 4))
        fs_put(site, [B(site, start)], 'capture: the screen\'s layout height instead of the layout file\'s 720 (texture size, copied rect)')
    # the capture panes' Calculate, at entry (x0 = the pane, x1-x3 the arguments, kept on the stack around the call)
    for site, orig, cls in ((0xf60cc0, '08604139', 0), (0xf59b2c, '086c4339', 1)):
        fs_site(site, orig)
        start = here()
        for w in (0xa9bd7bfd, 0xa90107e0, 0xa9020fe2, 0x52800003 | cls << 5):   # stp x29,x30,[sp,#-0x30]!; stp x0,x1,[sp,#0x10]; stp x2,x3,[sp,#0x20]; mov w3,#cls
            emit(w)
        emit(ADRP(1, here(), FS_STATE_OFF)); emit(0x91020021); vt(2, 0x1d0e788); emit(B(here(), anchor_code + DUO_SIZE) | 0x80000000)   # bl duo_size
        for w in (0xa9420fe2, 0xa94107e0, 0xa8c37bfd, struct.unpack('<I', bytes.fromhex(orig))[0]):   # ldp x2,x3; ldp x0,x1; ldp x29,x30,[sp],#0x30; the original instruction
            emit(w)
        emit(B(here(), site + 4))
        fs_put(site, [B(site, start)], 'capture pane Calculate: a full-screen capture made as tall as the screen (duo_anchor.c)')
    # The map's footprint trail is drawn into a 2400x1140 target through an orthographic projection of the HUD's frame (main+0x127a2c8 copies the frame's rect to the
    # trail's viewport, +0xb0, and sets the projection, +0xd0, from it), and that target is shown on a 1280x720 pane over the terrain. The frame is the taller screen's
    # 1280x896 now, so the trail came out 720 / 896 = 0.8 times as tall as the terrain under it, more off the further from the middle and shifting as the map zoomed
    # (Mac, a Master Mode save: the viewport and projection held 448 / -448 against 360 / -360 at 16:9). The half-height is kept at the pane's 360 at most: s1 = half the
    # height, w9 and s6 are unused there (nothing changes at 16:9)
    fs_site(0x127a2f8, '2108221e')
    start = here()
    for w in (0x1e220821, 0x52a87689, 0x1e270126, 0x1e265821):   # fmul s1,s1,s2 (the original); mov w9,#0x43b40000 (360.0); fmov s6,w9; fmin s1,s1,s6
        emit(w)
    emit(B(here(), 0x127a2fc))
    fs_put(0x127a2f8, [B(0x127a2f8, start)], 'map footprint trail: its viewport and projection at the 720 pane\'s half-height, not the taller frame\'s')
    # A picture whose texture is projected from the screen fitted to the layout's size (the layout library's projection texture coordinates, main+0x2deb0: the size
    # from the layout, +0x28, main+0x2df28; x22 = the pane's global matrix, x3 = its size) maps the layout file's 1280x720 onto the texture: the menus' blurred
    # background (a 320x180 copy of the whole screen) on a full-screen pane stretched to the taller screen read rows -0.12..1.12 of it, and its top and bottom 88
    # units were the edge rows drawn on (Mac, a Master Mode save: the inventory's guide bar over vertical streaks). A pane as tall as the taller screen on it (720 x
    # the stretch, or a capture made 896 tall) takes the screen's layout height (state +0x50); a 720 one keeps the file's (the map's captures in its cut-out). s16-s18
    # and x16 are unused there (loaded after)
    fs_site(0x2df28, '0305452d')
    start = here(); emit(0x2d450503); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xbd405a10, 0x1e202208, 0x540001a0, 0xbd405210, 0x1e302020, 0x5400014a,   # ratio on (+0x58); s16 = the layout height (+0x50); the file's shorter
              0xbd400471, 0xbd4016d2, 0x1e320a31, 0x1e20c231, 0x1e201012, 0x1e322a31,   # s17 = |size.h x global sy| + 2
              0x1e302220, 0x5400004b, 0x1e204201):                                      # as tall as the screen: s1 = the layout height
        emit(w)
    emit(B(here(), 0x2df2c))
    fs_put(0x2df28, [B(0x2df28, start)], 'projected texture fitted to the layout: a pane as tall as the taller screen maps its whole height')
    # The camera rune's photo, and any other copy of the screen into a 16:9 picture (main+0xf759c4, its copy at main+0xf75ba4 through main+0xed81f4: x2 the
    # picture's viewport, sead::Viewport min +8 / max +0x10; x3 the texture coordinates' scale, uv = 0.5 + (corner / 2 + offset) x scale, the copy shader's
    # constants +0x60 / +0x58), copies the whole screen: on the taller screen 1.24 times as many rows went into the same 854 x 480 photo and it came out squashed
    # to 0.8 of its height (Mac, a Master Mode save: the photo matched the same photo at 16:9 only stretched by 1.24). When the picture is about 16:9 (state +0x74)
    # the vertical scale is divided by k, so it takes the screen's middle 16:9 band, which the widened view makes the same picture as at 16:9 (matched at 1.0
    # after). x16 and s16-s20 are free at the call
    fs_site(0xf75ba4, '9489fd97')
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xbd405a10, 0x1e202208, 0x54000200,                         # ldr s16,[x16,#0x58] (ratio); fcmp s16,#0; b.eq the call (off)
              0x2d414851, 0x2d425053, 0x1e313a73, 0x1e323a94, 0x1e341a73, # s19 = the picture's width / height
              0xbd407614, 0x1e343a73, 0x1e20c273, 0x1e281014, 0x1e342260, 0x540000aa,   # not 16:9 (|s19 - 16/9 (+0x74)| >= 0.125; 854 / 480 is 0.0014 off): the call
              0xbd400471, 0xbd405e12, 0x1e321a31, 0xbd000471):            # scale y /= k (+0x5c)
        emit(w)
    emit(B(here(), 0xed81f4))
    fs_put(0xf75ba4, [B(0xf75ba4, start) | 0x80000000], 'screen copy into a 16:9 picture (camera photo): the screen\'s middle 16:9 band, not the whole taller screen squashed into it')


FRAME_STEP = {'cd57b23fa4bbad65803d9788c01821ee': frame_step}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'interval controller: the lower bound and the GPU-time path', 'now': 'build', 'goal': 'game', 'purpose': 'frame rate',
     'why': "sead::GameFrameworkNx's interval controller in this build; Tears of the Kingdom shares sead but not this code"},
    {'what': '60 fps frame step and render scale (ksys::VFR)', 'now': 'build', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own frame code; the frame-step state page is general. No engine sibling shares it: none of its 34 functions has an exact match in Tears of the "
            'Kingdom, SMO, MK8D, ACNH, Splatoon 2 / 3, Kirby, Mario Party, Ocarina, Smash, Metroid Dread or Origami King (only the widescreen stubs\' sead::PerspectiveProjection '
            'functions recur)',
     'checked': 'nso_fnmap of its 44 sites into those 13 mains (2026-11-09)',
     'kept': '44 sites in 34 functions to fingerprint for an update that may not come (1.9.0, the Switch 2 Edition release); convert when a new build appears'},
]
