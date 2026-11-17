# The Legend of Zelda: Tears of the Kingdom (0100F2C0115B6000): keyboard and mouse controls on the Mac (DM_KBM=1), the rewriter's half. patches.py loads this file and runs
# mouse_camera on 1.4.3 main beside its frame step; it is kept apart from patches.py so the feature stays in files of its own. The front end's half is dm/host/front_kbm_mac.m.
# The stubs keep their state in the frame step state page at MOUSE_STATE (x9 below points there; every "state +0x.." in this file is relative to it), clear of the
# frame step's own fields at the start of the page, which other games' stubs keep adding to
import struct
from nso_asm import ADDI, ADRP, B, SUBI

MOUSE_STATE = 0x800   # the mouse block's offset in the state page (front_kbm_mac.m MOUSE_STATE): 0x88 bytes

# Encoders the stubs use beyond nso_asm's
def BC(cond, frm, to):   # b.cond (EQ, NE, ... below)
    off = to - frm; assert -(1 << 20) <= off < (1 << 20) and off % 4 == 0
    return 0x54000000 | (((off >> 2) & 0x7FFFF) << 5) | cond
def CBZW(t, frm, to):   # cbz wt
    off = to - frm; assert -(1 << 20) <= off < (1 << 20) and off % 4 == 0
    return 0x34000000 | (((off >> 2) & 0x7FFFF) << 5) | t
EQ, NE, LO, HI, GE, LT = 0, 1, 3, 8, 10, 11   # condition codes for BC
def LDRS(t, n, off):   # ldr st,[xn,#off], or ldur for a negative off
    assert (0 <= off < 0x4000 and off % 4 == 0) or -256 <= off < 0
    return (0xBD400000 | ((off // 4) << 10) if off >= 0 else 0xBC400000 | ((off & 0x1FF) << 12)) | (n << 5) | t
def STRS(t, n, off):   # str st,[xn,#off], or stur for a negative off
    assert (0 <= off < 0x4000 and off % 4 == 0) or -256 <= off < 0
    return (0xBD000000 | ((off // 4) << 10) if off >= 0 else 0xBC000000 | ((off & 0x1FF) << 12)) | (n << 5) | t
def STRW(t, n, off):   # str wt,[xn,#off]
    assert 0 <= off < 0x4000 and off % 4 == 0
    return 0xB9000000 | ((off // 4) << 10) | (n << 5) | t

# Mouse camera: the mouse turns the player camera by angle, not through the stick. The right stick reaches the camera through QueryPlayerCameraControllerInput (main+0x100d030)
# and ExecutePlayerCameraPreUpdate (main+0x8749b0), which writes the processed stick (dead zone, curve, invert options) to the camera state at +0x8b0. The camera nodes
# (ExecutePlayerCamera*; the calc is slot 19 of each node's vtable) read it through their camera state + 0x490 (stick at +0x420 / +0x424). Each keeps its angles in degrees:
# - Chase (main+0x18e414, x20 = node, x26 = camera state + 0x490): yaw at camera +0x414. The pitch at +0x410 is rebuilt every frame as node +0x108 + (1 - node +0x88) *
#   node +0x10c within +-89.9, and in the player-controlled state (4) only the stick moves node +0x108, the player's pitch, between node +0xf8 and +0xfc (the bounds
#   main+0x191d78 clamps the follow pitch to); writing +0x410 did nothing (tested in game).
# - Aiming (bow, throw; main+0x23be978, x19 = node, x22 = camera): yaw at camera +0x414; the stick moves the target pitch node +0x154 from the pitch node +0x15c, clamped
#   to node +0x1d4 / +0x1d8, and +0x15c follows it (at once with the stick, halfway each frame with gyro aiming).
# - LockOn (main+0x23d7140, x19 = node, x24 = camera), which Target and UltraHand (a thunk at main+0x23d8e74) share: the stick moves the yaw offset node +0xbc from the
#   base yaw node +0xb8 and the pitch offset node +0xa4, which the calc clamps itself after the stick. The stub sits at the stick's first read (main+0x23d756c), after the
#   calc may have rebuilt +0xbc from the last yaw.
# - SpecialPower (Fuse, Ascend, Recall, Autobuild aiming; main+0x23d2d40, x19 = node, x23 = camera): the yaw is the camera's current direction (main+0x2e2468 into
#   [x29-0x4c], yaw at [x29-0x44]) plus the stick, so the stub adds the mouse to that slot at its copy (main+0x23d2dd0); the pitch node +0x50 follows the target node
#   +0x160, which the stick moves between node +0x1ac and +0x1b0 and which the calc first sets from +0x50. With bit 8 of camera +0x15c set (Recall) the calc ignores
#   that direction: the yaw is the base yaw node +0x54, which the node's enter (slot 18, main+0x23d25fc, through main+0x23d2bc8) sets once, minus a look offset node
#   +0x1d8 that the stick moves within a small bound. There the stub turns node +0x54 instead.
# The stub adds the mouse counts the host summed since they were last taken (state +0x40 / +0x44, taken totals at +0x48 / +0x4c): yaw -= dx * (state +0x50), wrapped to
# [-180, 180); pitch += dy * (state +0x54), within the node's bounds. Counts that no camera took for 1/8 s (another camera, a menu; the time of the last take at state
# +0x60, in system counter ticks) are dropped instead of turning the camera all at once afterwards. With the stick at rest it holds a 0.001 stick (both axes: with y at 0
# Chase eases the pitch back into its comfort band, and Aiming and SpecialPower take gyro input instead) for 10 frames after the mouse moved (counter at state +0x58), so
# the camera stays player-controlled instead of following Link. State +0x50 = 0 (the host's default without a mouse camera) runs the original instruction only.
# The stubs also write their camera's number to state +0x68 (1 Chase, 2 Aiming, 3 UltraHand, 4 SpecialPower; LockOn and Target leave it), which the front end reads
# with the time of the last take to map the mouse buttons.
# Ultrahand holding an object: the right stick moves the object, and the camera follows it. The move (main+0x1d58e80, x19 = its state) reads the stick at +0x11c / +0x120
# (shaped by UltraHandMoveStickInputMulX / Y), turns the object's offset from Link (+0x144 / +0x14c) around him by dt * UltraHandMoveSpeedHorizontal * x (cosf / sinf of
# half that angle, in s11 at main+0x1d595d0) and raises its height (+0x148) by dt * UltraHandMoveSpeedVertical * y, clamped to the bounds it loads at main+0x1d59670
# (the parameters: the getters main+0x1d3a9a0 to +0x1d3ab98, registered at main+0xbb5898). The turn stub takes the mouse counts there: half angle -= dx * (state +0x50)
# * pi / 360, and a height change of the same angle at the object's horizontal distance, -dy * (state +0x54) * pi / 180 * distance, which the height stub adds before
# the clamp (state +0x78). It stamps state +0x70 (8 bytes), and the LockOn stub leaves the mouse alone for 1/8 s after it, so the object moves and the camera follows
def mouse_stub(ctx, site, orig, kind, cam, yaw, pitch, bounds=None, yield_held=False, fixed=None):
    """At site (holding the instruction orig, run last): yaw = (register, offset), pitch = (register, [offsets]), bounds = (low, high) offsets in the pitch's register;
    yield_held: nothing while Ultrahand moves an object (state +0x70 stamped within 1/8 s); fixed = (bit, yaw): while that bit of camera +0x15c is set, the yaw turned is
    that (register, offset) instead."""
    here, emit = ctx.here, ctx.emit
    ctx.site(site, struct.pack('<I', orig).hex())
    start = here(); emit(ADRP(9, here(), ctx.state)); emit(ADDI(9, 9, MOUSE_STATE))
    flag = [0xb9400000 | 0x15c >> 2 << 10 | cam << 5 | 15,   # ldr w15,[cam,#0x15c]
            0x36000040 | fixed[0] << 19 | 15] if fixed else []   # tbz w15,#bit,+8: the next instruction only with the bit set
    code = []   # words, or (kind, label) for branches resolved below
    if kind: code += [0x5280000e | kind << 5, STRW(14, 9, 0x68)]   # mov w14,#kind; str w14,[x9,#0x68]  the camera in play
    code += [0xbd405130,   # ldr s16,[x9,#0x50]       yaw degrees per count
             0x1e202208,   # fcmp s16,#0.0
             ('eq', 'orig')]                              # mouse camera off
    if yield_held: code += [0xd53be04f,   # mrs x15,cntvct_el0
                            0xf940392e,   # ldr x14,[x9,#0x70]       Ultrahand's last move
                            0xcb0e01ee,   # sub x14,x15,x14
                            0xd53be00a,   # mrs x10,cntfrq_el0
                            0xeb4a0ddf,   # cmp x14,x10,lsr #3
                            ('lo', 'orig')]                       # an object held: the mouse moves it
    code += [0x29482d2a,   # ldp w10,w11,[x9,#0x40]   host totals
             0x2949352c,   # ldp w12,w13,[x9,#0x48]   taken totals
             0x29092d2a,   # stp w10,w11,[x9,#0x48]
             0x4b0c014c,   # sub w12,w10,w12          dx
             0x4b0d016d,   # sub w13,w11,w13          dy (up)
             0xd53be04f,   # mrs x15,cntvct_el0
             0xf940312e,   # ldr x14,[x9,#0x60]       last take
             0xf900312f,   # str x15,[x9,#0x60]
             0xcb0e01ee,   # sub x14,x15,x14
             0xd53be00a,   # mrs x10,cntfrq_el0
             0xeb4a0ddf,   # cmp x14,x10,lsr #3
             ('hi', 'hold'),                              # not taken for 1/8 s: dropped
             0x2a0d018e,   # orr w14,w12,w13
             ('cbz', 'hold'),
             0x1e220191,   # scvtf s17,w12
             0x1e2201b2,   # scvtf s18,w13
             0xbd405533]   # ldr s19,[x9,#0x54]       pitch degrees per count
    if fixed: yaw_at, yaw = yaw, (11, 0); code += [(SUBI if yaw_at[1] < 0 else ADDI)(11, yaw_at[0], abs(yaw_at[1])), *flag, ADDI(11, *fixed[1])]   # x11: the yaw's address
    code += [LDRS(20, *yaw),
             0x1f10d234,   # fmsub s20,s17,s16,s20    yaw -= dx * sens
             0x52a8668f,   # mov w15,#0x43340000      180.0
             0x1e2701f5,   # fmov s21,w15
             0x1e352ab6,   # fadd s22,s21,s21         360.0
             0x1e352280,   # fcmp s20,s21
             ('lt', 'w1'),
             0x1e363a94,   # fsub s20,s20,s22
             'w1', 0x1e2142b7,   # fneg s23,s21
             0x1e372280,   # fcmp s20,s23
             ('ge', 'w2'),
             0x1e362a94,   # fadd s20,s20,s22
             'w2', STRS(20, *yaw),
             LDRS(20, pitch[0], pitch[1][0]),
             0x1f135254]   # fmadd s20,s18,s19,s20    pitch += dy * sens
    if bounds: code += [LDRS(21, pitch[0], bounds[0]), LDRS(23, pitch[0], bounds[1]),
                        0x1e375a94,   # fmin s20,s20,s23
                        0x1e354a94]   # fmax s20,s20,s21
    code += [STRS(20, pitch[0], o) for o in pitch[1]]
    code += [0x5280014e,   # mov w14,#10
             0xb900592e,   # str w14,[x9,#0x58]       frames in the player-controlled state
             'hold', 0xb940592e,   # ldr w14,[x9,#0x58]
             ('cbz', 'orig'),
             0x510005ce,   # sub w14,w14,#1
             0xb900592e,   # str w14,[x9,#0x58]
             LDRS(17, cam, 0x420),   # processed stick x
             LDRS(18, cam, 0x424),   # processed stick y
             0x1e20c231,   # fabs s17,s17
             0x1e20c252,   # fabs s18,s18
             0x1e322a31,   # fadd s17,s17,s18
             0x1e202228,   # fcmp s17,#0.0
             ('ne', 'orig'),                              # the stick is in use
             0x52a7506f,   # mov w15,#0x3a830000      0.001
             STRW(15, cam, 0x420),
             STRW(15, cam, 0x424),
             'orig', orig]
    at, pc = {}, here()
    for c in code:
        if isinstance(c, str): at[c] = pc
        else: pc += 4
    for c in code:
        if isinstance(c, str): continue
        if isinstance(c, tuple):
            op, to = c
            c = CBZW(14, here(), at[to]) if op == 'cbz' else BC({'eq': EQ, 'ne': NE, 'ge': GE, 'lt': LT, 'hi': HI, 'lo': LO}[op], here(), at[to])
        emit(c)
    emit(B(here(), site + 4))
    ctx.put(site, [B(site, start)], 'mouse camera: host mouse counts added to the camera yaw / pitch')
def mouse_camera(ctx):
    mouse_stub(ctx, 0x18e468, ADDI(8, 26, 0x40c), 1, cam=26, yaw=(26, 0x414), pitch=(20, [0x108]), bounds=(0xf8, 0xfc))             # Chase
    mouse_stub(ctx, 0x23be9b4, ADDI(8, 22, 0x40c), 2, cam=22, yaw=(22, 0x414), pitch=(19, [0x15c, 0x154]), bounds=(0x1d4, 0x1d8))   # Aiming
    mouse_stub(ctx, 0x23d756c, LDRS(0, 24, 0x420), 0, cam=24, yaw=(19, 0xbc), pitch=(19, [0xa4]), yield_held=True)                 # LockOn, Target, UltraHand
    mouse_stub(ctx, 0x23d2dd0, 0xb85bc3a9, 4, cam=23, yaw=(29, -0x44), pitch=(19, [0x50, 0x160]), bounds=(0x1ac, 0x1b0),           # SpecialPower (ldur w9,[x29,#-0x44])
               fixed=(8, (19, 0x54)))                                                                                       # Recall: base yaw node +0x54
    ctx.site(0x23d8e74, struct.pack('<I', B(0x23d8e74, 0x23d7140)).hex())   # UltraHand's calc: b to LockOn's
    start = ctx.here(); ctx.emit(ADRP(9, ctx.here(), ctx.state)); ctx.emit(ADDI(9, 9, MOUSE_STATE))
    for w in (0x5280006e,   # mov w14,#3
              0xb900692e):  # str w14,[x9,#0x68]      UltraHand in play
        ctx.emit(w)
    ctx.emit(B(ctx.here(), 0x23d7140))
    ctx.put(0x23d8e74, [B(0x23d8e74, start)], 'mouse camera: UltraHand marks itself before the LockOn calc')
    ctx.site(0x1d595d0, '6041201e')   # fmov s0,s11
    start = ctx.here(); ctx.emit(ADRP(9, ctx.here(), ctx.state)); ctx.emit(ADDI(9, 9, MOUSE_STATE))
    for w in (0xbd405130,   # ldr s16,[x9,#0x50]       degrees per count
              0x1e202208,   # fcmp s16,#0.0
              0x54000440,   # b.eq orig (+34)           mouse camera off
              0xd53be04f,   # mrs x15,cntvct_el0
              0xf900392f,   # str x15,[x9,#0x70]       an object held
              0x29482d2a,   # ldp w10,w11,[x9,#0x40]   host totals
              0x2949352c,   # ldp w12,w13,[x9,#0x48]   taken totals
              0x29092d2a,   # stp w10,w11,[x9,#0x48]
              0x4b0c014c,   # sub w12,w10,w12          dx
              0x4b0d016d,   # sub w13,w11,w13          dy (up)
              0xf940312e,   # ldr x14,[x9,#0x60]       last take
              0xf900312f,   # str x15,[x9,#0x60]
              0xcb0e01ee,   # sub x14,x15,x14
              0xd53be00a,   # mrs x10,cntfrq_el0
              0xeb4a0ddf,   # cmp x14,x10,lsr #3
              0x540002a8,   # b.hi orig (+21)          not taken for 1/8 s: dropped
              0x1e220191,   # scvtf s17,w12
              0x1e2201b2,   # scvtf s18,w13
              0x52a781cf,   # mov w15,#0x3c0e0000
              0x729f46af,   # movk w15,#0xfa35         pi / 360
              0x1e2701f3,   # fmov s19,w15
              0x1e300a31,   # fmul s17,s17,s16
              0x1f13ae2b,   # fmsub s11,s17,s19,s11    half angle -= dx * sens * pi / 360
              0xbd414674,   # ldr s20,[x19,#0x144]
              0xbd414e75,   # ldr s21,[x19,#0x14c]
              0x1e340a94,   # fmul s20,s20,s20
              0x1f1552b4,   # fmadd s20,s21,s21,s20
              0x1e21c294,   # fsqrt s20,s20            horizontal distance
              0xbd405536,   # ldr s22,[x9,#0x54]       pitch degrees per count (its sign: mouse up looks up)
              0x1e360a52,   # fmul s18,s18,s22
              0x1e332a73,   # fadd s19,s19,s19         pi / 180
              0x1e330a52,   # fmul s18,s18,s19
              0x1e348a52,   # fnmul s18,s18,s20        height change
              0xbd407937,   # ldr s23,[x9,#0x78]
              0x1e322af7,   # fadd s23,s23,s18
              0xbd007937,   # str s23,[x9,#0x78]
              0x1e204160):  # orig: fmov s0,s11 (original)
        ctx.emit(w)
    ctx.emit(B(ctx.here(), 0x1d595d4))
    ctx.put(0x1d595d0, [B(0x1d595d0, start)], 'mouse camera: Ultrahand turns the held object by the mouse')
    ctx.site(0x1d59670, 'e30a412d')   # ldp s3,s2,[x23,#8]
    start = ctx.here(); ctx.emit(ADRP(9, ctx.here(), ctx.state)); ctx.emit(ADDI(9, 9, MOUSE_STATE))
    for w in (0xbd407930,   # ldr s16,[x9,#0x78]       height change from the turn stub
              0xb900793f,   # str wzr,[x9,#0x78]
              0x1e302800,   # fadd s0,s0,s16
              0x2d410ae3):  # ldp s3,s2,[x23,#8]       (original) the height bounds
        ctx.emit(w)
    ctx.emit(B(ctx.here(), 0x1d59674))
    ctx.put(0x1d59670, [B(0x1d59670, start)], 'mouse camera: Ultrahand raises the held object by the mouse')
    # Ascend's aiming keeps the Chase camera, so no camera stub tells it apart. Its player node, ExecutePlayerCeilingClipper (calc in slot 9 of its vtable,
    # main+0x23de46c; enter main+0x23de36c refuses unless the special power manager's current power, +0x6c, is 3), is the aiming phase: node +0x20 = 0 looks for a
    # ceiling (main+0x23de638, Link's ready pose) and starts the PlayerCeilingClipper event (the rise, another node) on A; 1 plays WarpHeightNG and returns to 0. The
    # calc stamps state +0x80 each frame, and the host counts it as an ability for 1/8 s after, so the mouse buttons are A (rise) and B (cancel)
    ctx.site(0x23de46c, 'fd7bbda9')   # stp x29,x30,[sp,#-0x30]!
    start = ctx.here(); ctx.emit(ADRP(9, ctx.here(), ctx.state)); ctx.emit(ADDI(9, 9, MOUSE_STATE))
    for w in (0xbd405130,   # ldr s16,[x9,#0x50]       degrees per count
              0x1e202208,   # fcmp s16,#0.0
              0x54000060,   # b.eq orig (+3)           mouse camera off
              0xd53be04f,   # mrs x15,cntvct_el0
              0xf900412f,   # str x15,[x9,#0x80]       Ascend aiming
              0xa9bd7bfd):  # orig: stp x29,x30,[sp,#-0x30]! (original)
        ctx.emit(w)
    ctx.emit(B(ctx.here(), 0x23de470))
    ctx.put(0x23de46c, [B(0x23de46c, start)], 'mouse camera: Ascend aiming marks itself as an ability')
