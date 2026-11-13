# Bayonetta 3 (01004A4010FEA000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py;
# FRAME_STEP as described there ("Frame step in whole vsyncs": the state page and the render scale the loader writes at +0x10).
import struct

from nso_asm import ADRP, B

# ---- Render size, update 1.2.0 (main b47cc79b88b7091ea514b2b725c50ae0) ----
# The engine draws the docked scene at 1600x896 into a 1600x900 window, and lowers the scene's size under load through eight levels (1600x896 twice, 1504x848,
# 1440x816, 1360x768, 1280x720, 1024x576, 640x360). Nothing is resized at run time: at start-up the game makes each render target once at the largest size ("backing":
# the window-sized depth and colour targets of main+0x69d344, the scene-sized targets and their half, quarter ... chains of main+0x69de80) and, for every level, a
# view of each backing with the level's size (the loops at main+0x69d6bc and main+0x6a05d8). A view takes no memory of its own: main+0xd0f2f0 refuses one that needs
# more storage than its backing has (the compare at main+0xd0f4fc), and a refused view fails the game's start-up, which then faults in its shutdown
# (main+0xb8462c). A second family of eight levels, 1280x720 down to 640x360, has its own 1280x720 backings (main+0x6a0fc8). The backings come out of the engine's
# FrameBufferHeap, 260 MB (main+0x69d164), which main+0xcd5714 takes from the nn::os heap as a memory block of its own (AllocateMemoryBlock), so its only parent
# is the application pool (the profile's app_pool).
#
# The sizes are immediates in the three set-up functions and two tables of eight words that only main+0x69d344 reads. Every one of them that belongs to the window,
# the docked scene and its levels or the second family is listed in sites below, found by changing each immediate and table entry of the set-up code alone and running
# the set-up in an interpreter (which arguments of which creation call moved); the stubs multiply them by the host's render scale (state +0x10, DM_RENDER_SCALE),
# so the backings, their views, the swap chain and the size the engine reports (main+0x70a740) grow together. Each stub runs the site's own instruction first and
# leaves its value alone at scale 1.0, compared by its bits: at 1.0 the game computes exactly what the unpatched build does.
#   W  a width:                        16 * round(v * s / 16)
#   H  a height of the scene's shape:  8 * round(v * s * 900/896 / 8): 896 gives 900 * s, 1080 rows at 1.2, 1440 at 1.6, 1800 at 2.0. The game's own 896 is 900
#      less four rows; a scaled scene has the window's rows, and every lower level keeps its share of them
#   V  the window's height (900):      H of 896, the very value the scene's largest level gets, so the scene's views never have more rows than the window-sized
#      backings they share (two roundings of 900 * s could differ by a step of 8)
#   P  the padded height of the window's buffers at main+0x69c6f4 (1024 = 900 in whole blocks of 128 rows): V, then up to a multiple of 128
#   M  the FrameBufferHeap's size: 293 MB * max(s, 1)^2, up to a whole 2 MB; 293 is the game's 260 MB and an eighth. Storage grows with the area at most (rows are
#      stored in whole blocks, and 896 and 900 already take 1024), and the targets of fixed size do not grow
#   C  the entries of the exposure's column buffer (main+0xd587b0): W of the game's 256. The exposure pass (averagebrightnessps1) draws one fragment for every two
#      columns of the quarter-size scene and stores that pair's luminance sum at the fragment's x in a buffer of 256 words (4 bytes times 256, main+0xd118e8), which
#      the second pass sums: 200 entries at 1600 wide, 240 at 1920, and 320 at 2560, where the writes ran past the buffer and the exposure came out wrong (a white
#      title screen, a flickering white picture in play, at 1.6 and 2.0 and not at 1.2)
#   T  the light grid's capacity (main+0xd57964): the Forward+ light lists are kept per tile of 16 pixels, (width + 15) / 16 by (height + 15) / 16 of the size being
#      drawn (main+0x686660, no upper bound), in buffers made once for 120 by 68 tiles (main+0xd578b0: 1920x1088, so 1.2 fills them exactly). The capacity becomes
#      the largest level's tiles, W of 1600 over 16 by H of 896 in whole tiles, and never less than the game's: the scale is taken as at least 1.2 here
#   U  the entries of the two buffers whose size is written out, 4 words a tile (main+0xd57994: 0x7f80 = 120 * 68 * 4): the capacity just stored, times 4. The
#      other two take theirs from the stored capacity already (main+0xd579ec, main+0xd57a58)
#      At 1.6 and 2.0 the lists of 14,400 and 22,600 tiles were written into room for 8,160: a white title screen and a flickering white picture in play
#   S  a shadow map's size: times the host's shadow factor (state +0xa0, DM_SHADOW_SCALE: 1, 2 or 4; nothing written = the game's). The shadow maps are one colour and
#      one depth backing of 2048 squares (main+0x69e7f4, main+0x69dda0) with three docked views each, 2048, 1600 and 1024 squares; all of them take the factor, so
#      every view still fits its backing. It does not depend on the render scale. The handheld views (1792, 1472, 896) keep their sizes and fit the larger backings.
#      M adds 32 MB * (f * f - 1) to the FrameBufferHeap: the two backings are 16 MB each at 2048 squares, 64 at 4096 and 256 at 8192, at 4 bytes a texel
#   L  the model level the selector hands back (main+0xbd5b2c, the end of main+0xbd563c: the level it picked by distance, at most the caller's cap; 0 the finest,
#      -1 hidden): with the host's model detail set (state +0xa4, DM_MODEL_DETAIL=1) every level above 0 becomes 0, and a hidden model stays hidden, so the
#      distance at which the game stops drawing a model is its own
#   D  the game's own dynamic resolution (main+0x6d3cd4: in main+0x6d3bb4, which each frame picks the level of the eight, 0 the largest, the controller's state is read from
#      a field at +0xd610 of the render object, written by the setter at main+0x6e8a14, and added to a small offset, then held to 0..6): with the host's DM_DYNAMIC_RESOLUTION
#      0 (state +0x40, the default of the launcher's Dynamic resolution) the state read is 0, so the level stays 0 or 1 (both the largest size) whatever load the controller
#      sees; 1 leaves the game's choice. The special levels of the loading screens and transitions (7, set before this read) are as the game has them
#   R  the distance a model's level is chosen by (main+0xbd57c0 and main+0xbd5884, the two fsqrt of the camera distance in main+0xbd563c, which the selector compares with the
#      level thresholds and with the last one, past which a model is not drawn, and, in its angular-size mode, divides the model's size by): times the host's reciprocal of the
#      draw distance (state +0xa8, DM_DRAW_DISTANCE: 1.5, 2, 3 or 4 stores 1 / that; 0 or nothing = the game's). Every threshold moves out by the factor, the one that
#      hides a model too. It reaches the models this selector chooses a level for; other culling (the level's own visibility ranges, effects, shadows) is not changed
#   A  the picture's shape (DM_ASPECT, the launcher's Picture shape): 16:9, the game's (the default), or 16:10. The heights take their own scale, state +0xb0 = the render scale times
#      (16/9) / the shape's ratio, 1.111 for 16:10, so 1.2 gives a 1920x1200 window and scene; every H, V, P and T site and the heap use it, the widths and everything else the
#      scale. The game computes its camera's aspect factor from the window's width and height (main+0xca8dc0 and main+0xca9218: 16/9 over width / height, stored at +0x38 of the
#      camera), so the camera follows the window; what the interface and the cut scenes do with a window of another shape is not known (experimental)
#   G  the interface's projection (main+0x64020c, in main+0x640120: the one 2D layer's ortho, set by main+0xd07610 from (left 0, right 1280, 0, bottom 720, near 0, far 65535) and
#      so spread over the whole window whatever its shape): with the host's DM_UI_SHAPE not 'screen' (state +0xb4, the float k above, 0 = stretch) the vertical pair, s2 and s3, are
#      widened by half of what the picture's rows add, e = 720 * (k - 1) / 2 (0 at 16:9; 40 for 16:10, k = 1.111): -e and 720 + e. The interface is laid out in the same 1280x720
#      space, so it keeps its proportions and sits in the middle of the taller picture, with the 3D scene filling it. With DM_UI_SHAPE=fill (state +0xb8 = k, +0xb4 = 0) the horizontal pair is narrowed instead, by c = 1280 * (k - 1) / k / 2 each side: the layout fills the picture's height and its sides are cut, so the 1280x720 quads of fades and bars cover the whole frame. k is the float the host writes at +0xb4 (the rows' factor
#      of DM_ASPECT, 0 for 'screen'). The renderer's own window fields (+0x130 / +0x134, what main+0xd02d70 / 0xd02d80 read) are NOT the shape: the game keeps the docked 1920x1080
#      there whatever the swap chain is, so a stub that reads them widens nothing (the first form of this stub, not effective in the game)
# Rounding to the nearest multiple never orders two sizes the other way round, so a view that fits its backing by its sizes still does at every scale.
# Handheld mode keeps the game's sizes: its levels come from other immediates and its second table, and none of that is scaled, so the first stub (I, after the
# operation mode is read at main+0x69c6c8) sets the scale to 1.0 there before anything reads it. It does the same for a scale below 1.0: the views of the handheld
# levels are made in docked mode too (1360x768 the largest) and would not fit window-sized backings made smaller, and the game lowers its size by its own levels.
# A game started docked and switched to handheld later (main+0x69fda8) makes the handheld views at the game's sizes in the scaled backings, where they fit.
# Not scaled, on purpose: the shadow maps (2048 / 1600 / 1024 squares over a 2048 backing), the 800x450 and 480x270 sets of main+0x69de80, the 1280x720 targets
# of main+0x69d344 and the small fixed squares; they are made and used at one size, whatever the scene's is.
# The stubs write x16, x17, v16, v17 and v18 and, but for the first, no flags (the flags of main+0x69c6cc are still live at the sites after it). The functions the
# sites are in use none of those registers, except main+0x69d344, which loads w16 at +0x69d86c and tests it in the next instruction, with no site between.
build_120 = 'b47cc79b88b7091ea514b2b725c50ae0'
render_scale_max = 4          # 6400x3600: the profile's pool holds it; 3.2 is 5120x2880 (5K), 2.4 3840x2160 (4K), 2.0 3200x1800, 1.2 1920x1080
heap_mb = 293                 # FrameBufferHeap at scale 1.0 and below, in MB, before the area factor (the game's own: 260)
k_shape = 900 / 896           # H: the window's rows over the scene's

# (offset in main, the site's instruction, kind)
sites = [
    (0x69c6cc, 0x7100001f, 'I'),   # cmp w0, #0          the operation mode just read (0 docked)
    (0x70a740, 0x5280c808, 'W'),   # mov w8, #0x640      the window's size, as the engine reports it (swap chain, layout)
    (0x70a748, 0x52807088, 'V'),   # mov w8, #0x384
    (0x69c6d4, 0x5280c800, 'W'),   # mov w0, #0x640      the docked display width, and the width of the window's buffers (main+0x69d090)
    (0x69c6e0, 0x52807009, 'H'),   # mov w9, #0x380      the docked display height
    (0x69c6f0, 0x52807081, 'V'),   # mov w1, #0x384      the height of the window's buffers
    (0x69c6f4, 0x52808008, 'P'),   # mov w8, #0x400      their padded height
    (0x69c720, 0x5280c804, 'W'),   # mov w4, #0x640      the window-sized backings (main+0x69d344)
    (0x69c728, 0x52807085, 'V'),   # mov w5, #0x384
    (0x69c73c, 0x5280c800, 'W'),   # mov w0, #0x640      the scene-sized backings (main+0x69de80)
    (0x69c740, 0x52807001, 'H'),   # mov w1, #0x380
    (0x69fde8, 0x5280c813, 'W'),   # mov w19, #0x640     the same three when the operation mode changes (main+0x69fda8)
    (0x69fdf8, 0x52807009, 'H'),   # mov w9, #0x380
    (0x69fe08, 0x52807001, 'H'),   # mov w1, #0x380
    (0x69d6d0, 0xb8686937, 'W'),   # ldr w23, [x9, x8]   the docked levels' widths (the table at main+0x182d670), for the views of the window-sized backings
    (0x69d6dc, 0xb868693c, 'H'),   # ldr w28, [x9, x8]   their heights (main+0x182d570)
    (0x69f0b8, 0x5280c819, 'W'),   # mov w25, #0x640     the docked levels of the scene-sized backings' views (main+0x6a05d8): level 0 and 1
    (0x69f0c8, 0x52807003, 'H'),   # mov w3, #0x380
    (0x69f0f4, 0x52807003, 'H'),   # mov w3, #0x380
    (0x69f114, 0x52807003, 'H'),   # mov w3, #0x380
    (0x69f128, 0x52807003, 'H'),   # mov w3, #0x380
    (0x69f140, 0x5280bc19, 'W'),   # mov w25, #0x5e0     level 2: 1504x848
    (0x69f144, 0x52806a1a, 'H'),   # mov w26, #0x350
    (0x69f180, 0x5280b419, 'W'),   # mov w25, #0x5a0     level 3: 1440x816
    (0x69f184, 0x5280661a, 'H'),   # mov w26, #0x330
    (0x69f1c0, 0x5280aa19, 'W'),   # mov w25, #0x550     level 4: 1360x768
    (0x69f1d0, 0x52806003, 'H'),   # mov w3, #0x300
    (0x69f1e4, 0x52806003, 'H'),   # mov w3, #0x300
    (0x69f1fc, 0x5280a01b, 'W'),   # mov w27, #0x500     level 5: 1280x720 (also the second family's levels 0 and 1)
    (0x69f200, 0x52805a1a, 'H'),   # mov w26, #0x2d0
    (0x69f23c, 0x52804819, 'H'),   # mov w25, #0x240     level 6: 1024x576
    (0x69f248, 0x52808002, 'W'),   # mov w2, #0x400
    (0x69f25c, 0x52808002, 'W'),   # mov w2, #0x400
    (0x69f278, 0x5280501c, 'W'),   # mov w28, #0x280     level 7: 640x360 (also the second family's levels 6 and 7)
    (0x69f27c, 0x52802d19, 'H'),   # mov w25, #0x168
    (0x69ddc8, 0x5280a001, 'W'),   # mov w1, #0x500      the second family's backings: its depth (main+0x69dd70)
    (0x69ddcc, 0x52805a02, 'H'),   # mov w2, #0x2d0
    (0x69df8c, 0x5280a001, 'W'),   # mov w1, #0x500      and its two colour targets
    (0x69df90, 0x52805a02, 'H'),   # mov w2, #0x2d0
    (0x69dfbc, 0x5280a001, 'W'),   # mov w1, #0x500
    (0x69dfc0, 0x52805a02, 'H'),   # mov w2, #0x2d0
    (0x69f2d8, 0x52808c02, 'W'),   # mov w2, #0x460      the second family's docked levels 2 to 5: 1120x628
    (0x69f2dc, 0x52804e83, 'H'),   # mov w3, #0x274
    (0x69f2e8, 0x52804383, 'H'),   # mov w3, #0x21c      960x540
    (0x69f2f0, 0x52807802, 'W'),   # mov w2, #0x3c0
    (0x69f2f8, 0x52806402, 'W'),   # mov w2, #0x320      800x448
    (0x69f300, 0x52803803, 'H'),   # mov w3, #0x1c0
    (0x69f308, 0x52803283, 'H'),   # mov w3, #0x194      720x404 (its width is level 5's height register)
    (0x69d164, 0x52a2080a, 'M'),   # mov w10, #0x10400000   the FrameBufferHeap's size
    (0x69dda0, 0x52810008, 'S'),   # mov w8, #0x800      the shadow maps' depth backing (main+0x69dd70)
    (0x69e7f4, 0x52810008, 'S'),   # mov w8, #0x800      and their colour backing
    (0x69f348, 0x52810002, 'S'),   # mov w2, #0x800      the docked views: 2048 squares of the colour backing
    (0x69f34c, 0x52810003, 'S'),   # mov w3, #0x800
    (0x69f380, 0x52810002, 'S'),   # mov w2, #0x800      and of the depth backing
    (0x69f384, 0x52810003, 'S'),   # mov w3, #0x800
    (0x69f3d0, 0x5280c802, 'S'),   # mov w2, #0x640      1600 squares (the height is a copy of the width)
    (0x69f400, 0x5280c802, 'S'),   # mov w2, #0x640
    (0x69f450, 0x52808002, 'S'),   # mov w2, #0x400      1024 squares
    (0x69f454, 0x52808003, 'S'),   # mov w3, #0x400
    (0x69f480, 0x52808002, 'S'),   # mov w2, #0x400
    (0x69f484, 0x52808003, 'S'),   # mov w3, #0x400
    (0xbd5b2c, 0x1a99b108, 'L'),   # csel w8, w8, w25, lt   the model level picked, at most the caller's cap (main+0xbd563c)
    (0x6d3cd4, 0xb8686948, 'D'),   # ldr w8, [x10, x8]    the dynamic resolution controller's state (main+0x6d3bb4)
    (0xbd57c0, 0x6ea1f801, 'R'),   # fsqrt v1.4s, v0.4s    the camera distance, in the distance mode (main+0xbd563c)
    (0xbd5884, 0x6ea1f800, 'R'),   # fsqrt v0.4s, v0.4s    and in the angular-size mode
    (0x64020c, 0x4ea01c02, 'G'),   # mov v2.16b, v0.16b   the interface's ortho, its top (main+0x640120)
    (0xd57964, 0xf2c00888, 'T'),   # movk x8, #0x44, lsl #32    the light grid's capacity: 120 tiles (the mov before it) by 68 (main+0xd578b0)
    (0xd57994, 0xf2cff008, 'U'),   # movk x8, #0x7f80, lsl #32  the entries of its two written-out buffers: 4 bytes times 120 * 68 * 4
    (0xd587b0, 0xf2c02008, 'C'),   # movk x8, #0x100, lsl #32   the exposure's column buffer: 4 bytes times 256 entries (main+0xd5870c)
]


def f32(x): return struct.unpack('<f', struct.pack('<f', x))[0]


def nearest(x):   # fcvtns: to the nearest whole number, a tie to the even one
    return round(x)


shadow_mb = 32                # what a shadow factor f adds to the FrameBufferHeap, in MB, is 32 * (f * f - 1): the two backings at 4 bytes a texel are 32 MB at 2048 squares, 128 MB at 4096, 512 MB at 8192


def scaled(kind, v, s, shadow=1, aspect=1.0):
    """what the stub of a site of this kind leaves for the site's value v at render scale s: the stubs' arithmetic in single precision, as they do it"""
    s = f32(s); hs = f32(s * f32(aspect))   # the height scale: the loader's single-precision product
    if kind == 'T':   # (tiles across, tiles down); at 1.0 the stub leaves the game's 120 by 68, which is also what this gives
        e, eh = max(s, f32(1.2)), max(hs, f32(1.2))
        return (nearest(f32(f32(1600) * e)) + 8) >> 4, ((((nearest(f32(f32(f32(896) * eh) * f32(k_shape))) + 4) & ~7) + 15) >> 4)
    if kind == 'U': x, y = scaled('T', None, s, aspect=aspect); return x * y * 4
    if kind == 'S': return v * max(shadow, 1)
    if kind == 'M':
        import math
        mb = 260 if hs == 1.0 else (math.ceil(f32(f32(heap_mb) * f32(max(hs, 1.0) * max(s, 1.0)))) + 1) & ~1
        return (mb + (shadow_mb * (shadow * shadow - 1) if shadow > 1 else 0)) << 20
    if kind in 'WC':
        return v if s == 1.0 else (nearest(f32(f32(v) * s)) + 8) & ~15
    if hs == 1.0: return v
    if kind == 'H': return (nearest(f32(f32(f32(v) * hs) * f32(k_shape))) + 4) & ~7
    if kind == 'V': return scaled('H', 896, s, aspect=aspect)
    if kind == 'P': return (scaled('H', 896, s, aspect=aspect) + 127) & ~127
    raise ValueError(kind)


def and_low_clear(rd, rn, k):   # and wRd, wRn, #~((1 << k) - 1)
    return 0x12000000 | (32 - k) << 16 | (31 - k) << 10 | rn << 5 | rd


def render_scale(ctx):
    here, emit, state = ctx.here, ctx.emit, ctx.state
    kbits = struct.unpack('<I', struct.pack('<f', k_shape))[0]
    for off, word, kind in sites:
        ctx.site(off, word.to_bytes(4, 'little').hex())
        rd = word & 31
        start = here()
        if kind == 'I':
            emit(ADRP(16, here(), state))
            emit(0xbd401210)                               # ldr s16, [x16, #0x10]  the render scale
            emit(0x1e2e1011)                               # fmov s17, #1.0
            emit(0x1e312200)                               # fcmp s16, s17
            emit(0x5400004b)                               # b.lt +8                below 1.0, or no number: the game's sizes
            emit(0x34000080)                               # cbz w0, +16            docked: the scale stays
            emit(0x52a7f011)                               # mov w17, #0x3f800000   1.0
            emit(0xb9001211)                               # str w17, [x16, #0x10]
            emit(0xb900b211)                               # str w17, [x16, #0xb0]   and the height scale
            emit(word)                                     # cmp w0, #0             the site's own instruction, and the flags the game goes on with
            emit(B(here(), off + 4))
            ctx.put(off, [B(off, start)], 'render scale: handheld mode and a scale below 1.0 keep the game\'s sizes (the scale is set to 1.0 before anything reads it)')
            continue
        if kind == 'S':
            emit(word)                                     # the site's own instruction: the game's size
            emit(ADRP(16, here(), state))
            emit(0xb940a211)                               # ldr w17, [x16, #0xa0]     the shadow factor
            emit(0x34000051)                               # cbz w17, +8               none written: the game's size
            emit(0x1b117c00 | rd << 5 | rd)                # mul wRd, wRd, w17
            emit(B(here(), off + 4))
            ctx.put(off, [B(off, start)], 'shadow scale: a shadow map size times the host\'s shadow factor (state +0xa0; 1 or none = the game\'s)')
            continue
        if kind == 'D':
            emit(word)                                     # ldr w8, [x10, x8]         the game's state
            emit(ADRP(16, here(), state))
            emit(0xb9404210)                               # ldr w16, [x16, #0x40]     DM_DYNAMIC_RESOLUTION
            emit(0x35000050)                               # cbnz w16, +8              1: the game's own choice
            emit(0x2a1f03e0 | rd)                          # mov wRd, wzr              0: the largest level
            emit(B(here(), off + 4))
            ctx.put(off, [B(off, start)], 'dynamic resolution: the level the game picks stays at the largest unless DM_DYNAMIC_RESOLUTION is 1 (state +0x40)')
            continue
        if kind == 'R':
            emit(word)                                     # fsqrt v?.4s, v0.4s        the game's distance
            emit(ADRP(16, here(), state))
            emit(0xb940aa11)                               # ldr w17, [x16, #0xa8]     1 / the draw distance, as a float; 0 = the game's
            emit(0x34000071)                               # cbz w17, +12
            emit(0x1e270230)                               # fmov s16, w17
            emit(0x1e300800 | rd << 5 | rd)                # fmul sD, sD, s16          the distance, nearer by the factor
            emit(B(here(), off + 4))
            ctx.put(off, [B(off, start)], 'draw distance: the distance a model\'s level is chosen by times the host\'s reciprocal draw distance (state +0xa8; 0 = the game\'s)')
            continue
        if kind == 'G':
            def cb(rt, label): return ('cbz', rt, label)
            items = [word, ('adrp', 16, state), 0xbd40b610,                            # the site's mov v2.16b, v0.16b; ldr s16, [x16, #0xb4]   the picture's rows relative to 16:9's, 0 = off
                     0x1e2e1011, 0x1e313a10,                                           # fmov s17, #1.0; fsub s16, s16, s17   the extra rows' share, 0 or less = none
                     0x1e230a10, 0x1e2c1011, 0x1e310a10,                               # fmul s16, s16, s3 (720); fmov s17, #0.5; fmul s16, s16, s17   e, the extra above and below
                     0x1e206a10,                                                       # fmaxnm s16, s16, s0 (0)    nothing for a share below zero
                     0x1e214202, 0x1e302863,                                           # fneg s2, s16; fadd s3, s3, s16
                     ('adrp', 16, state), 0xbd40ba10,                                  # ldr s16, [x16, #0xb8]   fill: the layout scaled to the picture's height, k, 0 = off
                     0x1e2e1011, 0x1e316a10, 0x1e313a11, 0x1e301a30,                   # fmov s17, #1.0; fmaxnm s16, s16, s17; fsub s17, s16, s17; fdiv s16, s17, s16   k at least 1; (k - 1) / k
                     0x1e210a10, 0x1e2c1011, 0x1e310a10,                               # fmul s16, s16, s1 (1280); fmov s17, #0.5; fmul s16, s16, s17   c, the columns cut at each side
                     0x1e206a10, 0x1e302800, 0x1e303821,                               # fmaxnm s16, s16, s0 (0); fadd s0, s0, s16; fsub s1, s1, s16
                     ('label', 'done')]
            pos, idx = {}, 0
            for it in items:
                if isinstance(it, tuple) and it[0] == 'label': pos[it[1]] = idx
                else: idx += 1
            idx = 0
            for it in items:
                if isinstance(it, tuple) and it[0] == 'label': continue
                if isinstance(it, tuple) and it[0] == 'adrp': emit(ADRP(it[1], here(), it[2]))
                elif isinstance(it, tuple): emit(0x34000000 | ((pos[it[2]] - idx) & 0x7ffff) << 5 | it[1])   # cbz wN, done
                else: emit(it)
                idx += 1
            emit(B(here(), off + 4))
            ctx.put(off, [B(off, start)], 'interface: its 2D projection keeps the layout\'s 16:9 in a taller window (state +0xb4; the vertical range widened by the window\'s extra rows)')
            continue
        if kind == 'L':
            emit(word)                                     # csel w8, w8, w25, lt      the level the game picked
            emit(ADRP(16, here(), state))
            emit(0xb940a610)                               # ldr w16, [x16, #0xa4]     model detail
            emit(0x34000050)                               # cbz w16, +8               the game's level
            emit(0x0a807c00 | rd << 16 | rd << 5 | rd)     # and wRd, wRd, wRd, asr #31   a level above 0 becomes 0; hidden (-1) stays
            emit(B(here(), off + 4))
            ctx.put(off, [B(off, start)], 'model detail: the finest level of a model that is drawn (state +0xa4; 0 = the game\'s level)')
            continue
        if kind == 'M':
            emit(word)                                     # mov w10, #0x10400000      the game's 260 MB
            emit(ADRP(16, here(), state))
            emit(0xb940b211)                               # ldr w17, [x16, #0xb0]     the height scale's bits
            emit(0x52a7f010)                               # mov w16, #0x3f800000
            emit(0x4a110210)                               # eor w16, w16, w17
            emit(0x1e270230)                               # fmov s16, w17
            emit(0x52800011 | 260 << 5)                    # mov w17, #260
            emit(0x34000190)                               # cbz w16, +48              1.0 (the height scale is 1 only with the scale): the game's size
            emit(0x1e2e1012)                               # fmov s18, #1.0
            emit(0x1e324a10)                               # fmax s16, s16, s18        the height scale, at least 1
            emit(ADRP(16, here(), state))
            emit(0xbd401211)                               # ldr s17, [x16, #0x10]     the render scale
            emit(0x1e324a31)                               # fmax s17, s17, s18
            emit(0x1e310a10)                               # fmul s16, s16, s17        the area's factor
            for w in (0x52800010 | heap_mb << 5,           # mov w16, #heap_mb
                      0x1e230211,                          # ucvtf s17, w16
                      0x1e300a31,                          # fmul s17, s17, s16
                      0x1e280231,                          # fcvtps w17, s17           whole MB, upwards
                      0x11000631,                          # add w17, w17, #1
                      and_low_clear(17, 17, 1)):           # and w17, w17, #~1         whole 2 MB
                emit(w)
            emit(ADRP(16, here(), state))
            emit(0xb940a210)                               # ldr w16, [x16, #0xa0]     the shadow factor
            emit(0x340000b0)                               # cbz w16, +20              none written
            emit(0x1b107e10)                               # mul w16, w16, w16
            emit(0x51000610)                               # sub w16, w16, #1          f * f - 1
            emit(0x531b6a10)                               # lsl w16, w16, #5          times 32 MB
            emit(0x0b100231)                               # add w17, w17, w16
            emit(0xd36cac00 | 17 << 5 | rd)                # lsl xRd, x17, #20          the size in bytes, 64 bits: the heap is over 4 GB above 3.4
            emit(B(here(), off + 4))
            ctx.put(off, [B(off, start)], 'render scale: the FrameBufferHeap grows with the scaled area and with larger shadow maps')
            continue
        body = [0x1e270230]                                # fmov s16, w17             the scale
        if kind == 'T':
            # the light grid's capacity: height first from the height scale (state +0xb0, in w17 already), then the width from the render scale; 'ADRP17' is the adrp x17 of that
            body += [0x52933350,                           # mov w16, #0x999a
                     0x72a7f330,                           # movk w16, #0x3f99, lsl #16   1.2
                     0x1e270212,                           # fmov s18, w16
                     0x1e324a10,                           # fmax s16, s16, s18        the height scale, the game's own capacity is the least
                     0x52800011 | 896 << 5,                # mov w17, #896
                     0x1e230231,                           # ucvtf s17, w17
                     0x1e300a31,                           # fmul s17, s17, s16
                     0x52800010 | (kbits & 0xffff) << 5,   # mov w16, #lo
                     0x72a00010 | (kbits >> 16) << 5,      # movk w16, #hi, lsl #16    900 / 896
                     0x1e270210,                           # fmov s16, w16
                     0x1e300a31,                           # fmul s17, s17, s16
                     0x1e200230,                           # fcvtns w16, s17
                     0x11001210,                           # add w16, w16, #4
                     and_low_clear(16, 16, 3),             # and w16, w16, #~7         H of 896
                     0x11003e10,                           # add w16, w16, #15
                     0x53047e10,                           # lsr w16, w16, #4          tiles down, the last one whole
                     'ADRP17',                             # adrp x17, state
                     0xbd401231,                           # ldr s17, [x17, #0x10]     the render scale
                     0x1e324a31,                           # fmax s17, s17, s18        at least 1.2: the capacity's width
                     0x52800011 | 1600 << 5,               # mov w17, #1600
                     0x1e230230,                           # ucvtf s16, w17
                     0x1e310a10,                           # fmul s16, s16, s17
                     0x1e200211,                           # fcvtns w17, s16
                     0x11002231,                           # add w17, w17, #8
                     0x53047e31,                           # lsr w17, w17, #4          tiles across: W of 1600, over 16
                     0xaa108220 | rd]                      # orr xRd, x17, x16, lsl #32
        elif kind == 'U':
            body += [0xb941f270,                           # ldr w16, [x19, #0x1f0]    the capacity the game has just stored (main+0xd5796c)
                     0xb941f671,                           # ldr w17, [x19, #0x1f4]
                     0x1b117e11,                           # mul w17, w16, w17
                     0x531e7631,                           # lsl w17, w17, #2          4 words a tile
                     0x2a0003e0 | rd << 16 | rd,           # mov wRd, wRd              the low half stays: 4 bytes an entry
                     0xaa118000 | rd << 5 | rd]            # orr xRd, xRd, x17, lsl #32
        elif kind == 'C':
            body += [0xd360fc11 | rd << 5,                 # lsr x17, xRd, #32         the entries
                     0x1e230231,                           # ucvtf s17, w17
                     0x1e300a31,                           # fmul s17, s17, s16
                     0x1e200231,                           # fcvtns w17, s17
                     0x11002231,                           # add w17, w17, #8
                     and_low_clear(17, 17, 4),             # and w17, w17, #~15
                     0x2a0003e0 | rd << 16 | rd,           # mov wRd, wRd              the low half stays: 4 bytes an entry
                     0xaa118000 | rd << 5 | rd]            # orr xRd, xRd, x17, lsl #32
        else:
            if kind in 'VP': body.append(0x52807000 | rd)  # mov wRd, #896             the window's rows are the scene's largest level's
            body += [0x1e230011 | rd << 5,                 # ucvtf s17, wRd
                     0x1e300a31]                           # fmul s17, s17, s16
            if kind in 'HVP':
                body += [0x52800010 | (kbits & 0xffff) << 5,   # mov w16, #lo
                         0x72a00010 | (kbits >> 16) << 5,      # movk w16, #hi, lsl #16    900 / 896
                         0x1e270212,                           # fmov s18, w16
                         0x1e320a31]                           # fmul s17, s17, s18
            k = 4 if kind == 'W' else 3
            body += [0x1e200231,                           # fcvtns w17, s17           to the nearest
                     0x11000231 | (1 << (k - 1)) << 10,    # add w17, w17, #half a step
                     and_low_clear(17 if kind == 'P' else rd, 17, k)]
            if kind == 'P':
                body += [0x11000231 | 127 << 10,           # add w17, w17, #127
                         and_low_clear(rd, 17, 7)]         # and wRd, w17, #~127
        emit(word)                                         # the site's own instruction: the game's value
        emit(ADRP(16, here(), state))
        emit(0xb940b211 if kind in 'HVPT' else 0xb9401211)  # ldr w17, [x16, #0xb0 | #0x10]   the height scale's bits for heights, the render scale's for widths
        emit(0x52a7f010)                                   # mov w16, #0x3f800000
        emit(0x4a110210)                                   # eor w16, w16, w17
        emit(0x34000010 | (len(body) + 1) << 5)            # cbz w16, done             1.0: the game's value
        for w in body: emit(ADRP(17, here(), state) if w == 'ADRP17' else w)
        emit(B(here(), off + 4))                           # done
        ctx.put(off, [B(off, start)], {'W': 'render scale: a width times the host\'s render scale (state +0x10; 1.0 = the game\'s value)',
                                       'V': 'render scale: the window\'s height times the render scale',
                                       'H': 'render scale: a scene height times the render scale and 900/896 (896 becomes the window\'s height)',
                                       'P': 'render scale: the padded height of the window\'s buffers',
                                       'T': 'render scale: the light grid holds the tiles of the largest level',
                                       'U': 'render scale: the light grid\'s buffers have 4 words for each tile of its capacity',
                                       'C': 'render scale: the exposure\'s column buffer has an entry for every two columns of the quarter-size scene'}[kind])
    ctx.render_scale = render_scale_max
    ctx.capabilities.add('SHADOW_SCALE'); ctx.capabilities.add('MODEL_DETAIL'); ctx.capabilities.add('DYNAMIC_RESOLUTION'); ctx.capabilities.add('DRAW_DISTANCE'); ctx.capabilities.add('ASPECT_RATIO')   # Picture shape, and Interface beside it (state +0xb4)   # the launcher's Shadow quality and Model detail rows (dm/games/settings.tsv)


FRAME_STEP = {build_120: render_scale}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'render scale', 'now': 'build', 'goal': 'build', 'purpose': 'rendering',
     'why': "PlatinumGames' own engine: the sizes are 69 immediates and loads spread over ten functions of this main, with no anchor that another build could be searched "
            "by, and no other game in the repository runs this engine build",
     'checked': 'update 1.2.0 main: every site\'s instruction, and the set-up functions run in an interpreter with the stubs at scale 1.0, 1.2, 1.6 and 2.0 (2026-11-11)'},
]
