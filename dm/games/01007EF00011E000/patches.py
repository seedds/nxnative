# The Legend of Zelda: Breath of the Wild (01007EF00011E000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
import struct
from nso_asm import ADRP, B, LDRX
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
    # but takes it only while [main+0x1e3a638]+0x1d2b8 is set (64 of 6,046 shrine frames); both of its checks become branches into it. The GPU time needs the profile's gpu_timestamps segment
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
    # Variable frame step (VAR_STEP: a cap above 60, hle_nvn.c vstep_tick). VFR::updateInterval turns the interval into the frame ratio, the frame's length in 30 fps frames
    # (interval / [+0x150], 2: 0.5 at 60 fps), keeps it at +0x30 and publishes it at +0x3c, from which every per-core step is taken (main+0x151d7e8: delta frames at +0x108,
    # delta time at +0x138). The host's step (state +0x50: the time the last frame took, clamped) is published instead, except while the game's own override is set (cutscenes keep
    # their steps); +0x30 keeps the interval's ratio for them. x9, x10 and s2 are dead at main+0x151d484 (w8 holds the changed flag stored next)
    fs_site(0x151d484, '2020201e')
    start = here(); emit(ADRP(9, here(), FS_STATE_OFF))
    for w in (0x3940a66a,   # ldrb w10,[x19,#0x29]   the VFR's override
              0x350000aa,   # cbnz w10,orig (+5)
              0xbd405122,   # ldr s2,[x9,#0x50]      the host's step (1/30 s units)
              0x1e202048,   # fcmp s2,#0.0
              0x5400004d,   # b.le orig (+2)         not set
              0x1e204041,   # fmov s1,s2             published at +0x3c
              0x1e202020):  # orig: fcmp s1,s0
        emit(w)
    emit(B(here(), 0x151d488))
    fs_put(0x151d484, [B(0x151d484, start)], 'VFR::updateInterval: the published frame ratio is the host\'s variable step (state page +0x50) when set, unless the game overrides its interval')
    # Logic that steps by the interval rather than the frame ratio. The menus (title screen, inventory) and the HUD animated at twice the speed at 120 fps: the layout system's
    # animation step is max(interval, 1) × 0.5 (main+0x1280ec8, the getter main+0x583118 reads the VFR's interval through its per-core pointers at +0x48) into +0xb20 of the
    # layout system [main+0x1e374b8], and a fade adds max(interval, 1) × 0.5 × 0.2 a frame (main+0x125f114). Both take the VFR's frame ratio (+0x3c) × 2 in place of the interval,
    # the same value at whole vsyncs and in cutscenes (ratio = interval / 2). A held button counts frames (sead::ControllerBase::updateDerivativeParams_, main+0xeb0f88: + 1.0 a
    # frame, its repeat delays and pulses in 30 fps frames, so the inventory cursor repeated at twice the speed at 60 fps already); it counts the frame ratio. The physics step
    # (main+0x156f8e8, called from main+0x1281084 with the frame time and the ratio) runs round(6 × ratio) solver substeps: two at 120 fps, where staggered enemies' ragdolls blew
    # apart; it gets at least 0.5, three substeps of half the length at 120 fps, as at 60. These sites were found by MacNativeGames (PR #103, games/totk/runtime/game_patches_botw.c)
    # and checked here by disassembly. x9 at main+0x1280ecc and x8 at main+0x125f118 / main+0xeb0f88 are set again before they are read; s9 is only read by the call's argument
    for site, reg in ((0x1280ecc, 9), (0x125f118, 8)):
        fs_site(site, '0000231e')   # ucvtf s0,w0
        start = here(); emit(ADRP(reg, here(), 0x1e93890)); emit(LDRX(reg, reg, 0x890 // 8))   # the VFR
        emit(0xbd403c00 | reg << 5)   # ldr s0,[xN,#0x3c]      its frame ratio
        emit(0x1e202800)              # fadd s0,s0,s0          × 2: the interval it stands for (× 0.5 below)
        emit(B(here(), site + 4))
        fs_put(site, [B(site, start)], 'interval step: the VFR\'s frame ratio × 2 in place of max(interval, 1) (UI animation / fade)')
    fs_site(0xeb0f88, '080140bd')   # ldr s8,[x8]
    start = here(); emit(ADRP(8, here(), 0x1e93890)); emit(LDRX(8, 8, 0x890 // 8))
    for w in (0xb4000068,   # cbz x8,one (+3)        no VFR yet
              0xbd403d08,   # ldr s8,[x8,#0x3c]      the frame ratio a held button counts
              0x14000002,   # b back (+2)
              0x1e2e1008):  # one: fmov s8,#1.0      as shipped
        emit(w)
    emit(B(here(), 0xeb0f8c))
    fs_put(0xeb0f88, [B(0xeb0f88, start)], 'held buttons count the VFR\'s frame ratio a frame (1.0 at 30 fps)')
    fs_site(0x128106c, '0940201e')   # fmov s9,s0
    start = here()
    for w in (0x1e2c1009,   # fmov s9,#0.5
              0x1e294809):  # fmax s9,s0,s9          at least three physics substeps a frame
        emit(w)
    emit(B(here(), 0x1281070))
    fs_put(0x128106c, [B(0x128106c, start)], 'physics step ratio at least 0.5 (three substeps)')
    ctx.keys['VAR_STEP'] = 1
    # Render scale above 1.0 (RENDER_POOL). gsys's render buffers (the scene's colour, depth, G-buffer and reduced buffers, gsys::RenderBufferContext) come from two 115 MB pools
    # the graphics system makes at start (main+0xed5e70: a compressible texture pool and a virtual range for it), sized by the system's config +0x28, read from [x19+0x4d8] at
    # main+0x106d328. Above a scale of about 1.3 the buffers no longer fitted: an allocation came back empty and RenderBufferContext crashed (main+0xed7128). The size is multiplied
    # by the host's factor (state +0x4c: ceil(scale²), 0 = as is). x10 and x11 are dead there (x8 and x9 are stored next)
    fs_site(0x106d328, '686e42f9')
    start = here(); emit(ADRP(10, here(), FS_STATE_OFF))
    for w in (0xb9404d4b,   # ldr w11,[x10,#0x4c]    pool factor
              0xf9426e68,   # ldr x8,[x19,#0x4d8]    the pool size
              0x3400004b,   # cbz w11,+2             no factor
              0x9b0b7d08):  # mul x8,x8,x11
        emit(w)
    emit(B(here(), 0x106d32c))
    fs_put(0x106d328, [B(0x106d328, start)], 'render buffer pool size × the host factor (state page +0x4c; 0 = the game\'s 115 MB)')
    # The larger pool comes out of the heap the rest of the game shares: nninitStartup sets a fixed 0xc3000000 (3.05 GB, main+0x71c) whatever memory the system reports, and the
    # RootHeap inside it is fixed too (0xc1000000, made at main+0xc617c0), so gsys::SystemTask and KingSystem grew inside a RootTask of the same size and PlacementMgr found no room
    # for its 328 MB heap. Both grow by the bytes the host adds to the application pool for the larger pool (state +0x58, hle_svc.c mem_extra; 0 = the game's sizes)
    fs_site(0x71c, '0060b852')
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xf9402e10,   # ldr x16,[x16,#0x58]    the bytes the host adds
              0x52b86000,   # mov w0,#0xc3000000     the game's heap
              0x8b100000):  # add x0,x0,x16
        emit(w)
    emit(B(here(), 0x720))
    fs_put(0x71c, [B(0x71c, start)], 'nninitStartup heap size = the game\'s + the host\'s added bytes (state page +0x58)')
    fs_site(0xc617c0, '0820b852')
    start = here(); emit(ADRP(9, here(), FS_STATE_OFF))
    for w in (0xf9402d29,   # ldr x9,[x9,#0x58]      the bytes the host adds
              0x52b82008,   # mov w8,#0xc1000000     the game's RootHeap
              0x8b090108):  # add x8,x8,x9
        emit(w)
    emit(B(here(), 0xc617c4))
    fs_put(0xc617c0, [B(0xc617c0, start)], 'RootHeap size = the game\'s + the host\'s added bytes (state page +0x58)')
    ctx.keys['RENDER_POOL'] = '0x7300000'
    # Output size (OUTPUT_W / OUTPUT_H: state +0x40 u64 width | height << 32 and +0x48 float width, written by the loader where the 3D views outgrow 1920x1080; 0 = the game's).
    # Docked, the game composes each frame into 1920x1080 display buffers whatever its 3D views render at, so a larger render scale was only supersampled down to 1080p. The size
    # is a global (main+0x1e43dc0, set at three guarded inits, from which the window and its display buffers are made); the docked mode (main+0x12660bc) sets the output width
    # (+0x1210, the divisor of the view scale above, so the 3D views stay at render scale × the docked base width 1600 at +0x1214), the system's render size (main+0x106dc70) and
    # the window crop (main+0xe9af58). All four take the host's size; the handheld mode keeps 1280x720. Each stub only rewrites the register the site is setting (x4 at the crop
    # holds the height, set just before)
    for site in (0xc61ef4, 0xc61f28, 0x12618c4):
        fs_site(site, '0987c0f2')
        start = here(); emit(ADRP(9, here(), FS_STATE_OFF))
        for w in (0xf9402129,   # ldr x9,[x9,#0x40]      the host's size
                  0xb5000069,   # cbnz x9,+3
                  0xd280f009,   # mov x9,#0x780          the game's 1920x1080
                  0xf2c08709):  # movk x9,#0x438,lsl #32
            emit(w)
        emit(B(here(), site + 4))
        fs_put(site, [B(site, start)], 'output size global = the host\'s (state page +0x40; 0 = 1920x1080)')
    fs_site(0x12660cc, '680a09f9'); fs_site(0x12660d4, '0887c0f2'); fs_site(0x1266104, '03f08052')
    start = here()
    emit(0xf9090a68)            # str x8,[x19,#0x1210]   the game's widths (1920.0, base 1600.0)
    emit(ADRP(8, here(), FS_STATE_OFF))
    for w in (0xb9404908,       # ldr w8,[x8,#0x48]      the host's width (float)
              0x34000048,       # cbz w8,+2
              0xb9121268):      # str w8,[x19,#0x1210]
        emit(w)
    emit(B(here(), 0x12660d0))
    fs_put(0x12660cc, [B(0x12660cc, start)], 'docked output width = the host\'s (state page +0x48; 0 = 1920.0)')
    start = here(); emit(ADRP(8, here(), FS_STATE_OFF))
    for w in (0xf9402108,       # ldr x8,[x8,#0x40]
              0xb5000068,       # cbnz x8,+3
              0xd280f008,       # mov x8,#0x780
              0xf2c08708):      # movk x8,#0x438,lsl #32
        emit(w)
    emit(B(here(), 0x12660d8))
    fs_put(0x12660d4, [B(0x12660d4, start)], 'docked render size = the host\'s output size')
    start = here(); emit(ADRP(3, here(), FS_STATE_OFF))
    for w in (0xf9402063,       # ldr x3,[x3,#0x40]
              0xb4000083,       # cbz x3,+4
              0xd360fc64,       # lsr x4,x3,#32          height
              0x2a0303e3):      # mov w3,w3              width
        emit(w)
    emit(B(here(), 0x1266108))
    emit(0x5280f003)            # mov w3,#0x780          the game's crop
    emit(B(here(), 0x1266108))
    fs_put(0x1266104, [B(0x1266104, start)], 'docked window crop = the host\'s output size')
    ctx.keys.update(VIEW_W='0x640', VIEW_H='0x384', OUTPUT_W='0x780', OUTPUT_H='0x438')   # docked 3D view base 1600x900, output 1920x1080
    # Render scale (state +0x10, float written by the loader: DM_RENDER_SCALE, 1.0 by default; independent of the frame step). The dynamic-resolution method (main+0x12666e4) keeps a
    # scale in 0.8–1.0 rounded to 0.05 steps (+0x1218, main+0x1266818), or the game's forced 0.75 / 0.5 (+0x284 & 0x1800), in s8; from main+0x1266880 it applies s8: each view's
    # scale (+0x28a0 / +0x28a4 = s8 × base width (+0x1214: 1600 docked, 1280 handheld) / output width), from which the scene buffers are sized each frame (main+0x106340c), the
    # shader ratio uking_dynamic_resolution_ratio and the other readers (main+0xc3c3f4, an LOD check at main+0x14126b4). Setting s8 there keeps them all consistent (a multiply in the
    # size function, TOTK's way, would leave the ratio and those readers at the old scale). The apply block runs only when the dynamic path has just recomputed the scale (+0x280 bit 4,
    # set at main+0x1266830) or in a forced mode, else main+0x126686c skips it; with a factor it is taken every frame, as in the dynamic mode, so the factor holds with dynamic
    # resolution off too. Factors above 1.0 grow the render buffer pool and the heaps around it (above). x8, s0, s1 and the flags are dead at both sites
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

FRAME_STEP = {'cd57b23fa4bbad65803d9788c01821ee': frame_step}
