"""Bananza v458752 startup display dimensions; disabled unless host state enables them.

Original instructions were checked against the main NSO identified below. The
geometry comes from the window scene before guest entry, rather than a device
model or a change to executable pages at runtime.
"""
import struct
from nso_asm import ADDI, ADRP, B, LDUR, STP_PRE, LDP_POST


def display(ctx):
    def load_at(site, original, reg, field, packed=False, replay=False):
        """Replace a getter, or replace its dimension register before a store.

        Preserve scratch registers, SP and NZCV, including at non-call sites.
        Disabled execution replays the exact original instruction.
        """
        ctx.site(site, original)
        here, emit = ctx.here, ctx.emit
        start = here()
        emit(STP_PRE(16, 17, 31, -16))
        emit(ADRP(16, here(), ctx.state))
        emit(ADDI(16, 16, ctx.state & 0xfff))
        emit(0xb9400211)                       # ldr w17,[x16] (enabled)
        emit(0x34000000 | (5 << 5) | 17)       # cbz w17,original
        emit(LDUR(reg, 16, field) if packed else
             0xb9400000 | ((field // 4) << 10) | (16 << 5) | reg)
        emit(LDP_POST(16, 17, 31, 16))
        emit(struct.unpack('<I', bytes.fromhex(original))[0] if replay else 0xd503201f)
        emit(B(here(), site + 4))
        emit(LDP_POST(16, 17, 31, 16))
        emit(struct.unpack('<I', bytes.fromhex(original))[0])
        emit(B(here(), site + 4))
        ctx.put(site, [B(site, start)], 'startup window-scene dimensions; zero state preserves original dimensions')

    # Window and capture dimensions; the rendering initializer calls these too.
    load_at(0x5bf770, '00688152', 0, 4)
    load_at(0x5bf780, '80ca8052', 0, 8)
    # Project camera screen-size accessors. Gameplay stage setup (0x3d3698,
    # including CityLevelStage/CoreLevelStage/GroundLevelStage) and the other
    # camera initializers divide these values before setting camera +0x100.
    # Its pose update (0x45e204) updates both projection and culling frustum.
    load_at(0x45d560, '00a08052', 0, 4)
    load_at(0x45d570, '005a8052', 0, 8)
    # Screen render configuration: its default dimension table and the explicit
    # docked/handheld overrides in the graphics initializer use the same extent.
    load_at(0x539cc8, '0ac000f8', 10, 4, packed=True, replay=True)
    load_at(0x5399c0, 'e84305f8', 8, 4, packed=True, replay=True)
    load_at(0x5399d8, 'a8c208f8', 8, 4, packed=True, replay=True)

    def ui_calculate(site, original, restore=False):
        """Fit the root projection during Calculate; offscreen cameras stay authored.

        The 46880 update passes renderer+0x60 DrawInfo to root Calculate.
        Save its projection Y row in a per-call stack frame and restore it after
        the virtual Calculate returns. Nested calls own separate frames.
        """
        ctx.site(site, original)
        start = ctx.here()
        words = []
        def add(word):
            words.append(word)
        def address():
            return start + len(words) * 4
        if not restore:
            add(0xd10083ff)                   # sub sp,sp,#32 (persistent frame)
        add(STP_PRE(16, 17, 31, -48))
        add(0x3d8007f0)                       # str q16,[sp,#16]
        add(0x3d800bf1)                       # str q17,[sp,#32]
        branches = []
        if restore:
            add(0xf94023f0)                   # ldr x16,[sp,#64] (saved DrawInfo)
            branches.append((len(words), 0xb4000010))
            add(0)                           # cbz x16,done
            add(0x3dc00ff0)                   # ldr q16,[sp,#48] (original Y row)
            add(0x3d800a10)                   # str q16,[x16,#32]
        else:
            add(0xf90023ff)                   # str xzr,[sp,#64] (inactive)
            add(ADRP(16, address(), ctx.state))
            add(ADDI(16, 16, ctx.state & 0xfff))
            add(0xb9400211)                   # ldr w17,[x16]
            branches.append((len(words), 0x34000011))
            add(0)                           # cbz w17,done
            add(0xf9400030)                   # ldr x16,[x1] (DrawInfo vtable)
            add(ADRP(17, address(), 0x23dc990))
            add(ADDI(17, 17, 0x990))
            add(0xca110210)                   # eor x16,x16,x17; preserve NZCV
            branches.append((len(words), 0xb5000010))
            add(0)                           # cbnz x16,done
            add(ADRP(16, address(), ctx.state))
            add(ADDI(16, 16, ctx.state & 0xfff))
            add(0xbd401210)                   # ldr s16,[x16,#16] (native aspect)
            add(0xbd410271)                   # ldr s17,[x19,#0x100] (authored width)
            add(0x5e21da31)                   # scvtf s17,s17
            add(0x1e311a10)                   # fdiv s16,s16,s17
            add(0xbd410671)                   # ldr s17,[x19,#0x104] (authored height)
            add(0x5e21da31)                   # scvtf s17,s17
            add(0x1e310a11)                   # fmul s17,s16,s17 (native/authored)
            # Root Calculate skips its cached matrix unless dirty. Mark only
            # this root dirty; Calculate consumes the bit and propagates the
            # parent transform to children, without changing call arguments.
            add(0x39416011)                   # ldrb w17,[x0,#0x58]
            add(0x321c0231)                   # orr w17,w17,#0x10
            add(0x39016011)                   # strb w17,[x0,#0x58]
            add(0xf90023e1)                   # str x1,[sp,#64]
            add(0x3dc00830)                   # ldr q16,[x1,#32]
            add(0x3d800ff0)                   # str q16,[sp,#48]
            for offset in range(32, 48, 4):
                add(0xbd400030 | ((offset // 4) << 10))  # ldr s16,[x1,#offset]
                add(0x1e310a10)               # fmul s16,s16,s17
                add(0xbd000030 | ((offset // 4) << 10))  # str s16,[x1,#offset]
        done = address()
        add(0x3dc00bf1)                       # ldr q17,[sp,#32]
        add(0x3dc007f0)                       # ldr q16,[sp,#16]
        add(LDP_POST(16, 17, 31, 48))
        if restore:
            add(ADDI(31, 31, 32))
        add(struct.unpack('<I', bytes.fromhex(original))[0])
        add(B(address(), site + 4))
        for index, opcode in branches:
            words[index] = opcode | (((done - (start + index * 4)) // 4) << 5)
        for word in words:
            ctx.emit(word)
        ctx.put(site, [B(site, start)], 'root Calculate projection Y fit with paired restoration; offscreen cameras unchanged')

    ui_calculate(0x47188, '080040f9')
    ui_calculate(0x47194, '9f6200f9', restore=True)

    def full_cover():
        """Extend reviewed full-canvas pictures, leaving text and anchors intact.

        The v458752 BFLYT inventory identifies these exact picture names with
        authored 1920x1080 extents. Never apply this to character/logo captures.
        Assignment changes only the height, and the original-size gate makes
        repeated Calculate calls idempotent. Source resources remain unchanged.
        """
        names = ('P_BgBlack_00', 'P_BgBottom_00', 'P_BgCapture_00', 'P_BgColor_00', 'P_BgEft_00', 'P_BgGorongEft_00', 'P_BgGorongo_00', 'P_BgKingKRoolEft_00', 'P_BgKingKRool_00', 'P_BgOutBlack_00', 'P_BgTop_00', 'P_Bg_00', 'P_Bg_01', 'P_BlackBg_00', 'P_BlackBlur_00', 'P_BlackBlur_01', 'P_Blur_00', 'P_Blur_01', 'P_Bs_00', 'P_CaptureBg_00', 'P_CaptureBlur_00', 'P_CircleBg_00', 'P_DarkBgColor_00', 'P_FadeBlack_00', 'P_Fade_00', 'P_GoldBlur_00', 'P_Gradation_00', 'P_Gradation_01', 'P_LightBgColor_00', 'P_PauseBgColor_00', 'P_PauseBgCrack_00', 'P_PauseBg_00', 'P_SkyBg_00', 'P_SkyBg_01', 'P_VoidBg_00', 'P_WipeVoid_00', 'P_Wipe_00')
        def hash_name(name):
            value = 0
            for char in name.encode():
                value = (value * 33 + char) & 0xffffffff
            return value
        hashes = [hash_name(name) for name in names]
        assert len(hashes) == len(set(hashes))
        site = 0x2f8440
        ctx.site(site, '08604139')
        start = ctx.here()
        words = []
        def add(w): words.append(w)
        def address(): return start + 4 * len(words)
        def constant(reg, value, wide=True):
            add((0xd2800000 if wide else 0x52800000) | ((value & 65535) << 5) | reg)
            for shift in range(16, 64 if wide else 32, 16):
                if value >> shift:
                    add((0xf2800000 if wide else 0x72800000) | ((shift // 16) << 21) | (((value >> shift) & 65535) << 5) | reg)
        exits = []
        add(STP_PRE(14, 15, 31, -64))
        add(0xa90147f0)                       # stp x16,x17,[sp,#16]
        add(0x3d800bf0)                       # str q16,[sp,#32]
        add(0x3d800ff1)                       # str q17,[sp,#48]
        add(ADRP(16, address(), ctx.state))
        add(ADDI(16, 16, ctx.state & 0xfff))
        add(0xb9400211)
        exits.append((len(words), 0x34000011)); add(0)
        add(0xf9400030)                       # ldr x16,[x1] (DrawInfo vtable)
        add(ADRP(17, address(), 0x23dc990))
        add(ADDI(17, 17, 0x990))
        add(0xca110210)
        exits.append((len(words), 0xb5000010)); add(0)
        add(ADDI(16, 0, 0xb0))                # inline pane name
        constant(17, 0, False)
        loop = address()
        add(0x3840160f)                       # ldrb w15,[x16],#1
        end_hash_branch = len(words); add(0)
        add(0x0b111631)                       # add w17,w17,w17,lsl#5
        add(0x0b0f0231)                       # add w17,w17,w15
        add(B(address(), loop))
        end_hash = address()
        add(0xf940280e)                       # ldr x14,[x0,#0x50] (width,height)
        constant(15, 0x4487000044f00000)
        add(0xca0f01ce)                       # eor x14,x14,x15
        rotated_branch = len(words); add(0)
        matches = []
        for value in hashes:
            constant(14, value, False)
            add(0x4a0e022f)                   # eor w15,w17,w14
            matches.append(len(words)); add(0)
        no_match = len(words); add(0)
        match = address()
        add(ADRP(16, address(), ctx.state))
        add(ADDI(16, 16, ctx.state & 0xfff))
        add(0xbd401210)                       # native aspect
        constant(17, 0x44f00000, False)       # authored width 1920.0
        add(0x1e270231)                       # fmov s17,w17
        add(0x1e301a30)                       # fdiv s16,s17,s16
        add(0xbd005410)                       # str s16,[x0,#0x54]
        normal_done = len(words); add(0)
        rotated = address()
        # MenuSkill's reviewed -90-degree pictures have a +90-degree parent.
        # Runtime world matrices are axis-aligned (Y offsets 23 and 2).
        # Extend height; local rotation alone would choose the wrong axis.
        rotated_next = []
        rotated_done = []
        for name, height in [('P_Bg_00', 1280.), ('P_BlackBlur_00', 1080.)]:
            for index, opcode in rotated_next:
                words[index] = opcode | (((address() - (start + index * 4)) // 4) << 5)
            rotated_next = []
            def reject(opcode):
                rotated_next.append((len(words), opcode)); add(0)
            constant(14, hash_name(name), False)
            add(0x4a0e022f)
            reject(0x3500000f)
            add(0xf940280e)
            constant(15, struct.unpack('<Q', struct.pack('<ff', 2100., height))[0])
            add(0xca0f01ce)
            reject(0xb500000e)
            add(0xb940440e)                   # ldr w14,[x0,#0x44] rotation Z
            constant(15, 0xc2b40000, False)   # -90 degrees
            add(0x4a0f01ce)
            reject(0x3500000e)
            add(ADRP(16, address(), ctx.state))
            add(ADDI(16, 16, ctx.state & 0xfff))
            add(0xbd401210)
            constant(17, struct.unpack('<I', struct.pack('<f', height * 16 / 9))[0], False)
            add(0x1e270231)
            add(0x1e301a30)
            add(0xbd005410)                   # str s16,[x0,#0x54] height
            rotated_done.append(len(words)); add(0)
        done = address()
        for index, opcode in rotated_next:
            words[index] = opcode | (((done - (start + index * 4)) // 4) << 5)
        words[rotated_branch] = 0xb500000e | (((rotated - (start + rotated_branch * 4)) // 4) << 5)
        for index in [normal_done] + rotated_done:
            words[index] = B(start + index * 4, done)
        add(0x3dc00ff1)
        add(0x3dc00bf0)
        add(0xa94147f0)
        add(LDP_POST(14, 15, 31, 64))
        add(0x39416008)                       # original ldrb w8,[x0,#0x58]
        add(B(address(), site + 4))
        for index, opcode in exits:
            words[index] = opcode | (((done - (start + index * 4)) // 4) << 5)
        words[end_hash_branch] = 0x3400000f | (((end_hash - (start + end_hash_branch * 4)) // 4) << 5)
        for index in matches:
            words[index] = 0x3400000f | (((match - (start + index * 4)) // 4) << 5)
        words[no_match] = B(start + no_match * 4, done)
        for w in words: ctx.emit(w)
        ctx.put(site, [B(site, start)], 'reviewed full-canvas background pictures cover native height; UI coordinates remain authored')

    full_cover()


DISPLAY = {'e25c90c1c136638885659fd55bf8a03b': display}

BINDING = [{
    'what': 'Startup display dimensions, project-camera aspect and uniform UI fit',
    'now': 'build', 'goal': 'game', 'purpose': 'rendering',
    'why': 'The game fixes its window and project camera to 16:9. The main DrawInfo projection fits UI uniformly with vertical inset during Calculate, preserving CPU coordinates. Reviewed full-canvas picture extents cover the extra height. Only reviewed offsets in v458752 are supported; function matching for other builds is not implemented.',
    'checked': 'v458752 main disassembly and camera ownership, 2026-11-01; original-byte assertions and 2076 AArch64 emulation cases, including DrawInfo vtable exclusion and register preservation; onscreen layer DrawInfo ownership confirmed by runtime probes; paired projection restoration tested; Mac open/closed geometry visually validates title, loading, HUD, full background coverage and Skill selection anchors',
}]
