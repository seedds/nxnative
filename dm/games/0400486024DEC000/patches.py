# The Legend of Zelda: Ocarina of Time (0400486024DEC000): the rewriter's patches of this game's own code. dm/tools/patch_nso.py loads every profile's patches.py;
# MATCH_FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
from nso_asm import ADRP, B, BL

# The game's frame ratio. Once a frame the function at main+0x6e6be0 (v131072) divides its base rate (60.0, or 30.0 while bit 0 of [x21+0x24f] is set) by the int frame rate at
# [x21+0x2c8] (fdiv s8,s1,s0 at +0xe4) and hands the result to the time object (main+0x3b4e300: ratio at +0x10, 1 / ratio at +0x14, round(60 / ratio) at +0x1c), which the game
# steps by; the frame's own time never enters it, so a frame that takes two vsyncs at rate 60 plays at half speed. The stub gives the ratio the host's step instead: step × base / unit,
# the unit the ticks a second the host counts the step in (state +0x38: 120, written by the loader for this stub, so 2 / 3 / 4 ticks are 60 / 40 / 30 fps; 0, a page nothing wrote
# there, divides by 60: the division's value when the host keeps frames 60 / rate vsyncs), and counts the units stepped (+0x30). The ratio is a float, so 3 ticks (25 ms, 40 fps on a
# 120 Hz screen) are a ratio of 1.5 at base 60 and 0.75 at base 30. Until the host has decided a step, and at fps_target 0, the division runs.
# s0 and x16 / x17 are free there (s0 is written again on every path before it is read; the function uses neither x16 nor x17); s1 keeps the base rate.
# Lichii (a GPL emulator; only the fact was taken) found the division (its notes: main+0x6E6CC4, 60 / [x21+0x2C8])
frame_ratio = (   # after the ADRP of the state page into x16; the ADRP into x17 after the first 7 words (the add has read x17 then), a B back to the site + 4 after words 14 and 15
    0xb9400211,   # ldr w17,[x16]          target fps (0 = off)
    0x340001f1,   # cbz w17,orig (+15)
    0xb9402211,   # ldr w17,[x16,#0x20]    step
    0x340001b1,   # cbz w17,orig (+13)     not decided yet
    0x1e230220,   # ucvtf s0,w17
    0xf9401a10,   # ldr x16,[x16,#0x30]
    0x8b110210,   # add x16,x16,x17
    0xf9001a30,   # str x16,[x17,#0x30]    units stepped
    0x1e210808,   # fmul s8,s0,s1          step × base rate
    0xb9403a30,   # ldr w16,[x17,#0x38]    the step's ticks a second (0 = 60)
    0x35000050,   # cbnz w16,+2
    0x52800790,   # mov w16,#60
    0x1e230200,   # ucvtf s0,w16
    0x1e201908,   # fdiv s8,s8,s0          / the unit
    0x1e201828)   # orig: fdiv s8,s1,s0
def frame_step(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF, base = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state, ctx.base
    site = base + 0xe4
    fs_site(site, '2818201e')   # fdiv s8,s1,s0
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in frame_ratio[:7]: emit(w)
    emit(ADRP(17, here(), FS_STATE_OFF))
    for w in frame_ratio[7:14]: emit(w)
    emit(B(here(), site + 4))
    emit(frame_ratio[14]); emit(B(here(), site + 4))   # orig: fdiv s8,s1,s0; back
    fs_put(site, [B(site, start)], 'frame ratio: the host\'s step × base rate / its unit (state +0x38: 120 = ticks of 1/120 s, else 60) instead of base rate / the game\'s frame rate (state page target fps; 0 = the game\'s ratio)')
    ctx.capabilities.add('FRAME_STEP_120')   # takes its step in ticks of 1/120 s: the loader writes 120 at +0x38, and 40 fps (3 ticks) is offered
    dynamic_resolution(ctx)
    render_scale(ctx)
    screen_shape(ctx)

# The game's dynamic resolution (game__gfx__DynamicResolutionParam: LoadThreshold, ReduceCounterMax, IncreaseCounterMax and a list of levels, each a ResolutionReduceScale and a
# Priority; on v131072 levels 0..9 at 0.75..0.333 of 2048x1152, 1536x864 to 683x384). Its update (main+0x805aa0) reads the frame's GPU load (about 0.2 ms a unit) into +0x8, subtracts
# its small model of the fixed costs and divides the rest by the square of the current scale: it takes the rest of the frame's cost to grow with the pixel count, as on the Switch 2
# its constants were tuned for. The level function (main+0x8065e0) adds that back times each level's scale squared and returns the best level under the threshold (+0x10, 83 = about
# 16.4 ms). On Apple GPUs this game's cost hardly depends on the resolution (mesh, compute and the upscaler's fixed 2048x1152 dominate): 683x384 cost 14.3 ms and 1536x864 14.7 ms on
# an M4 Pro, yet at 683x384 the extrapolation put 1536x864 at about 300 units against the 83 it allows, so the controller sat at its floor for good (game note, Dynamic resolution).
# The stub replaces the level function. With DM_DYNAMIC_RESOLUTION 0 (the default; state +0x40 0) it returns 0: full resolution. With 1 it climbs on what was measured at each
# level, with no model of how the cost grows. The threshold is the game's at 60 fps; under a host frame rate target below 60 (state +0, DM_FPS_TARGET 30) it is scaled by
# 60 / target, the GPU time a frame of that rate has (83 → 166, about 32.8 ms at 30 fps), so the resolution only drops when the frame would miss the target the player chose.
# One level down when the load (+0x8) has been over the threshold for 8 calls in a row, and that level is marked as not fitting; one level
# up when the load fits and the level above was never measured, fitted when it was last measured, or was measured more than 600 calls (about 10 s at 60 fps, 20 s at 30) ago; else the current level
# (+0x174). Nothing is decided or recorded until a level has held 8 calls: the load is read a few frames late, so before that it may be the previous level's. The 8 calls over in a
# row keep a single slow frame, or the first frames at a new size (its render targets), from marking a level that fits (on the full library at level 1: one burst to 90 against loads
# of 77-80 at levels 2 and 1). The game's counters still pace the steps (down after ReduceCounterMax frames, up after IncreaseCounterMax); its level count (+0x1a0) is the floor.
# The memory is in the state page: +0x200 calls, +0x204 the last call's level, +0x208 calls at it, +0x20c calls over in a row, +0x210 the load measured at each level (16 floats),
# +0x250 the call it was measured at (0 = never). Run against a model of the update (a level moves after the stub asked 5 times up or 2 times down; the load two frames late): a load
# of 70 at every level climbs from the floor to full size in 9 steps, as does Ocarina's measured 72-80 with a spike every 37 frames and a burst after each change; a load that grows
# with the pixel count settles at the highest level that fits and tries the one above once per 600 calls; a scene that gets lighter climbs again, a heavier one steps down.
# The function is a leaf: x2..x11, x16, x17 and s0..s2 are free, w0 its only result (the original also stores its predicted cost at +0x18, which only it and the update's own
# decision read). The update's fingerprint is required too: +0x8 and +0x174 are its fields
drs_controller = (   # drs_stub.s, assembled with clang; x0 the controller, x16 the state page (the ADRP before it)
    0xb9404211, 0x35000071, 0x2a1f03e0, 0xd65f03c0,                                                   # off (state +0x40 0): level 0
    0xbd401001, 0xbd400202, 0x1e202048, 0x540000ed, 0x52a84e0a, 0x1e270140, 0x1e202040, 0x5400006a,   # threshold, × 60 / the target fps (state +0, a float) under 60
    0x1e200821, 0x1e221821,
    0xbd400800, 0xb9417402, 0xb941a003, 0x7100005f, 0x1a9fa042, 0x528001e9, 0x6b09005f, 0x1a89d042,   # load, level (0..15), count
    0xb9420204, 0x11000484, 0xb9020204, 0xb9420605, 0xb9420a06, 0x6b0200bf, 0x1a8617e6, 0xb9020602,   # calls + 1; calls at this level
    0xb9020a06,
    0x35000046, 0xb9020e1f, 0x91084207, 0x91094208, 0x710020df, 0x540003cb,                           # a new level: none over; under 8 calls: stay
    0x1e212000, 0x540001ad, 0xb9420e0b, 0x1100056b, 0xb9020e0b, 0x7100217f, 0x540002eb,               # over: under 8 in a row, stay
    0xbc2258e0, 0xb8225904, 0x51000469, 0x1100044a, 0x6b09015f, 0x1a89d140, 0xd65f03c0,               # 8 in a row: record it, down one (to the floor)
    0xb9020e1f, 0xbc2258e0, 0xb8225904, 0x34000182, 0x51000449, 0xb869590a, 0x340000ea, 0x4b0a008a,   # fits: record it; at 0 stay; the level above's record
    0x7109615f, 0x54000088, 0xbc6958e2, 0x1e212040, 0x5400006a, 0x2a0903e0, 0xd65f03c0,               # never, stale or fitting: up one
    0x2a0203e0, 0xd65f03c0)                                                                           # or stay
# shape_anchor.c built with the command in its header comment (__text words, little-endian hex)
_shape_anchor = (
    '484840b908530034405c40bd01103e1e0028211ee890a8520101271e0108211e2820201e0d520054203840bd0890b8520201271e0208221e0390201e4020231e'
    '03d0231e42a4231ee9090054090080520a0c40f98a5000b44b0940f94b5000b4680140b9081153511f552571c14f00546c0d40f9ac0100b40d0080d2a8b89352'
    '8823b0726e3280520e20a0728f696d38ef00003408010f4a087d0e1bad050091bfc100f141ffff54080080524d8140f94a7942794c3d0012ae0500d14a3d4092'
    '0f1680524a7daf9b707491d2d045b7f2105dd4f270d1e5f20d000dcbbf010aebaa7dd09b4afd45d34db50f9bc23140faa03940fa4a018c1a8ddb86528d9bbb72'
    '1f010d6b8d060054addb86528d9bbb721f010d6b000900542d398c522d2abc721f010d6be0090054edfc8e52ede6af721f010d6b0116005402e4006f5f290071'
    '2d0f00545f3100712d1100545f350071e01f00545f390071a02200545f510071a11400540e0080526d2d9e52ad46b272300080520f89a852e301271e2f008052'
    'a300001403102c1e4328231e6800381e63b8a15e63d8215e4020231ee913881a02e4006f001c221e28791e121f150071a04500543f190071c1f4ff54223440bd'
    '03103c1e2108031f213400bd087cb75222020014edfe8b520df0b2721f010d6ba00500546d2e9d526d6cb6721f010d6b2110005402e4006f5f4501718c070054'
    '5f4100712d0d00545f450071a01f00545f010171a02000545f450171c10e00540e0080520f0080528d4881526d4fa1726f0000145f150071e10d00540e008052'
    '0daf81524d8aa272ef008052b000805202e4006f119fa8522302271e6c00001402e4006f5f0d00712c0200545f090071601400545f0d0071e10b00540e008052'
    'add98e52ad32af722f008052500080525e00001402e4006f5f150071c00100545f7d0071c0000054530000145f110071200100545f1d0071e10900540e008052'
    '0f00805250008052cdfe88528da4bc724e0000140e0080520f008052300080528d9c93520d56aa72480000145f710171ed0600545f750171001900545f790171'
    '001a00545f7d0171610700540e008052adf29452adb6b8722f00805210018052cd0000145f1100712c0100545f090071400f00545f0d0071e10500540e008052'
    '0d8d83526d63bb72770000145f150071e00e00545f190071e10400540e008052adfc9d52cd73bd72740000145f2d0071601000545f310071e10300540e008052'
    '6dc981522d53ae722f008052d0008052110090525192a8722302271e1199a8522202271e1a0000145f2d0071601100545f410071010200540e0080520f008052'
    'cd2a86522da6ae72090000145f490171c01000545f690171e10000540e0080520f008052ad488e52ed93a972f000805206000014100080520f0080520d008052'
    '2e00805202e4006f03e4006f5f010c6b10329f1ad01e0034713940b93f020c6bcc059f1a0c3200370ca840b98c6516539f010f6b813100546c1140f94c3100b4'
    '6b1540f90b3100b48e596af8ae0100b4110080d2acb893528c23b072623280520220a072c3697138e30000348c01034a8c7d021b310600913fc200f141ffff54'
    '0c0080529f010d6be12e00546b596af8ab2e00b46c3940b98c2907539f010f6b212e00546820201e80000054643140bd8020231e812d00544820201e80000054'
    '642d40bd8020221ee12c00541f1200718c0c00540a0e00515f090071231200541f060071e01700541f0a007101180054223440bd4238211ebc0000140e008052'
    '6daa83520d91b2723000805203e4006f2f008052beffff170e0080526dbf83526d83bb72af008052050000140e0080528d659d52cd13bd722f00805270008052'
    '519fa8522302271eb1ffff170e008052cdfb81522d73ae722f008052d0008052110090525192a8722302271e11009852319ba8722202271ea5ffff170e008052'
    '0d9781522d33ae722f008052d0008052110090525192a8722302271e11008852519aa8722202271e99ffff170e0080522d2e82522d93ae722f00805250008052'
    '110090525192a8722302271ed172a8522202271e8effff170e0080526de682528d22be720a0000140e0080520f008052cd8195520dc0b5723001805283ffff17'
    '0e008052cd4096524d03a9722f00805270008052100000140e0080524daf9052cdb8a5720a0000140e0080520f0080520d3f9b52cd7db1723001805273ffff17'
    '0e0080520d1e8e522d02b3722f00805290008052f1a0a8522302271e6cffff171f1e00712c0400541f160071e00900541f1a0071c10b00545f3100710b009052'
    '2b91b8726301271e8b57a8526401271e830c231e5f2d00710a009c52ca90a8724401271e830c231e0aaea8524401271e8218221e2440211e2518462d270040fd'
    '6514071f6410a75fc328241e250c062de390820f230000fd230840bd4208231e220800bd420000141f220071a00100541f260071c1070054223040bd4a8fa852'
    '4301271e4020231e2d070054223440bd4138211e213400bd0874b752cf000014220040bd4440211e4820201e844c221e221440bd4540211e4820201ea54c221e'
    'a518241eea4d82526a50a7724601271e8020261e04e4006fa4cc241e8020261e6d0400540510201e260c051feaa0a8524701271e251c051f1f120071c50c251e'
    'a318231e6318241e241040bd6408241e4208231e2408022d221840bd6208221e221800bd12000014045c40bd2228231e8020231e61000054025c00bd441ca24e'
    '8020221e01150054223440bd03103c1e2108031f213400bd0878b7529f000014223440bd2228221e223400bd3f0d0071201300543f11007180120054a97e9452'
    '292dbe721f01096b8c020054492e9d52696cb6721f01096b6c040054c9fe8b5209f0b2721f01096bad07005469dc9c52297eb3721f01096bac0c0054e9fe8b52'
    '09f0b2721f01096b6010005409d689528914b3727b00001429648952e9e4a6721f01096bec030054e9b688520961a5721f01096bad060054a9d79c52c955a672'
    '1f01096b2c0b005409b788520961a5721f01096ba00d0054c9609e5249fba57268000014a9a38752c9a0ba721f01096b2d06005409398c52292abc721f01096b'
    '2c0a0054c9a38752c9a0ba721f01096b200c0054a9f59652e9efba7250000014695a9e5209f9ab721f01096bad05005449c1845249fcad721f01096b6c090054'
    '895a9e5209f9ab721f01096be0090054897b9752e9c6ad724a000014c9d28552a919b0721f01096b6009005429369c52695bb0721f01096b80080054c9179b52'
    'a9e0b2723f000014c97e9452292dbe721f01096ba0070054693a9a520941bf721f01096b2007005429299d52c964a27234000014692e9d52696cb6721f01096b'
    '4006005469f49852e9d1b9721f01096b20060054c98e805229ecb9722900001449648952e9e4a6721f01096be004005489168352895ca8721f01096b60040054'
    '29e186526969aa721e00001489dc9c52297eb3721f01096be0030054e9af9452a95fb67217000014c9d79c52c955a6721f01096ba0020054a9f28f52a999a672'
    '0700001429398c52292abc721f01096b20020054c9118a52e975bc721f01096ba001005410000014e9fc8e52e9e6af721f01096b0001005469c1845249fcad72'
    '1f01096b01010054081e00511f050071c80000540868b7520101271e0028211e203800bdc0035fd6223440bd080090524884a8720301271e4020231e2a010054'
    '0310221e080090524884b8720401271e4020241ec80000544138211e020000142128221e213400bd0390221e0850b7520101271e6000011feaffff17'
)

screen_shape_functions = {
    'graphics init': {'size': 0x570, 'fingerprints': ['f2c5558922a471ab585d374cc9293013d48da1562d43169367efdc8ea90a3fe8']},   # main+0x1202ce0
    'scene app init': {'size': 0x273c, 'fingerprints': ['2227b93a9fa9dd0fd9aeb64359649df6e2ade519a8e47fa560f840efcdb5d7e7']},   # main+0x31f3890
    'base scene defaults': {'size': 0x3cc, 'fingerprints': ['4d34ae0b1d0fa05603a24e64aae393040502bdfda2fc053a5c3c02ffda42053e']},   # main+0x108b300
    'display mgr defaults': {'size': 0x4e0, 'fingerprints': ['871e4f9e24b2007d3a9ddbb061b6d1a3709c05f623b356daba35fe1dfeb2004c']},   # main+0x36b2bc0
    'base res read': {'size': 0x1a0, 'fingerprints': ['c49c68fad34b6fce5c0dc0f519f5ddda261ad56778917dc2a711553a10cf4db1']},   # main+0xe82f30
    'display target a': {'size': 0x58, 'fingerprints': ['125411e6b2df56ae4e8c0f9c16f134b4ed9247aca4e426c72b406b1f838bfbb9']},   # main+0xd565a0
    'display target b': {'size': 0x1a8, 'fingerprints': ['212c9a344038bdcf99e4a462ecbe1bb58a2b6374d364debd0d0a0664e1dc1175']},   # main+0xebf370
    'display target c': {'size': 0x188, 'fingerprints': ['0b669aaafc4c17cef5c96ac9e4054895152937e9c780f0c72893973256e9dca7']},   # main+0xf96310
    'layout ortho': {'size': 0x7b0, 'fingerprints': ['fed0ce1f2ad6c8f6118a974ec7d5d5a346194a4c10a0fc01f894cfec3d0a1b68']},   # main+0x1520c80
    'layout view ortho': {'size': 0x62c, 'fingerprints': ['103de33fb929cda858c2fbd57b726e6e1bca705bad66de872701819fbf5c6089']},   # main+0x773b90
    'perspective proj': {'size': 0xb8, 'fingerprints': ['e36f61aa83e94dfacc860ab4dd60dd1362cdab40fa490aee9e921a7ae0f3156d']},   # main+0x3485cc0
    'camera update': {'size': 0x1648, 'fingerprints': ['66a50abf2237dee694c40d6a2371071d62e91379d40b643869d680f597bfab02']},   # main+0x33a8c0
    'device proj': {'size': 0x3b0, 'fingerprints': ['ab5c2809cdbc7f9e30b966872350ac69301aa246a9169610d038ea9480bde910']},   # main+0x23b910
    'layout calc matrix': {'size': 0x208, 'fingerprints': ['0e057804c3c8c7d14ff833983335d834e0bd7505e05263e9c065472b9d206c74']},   # main+0x14e9980
    'pane global matrix': {'size': 0x46c, 'fingerprints': ['4eba5ac4bdf9d227e40612fa426d39fbe1abaf00fc069d5d6ba382809b5c5a5c']},   # main+0x152caa0
    # shape_anchor.c reads the array bounds and class fields written by these two functions.
    'layout pane array': {'size': 0x1d0, 'fingerprints': ['cbf1ceccfe3fca0e81ff2f0c4013e728f16c16de2ee4fc928ec2e9f848d2be8f']},   # main+0x1525d60
    'pane constructor': {'size': 0xfbc, 'fingerprints': ['48ccdf26b4c9a8212e6f4b169b89bb834b3c32f8804bb07a172f37b52f8535ef']},   # main+0x1529620
    'movie draw': {'size': 0x158, 'fingerprints': ['cc1ca04feb1ca4e553a6c7a4f36f99b13f56977185eea8ef5cd8dd3dbbdcdbcd']},  # main+0x3b5c710
}


def screen_shape(ctx):
    # Render sizes, projections, panes and movies must agree on the output shape.
    if any(ctx.others.get(name) is None for name in screen_shape_functions):
        return
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    def patch_height(site, orig_hex, state_off, kind, why):
        fs_site(site, orig_hex)
        start = here()
        emit(int.from_bytes(bytes.fromhex(orig_hex), 'little'))
        emit(ADRP(16, here(), FS_STATE_OFF))
        emit(0xb9400210 | ((state_off // 4) << 10))   # ldr w16, [x16, #state_off]
        if kind == 'hi32_4_3':
            body = (0x340000b0,   # cbz w16, +20
                    0x531e7610,   # lsl w16, w16, #2
                    0x52800071,   # mov w17, #3
                    0x1ad10a10,   # udiv w16, w16, w17
                    0xb3607e08)   # bfi x8, x16, #32, #32
        elif kind == 'hi32':
            body = (0x34000050,   # cbz w16, +8
                    0xb3607e08)   # bfi x8, x16, #32, #32
        elif kind == 'w10':
            body = (0x34000050,   # cbz w16, +8
                    0x2a1003ea)   # mov w10, w16
        elif kind == 'f32_w8':
            body = (0x34000070,   # cbz w16, +12
                    0x1e230200,   # ucvtf s0, w16 (s0 overwritten at site+4)
                    0x1e260008)   # fmov w8, s0
        for w in body: emit(w)
        emit(B(here(), site + 4))
        fs_put(site, [B(site, start)], why)

    # All 1920x1080, 2560x1440, 1280x720, and 1080.0f height sites across InitializeGraphicsSystem, scene defaults, and display manager.
    sites = [
        ('graphics init',        0x204, '08b4c0f2', 0x48, 'hi32_4_3', 'swapchain height = host oh * 4 / 3'),
        ('graphics init',        0x3c0, '8886e8f2', 0x50, 'hi32',     'virtual screen height = host layout height'),
        ('scene app init',       0x168, '08b4c0f2', 0x48, 'hi32_4_3', 'scene app RT height (2560x1440 -> 2560 x oh*4/3)'),
        ('scene app init',       0x174, '0887c0f2', 0x48, 'hi32',     'scene app RT height (1920x1080 -> 1920 x oh)'),
        ('base scene defaults',  0x38c, '0887c0f2', 0x48, 'hi32',     'base scene full height (1920x1080 -> 1920 x oh)'),
        ('base scene defaults',  0x39c, '085ac0f2', 0x4c, 'hi32',     'base scene layout height (1280x720 -> 1280 x lh)'),
        ('display mgr defaults', 0x00c, '0a878052', 0x48, 'w10',      'display mgr table height (1080 -> oh)'),
        ('base res read',        0x114, 'e890a852', 0x48, 'f32_w8',   'base res reference height (1080.0f -> (float)oh)'),
        ('display target a',     0x044, '0887c0f2', 0x48, 'hi32',     'display target height a (1920x1080 -> 1920 x oh)'),
        ('display target b',     0x110, '0887c0f2', 0x48, 'hi32',     'display target height b (1920x1080 -> 1920 x oh)'),
        ('display target c',     0x130, '0887c0f2', 0x48, 'hi32',     'display target height c (1920x1080 -> 1920 x oh)'),
    ]
    for key, off, orig_hex, state_off, kind, why in sites:
        patch_height(ctx.others[key] + off, orig_hex, state_off, kind, why)

    # Layout2 ortho projection builder: scales 16:9 canvas height (>= 1000 wide, e.g. 1920x1080 or 1280x720) by state +0x5c (k = (16/9) / aspect) so UI widgets keep 1:1 pixel aspect.
    # At +0x208 (orig ldr q6, [sp, #0xa0]), x16/x17, v0..v3 and nzcv are free; stores updated {W, H'} back to [sp, #0xa0] for both ortho blocks (x19+0xbf0 and x19+0xca0).
    ortho_fn = ctx.others['layout ortho']
    ortho_site = ortho_fn + 0x208
    fs_site(ortho_site, 'e62bc03d')   # ldr q6, [sp, #0xa0]
    start = here()
    emit(0x3dc02be6)
    emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9404a11, 0x34000291, 0x52a88f51, 0x1e270222, 0x5e0404c0, 0x1e222000, 0x540001eb,
              0x5e0c04c1, 0x1e245002, 0x1e261003, 0x1e220800, 0x1e230822, 0x1e223800, 0x1e20c000,
              0x1e221002, 0x1e222000, 0x540000aa, 0xbd405e02, 0x1e220821, 0x6e0c0426, 0x3d802be6):
        emit(w)
    emit(B(here(), ortho_site + 4))
    fs_put(ortho_site, [B(ortho_site, start)], 'Layout2 ortho canvas height scaled by (16/9) / aspect when screen aspect is active')

    # Layout view draw (main+0x773b90): builds the screen-space OrthoProjection (-W/2..W/2, -H/2..H/2) at +0x398 (orig ldp s0, s1, [x0]) into the 0x90-byte UBO copied to Layout2.
    # Scales 16:9 viewport height s1 (W >= 1000) by state +0x5c (k = (16/9) / aspect) so 2D HUD widgets render 1:1 without vertical stretch. x16/x17, s2..s4 and nzcv are free there.
    vo_fn = ctx.others['layout view ortho']
    vo_site = vo_fn + 0x398
    fs_site(vo_site, '0004402d')   # ldp s0, s1, [x0]
    start = here()
    emit(0x2d400400)
    emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9404a11, 0x34000211, 0x52a88f51, 0x1e270222, 0x1e222000, 0x5400018b,
              0x1e245002, 0x1e261003, 0x1e220804, 0x1e230822, 0x1e223884, 0x1e20c084,
              0x1e221002, 0x1e222080, 0x5400006a, 0xbd405e02, 0x1e220821):
        emit(w)
    emit(B(here(), vo_site + 4))
    fs_put(vo_site, [B(vo_site, start)], 'Layout view ortho height scaled by (16/9) / aspect when screen aspect is active')

    # The 3D projections (sead::PerspectiveProjection: fovy +0xa0, sin/cos/tan of fovy/2 at +0xa4..+0xac, aspect +0xb0).
    # On a taller screen, a ~16:9 projection (|aspect - 16/9| <= 0.01) updates the object itself before the matrix is built so
    # every reader — including camera update (+0x33ab44/+0x33ab64) and scene view culling and sun-shadow fitting (main+0x235be0,
    # tanf at main+0x237468) — sees the view that is drawn (issue #3202).
    # - Always writes aspect = r (state +0x58) into +0xb0.
    # - When Vert+ is active (state +0x60 tol > 0; DM_SCREEN_FOV=off sets -1.0 for Hor-), updates +0xac = tan * k (state +0x5c),
    #   +0xa8 = 1 / sqrt(1 + tan'^2), +0xa4 = tan' * cos', and +0xa0 = 2 * atan(tan') (Cephes single-precision range reduction +
    #   degree-7 odd minimax polynomial, < 1e-7 rad error).
    # - Guards against double-widening via a 64-entry ring of widened fovy bit patterns in the state page (+0x2fc cursor,
    #   +0x300..+0x3fc seen[64]), matching Breath of the Wild's guard, if a widened fovy is ever copied into another 16:9 projection.
    # Fast path (16:9 or non-16:9 projection, including NaN) uses only x16/x17/s0-s2/nzcv with no stack traffic; slow path saves
    # x8..x12 and x30 around the shared leaf helper (s0..s5, x16, x17, nzcv are free at all three sites).
    proj_apply = here()
    for w in (
            0xbd405a00, 0xbd00b100, 0xbd406200, 0x1e202008, 0x54000a2d, 0xbd40ad00, 0x1e202008, 0x540009cd,
            0xb940a109, 0x910c020a, 0x5280080b, 0xb840454c, 0x6b09019f, 0x54000900, 0x7100056b, 0x54ffff81,
            0xbd405e01, 0x1e210800, 0xbd00ad00, 0x1e2e1001, 0x1f000402, 0x1e21c042, 0x1e221821, 0x1e210802,
            0x2d148502, 0x52904f51, 0x72a80351, 0x1e270221, 0x1e212000, 0x5400010c, 0x528279b1, 0x72a7da91,
            0x1e270221, 0x1e212000, 0x5400012c, 0x1e2703e2, 0x1400000e, 0x5281fb71, 0x72a7f931, 0x1e270222,
            0x1e3e1001, 0x1e201820, 0x14000008, 0x5281fb71, 0x72a7e931, 0x1e270222, 0x1e2e1003, 0x1e233801,
            0x1e232803, 0x1e231820, 0x1e200801, 0x529e1a31, 0x72a7b491, 0x1e270223, 0x528370b1, 0x72b7c1d1,
            0x1e270224, 0x1f011063, 0x52924bf1, 0x72a7c991, 0x1e270224, 0x1f011063, 0x52954551, 0x72b7d551,
            0x1e270224, 0x1f011063, 0x1e210863, 0x1f000063, 0x1e232842, 0x1e222842, 0xbd00a102, 0x1e260049,
            0x910c020a, 0x5280080b, 0xb840454c, 0x6b09019f, 0x54000120, 0x7100056b, 0x54ffff81, 0xb942fe0b,
            0x1200156c, 0x1100056b, 0xb902fe0b, 0x910c020a, 0xb82c5949, 0xd65f03c0):
        emit(w)
    for key, off, orig_hex, base, what in (
            ('perspective proj', 0x000, '0084552d', 0, 'sead::PerspectiveProjection::doUpdateMatrixImpl_ (ldp s0, s1, [x0, #0xac])'),
            ('camera update',    0xf6c, '4084552d', 2, 'camera update inlined projection update (main+0x33b82c, ldp s0, s1, [x2, #0xac])'),
            ('device proj',      0x098, '008c552d', 0, 'device projection inlined update (main+0x23b9a8, ldp s0, s3, [x0, #0xac])')):
        site = ctx.others[key] + off
        fs_site(site, orig_hex)
        start = here()
        emit(ADRP(16, here(), FS_STATE_OFF))
        for w in (0xb9404a11, 0x34000271,                  # ldr w17, [x16, #0x48]; cbz w17, orig
                  0xbd40b000 | (base << 5),                # ldr s0, [P, #0xb0]
                  0x5291c731, 0x72a7fc71, 0x1e270221,      # s1 = 16/9
                  0x7ea1d401,                              # fabd s1, s0, s1
                  0x529ae151, 0x72a78471, 0x1e270222,      # s2 = 0.01
                  0x1e222020, 0x54000128,                  # fcmp s1, s2; b.hi orig (also skips NaN)
                  0xa9bd27e8, 0xa9012fea, 0xa9027bec,      # stp x8,x9,[sp,#-0x30]!; stp x10,x11,[sp,#0x10]; stp x12,x30,[sp,#0x20]
                  0xaa0003e8 | (base << 16)):              # mov x8, P
            emit(w)
        emit(BL(here(), proj_apply))
        for w in (0xa9427bec, 0xa9412fea, 0xa8c327e8):     # ldp x12,x30,[sp,#0x20]; ldp x10,x11,[sp,#0x10]; ldp x8,x9,[sp],#0x30
            emit(w)
        emit(int.from_bytes(bytes.fromhex(orig_hex), 'little'))   # orig: reload updated tan and aspect from the object
        emit(B(here(), site + 4))
        fs_put(site, [B(site, start)], what + ': a ~16:9 projection updates aspect and Vert+ fovy/sin/cos/tan in the object (Hor- if DM_SCREEN_FOV=off)')

    # The movie's draw (main+0x3b5c710, the title screen's background and the opening): its sead::Viewport at x29 - 0x28 (min x, y at +0x8, +0xc, max x, y at +0x10, +0x14) is
    # the whole frame buffer, so the 16:9 movie (1920x1080, 2560x1440 after the upscale) filled a 2560x1920 frame at 4:3, 1.33x taller (an oval moon). At +0x64, after the viewport
    # is built and before main+0x2846e0 applies it (viewport and scissor), a rect taller than 16:9 becomes 16:9 about its centre; the frame is already cleared to black above and
    # below. x16 / x17 and s0-s7 are free there (x8 / x9 are loaded by the original instruction, the next call's arguments after it)
    movie_fn = ctx.others['movie draw']
    movie_site = movie_fn + 0x64
    fs_site(movie_site, '69a241a9')   # ldp x9, x8, [x19, #0x18]
    start = here()
    emit(0xa941a269)
    emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9404a11,   # ldr w17, [x16, #0x48]   oh (0 = 16:9)
              0x34000271,   # cbz w17, out
              0xd100a3b1,   # sub x17, x29, #0x28     the viewport
              0x2d410620,   # ldp s0, s1, [x17, #0x8]
              0x2d420e22,   # ldp s2, s3, [x17, #0x10]
              0x1e203844,   # fsub s4, s2, s0         width
              0x1e213865,   # fsub s5, s3, s1         height
              0x52a7e210,   # mov w16, #0x3f100000    9 / 16
              0x1e270206,   # fmov s6, w16
              0x1e260886,   # fmul s6, s4, s6         the 16:9 height
              0x1e2620a0,   # fcmp s5, s6
              0x5400012d,   # b.le out                not taller
              0x1e2638a7,   # fsub s7, s5, s6
              0x52a7e010,   # mov w16, #0x3f000000    0.5
              0x1e270204,   # fmov s4, w16
              0x1e2408e7,   # fmul s7, s7, s4         half the extra height
              0x1e272821,   # fadd s1, s1, s7
              0x1e273863,   # fsub s3, s3, s7
              0xbd000e21,   # str s1, [x17, #0xc]
              0xbd001623):  # str s3, [x17, #0x14]
        emit(w)
    emit(B(here(), movie_site + 4))   # out
    fs_put(movie_site, [B(movie_site, start)], 'Movie draw viewport kept 16:9 about its centre on a taller screen when screen aspect is active')


    # Layout2 pane matrix (main+0x14e9980): for fullscreen PicturePanes (width >= 1000, height >= 700, ~16:9), scales Y matrix row by state +0x5c (k = (16/9) / aspect) so full-screen faders cover the expanded canvas without letterboxing.
    calc_fn = ctx.others['layout calc matrix']
    calc_site = calc_fn + 0x19c
    fs_site(calc_site, '6042883c')   # stur q0, [x19, #0x84]
    start = here()
    emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9404a11, 0x34000351, 0xb940aab1, 0x53166631, 0x7100063f, 0x540002c1, 0x2d4b0ea2,
              0x52a88f51, 0x1e270224, 0x1e242040, 0x54000224, 0x52a885f1, 0x1e270224, 0x1e242060,
              0x540001a4, 0x1e245004, 0x1e261005, 0x1e240844, 0x1e250865, 0x1e253884, 0x1e20c084,
              0x52a86911, 0x1e270225, 0x1e252080, 0x5400006a, 0xbd405e02, 0x4f829000, 0x3c884260):
        emit(w)
    emit(B(here(), calc_site + 4))
    fs_put(calc_site, [B(calc_site, start)], 'Layout2 fullscreen picture panes scaled by (16/9) / aspect when screen aspect is active')

    # The HUD and the menu bars at the taller screen's edges (shape_anchor.c, which says what moves). The pane global matrix function (main+0x152caa0) has the new
    # matrix in v0-v3 at +0x3b0 on both paths. At 16:9 skip the stack and call. Otherwise save v0-v3, call shape_calc, reload v0-v3 and run the original instruction.
    # x0-x17 are written before they are read after this point, and x30 is reloaded by the epilogue. The helper leaves x18 alone.
    pane_fn = ctx.others['pane global matrix']
    site = pane_fn + 0x3b0
    fs_site(site, '68c20091')   # add x8,x19,#0x30
    start = here()
    original = start + 13 * 4
    emit(ADRP(16, here(), FS_STATE_OFF))
    emit(0xb9404a11)   # ldr w17, [x16, #0x48]
    emit(0x34000011 | ((original - here()) // 4 << 5))   # cbz w17, original
    for w in (0xd10103ff,    # sub sp,sp,#0x40
              0xad0007e0,    # stp q0,q1,[sp]
              0xad010fe2,    # stp q2,q3,[sp,#0x20]
              0xaa1303e0,    # mov x0,x19            the pane
              0x910003e1,    # mov x1,sp             its matrix (q0-q3)
              0xaa1003e2):  # mov x2,x16            the state page
        emit(w)
    emit(BL(here(), original + 8))   # shape_calc follows the original instruction and its return branch
    for w in (0xad4007e0,    # ldp q0,q1,[sp]
              0xad410fe2,    # ldp q2,q3,[sp,#0x20]
              0x910103ff):  # add sp,sp,#0x40
        emit(w)
    assert here() == original
    emit(0x9100c268)                     # add x8,x19,#0x30      (the original)
    emit(B(here(), site + 4))
    for k in range(0, len(_shape_anchor), 8): emit(int.from_bytes(bytes.fromhex(_shape_anchor[k:k + 8]), 'little'))
    fs_put(site, [B(site, start)], 'Layout2 pane global matrix: HUD panes past the 16:9 edge region and the menu bars moved to the taller screen\'s edges with their subtrees (shape_anchor.c)')
    ctx.capabilities.add('SCREEN_ASPECT')


# Under a host render scale below 1.0 (state +0x10, float bits in 1..0x3f7fffff), return level 0 (the game's full size, which render_scale then multiplies) so
# DM_DYNAMIC_RESOLUTION=1 does not fight DM_RENDER_SCALE. Self-contained (no jump into drs_controller), emitted only when render_scale also matches
drs_render_scale_guard = (   # x16 the state page; w17, w2 free (same leaf contract as drs_controller)
    0xb9401211,   # ldr w17, [x16, #0x10]   the host render scale's bits (1.0 = the game's own size)
    0x340000d1,   # cbz w17, +6             unset (0): fall through to drs_controller
    0x52a7f002,   # mov w2, #0x3f800000     1.0
    0x6b02023f,   # cmp w17, w2
    0x54000062,   # b.hs +3                 1.0 or more, negative, or NaN: fall through to drs_controller
    0x2a1f03e0,   # mov w0, wzr             level 0
    0xd65f03c0)   # ret


def dynamic_resolution(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    site = ctx.others.get('dynamic resolution')
    if site is None or ctx.others.get('dynamic resolution update') is None: return   # not both found in this build (a red line from the locator): the game's own dynamic resolution
    fs_site(site, '017c40bd')   # ldr s1,[x0,#0x7c]
    has_render_scale = ctx.others.get('render scale') is not None
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    if has_render_scale:
        for w in drs_render_scale_guard: emit(w)
    for w in drs_controller: emit(w)
    why = ('dynamic resolution level: 0 (full resolution) also under a render scale below 1.0, or with state +0x40 (DM_DYNAMIC_RESOLUTION) a climb on the load measured at each level'
           if has_render_scale else
           'dynamic resolution level: 0 (full resolution), or with state +0x40 (DM_DYNAMIC_RESOLUTION) a climb on the load measured at each level')
    fs_put(site, [B(site, start)], why)
    ctx.capabilities.add('DYNAMIC_RESOLUTION')


# Render scale (DM_RENDER_SCALE, state +0x10, dm/host/loader.c): the 3D views at that fraction of the game's own size, the dynamic resolution held at its top level (as Tears of the
# Kingdom's and Breath of the Wild's). The views' sizes come from the scale the engine stores at +0xd48 of its view-size update (str s0,[x20,#0xe5c]; v131072 main+0x31d3200, found by
# its fingerprint; the only float store to #0xe5c in main): the level's ResolutionReduceScale (0.75 at the top level) normalized by [x20+0xe58]/[x20+0xe50]. That store and the size
# loop after it (+0xdd8, which clears the "scale changed" bit 3 of [x20+0xe2c]) run only in a frame where the dynamic-resolution update (main+0x805aa0, which always returns the
# level's float scale in w0) answers status 0, a new scale: at a steady frame rate it answers status 2, so a stub at the store alone changes nothing until the game comes under load.
# Two stubs: at the update's answer (+0xce4, ubfx of its status byte) a host scale in (0.0, 1.0) whose last stored value (state +0x290, private area after drs_controller) does not
# match [x20+0xe5c] makes any non-1 status 0 so that frame recomputes the sizes (and re-applies if [x20+0xe5c] is ever reset); at the store (+0xd48) the freshly computed s0 is
# multiplied by the host's scale and recorded at state +0x290. The level stub above returns level 0 under such a scale, so the size is fixed; 1.0 (the loader's default) runs the
# game's own code. Registers and flags are preserved except x8 (the status read next) and s0 (stored, dead after +0xd48: reloaded at +0xd84)
def _bcond(frm, to, cond): return 0x54000000 | (((to - frm) >> 2) & 0x7ffff) << 5 | cond
def _cbz_w(frm, to, rt): return 0x34000000 | (((to - frm) >> 2) & 0x7ffff) << 5 | rt


def render_scale_status_body(state_adrp):   # 19 words; state_adrp is word 4 (ADRP x16, state) or MOV x16, x1 in unit tests
    at = lambda k: 4 * k
    return (
        0xd3609c08,                       # ubfx x8,x0,#32,#8      the original
        0xa9be47f0,                       # stp x16,x17,[sp,#-32]!
        0xd53b4210,                       # mrs x16,nzcv
        0xf9000bf0,                       # str x16,[sp,#16]
        state_adrp,                       # x16 = state page
        0xb9401211,                       # ldr w17,[x16,#0x10]    the host scale's bits
        _cbz_w(at(6), at(16), 17),        # cbz w17,done           unset
        0x53177e31,                       # lsr w17,w17,#23        sign + exponent (< 0x7f iff positive finite < 1.0)
        0x7101fe3f,                       # cmp w17,#0x7f
        _bcond(at(9), at(16), 0x2),       # b.hs done              1.0 or more, negative or NaN: the game's own
        0xb9429210,                       # ldr w16,[x16,#0x290]   last scaled value stored to [x20,#0xe5c]
        0xb94e5e91,                       # ldr w17,[x20,#0xe5c]   current view scale
        0x6b11021f,                       # cmp w16,w17
        _bcond(at(13), at(16), 0x0),      # b.eq done              already applied and intact
        0x7100051f,                       # cmp w8,#1
        0x1a9f0108,                       # csel w8,w8,wzr,eq      keep status 1; any other status -> 0 (recompute sizes)
        0xf9400bf0,                       # done: ldr x16,[sp,#16]
        0xd51b4210,                       # msr nzcv,x16
        0xa8c247f0)                       # ldp x16,x17,[sp],#32


def render_scale_store_body(state_adrp):   # 16 words; state_adrp is word 2 (ADRP x16, state) or MOV x16, x1 in unit tests
    at = lambda k: 4 * k
    return (
        0xadbf7ffe,                       # stp q30,q31,[sp,#-32]!
        0xa9bf47f0,                       # stp x16,x17,[sp,#-16]!
        state_adrp,                       # x16 = state page
        0xd53b4211,                       # mrs x17,nzcv
        0xbd40121e,                       # ldr s30,[x16,#0x10]    the host scale
        0x1e2023c8,                       # fcmp s30,#0.0
        _bcond(at(6), at(12), 0xd),       # b.le original          unset, negative or NaN
        0x1e2e101f,                       # fmov s31,#1.0
        0x1e3f23c0,                       # fcmp s30,s31
        _bcond(at(9), at(12), 0xa),       # b.ge original          1.0 or more
        0x1e3e0800,                       # fmul s0,s0,s30         the game's scale x the host's
        0xbd029200,                       # str s0,[x16,#0x290]    applied value (checked by the status stub)
        0xbd0e5e80,                       # original: str s0,[x20,#0xe5c]
        0xd51b4211,                       # msr nzcv,x17
        0xa8c147f0,                       # ldp x16,x17,[sp],#16
        0xacc17ffe)                       # ldp q30,q31,[sp],#32


def render_scale(ctx):
    here, emit = ctx.here, ctx.emit
    fn = ctx.others.get('render scale')
    if fn is None: return   # not found in this build (a red line from the locator): the game's own sizes
    status, store = fn + 0xce4, fn + 0xd48
    ctx.site(status, '089c60d3')   # ubfx x8,x0,#32,#8: the update's status byte
    s = here()
    for w in render_scale_status_body(ADRP(16, s + 4 * 4, ctx.state)): emit(w)
    emit(B(here(), status + 4))
    ctx.put(status, [B(status, s)], 'dynamic-resolution status: a host render scale not yet stored asks for new 3D sizes (state +0x10)')
    ctx.site(store, '805e0ebd')    # str s0,[x20,#0xe5c]
    s = here()
    for w in render_scale_store_body(ADRP(16, s + 4 * 2, ctx.state)): emit(w)
    emit(B(here(), store + 4))
    ctx.put(store, [B(store, s)], 'final 3D view scale x the host render scale below 1.0 (1.0 = the game\'s own)')
    ctx.render_scale = 1

MATCH_FRAME_STEP = {'main': [
    {'name': 'frame ratio from the host\'s step', 'size': 0x9b4, 'fingerprints': ['6ae2aff5344d14568e4dbaf93455e2d8278384bf892ed6c6eca87fef98fbcd1d'], 'stub': frame_step,   # v131072 main+0x6e6be0
     'others': {'dynamic resolution': {'size': 0x294, 'fingerprints': ['7af6ceb0aedf8e0c17dc08ce17fcd6a7cba6d394a0d09e3b504c6d668dc418e7']},   # v131072 main+0x8065e0
                'dynamic resolution update': {'size': 0xb24, 'fingerprints': ['5a423697838976baf286cffa5e2ceef0d4e037abbcd9d371e90e9e702d61b3d4']},   # main+0x805aa0
                'render scale': {'size': 0xfc8, 'fingerprints': ['2c1804f08798ceee012ae30a1ba6b177a6b6a61ce732185bad28e9989d05e165']},   # main+0x31d3200, the 3D view size update
                **screen_shape_functions}},
]}

# The movie player's pictures per frame. Once a frame the player's update (main+0x3b5ca60 on v131072) adds the frame's time (the time object's +0x18, the frame ratio / 60 s, times
# 60) to a count at +0x8, takes its whole pictures (the movie's rate at the player's +0x324; the title's and the opening's are 60 fps) and advances that many (main+0x3b61ea0 pops them from its
# queue and shows the last), but never more than the player's +0x20 (csel at main+0x3b5cb2c), and the count keeps what was left. The constructor (main+0x3b5cec0) sets +0x20 to
# 1, or to 2 when its last argument is 2 or more (cmp w27, #2; cinc w8, w28, ge at +0x1c4, w28 = 1), and the queue's size at +0x50 to twice that; the game's one caller passes 1
# (main+0x3b5c55c). So a frame of two vsyncs shows one picture where the movie had two, the picture falls behind for good (the count only grows), and the movie's sound, a stream of
# the sound engine on real time, runs ahead and ends before the picture: at a 30 fps frame step a movie played at exactly 0.500 of its speed (58.00 s of movie in 115.95 s) with
# the count at 1,350 pictures after 45 s. The console holds one vsync a frame in its movies, so the limit never shows there; here the frame step hands the game steps of 2 and 3.
# The cut gives the constructor's own value for 2 (+0x20 = 2, +0x50 = 4), so a frame of two vsyncs shows every second picture and a movie that fell behind catches up at two
# pictures a frame: at the 30 fps frame step 1.000 (95.00 s in 94.97 s).
movie_pictures = 'movie pictures a frame: up to 2, the constructor\'s value for an argument of 2 or more, whatever its caller passes (1)'
# The update's cap, 3. The constructor's limit also sizes the decoder's pipeline: the picture pool is twice it (video-frame-count, registered from a four-slot array at the player's
# +0x168, which the teardown frees slot by slot) and the inputs in flight are as many as it (their buffers two slots at +0x188 and +0x190: main+0x3b5ff30 takes slot 0 for an id of
# 2 or more). So 3 cannot be the limit: with the game's pool of 6 the registration wrote over the two input buffers (the game stopped at the movie's start with a copy to a null
# pointer in sdk), and with the pool kept at 4 the third input reused slot 0 while in flight (a VideoToolbox decode error after 150 pictures of the title movie). The frame step's
# largest hold is 3 vsyncs, and with a cap of 2 each such frame left the picture one behind for as long as the step stayed at 2. The update reads the limit only for its cap
# (ldr w9, [x0, #0x20] at +0xc0, before the csel); the cut puts 3 there and leaves the limit, the pool and the inputs at the constructor's 2. The advance pops up to that many from
# its own queue and stops when the queue is empty, so a frame takes a third picture only when the decoder has one.
movie_cap = 'movie update cap: 3 pictures a frame, the frame step\'s largest hold, in place of the limit (2), which also sizes the decoder pipeline'
# The movie's frame, which its event follows. The event that plays a movie (its update main+0x3b3e9a0 on v131072) reads the movie's frame (its vtable's +0x78, main+0x3b3ec10: the
# movie object's int at +0x2f8) and moves its timeline up to it when the timeline is behind (main+0x122d520), then measures the movie's sound stream against it (main+0x3b3ea50).
# The opening's timeline carries its subtitles, the narrator's voice lines (EventFlowMsg/Dm_Kokiri_0000, played with them), its rumble and the title logo
# (EventOpenOpeningTitle). The movie's draw (main+0x3b5c710, and main+0x3b5c870 for its first draw) adds 1 to that frame each frame a picture is on screen (ldr, add #1,
# str at +0xb4 and +0x98), so the frame counts frames drawn, not pictures shown: the same while the player takes one picture a frame, half of them when it takes two. With the
# cut above, a frame of two vsyncs showed the picture in real time but the subtitles and voice at half speed, and the logo, due after the movie's end, never showed. The cut
# sets the frame to the pictures the player has taken (the player's +0xc: its update adds what it advances each frame; reset when a movie starts, as +0x2f8 is), so the
# timeline follows the picture whatever the frame takes. The player is the object at +0x10, already read by both draws (main+0x3b5ce80 on it) just before.
movie_frame = 'movie frame: the pictures the player has taken (its +0xc), not 1 more for each frame drawn'
MATCH_CUTS = {'main': [
    {'name': movie_pictures, 'size': 828, 'fingerprints': ['f61ad468b6b4e1d574fe8678f5a12dfc60e1aab71be7375142a4b48b0d73ce19'],   # v131072 main+0x3b5cec0
     'cuts': [(452, '88b79c1a', '48008052', movie_pictures + ' (cinc w8, w28, ge → mov w8, #2; as v131072 main+0x3b5d084)')]},
    {'name': movie_cap, 'size': 368, 'fingerprints': ['6a29b27559214b3b89cc31a5b85f14bda4a55685f1ae4fe486d53eda66855867'],   # v131072 main+0x3b5ca60, the update
     'cuts': [(192, '092040b9', '69008052', movie_cap + ' (ldr w9, [x0, #0x20] → mov w9, #3; as v131072 main+0x3b5cb20)')]},
    {'name': movie_frame, 'functions': [
        {'size': 344, 'fingerprints': ['cc1ca04feb1ca4e553a6c7a4f36f99b13f56977185eea8ef5cd8dd3dbbdcdbcd'],   # v131072 main+0x3b5c710, the draw
         'cuts': [(180, '88fa42b908050011', '880a40f9080d40b9', movie_frame + ' (ldr w8, [x20, #0x2f8]; add w8, w8, #1 → ldr x8, [x20, #0x10]; ldr w8, [x8, #0xc]; as v131072 main+0x3b5c7c4)')]},
        {'size': 196, 'fingerprints': ['8fe123ca6fcd016cd9c718e5e66035c807b27f63408861235765e47b4ec34da3'],   # v131072 main+0x3b5c870, the first draw
         'cuts': [(152, '88fa42b908050011', '880a40f9080d40b9', movie_frame + ' (the same; as v131072 main+0x3b5c908)')]},
    ]},
]}

BINDING = [
    {'what': 'movie pictures a frame: up to 2', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own movie player constructor, found by its function's fingerprint on any build that keeps the function; no other game has it: the fingerprint matches no function in the mains of Donkey Kong Bananza, Splatoon Raiders, Pokopia, Mario Tennis Fever, Switch Sports, Yoshi and the Mysterious Book, Mario Golf, Xenoblade Chronicles 2 or Super Mario 3D All-Stars, and Breath of the Wild's and Tears of the Kingdom's movies go through subsdk0's movie::Decoder, not a player in main",
     'checked': 'v131072 main: one function with this fingerprint, the constructor, its one caller, the update and the advance read by disassembly; the limit and the count read in play with DM_PROBE, the opening movie timed at a 30 fps frame step without and with the cut (2026-11-12)'},
    {'what': 'movie update cap: 3 pictures a frame', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own movie player update, found by its function's fingerprint on any build that keeps the function; it goes with the player's two-picture cut and has no wider binding",
     'checked': 'v131072 main: one function with this fingerprint, the cap\'s only read of the limit, the pool and input slots the limit sizes and the advance read by disassembly; the opening with frames forced to three vsyncs at cap 2, limit 3 with pool 4 (decode error) and cap 3 (2026-11-16)'},
    {'what': 'movie frame: the pictures the player has taken', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own movie draw functions, found by their functions' fingerprints on any build that keeps them; it is the counter the same game's player keeps, so it goes with the player's two-picture cut and has no wider binding",
     'checked': 'v131072 main: one function with each fingerprint, each with one caller, the event update that reads the frame (vtable +0x78), the timeline and sound syncs and the player\'s +0xc read by disassembly; the opening timed with frames forced to two vsyncs (2026-11-16)'},
    {'what': 'frame ratio from the host\'s step', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own frame-ratio division, found by its function's fingerprint on any build that keeps the function",
     'checked': 'v131072 main: one function with this fingerprint, the site and its free registers read by disassembly (2026-11-08); the stub in ticks of 1/120 s (state +0x38) run natively at 2 / 3 / 4 ticks and without a unit (test_frame_ratio_stub.py) and disassembled in the rewritten main (2026-11-14)'},
    {'what': 'dynamic resolution: full size, or a climb on the load measured at each level', 'now': 'game', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's own dynamic-resolution level function and update, found by their functions' fingerprints on any build that keeps both; switchable (DM_DYNAMIC_RESOLUTION)",
     'checked': 'v131072 main: one function with each fingerprint (the level function a leaf, x2..x11 / x16 / x17 / s0..s2 free), its callers, the update and the controller fields read by disassembly, the fields read in play (2026-11-08); the climb run natively against a model of the update and in play to full size (2026-11-09); 30 fps threshold scaling checked in test_ocarina_drs.py and in play at 30 fps on an M2 Air (PR #1954, 2026-11-16)'},
    {'what': 'render scale: the 3D views at a fraction of the game\'s own size (DM_RENDER_SCALE)', 'now': 'game', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's own view-size update (its test of the dynamic-resolution status and its scale store), found by its function's fingerprint on any build that keeps it; the level function's stub above holds level 0 under a scale",
     'checked': 'v131072 main: one function with this fingerprint (main+0x31d3200, 0xfc8 bytes, one caller main+0x31c00cc); the status ubfx at +0xce4, the store at +0xd48 (the only float store to #0xe5c in main; s0 computed from the update\'s w0 at +0xd34, never reread from +0xe5c) and the size loop clearing bit 3 of +0xe2c read by disassembly (2026-11-09, 2026-11-23); guard, status and store stubs executed natively on ARM64 in test_ocarina_drs.py'},
    {'what': 'screen shape: taller render targets, cameras, Layout2 panes and a 16:9 movie viewport (DM_SCREEN_ASPECT)', 'now': 'game', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's own display setup, camera, pane and movie code; installed together after every function matches. Indexed pane edits verify the authored name, class and dimensions they use",
     'checked': 'v131072 main: one function per fingerprint, free registers read at each site; indexed pane names, classes and dimensions checked against the UIL records. In-place Vert+ projection-object updates (and DM_SCREEN_FOV=off Hor-) executed on ARM64 in test_ocarina_screen_shape.py and verified in Kakariko Village on Mac at 16:9, 16:10 and 4:3 (2026-11-23); 0/3 projection fingerprint matches in TOTK, BOTW and Mario Kart World; physical iPad/iPhone retest pending'},
]
