# LEGO Harry Potter Collection (010052A00B5D2000): the rewriter's patches of this game's own modules. dm/tools/patch_nso.py
# applies those of a module's NSO build ID — CUTS, OBS, FRAME_STEP and STUBS as described there ("Registered patches").
# Two stubs, both on libharry2.nro (Years 5-7, build 7e19fa8490f09d9d2a71f74f5903211d): publish_record (the crash on
# selecting Years 5-7) and blit_guard (the glyph/atlas lattice). PATCHES.md has the sites, the bytes and the rebuild.
from nso_asm import B, ADRP

# ---- instruction words the shared encoders (nso_asm) do not carry: this stub needs a 32-bit load, a compare-and-branch and a call
def _ldrw(t, n, off):                       # ldr wt,[xn,#off]
    return 0xB9400000 | ((off // 4) << 10) | (n << 5) | t
def _cbz(t, pc, tgt):                       # cbz wt, tgt
    off = (tgt - pc) >> 2
    assert -0x40000 <= off < 0x40000, f'cbz reach {off:#x}'
    return 0x34000000 | ((off & 0x7FFFF) << 5) | t
def _beq(pc, tgt):                          # b.eq tgt
    off = (tgt - pc) >> 2
    assert -0x40000 <= off < 0x40000, f'b.eq reach {off:#x}'
    return 0x54000000 | ((off & 0x7FFFF) << 5)
def _bl(pc, tgt):                           # bl tgt
    off = (tgt - pc) >> 2
    assert -0x8000000 <= off < 0x8000000, f'bl reach {off:#x}'
    return 0x94000000 | (off & 0x3FFFFFF)

# libharry2.nro (Years 5-7, build 7e19fa8490f09d9d2a71f74f5903211d): the pass table's publish, moved to record time.
#
# The recorder nro0+0xd2ff0 writes one pass state block per call into the renderer's *live* table (singleton+0x9d0, indexed by the slot it returns, 0xd3044) and the
# begin-frame routine nro0+0x15f420 copies those 24 pointers into the *published* table (singleton+0xa90, 0x15f4b8..0x15f57c) that every replay reads: begin_pass
# nro0+0xd39c0 and the eleven other sites (0x128668 0x12926c 0x12a234 0x12a93c 0x153f80 0x15446c 0x154dec 0x155648 0x155ac4 0x1562ec 0x156a5c 0x15710c) index it by the
# pass id the recorder returned, and they replay in the same frame the record is made. The two run on different threads under two locks (0x11541a8 and 0x11541c0) that
# do not order them: in the first failing frame the worker held the frame lock and sat in the pass loop while the main thread was still at 0x15f458, the lock acquire
# at the top of 0x15f420, so the replay read a table that had never been published — NULL at 0xd3a2c, and one site further on a record whose memory the frame's bump
# allocator (x26) had already reused (the two 1.0f at 0xd3adc). The records live only for their frame, so the publish cannot wait for a routine that runs a whole
# frame phase later: it has to happen when the record is made.
#
# The site is the recorder's own increment, "add w8, w0, #1" at 0xd3138: x19 is the singleton, w0 the slot the recorder is about to return (loaded from
# singleton+0x9c8 at 0xd3134), and x8, x9 and x10 are dead there — the epilogue that follows restores x19-x30 from the stack and w0 is the result. The stub stores
# live[slot] into published[slot] before it increments the slot count, then returns to 0xd3140 (the epilogue), so the caller still gets the original slot id. The whole
# double buffer is kept: live stays the recorder's target and published stays what the replays read; only the one-frame lag the copy imposed is removed, and a
# slot that is not recorded in a frame keeps the published pointer it had.
def publish_record(ctx):
    here, emit, site, put = ctx.here, ctx.emit, ctx.site, ctx.put
    site(0xd3138, '08040011')                        # add w8, w0, #1
    start = here()
    for w in (0x11000408,   # add w8, w0, #1                  the original increment
              0x91274269,   # add x9, x19, #0x9d0             &live[slot]
              0xf860d92a,   # ldr x10, [x9, w0, sxtw #3]      live[slot], stored at 0xd3044
              0x912a4269,   # add x9, x19, #0xa90             &published[slot]
              0xf820d92a,   # str x10, [x9, w0, sxtw #3]      published[slot] = live[slot]
              0xb909ca68):  # str w8, [x19, #0x9c8]           ++slot count (the original 0xd313c)
        emit(w)
    emit(B(here(), 0xd3140))
    put(0xd3138, [B(0xd3138, start)],
        'pass-state recorder: publish live[slot] into published[slot] at record time, before the slot count is incremented (the replay reads the record in the same '
        'frame, and the records die with the frame; without it the published table is only filled by 0x15f420, which the worker\'s pass loop can outrun)')

# libharry2.nro (Years 5-7): the whole-texture blit that the HUD entity appends into the *glyph* run — the glyph/atlas lattice.
#
# What the entity does (nro0+0x6d9810, one caller: the draw method's "bl #0x6d9810" at 0x6d9f64): it emits a quad whose uv is the unit square by construction
# (0x105314 "mov x10,#0x3f8000003f800000") over a constant screen rect (y from 0x3e924924 = 2/7 to 0x3f36db6e = 5/7 mapped by ctx+0x78..0x84, i.e. the lower
# 5/7 of the screen), with its own texture handle [0x11b0bb8] as the emitter's texture argument, through the ordinary UI vertex emitter 0x1053b0. The emitter
# *begins* the batch it writes into (0xedb80(4, 0x25, handle, 2, 0) at 0x10541c) and every four vertices calls the append 0x15d720 with batch 0 — and batch 0 is
# the run the HUD's glyph quads are in. A whole-texture blit therefore lands in a run whose program is the SDF text shader and whose texture is the 4096x2048
# font atlas: the atlas is painted over the lower 5/7 of the frame for one presented frame (orch/dither, orch/emit, orch/state; 497 of 25,608 in-level present
# rows, 23.0 per 1,000 presents, 118.2 per 1,000 moving presents, 495 of 497 runs of length 1).
#
# The condition is texture identity, not geometry: the batch descriptor at 0x11539c8 (0x88 bytes, batch 0; [0x11539c8+0x10] vertices in the open run,
# [0x11539c8+0x30] the handle its last begin recorded) is compared with the blit's own handle [0x11b0bb8]. Different handle and a non-empty run means this
# quad would be drawn by another element's texture — precisely the defect — so the blit is skipped. Equal (or an empty run, where nothing can be contaminated
# and the blit's own begin takes effect first) keeps the original call. Nothing is deleted: 0x6d9810 and its emitter are untouched and still run whenever the
# batch is theirs. x8/x9/x10 are dead at the site (the code resumes at 0x6d9f68 with "ldr x0,[x19,#8]", and x19 is callee-saved by the blit), so the guard
# clobbers nothing live; the site's own "mov x0,x19" at 0x6d9f60 stays and the stub repeats it.
#
# In this corpus the guard always takes the skip branch: the entity's handle 0x7a056f30 is never the handle recorded by the glyph run (the draw's atlas is
# st0 slot 6 id 70095), so every instance of the blit today is drawn with the atlas — its intended visual never renders on this path, and the A/B in
# orch/guestpatch/REPORT.md measures what removing it does to the frame.
def blit_guard(ctx):                             # 0x6d9f64 bl #0x6d9810 -> the guard below
    here, emit, site, put = ctx.here, ctx.emit, ctx.site, ctx.put
    site(0x6d9f64, '2bfeff97')                   # bl #0x6d9810
    s = here()
    emit_at, skip_at = s + 40, s + 48            # the blit, and the instruction after the call site
    words = (
        ADRP(8, s, 0x1153000),                   # adrp x8,#0x1153000
        0x91272108,                              # add  x8,x8,#0x9c8        &ui_batch[0]
        _ldrw(9, 8, 0x10),                       # ldr  w9,[x8,#0x10]      vertices in the open run
        _cbz(9, s + 12, emit_at),                # cbz  w9,emit            empty: the blit starts its own run
        0xf9401909,                              # ldr  x9,[x8,#0x30]      the run's texture handle
        ADRP(10, s + 20, 0x11b0000),             # adrp x10,#0x11b0000
        0xf945dd4a,                              # ldr  x10,[x10,#0xbb8]   the blit's own handle
        0xeb0a013f,                              # cmp  x9,x10
        _beq(s + 32, emit_at),                   # b.eq emit               same texture: it belongs here
        B(s + 36, skip_at),                      # b    skip               another element's run: do not append
        0xaa1303e0,                              # mov  x0,x19             (the original argument)
        _bl(s + 44, 0x6d9810),                   # bl   #0x6d9810
        B(s + 48, 0x6d9f68),                     # b    0x6d9f68           (skip lands here too)
    )
    for w in words:
        emit(w)
    put(0x6d9f64, [B(0x6d9f64, s)],
        'whole-texture blit (nro0+0x6d9810, sole caller) appended into the glyph run: skip it when the open UI run carries another texture — the unit-uv quad '
        'would otherwise be drawn by the font atlas and paint the 4096x2048 sheet over the lower 5/7 of the frame for one present')

def stubs(ctx):
    publish_record(ctx)
    blit_guard(ctx)

STUBS = {'7e19fa8490f09d9d2a71f74f5903211d': stubs}

# Whether a general rule could replace these patches (dm/tools/docs_patches.py lists it in docs/agents/game-patches.md)
GENERALIZE = ("No: both stubs are this build's own renderer — its pass tables, its UI batch descriptor and its vtable-keyed entity draw. The condition blit_guard "
              "tests (a whole-texture blit appended into a run another entity's texture already opened) is the guest's own batch state, which the host does not "
              "model; the only general form is the emitter refusing to append a unit-uv quad into a non-empty run, which would change the behaviour of every game "
              "that draws through its own UI emitter")
