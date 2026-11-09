# The Legend of Zelda: Link's Awakening (01006BB00C6F0000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
import struct
from nso_asm import ADRP, B

CUTS = {
  # The Legend of Zelda: Link's Awakening 1.1.0 (v131072) main: the physics step (main+0x15730, main thread) rebuilds each world's PhysX completion task in place
  # (a PxLightCpuTask at world+0xc8) and passes it to simulate. A worker runs it (main+0x10ccf80: fetchResults, world+0x1d8 done = 1, notify, unlock) and then calls its
  # release (main+0x10cd070), which calls the task's own destructor: vtable back to the PxLightCpuTask base, mTm = 0 (main+0x10cced0). At 30 fps the game steps twice
  # per frame (step, wait, step, wait); the unlock hands the world's mutex to the woken main thread, and when the worker is descheduled there the main thread's
  # next step rebuilds the task before the late destructor runs, which then wipes the new task. The next simulation's NpSceneCompletion::release reads its mTm = 0:
  # crash at main+0x10ccf28 (lr main+0x13c236c; seen on the iPhone and in 2 of 5 Mac runs, in every Mac run under CPU load). The workers can run on other cores:
  # unlock does not protect their later destructor writes. Actual-instruction replay confirms the guest lifetime race. The release becomes a plain ret: the destructor only resets the vtable and mTm (no resources), the step rebuilds every field, and this
  # release is used by this one vtable (main+0x1c51308 slot 7) only
  '909e904af78ac1b8deefe97ab2ccdb51': [
    (0x10cd070, '080040f9', 'c0035fd6', 'physics completion task release (calls its own destructor): ldr x8,[x0] → ret (no longer destroyed; the step function rebuilds it in place every step)'),
  ],
}

# Screen shape (DM_SCREEN_ASPECT, loader.c: state +0x48 the output height for a width of 1920, 0 = 16:9; +0x58 the screen's width / height as a float), as for Breath of the Wild,
# Animal Crossing: New Horizons and Hollow Knight: Silksong. The game draws into its 1920x1080 window textures at the size main+0x1023720 picks for the operation mode (1280x720
# handheld, 1920x1080 docked; both stored at the renderer's +0x6e28 / +0x6e2c, the layer cropped to them by vi::SetLayerCrop) and its cameras keep a 16:9 shape of their own: the
# field camera (main+0x1025d00, its projection {near, far, aspect, fov y} at +0x1d4, made 16:9 with a 45 degree fov at main+0xc0c24 and main+0x1913c) builds its matrix from them
# (main+0x102665c, sine and cosine inline). Only the size taller (Mac, 1728x1080) squeezed the 16:9 view into it. So three stubs: the size keeps its height and takes the screen's
# width / height; the camera's aspect takes the same ratio; its fov y opens so that the width it sees stays 16:9's: tan(fov' / 2) = tan(fov / 2) x (16 / 9) / (width / height),
# more of the scene above and below. tan and atan are series (tan to h^9 for h = fov / 2; atan after halving its argument, atan t = 2 atan(t / (1 + sqrt(1 + t^2))), to u^11): under
# 1e-5 relative for a fov up to 80 degrees. x16 / x17 and the scratch registers each stub names are free where it runs. With DM_SCREEN_ASPECT unset the stubs run the original code
_size_words = [0x0b010c31, 0x6b02123f, 0x540000c1, 0xbd405a10, 0x1e230051, 0x1e310a10, 0x1e200201, 0x121f7821]   # w1 x w2 16:9: w1 = w2 x shape, even (s16 / s17)
_fov_words = [0xbd405a01, 0x5291c731, 0x72a7fc71, 0x1e270222, 0x1e211842, 0x1e2c1003, 0x1e230804, 0x1e240885,   # s0 = fov y; k = (16 / 9) / shape; h = s0 / 2 (s1..s7 free)
              0x5284f491, 0x72a79671, 0x1e270226, 0x5281ba31, 0x72a7abb1, 0x1e270227, 0x1f051cc6, 0x52911131, 0x72a7c111, 0x1e270227, 0x1f051cc6,
              0x52955571, 0x72a7d551, 0x1e270227, 0x1f051cc6, 0x1e2e1007, 0x1f051cc6, 0x1e2408c6,                    # tan h
              0x1e2208c6, 0x1e2608c4, 0x1e272884, 0x1e21c084, 0x1e272884, 0x1e2418c6, 0x1e2608c5,                    # t = k tan h; u = t / (1 + sqrt(1 + t^2))
              0x5285d191, 0x72b7b751, 0x1e270224, 0x5291c731, 0x72a7bc71, 0x1e270227, 0x1f051c84, 0x528924b1, 0x72b7c251, 0x1e270227, 0x1f051c84,
              0x529999b1, 0x72a7c991, 0x1e270227, 0x1f051c84, 0x52955571, 0x72b7d551, 0x1e270227, 0x1f051c84, 0x1e2e1007, 0x1f051c84,
              0x1e260884, 0x1e221007, 0x1e270880]                                                                      # s0 = 4 atan u = 2 atan t
_save_q1_7 = [0xd101c3ff, 0xad000be1, 0xad0113e3, 0xad021be5, 0x3d801be7]   # sub sp, sp, #0x70; stp q1, q2, [sp]; stp q3, q4, [sp, #0x20]; stp q5, q6, [sp, #0x40]; str q7, [sp, #0x60]
_load_q1_7 = [0xad400be1, 0xad4113e3, 0xad421be5, 0x3dc01be7, 0x9101c3ff]
# x8 = width | height << 32 (floats, then ints): lsr x17, x8, #32; fmov s17, w17 (ucvtf s17, w17); ldr s16, [x16, #0x58]; fmul s16, s16, s17; fcvtzs w17, s16;
# and w17, w17, #~1; (scvtf s16, w17; fmov w17, s16;) bfxil x8, x17, #0, #32
_vp_float_words = [0xd360fd11, 0x1e270231, 0xbd405a10, 0x1e310a10, 0x1e380211, 0x121f7a31, 0x1e220230, 0x1e260211, 0xb3407e28]
_vp_int_words = [0xd360fd11, 0x1e230231, 0xbd405a10, 0x1e310a10, 0x1e380211, 0x121f7a31, 0xb3407e28]
# x0 the pane, s4 the layout height, s5 720.0. A root (no parent): its Y column x 720 / the layout height. A full-screen leaf (no children, at least 1270 wide, 710 to 800 tall):
# its X and Y columns and its X and Y translation (+0x7c, +0x8c) x the layout height / 720, about the layout's centre whatever its pivot. Any other pane of a root's child list whose near edge lies at least 359 from the centre: its Y translation
# (+0x8c) out by (layout height - 720) / 2 x the root's Y scale, up or down by its sign (s0..s5, x9..x11; clang)
_pane_stretch_words = [0xbd405204, 0x52a88689, 0x1e270125, 0xf9400c09, 0xb5000069, 0x1e2418a2, 0x14000024, 0xf940140a, 0x9100800b, 0xeb0b015f, 0x54000541,
                       0x2d4a0400, 0x5298000a, 0x72a893ca, 0x1e270142, 0x1e222000, 0x5400048b, 0x5290000a, 0x72a8862a, 0x1e270142, 0x1e222020, 0x540003eb,
                       0x52a8890a, 0x1e270142, 0x1e222020, 0x5400036c, 0x1e251882, 0xbd407003, 0x1e220863, 0xbd007003, 0xbd408003, 0x1e220863, 0xbd008003,
                       0xbd409003, 0x1e220863, 0xbd009003, 0xbd407c03, 0x1e220863, 0xbd007c03, 0xbd408c03, 0x1e220863, 0xbd008c03, 0xbd407403, 0x1e220863,
                       0xbd007403, 0xbd408403, 0x1e220863, 0xbd008403, 0xbd409403, 0x1e220863, 0xbd009403, 0x1400001c, 0xf9400d2a, 0xb500034a, 0xbd403400,
                       0xbd405401, 0xbd404c02, 0x1e220821, 0x1e20c021, 0x1e2c1002, 0x1e220821, 0x1e20c003, 0x1e213863, 0x5290000a, 0x72a8766a, 0x1e270142,
                       0x1e222060, 0x5400018b, 0x1e253883, 0x1e2c1002, 0x1e220863, 0xbd408522, 0x1e220863, 0x1e214062, 0x1e202008, 0x1e22ac63, 0xbd408c02,
                       0x1e232842, 0xbd008c02]
_ldr_w17_state_h = 0xb9404a11   # ldr w17, [x16, #0x48]


def _cbz_w17(frm, to): return 0x34000000 | ((((to - frm) >> 2) & 0x7FFFF) << 5) | 17


def screen_shape(ctx):
    here, emit, site, put, STATE = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    def stub(at, orig, body, note, before=None):
        # adrp x16, state; [the original instruction]; ldr w17, [x16, #0x48]; cbz w17, out; body; out: [the original instruction, after]; b at + 4
        site(at, struct.pack('<I', orig(at)).hex())
        start = here(); out = start + 4 * (3 + (1 if before else 0) + len(body))
        emit(ADRP(16, here(), STATE))
        if before: emit(before)
        emit(_ldr_w17_state_h); emit(_cbz_w17(here(), out))
        for w in body: emit(w)
        assert here() == out, 'screen shape stub layout'
        if not before: emit(orig(out))
        emit(B(here(), at + 4))
        put(at, [B(at, start)], note)
    # the scene's render targets: height 1080 - 36 x the dynamic resolution level, width that x 16 / 9 (main+0x1023ce0, the constant read at main+0x1023d40): the screen's shape
    stub(0x1023d40, lambda pc: 0xbd4ba541, [0xbd405a01], "the scene's render targets: their width takes the screen's shape over 16:9 (DM_SCREEN_ASPECT)", before=0xbd4ba541)   # ldr s1, [x10, #0xba4]; ldr s1, [x16, #0x58]
    # the display's viewport and scissor (main+0x1019740 handheld, main+0x1019808 docked: x8 = width | height << 32, as floats into the builder's +0x28, as ints into its
    # +0x58; the objects at the renderer's +0x56b0 / +0x56b8 that the final passes apply, main+0x113ef90): width = height x shape, even, as the render targets'
    for at in (0x1019758, 0x1019814): stub(at, lambda pc: 0xf90017e8, _vp_float_words, "the display viewport: its width takes the screen's shape (DM_SCREEN_ASPECT)")   # str x8, [sp, #0x28]
    for at in (0x1019770, 0x1019828): stub(at, lambda pc: 0xf9002fe8, _vp_int_words, "the display scissor: its width takes the screen's shape (DM_SCREEN_ASPECT)")     # str x8, [sp, #0x58]
    # the 2D layouts keep the screen's shape: each layout stays 1280x720 (the game places the 3D view's picture by that rect) and its projection spans the screen, so a
    # root pane's global matrix takes 720 / the layout height in Y and everything in it keeps its shape; a full-screen picture (movies, backgrounds) takes that back in
    # both X and Y, so it fills the screen's height in its own shape and loses a little of each side; a pane at the top or bottom edge (the dialog's arrow, prompts)
    # moves out to the taller screen's edge. In nn::ui2d's pane global matrix (main+0x11674b0: x0 the pane; parent +0x18, children +0x20, size +0x50, global matrix
    # +0x70 in rows of 4 floats, the Y column +0x74, +0x84, +0x94), at its three returns after the store (main+0x1167694, main+0x1167854, main+0x1167900)
    for at in (0x1167694, 0x1167854, 0x1167900):
        stub(at, lambda pc: 0xd65f03c0, _pane_stretch_words, "the 2D layouts keep their shape on the taller screen, full-screen pictures fill it (DM_SCREEN_ASPECT)")   # ret
    # the size: adrp x8, main+0x1cc8000 there, made again where the stub runs
    stub(0x102376c, lambda pc: ADRP(8, pc, 0x1cc8000), _size_words, "the render size: a 16:9 one keeps its height and takes the screen's shape (DM_SCREEN_ASPECT)")
    # the field camera's fov y and aspect where its controller writes them each frame (main+0x10918 from +0x564 or a constant, main+0x10930 16:9 or +0x570): several
    # projections read them (the camera's own at main+0x102665c, the renderer's at main+0x487ef8, main+0x94b470, main+0x94e83c, main+0xb19a70), so the values change, not
    # the readers. The fov stub keeps q1..q7 (live vector registers there); s1 and s0 are loaded again after the aspect store
    stub(0x10918, lambda pc: 0xbd01e100, _save_q1_7 + _fov_words + _load_q1_7, "the field camera's fov y: opened so that its width stays 16:9's on the screen's shape (DM_SCREEN_ASPECT)")   # str s0, [x8, #0x1e0]
    stub(0x10930, lambda pc: 0xbd01dd00, [0xbd405e01, 0x1e211800], "the field camera's aspect: / (16 / 9) x the screen's shape (DM_SCREEN_ASPECT)")   # str s0, [x8, #0x1dc]; ldr s1, [x16, #0x5c]; fdiv s0, s0, s1
    # the culling frustum (main+0x112f690: fov y, aspect, near, far in s0..s3, the view matrix at x1), built at main+0x852cd8 from the controller's own fov y and aspect (+0x564,
    # +0x570, 16:9), not the camera's: its top and bottom stopped at 16:9's, and ground pieces in the taller view's extra band were culled (black until walked into). The same
    # fov y and aspect as the camera's: s0 opened as above (q1..q7 kept), s1 / (16 / 9) x the shape (s4 free before the call)
    stub(0x852cd4, lambda pc: 0x4eab1d63, _save_q1_7 + _fov_words + _load_q1_7 + [0xbd405e04, 0x1e241821], "the culling frustum: the field camera's fov y and aspect on the screen's shape (DM_SCREEN_ASPECT)", before=0x4eab1d63)   # mov v3.16b, v11.16b; ...; ldr s4, [x16, #0x5c]; fdiv s1, s1, s4

FRAME_STEP = {
    '909e904af78ac1b8deefe97ab2ccdb51': screen_shape,   # 1.1.0 (v131072) main
}


# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'PhysX completion task release', 'now': 'build', 'goal': 'game', 'purpose': 'race',
     'why': "a lifetime race in the game's own completion task (its release slot serves this one vtable)"},
    {'what': "screen shape: the render size and targets, the display viewport, the field camera's aspect and fov y, its culling frustum, and the 2D layouts' panes (DM_SCREEN_ASPECT)", 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "the sizes and the camera's projection are this game's own code; the pane rule (which panes are full-screen pictures) is this game's layouts'",
     'blocked': 'the stubs have no function-matched form yet'},
]
