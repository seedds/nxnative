# Hyrule Warriors: Age of Calamity (01002B00111A2000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
import struct
from nso_asm import ADRP, B
# Hyrule Warriors: Age of Calamity steps its game a fixed amount per update, from two 30 fps values. A KIDS task (main+0x166d980, on a KIDSTaskScheduler thread, once per update)
# waits until 1 / fps has passed since the last update (fps at time +0x30: 30 from a frame-rate object, main+0x166daa4; csinc at main+0x166db80) and then steps the game clock
# (system +0x3348, the step at +0x3350) by that object's step (x23 +0x34: 1/30 s times the game's timescale, a second time at +0x44, fps 30 at +0x48), not by the time that
# passed (the measured path, main+0x166dca8, runs only without the object). The battle itself steps 1 / the game's frame rate per update: a block at main+0x269e150 {fps,
# interval, fps, float fps} = {30, 2, 30, 30.0}, read by the scene update's caller (main+0x8c3610: step = 1.0 / fps) and its jobs (32 reads; the game's own writers at
# main+0x8a35e0 / main+0xfc1618, a run-time interval request at +0x1d58, never ran in a probe). The event scripts step by the KTGL device's frame rate (device +0x32d0, from the
# interval in its config: the third stub). The updates are not held by the presents (a task kept 60 a second while the host held frames 3 vsyncs), so they run at the cap and
# the host's frame step paces only the presents: at main+0x166dab4 (where both fps paths join, before the update's tasks and the wait) the task's fps and the frame-rate block
# become the cap (60, or 30 at a 30 cap), the update's vsyncs (60 / cap) are marked (+0x28) and the device frame rate becomes 59.94 / those vsyncs; at main+0x166dc60 the
# object's steps are scaled by its fps × vsyncs / 60 and the vsyncs are counted. Every step is then per update as on the Switch, so an update rate below the cap slows them all
# together. With only the task's wait at 1/60 s the game clock ran at twice real time; with the clock fixed and the block left at 30, battles still ran at twice (Link covered
# twice the ground in a 3 s stick hold; enemy waves came early). Off (target 0), the device frame rate follows the game's fps (29.97 at 30, as with interval 2) and the steps and
# the block stay the game's. x16, x17, s16–s18 and the flags are dead at both sites; the function uses no x16 / x17
def stub(ctx, kids, nn_main, scene, block, device, rate=None):
    """The frame step at the KIDS frame task (kids), nnMain's device config (nn_main), the scene update's event counters (scene), the game's frame-rate block (block: its four
    words) and the global holding the KTGL device (device); rate is 59.94's float in read-only data, or None to make it in a register (one instruction more)"""
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    assert block % 4 == 0 and (block & 0xfff) + 0xc < 0x1000 and device % 8 == 0
    more = 0 if rate else 1   # the forward branches over the device part
    s_task, s_steps, s_device = kids + 0x134, kids + 0x2e0, nn_main + 0x1e60
    fs_site(s_task, '68734339'); fs_site(s_steps, 'e83640b9'); fs_site(s_device, '4900c0f2')
    # nnMain builds KTGL's device config on its stack and inits the device once (main+0x8d94c4 → main+0x17c34f0): {1920, 1080, 3, 9, 5, interval 2, 9, …}. The device keeps the
    # interval (main+0x17c4ba8 → device +0x2aa4, passed to every present at main+0x17ced78, which sets it on the window when it changes) and its frame rate, 59.94 / interval
    # (main+0x17c37c8 → device +0x32d0). Interval 1 above a 30 fps cap (the engine's own 60 fps mode), else the game's 2. x16, x17 and the flags are dead up to the init call
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9400210,   # ldr w16,[x16]          target fps (float bits)
              0x52a83e11,   # mov w17,#0x41f00000    30.0
              0x6b11021f,   # cmp w16,w17
              0x5400006d,   # b.le two (+3)
              0xf2c00029):  # movk x9,#0x1,lsl #32   interval 1
        emit(w)
    emit(B(here(), s_device + 4)); emit(0xf2c00049); emit(B(here(), s_device + 4))   # two: movk x9,#0x2,lsl #32 (the original)
    fs_put(s_device, [B(s_device, start)], 'KTGL device config present interval: 1 above a 30 fps cap, else the game\'s 2 (state page target fps)')
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xbd400210,   # ldr s16,[x16]          target fps (0 = off)
              0x1e202208,   # fcmp s16,#0.0
              0x540001ad,   # b.le off (+13)
              0x1e380208,   # fcvtzs w8,s16          the cap
              0xb9003268,   # str w8,[x19,#0x30]     the task's fps
              0x52800791,   # mov w17,#60
              0x1ac80a31,   # udiv w17,w17,w8        vsyncs per update
              0xb9002a11):  # str w17,[x16,#0x28]    this update's vsyncs (on)
        emit(w)
    emit(ADRP(16, here(), block)); lo = block & 0xfff
    for w in (0xb9000208 | lo // 4 << 10,         # str w8,[x16,#lo]       the game's frame-rate block: fps
              0xb9000211 | (lo + 4) // 4 << 10,   # str w17,[x16,#lo+4]    interval
              0xb9000208 | (lo + 8) // 4 << 10,   # str w8,[x16,#lo+8]     fps
              0x1e220111,                         # scvtf s17,w8
              0xbd000211 | (lo + 12) // 4 << 10,  # str s17,[x16,#lo+12]   float fps (the battle's step is 1 / it)
              0x14000007,   # b dev (+7)
              0xb9002a1f,   # off: str wzr,[x16,#0x28]
              0xb9403268,   # ldr w8,[x19,#0x30]     the game's fps
              0x34000008 | (12 + more) << 5,   # cbz w8,orig
              0x52800791,   # mov w17,#60
              0x1ac80a31,   # udiv w17,w17,w8        vsyncs per update at that fps
              0x34000011 | (9 + more) << 5):   # cbz w17,orig
        emit(w)
    emit(ADRP(16, here(), device))                                   # dev:
    for w in (0xf9400210 | (device & 0xfff) // 8 << 10,   # ldr x16,[x16,#lo]     the KTGL device
              0xb4000010 | (6 + more) << 5):              # cbz x16,orig
        emit(w)
    if rate: emit(ADRP(8, here(), rate)); emit(0xbd400110 | (rate & 0xfff) // 4 << 10)   # ldr s16,[x8,#lo]  59.94
    else:
        bits = struct.unpack('<I', struct.pack('<f', 59.94))[0]
        emit(0x52800008 | (bits & 0xffff) << 5); emit(0x72a00008 | (bits >> 16) << 5); emit(0x1e270110)   # mov / movk w8; fmov s16,w8: 59.94
    for w in (0x1e230231,   # ucvtf s17,w17
              0x1e311a10,   # fdiv s16,s16,s17
              0xbd32d210,   # str s16,[x16,#0x32d0]  the device frame rate
              0x39437368):  # orig: ldrb w8,[x27,#0xdc]
        emit(w)
    emit(B(here(), s_task + 4))
    fs_put(s_task, [B(s_task, start)], 'KIDS frame task: fps and the frame-rate block = the cap, vsyncs per update marked; device frame rate 59.94 / vsyncs (state page target fps; 0 = the game\'s 30)')
    start = here(); emit(0xbd4036f0)   # ldr s16,[x23,#0x34]    the frame-rate object's step
    emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9402a11,   # ldr w17,[x16,#0x28]    vsyncs per update (0 = off)
              0x340001f1,   # cbz w17,off (+15)
              0xf9401a08,   # ldr x8,[x16,#0x30]
              0x8b314108,   # add x8,x8,w17,uxtw
              0xf9001a08,   # str x8,[x16,#0x30]     vsyncs stepped
              0xb9404ae8,   # ldr w8,[x23,#0x48]     the object's fps
              0x34000148,   # cbz w8,off (+10)
              0x1b087e31,   # mul w17,w17,w8
              0x1e230231,   # ucvtf s17,w17
              0x52800788,   # mov w8,#60
              0x1e230112,   # ucvtf s18,w8
              0x1e321a31,   # fdiv s17,s17,s18       its fps × vsyncs / 60
              0xbd4046e0,   # ldr s0,[x23,#0x44]
              0x1e310a10,   # fmul s16,s16,s17
              0x1e310800,   # fmul s0,s0,s17
              0x14000002,   # b out (+2)
              0xbd4046e0,   # off: ldr s0,[x23,#0x44]
              0x1e260208,   # out: fmov w8,s16
              0xb9001a68):  # str w8,[x19,#0x18]     the update's step
        emit(w)
    emit(B(here(), s_steps + 0xc))
    fs_put(s_steps, [B(s_steps, start)], 'KIDS frame task: the frame-rate object\'s steps × its fps × vsyncs per update / 60 (state page target fps; 0 = as the game sets them)')
    # In-engine cutscenes (events) count whole 30 fps frames, one per update. While an event plays, the scene's update (main+0x857510, called from the scene update's
    # caller at main+0x8c39a8) keeps the cutscene's frame ([sp,#0x14]: property 0x91a41ad2, which the event timeline reads at main+0xb0be68 as its target frame) and
    # its frames left ([sp,#0x10], counting down from the cutscene's length; the frame advances only while two or more are left). Each update adds 1 to the frame
    # (main+0x857d7c) and takes 1 from the frames left (main+0x857da0), whatever the update's length, so at a 60 fps cap the cameras and animations ran at twice
    # their speed while voices and subtitles kept real time. Both counters now move on every other update at a 60 fps cap (the state page's vsyncs per update = 1,
    # on even counted vsyncs, +0x30, which the stub at main+0x166dc60 steps once per update); at a 30 fps cap or with the frame step off they move on every update,
    # as the game does. Moving only the frame froze the cutscene halfway: its frames left ran out first. x16, x17 and the flags are dead at both sites
    fs_site(scene + 0x86c, '4a050011'); fs_site(scene + 0x890, '4a050051')
    for site, step, what in ((scene + 0x86c, 0x1100054a, 'frame'), (scene + 0x890, 0x5100054a, 'frames left')):
        start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
        for w in (0xb9402a11,   # ldr w17,[x16,#0x28]    vsyncs per update (1 at a 60 fps cap, 2 at 30, 0 off)
                  0x7100063f,   # cmp w17,#1
                  0x54000061,   # b.ne step (+3)
                  0xf9401a11,   # ldr x17,[x16,#0x30]    vsyncs counted
                  0x37000051,   # tbnz w17,#0,back (+2)  odd: not on this update
                  step):        # step: add / sub w10,w10,#1 (the original)
            emit(w)
        emit(B(here(), site + 4))                        # back
        fs_put(site, [B(site, start)], f"event cutscene's {what}: stepped on every other update at a 60 fps cap (state page vsyncs per update; 30 fps frames, as on the Switch)")

def frame_step(ctx):   # 1.3.0 (v327680): the addresses found there; its main is not on the drives to fingerprint, so it keeps this build entry
    stub(ctx, kids=0x166d980, nn_main=0x8d94b0 - 0x1e60, scene=0x857510, block=0x269e150, device=0x2bac798, rate=0x1ef5f3c)

def matched_frame_step(ctx):
    """Any build whose KIDS frame task, nnMain, scene update and frame-rate block writer match: all four or nothing, as a part alone runs the game at the wrong speed (the
    locator prints a red line for the one not found). The KTGL device's global comes from the task's own load of it (+0xd70), the frame-rate block from its writer's store
    of the fps (+0x3c); 59.94 is made in a register"""
    o = ctx.others
    if None in (o.get('nnMain'), o.get('scene update'), o.get('frame-rate block writer')): return
    stub(ctx, kids=ctx.base, nn_main=o['nnMain'], scene=o['scene update'],
         block=ctx.address(o['frame-rate block writer'] + 0x3c), device=ctx.address(ctx.base + 0xd70))

FRAME_STEP = {'c3cf52bf2b05d7310884971b50bcccb8': frame_step}

# Builds other than 1.3.0, placed by the functions they patch (checked on v458752, 2026-11-09: the task at main+0x166fff0, nnMain at main+0x8d7c80, the scene update at
# main+0x857510, the block writer at main+0xfc3c50; the device global main+0x2bb0798 and the block main+0x26a2280 decoded from them)
MATCH_FRAME_STEP = {'main': [
    {'name': '60 fps frame step: the KIDS frame task, frame-rate block and event cutscene counters', 'size': 4216,
     'fingerprints': ['e87daa5f570f160ba7e99afcba49df5990cf4cfcc30695508b88cf376205ada7'], 'stub': matched_frame_step,   # v458752 main+0x166fff0
     'others': {'nnMain': {'size': 14912, 'fingerprints': ['c88e49226856f8138690a6a65ebfd180026ee2bc8c9d739a79690e8be551067a']},   # main+0x8d7c80
                'scene update': {'size': 2952, 'fingerprints': ['f1e25dd607f97a1e6636a9efc3b3d8dd1c658e3e5ac861eef6ca171692a881c8']},   # main+0x857510
                'frame-rate block writer': {'size': 124, 'fingerprints': ['4109c2f920af61b6779eb03a7a6df9c118b1e123a90de9b3aa48d00c41979f4c']}}},   # main+0xfc3c50
]}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': '60 fps frame step: the KIDS frame task, frame-rate block and event cutscene counters (any build but 1.3.0)', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own frame code, found by its four functions' fingerprints, the KTGL device global and the frame-rate block read from their code. No KTGL sibling shares "
            'it: the KIDS task has no exact match in Three Hopes, Taiko Risshiden V DX, Three Houses, Hyrule Warriors: Definitive Edition or Age of Imprisonment',
     'checked': 'v458752 main: one function of each fingerprint, the five sites and the decoded addresses read by disassembly; nso_fnmap into those five mains (2026-11-09)'},
    {'what': '60 fps frame step on 1.3.0 (the same stub at its known addresses)', 'now': 'build', 'goal': 'game', 'purpose': 'frame rate',
     'why': 'the stub above, placed at 1.3.0\'s addresses: the matched rule would serve it once its functions are fingerprinted',
     'blocked': "1.3.0's main is on neither drive to fingerprint (v458752 is the update on them)"},
]
