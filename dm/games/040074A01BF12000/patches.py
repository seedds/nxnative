# Donkey Kong Bananza (040074A01BF12000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO
# build ID; FRAME_STEP as described there ("Registered patches"). This build has no frame-step stubs: its state page carries only the render scale
from nso_asm import ADRP, B

# Render scale (DM_RENDER_SCALE). The scene renders at the NDSR upscaler's output size (2304x1296 docked, 1920x1080 handheld) divided by an upscale rate that dynamic resolution picks
# from a table of steps between two settings of the graphics parameters, NdsrUpscaleMinRate (default 1.375: 1676x943 at best) and NdsrUpscaleMaxRate (1.9, the game's
# 1152x648 in the heavy areas); the parameter objects are registered at main+0x4ca848 / +0x4ca8cc and their values read at +0x64 by the two dynamic-resolution steps
# (main+0x38db98 stepping down, main+0x38e0f0 stepping up). A stub after each of the four reads replaces the value with the loader's rate, 1 / render scale (state +0x14,
# 0 = the game's own), so both bounds are that rate and the scene keeps that size. x16 and x17 are not used in either function
def frame_step(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    for site, ins, why in ((0x38dd04, 0xbd406502, "NdsrUpscaleMinRate (stepping down) = the host render scale's rate"),   # ldr s2,[x8,#0x64]
                           (0x38dd98, 0xbd406545, "NdsrUpscaleMaxRate (stepping down) = the host render scale's rate"),   # ldr s5,[x10,#0x64]
                           (0x38e104, 0xbd406503, "NdsrUpscaleMaxRate (stepping up) = the host render scale's rate"),     # ldr s3,[x8,#0x64]
                           (0x38e194, 0xbd406542, "NdsrUpscaleMinRate (stepping up) = the host render scale's rate")):    # ldr s2,[x10,#0x64]
        fs_site(site, ins.to_bytes(4, 'little').hex())
        start = here(); emit(ins)                    # the original load
        emit(ADRP(16, here(), FS_STATE_OFF))
        for w in (0xb9401611,                        # ldr w17,[x16,#0x14]   rate (float bits), 0 = the game's own
                  0x34000051,                        # cbz w17,+8
                  0x1e270220 | (ins & 0x1f)):        # fmov sN,w17
            emit(w)
        emit(B(here(), site + 4))
        fs_put(site, [B(site, start)], why)
    # The rate the update settles on (s8 at main+0x38de2c, where both steps and the hold meet before it is compared with the current one and applied): the host's rate, so the
    # scene reaches it whatever the GPU time says (the steps move one table entry at a time, and only with GPU time to spare: with the bounds alone it stopped at 1536x864)
    fs_site(0x38de2c, '697a40f9')
    start = here(); emit(0xf9407a69)                 # ldr x9,[x19,#0xf0]   the original instruction
    emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9401611,                            # ldr w17,[x16,#0x14]
              0x34000051,                            # cbz w17,+8
              0x1e270228):                           # fmov s8,w17
        emit(w)
    emit(B(here(), 0x38de30))
    fs_put(0x38de2c, [B(0x38de2c, start)], "upscale rate the dynamic-resolution update applies = the host render scale's rate")
    # (Writing a rate between the steps into the table's second step, [main+0x26ce408]+0x78 +4, made the snap land on that step but the scene kept the step's own size, 1397x786
    # for 1.2: the targets' sizes are taken per step elsewhere)
    # (Storing the host's rate unsnapped at main+0x38df0c does not give sizes between the steps: the targets follow the step, and 1.2 drew at 2304x1296)
    ctx.render_scale = 1

FRAME_STEP = {'e25c90c1c136638885659fd55bf8a03b': frame_step}

BINDING = [
    {'what': 'render scale (NDSR upscale rate bounds)', 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's graphics parameter objects, read in its dynamic-resolution steps",
     'blocked': 'no function-matched form for stubs yet'},
]
