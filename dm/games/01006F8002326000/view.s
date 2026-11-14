// Owned adaptive-view routines for ACNH main ff1d1c05670db6021c85b624a710b963.
// The projection/scissor entry-point lead comes from the MIT-0 ACNH 3.0.X
// Ultrawide project. Vertical coverage and dynamic cache checks are local.
// State (the frame step page): +0x58 target aspect (0 = 16:9), +0xa0 layout match,
// +0xa8/+0xb0/+0xb8 native UI frame stamps. No host-managed pointer is needed.
.text
.p2align 2
.globl _dm_view_matrix
_dm_view_matrix:
    ldp s1, s2, [x0, #0xac]
    adrp x15, _dm_view_state@PAGE
    add x15, x15, _dm_view_state@PAGEOFF
    ldr w15, [x15, #0x58]
    cbz w15, Lmatrix_done
    ldr s6, Lbase
    fcmp s2, s6
    b.ne Lmatrix_done
    fmov s7, w15
    fdiv s6, s6, s7
    fmov s3, #1.0
    fmax s6, s6, s3
    fmul s1, s1, s6
    fmov s2, s7
    fmov s3, #0.5
Lmatrix_done:
    b _dm_return_037e5978

// A cached projection must follow target changes even when the game has not
// changed its own camera parameters. The original dirty byte is returned for
// every other object. No padding or lifetime-sensitive pointers are written.
.globl _dm_view_cache
_dm_view_cache:
    ldrb w8, [x0, #8]
    cbnz w8, Lcache_return
    adrp x15, _dm_view_state@PAGE
    add x15, x15, _dm_view_state@PAGEOFF
    ldr w15, [x15, #0x58]
    cbz w15, Lcache_return
    stp q0, q1, [sp, #-64]!
    stp q2, q3, [sp, #32]
    mrs x15, nzcv
    str x15, [sp, #-16]!
    ldr x15, [x0]
    adrp x16, _dm_code_04a65608@PAGE
    add x16, x16, _dm_code_04a65608@PAGEOFF
    cmp x15, x16
    b.ne Lcache_done
Lcache_perspective:
    ldr s0, [x0, #0xb0]
Lcache_expected:
    ldr s1, Lbase
    fcmp s0, s1
    b.ne Lcache_done
    adrp x15, _dm_view_state@PAGE
    add x15, x15, _dm_view_state@PAGEOFF
    ldr w15, [x15, #0x58]
    cbz w15, Lcache_compare
    fmov s0, w15
Lcache_compare:
    ldr s1, [x0, #0xc]
    ldr s2, [x0, #0x20]
    fdiv s1, s2, s1
    fabs s1, s1
    fsub s1, s1, s0
    fabs s1, s1
    ldr s2, Lepsilon
    fmul s2, s0, s2
    fcmp s1, s2
    cset w8, hi
Lcache_done:
    ldr x15, [sp], #16
    msr nzcv, x15
    ldp q2, q3, [sp, #32]
    ldp q0, q1, [sp], #64
Lcache_return:
    ret


// UI jobs can bypass Projection::getMatrix and return a retained override.
// This getter belongs to the screen UI manager. Recognize its verified 40-degree
// lens and 1..10000 depth transform, keeping square/local and off-axis captures
// untouched. Narrow screens preserve X; wide screens preserve Y. Both values
// are derived from the canonical lens, including restoration to target zero.
.globl _dm_view_ui_override
_dm_view_ui_override:
    add x0, x8, #8
    stp x8, x30, [sp, #-16]!
    bl Lscreen_projection
    ldp x8, x30, [sp], #16
    ret
// x0 = screen projection, w8 = accepted. No retained matrix is a scale source.
Lscreen_projection:
    mov w8, wzr
    adrp x15, _dm_view_state@PAGE
    add x15, x15, _dm_view_state@PAGEOFF
    ldr w16, [x15, #0x58]
    cbz w16, Lui_return
    ldr w16, [x0, #0x3c]
    cbnz w16, Lui_return
    ldr w16, [x0, #0x38]
    mov w14, #0xbf800000
    cmp w16, w14
    b.ne Lui_return
    ldr w16, [x0, #0x28]
    ldr w14, Lui_depth_z
    cmp w16, w14
    b.ne Lui_return
    ldr w16, [x0, #0x2c]
    ldr w14, Lui_depth_w
    cmp w16, w14
    b.ne Lui_return
    ldr w16, [x0, #8]
    ldr w14, [x0, #0x18]
    orr w16, w16, w14
    cbnz w16, Lui_return
    ldr s0, [x0]
    ldr s1, [x0, #0x14]
    ldr s2, Lui_x
    ldr s3, Lbase
    ldr s4, Lepsilon
    fsub s2, s0, s2
    fabs s2, s2
    fcmp s2, s4
    b.ls Lui_apply
    // A wider initial target retains the canonical vertical coefficient.
    fcmp s0, #0.0
    b.le Lui_return
    ldr s2, Lui_x
    fcmp s0, s2
    b.hi Lui_return
    fmul s2, s2, s3
    fsub s2, s1, s2
    fabs s2, s2
    fcmp s2, s4
    b.hi Lui_return
    fdiv s0, s1, s3
Lui_apply:
    ldr w16, [x15, #0x58]
    cbz w16, Lui_original
    fmov s3, w16
Lui_original:
    ldr s0, Lui_x
    ldr s2, Lbase
    fdiv s2, s2, s3
    fmov s4, #1.0
    fmin s2, s2, s4
    fmul s0, s0, s2
    fmul s1, s0, s3
    str s0, [x0]
    str s1, [x0, #0x14]
    mov w8, #1
Lui_return:
    ret
Lui_x: .float 1.5454561710357666
Lui_depth_z: .float -1.0002000331878662
Lui_depth_w: .float -2.000200033187866

// Scissor conversion follows the current perspective matrix. Capture-local
// orthographic projections retain their native coordinates. Preserve NZCV.
.globl _dm_view_scissor
_dm_view_scissor:
    fdiv s3, s0, s2
    fdiv s2, s1, s4
    stp x15, x16, [sp, #-16]!
    adrp x15, _dm_view_state@PAGE
    add x15, x15, _dm_view_state@PAGEOFF
    ldr w15, [x15, #0x58]
    cbz w15, Lscissor_done
    ldr w16, [x19, #0x4c]
    cbnz w16, Lscissor_done
    stp q5, q6, [sp, #-48]!
    str q7, [sp, #32]
    // Match the actual projection to this layout's reference rectangle.
    ldr s5, [x19, #0x24]
    ldr s6, [x19, #0x10]
    fdiv s5, s5, s6
    fabs s5, s5
    ldr x15, [x19, #0xc0]
    ldp s6, s7, [x15, #0x28]
    fdiv s6, s6, s7
    fdiv s5, s5, s6
    // A target narrower than stock extends Y; a wider target extends X.
    fmov s6, #1.0
    fmin s6, s6, s5
    fmul s2, s2, s6
    fmov s6, #1.0
    fdiv s5, s6, s5
    fmin s5, s5, s6
    fmul s3, s3, s5
    ldr q7, [sp, #32]
    ldp q5, q6, [sp], #48
Lscissor_done:
    ldp x15, x16, [sp], #16
    b _dm_return_03a308f0

.globl _dm_view_capture
_dm_view_capture:
    fdiv s2, s9, s2
    stp x15, x16, [sp, #-16]!
    adrp x15, _dm_view_state@PAGE
    add x15, x15, _dm_view_state@PAGEOFF
    ldr w15, [x15, #0x58]
    cbz w15, Lcapture_done
    ldr w16, [x21, #0x4c]
    cbnz w16, Lcapture_done
    stp q4, q5, [sp, #-48]!
    str q6, [sp, #32]
    ldr s4, [x21, #0x24]
    ldr s5, [x21, #0x10]
    fdiv s4, s4, s5
    fabs s4, s4
    ldr x15, [x21, #0xc0]
    ldp s5, s6, [x15, #0x28]
    fdiv s5, s5, s6
    fdiv s4, s4, s5
    fmov s5, #1.0
    fmin s5, s5, s4
    fmul s3, s3, s5
    fmov s5, #1.0
    fdiv s4, s5, s4
    fmin s4, s4, s5
    fmul s2, s2, s4
    ldr q6, [sp, #32]
    ldp q4, q5, [sp], #48
Lcapture_done:
    ldp x15, x16, [sp], #16
    b _dm_return_03a2eac0

// Field rectangles select which terrain, trees and items are instantiated.
// Extend depth as well as width; the margin covers the five-unit cell grid.
.globl _dm_view_create
_dm_view_create:
    str x15, [sp, #-16]!
    adrp x15, _dm_view_state@PAGE
    add x15, x15, _dm_view_state@PAGEOFF
    ldr w15, [x15, #0x58]
    cbz w15, Lcreate_done
    stp q2, q3, [sp, #-32]!
    fmov s2, w15
    ldr s1, Lbase
    fdiv s0, s1, s2
    fdiv s1, s2, s1
    fmov s2, #1.0
    ldr s3, Lmargin
    fmul s0, s0, s3
    fmax s0, s0, s2
    fmul s1, s1, s3
    fmax s1, s1, s2
    fmul s8, s8, s0
    fmul s12, s12, s0
    fmul s9, s9, s1
    fmul s13, s13, s1
    ldr s2, [sp, #0x48]
    fmul s2, s2, s1
    str s2, [sp, #0x48]
    ldp q2, q3, [sp], #32
Lcreate_done:
    ldr x15, [sp], #16
    fsub s0, s10, s9
    b _dm_return_013a29a8

.globl _dm_view_area
_dm_view_area:
    str x15, [sp, #-32]!
    str q7, [sp, #16]
    adrp x15, _dm_view_state@PAGE
    add x15, x15, _dm_view_state@PAGEOFF
    ldr w15, [x15, #0x58]
    cbz w15, Larea_done
    stp q1, q2, [sp, #-32]!
    fmov s2, w15
    ldr s7, Lbase
    fdiv s0, s7, s2
    fdiv s1, s2, s7
    ldr s7, Lmargin
    fmul s0, s0, s7
    fmul s1, s1, s7
    fmov s7, #1.0
    fmax s0, s0, s7
    fmax s1, s1, s7
    fmul s10, s10, s0
    fmul s11, s11, s0
    fmul s8, s8, s1
    fmul s9, s9, s1
    ldp q1, q2, [sp], #32
Larea_done:
    ldr q7, [sp, #16]
    ldr x15, [sp], #32
    ldr s0, [x20]
    ret

.globl _dm_view_units
_dm_view_units:
    ldr w8, [x0]
    str x15, [sp, #-16]!
    adrp x15, _dm_view_state@PAGE
    add x15, x15, _dm_view_state@PAGEOFF
    ldr w15, [x15, #0x58]
    cbz w15, Lunits_done
    lsl w8, w8, #1
Lunits_done:
    ldr x15, [sp], #16
    b _dm_return_01462bb8


// Near-plane unprojection follows the adapted matrix without mutating camera parameters.
.globl _dm_view_near_y
_dm_view_near_y:
    ldr s1, [x0, #0xac]
    adrp x15, _dm_view_state@PAGE
    add x15, x15, _dm_view_state@PAGEOFF
    ldr w15, [x15, #0x58]
    cbz w15, Lnear_y_done
    ldr s6, Lbase
    ldr s7, [x0, #0xb0]
    fcmp s6, s7
    b.ne Lnear_y_done
    fmov s7, w15
    fdiv s6, s6, s7
    fmov s16, #1.0
    fmax s6, s6, s16
    fmul s1, s1, s6
Lnear_y_done:
    b _dm_return_037e5a50

.globl _dm_view_near_xy
_dm_view_near_xy:
    ldp s2, s3, [x0, #0xac]
    adrp x15, _dm_view_state@PAGE
    add x15, x15, _dm_view_state@PAGEOFF
    ldr w15, [x15, #0x58]
    cbz w15, Lnear_xy_done
    ldr s6, Lbase
    fcmp s6, s3
    b.ne Lnear_xy_done
    fmov s7, w15
    fdiv s6, s6, s7
    fmov s16, #1.0
    fmax s6, s6, s16
    fmul s2, s2, s6
    fmov s3, s7
Lnear_xy_done:
    b _dm_return_037e5a80

.globl _dm_view_sphere
_dm_view_sphere:
    str x15, [sp, #-16]!
    adrp x15, _dm_view_state@PAGE
    add x15, x15, _dm_view_state@PAGEOFF
    ldr w15, [x15, #0x58]
    cbz w15, Lsphere_original
    fmul s2, s0, s0
    fmadd s2, s1, s1, s2
    fmadd s2, s13, s13, s2
    fsqrt s2, s2
    fmul s0, s2, s9
    b Lsphere_done
Lsphere_original:
    fcmp s0, s1
    fcsel s0, s0, s1, gt
    fcmp s13, s0
    fcsel s0, s13, s0, gt
    fmul s0, s0, s9
Lsphere_done:
    ldr x15, [sp], #16
    b _dm_return_013a37c8

.p2align 2
Lbase: .float 1.7777777777777777778
Lmin_width: .float 1280.0
Lmargin: .float 1.1
Lepsilon: .float 0.00001

// The minimap captures already clip at their own render-target edges. Their
// extra screen-space ScissorOn pane was authored for a 16:9 projection and
// trims the captured terrain after changing that projection. Remove only
// that redundant registration, on two exact whole-resource fingerprints.
// Resource bytes remain in the game's writable load buffer; disk is untouched.
.p2align 2
.globl _dm_view_layout
_dm_view_layout:
    stp x9, x10, [sp, #-80]!
    stp x11, x12, [sp, #16]
    stp x13, x14, [sp, #32]
    stp x15, x16, [sp, #48]
    str x17, [sp, #64]
    mrs x17, nzcv
    adrp x15, _dm_view_state@PAGE
    add x15, x15, _dm_view_state@PAGEOFF
    ldr w16, [x15, #0x58]
    cbz w16, Llayout_done
    ldr w9, [x21]
    ldr w10, Lflyt_magic
    cmp w9, w10
    b.ne Llayout_done
    ldr w9, [x21, #0xc]
    mov w10, #14012
    cmp w9, w10
    b.eq Llayout_minimap
    mov w10, #11108
    cmp w9, w10
    b.ne Llayout_done
    ldr x13, Lcloud_map_hash
    mov x14, #3292
    b Llayout_hash
Llayout_minimap:
    ldr x13, Lmap_hash
    mov x14, #2908
Llayout_hash:
    mov x10, x21
    ldr x11, Lfnv_offset
    ldr x12, Lfnv_prime
Llayout_byte:
    ldrb w8, [x10], #1
    eor x11, x11, x8
    mul x11, x11, x12
    sub w9, w9, #1
    cbnz w9, Llayout_byte
    cmp x11, x13
    b.ne Llayout_done
    str wzr, [x21, x14]
    mov w16, #1
    str w16, [x15, #0xa0]
Llayout_done:
    msr nzcv, x17
    ldr x17, [sp, #64]
    ldp x15, x16, [sp, #48]
    ldp x13, x14, [sp, #32]
    ldp x11, x12, [sp, #16]
    ldp x9, x10, [sp], #80
    ldrh w8, [x21, #6]
    b _dm_return_00268ee4
.p2align 3
Lmap_hash: .quad 0x1e453d087f41e3bf
Lcloud_map_hash: .quad 0xb7299d38fd75c925
Lfnv_offset: .quad 0xcbf29ce484222325
Lfnv_prime: .quad 0x100000001b3
Lflyt_magic: .word 0x54594c46

// Mark only actual native UI render jobs. No retained game-object pointers are
// exposed to the host; old stamps expire after two original presents.
.p2align 2
.globl _dm_view_keyboard_frame
_dm_view_keyboard_frame:
    stp x15, x16, [sp, #-32]!
    stp x17, x30, [sp, #16]
    mov x17, #32
    bl Ltouch_stamp
    ldp x17, x30, [sp, #16]
    ldp x15, x16, [sp], #32
    sub sp, sp, #0xb0
    b _dm_return_03268180

.globl _dm_view_touch_page
_dm_view_touch_page:
    stp x14, x15, [sp, #-48]!
    stp x16, x17, [sp, #16]
    stp x13, x30, [sp, #32]
    mrs x13, nzcv
    ldr x14, [x0]
    adrp x16, _dm_code_049adc80@PAGE
    add x16, x16, _dm_code_049adc80@PAGEOFF
    cmp x14, x16
    b.ne Ltouch_page_done
    mov x17, #40
    bl Ltouch_stamp
Ltouch_page_done:
    msr nzcv, x13
    ldp x13, x30, [sp, #32]
    ldp x16, x17, [sp, #16]
    ldp x14, x15, [sp], #48
    ret
Ltouch_stamp:
    adrp x15, _dm_view_state@PAGE
    add x15, x15, _dm_view_state@PAGEOFF
    ldr x16, [x15, #0xa8]
    add x16, x16, #1
    str x16, [x15, #0xa8]
    add x15, x15, x17
    stlr x16, [x15]
Ltouch_stamp_done:
    ret

// Only the keyboard docks the whole UI projection. Drawing tools retain
// their original centred composition; KeyGuide has its own lower-edge anchor.
// x15 = state, w8 = recently rendered inline keyboard.
Lrecent_keyboard:
    ldr x16, [x15, #0xa8]
    mov w8, wzr
    ldr x17, [x15, #0xb0]
    cbz x17, Lrecent_done
    sub x17, x16, x17
    cmp x17, #2
    b.hi Lrecent_done
    mov w8, #1
Lrecent_done:
    ret

// DrawInfo owns a fresh copy of the source projection at this boundary. Dock
// native touch panels to the bottom without moving the 3D camera or accumulating
// translation in a retained source matrix. Local captures keep their projection.
.globl _dm_view_ui_projection
_dm_view_ui_projection:
    stp x0, x30, [sp, #-16]!
    add x0, x0, #0x10
    bl _dm_view_panel_projection
    ldp x0, x30, [sp], #16
    ret
.globl _dm_view_panel_projection
_dm_view_panel_projection:
    stp x15, x30, [sp, #-16]!
    bl Lscreen_projection
    cbz w8, Lpanel_done
    ldr s1, Lbase
    ldr w16, [x15, #0x58]
    cbz w16, Lpanel_ratio
    fmov s1, w16
Lpanel_ratio:
    ldr s2, Lbase
    fcmp s1, s2
    b.ge Lpanel_done
    bl Lrecent_keyboard
    cbz w8, Lpanel_done
    fdiv s1, s1, s2
    fmov s2, #1.0
    fsub s1, s2, s1
    ldr s3, [x0, #0x18]      ; the y row element after the upstream stretch division
    ldr s4, [x16, #0x84]     ; dm_fps_state +0x84: the vertical stretch
    fdiv s1, s1, s4          ; normalise the dock offset to the same scale
    fadd s1, s3, s1          ; option B: additive instead of overwriting index 6
    str s1, [x0, #0x18]
; Option A (drop the fdiv line) is the fallback if a device run shows the dock
; direction is inverted. Both are open until checked on a device.
Lpanel_done:
    ldp x15, x30, [sp], #16
    ret

// Scissor rectangles use the layout's centre rather than the projection's
// off-axis term. Apply that same bottom-docking translation to their origin.
.globl _dm_view_scissor_origin
_dm_view_scissor_origin:
    ldp s5, s6, [x8, #8]
    stp x14, x15, [sp, #-32]!
    stp x16, x17, [sp, #16]
    stp q0, q7, [sp, #-48]!
    str q16, [sp, #32]
    mrs x17, nzcv
    adrp x15, _dm_view_state@PAGE
    add x15, x15, _dm_view_state@PAGEOFF
    ldr w16, [x15, #0x58]
    cbz w16, Lscissor_origin_done
    ldr w16, [x19, #0x4c]
    cbnz w16, Lscissor_origin_done
    ldr s7, [x19, #0x10]
    ldr s16, [x19, #0x24]
    fdiv s16, s16, s7
    ldr s7, Lbase
    fdiv s16, s16, s7
    fmov s7, #1.0
    fsub s16, s7, s16
    ldr s7, [x19, #0x28]
    fcmp s7, #0.0
    b.le Lscissor_origin_done
    fsub s0, s7, s16
    fabs s0, s0
    ldr s16, Lepsilon
    fcmp s0, s16
    b.hi Lscissor_origin_done
    fmov s16, #0.5
    fmul s7, s7, s6
    fmul s7, s7, s16
    fsub s2, s2, s7
Lscissor_origin_done:
    msr nzcv, x17
    ldr q16, [sp, #32]
    ldp q0, q7, [sp], #48
    ldp x16, x17, [sp, #16]
    ldp x14, x15, [sp], #32
    b _dm_return_03a30988

// Background and iris coverage constants are private to this draw; the pane,
// layout and resource are untouched. Only full-screen centred backgrounds
// expand their repeating UVs with the quad, retaining the pattern density.
.globl _dm_view_backdrop_call
_dm_view_backdrop_call:
    stp x1, x30, [sp, #-16]!
    bl _dm_code_00264a10
    ldr x2, [sp]
    mov x0, x19
    mov x1, x20
    bl _dm_view_backdrop
    ldr x30, [sp, #8]
    add sp, sp, #16
    ret
.globl _dm_view_backdrop
_dm_view_backdrop:
    str x30, [sp, #-16]!
    mov w3, wzr
    bl _dm_view_transition
    ldr x30, [sp], #16
    cbnz w8, Lbackdrop_done
    adrp x15, _dm_view_state@PAGE
    add x15, x15, _dm_view_state@PAGEOFF
    ldr w16, [x15, #0x58]
    cbz w16, Lbackdrop_done
    ldr w16, [x15, #0x58]
    cbz w16, Lbackdrop_done
    // WipeCircle's animated side curtains use reference-screen coverage.
    // Preserve their aperture edge while extending only the outer coverage.
    ldr x16, [x0, #0xb0]
    and x16, x16, #0xfffffeffffffffff
    ldr x14, Lwipe_left_name
    cmp x16, x14
    b.eq Lwipe_side
    ldr x14, Lwipe_right_name
    cmp x16, x14
    b.eq Lwipe_side
    ldrb w16, [x0, #0x5b]
    // Full-screen bases can have top/bottom origins; their fresh quad centre
    // still identifies the same coverage. Keep horizontal off-centre panes out.
    and w14, w16, #15
    cmp w14, #8
    b.hi Lbackdrop_done
    and w16, w16, #3
    cbnz w16, Lbackdrop_done
    ldr w16, [x0, #0xb0]
    ldr w14, Lbg_name
    cmp w16, w14
    b.eq Lbackdrop_size
    ldr w14, Lbg_lower_name
    cmp w16, w14
    b.eq Lbackdrop_size
    ldr w14, Ldot_name
    cmp w16, w14
    b.eq Lbackdrop_size
    ldr w14, Lcolor_bg_name
    cmp w16, w14
    b.eq Lbackdrop_size
    ldr w14, Lall_bg_name
    cmp w16, w14
    b.eq Lbackdrop_size
    ldr w14, Lstripe_name
    cmp w16, w14
    b.eq Lbackdrop_size
    ldr x16, [x0, #0xb0]
    ldr x14, Ltexture_bg_name
    cmp x16, x14
    b.eq Lbackdrop_texture_suffix
    ldr x14, Lbase_bg_name
    cmp x16, x14
    b.eq Lbackdrop_suffix
    // Fade and VideogameFade use a separate full-screen picture. Its alpha
    // animation must cover the same expanded viewport as the scene beneath it.
    ldr x14, Lfade_bg_name
    cmp x16, x14
    b.ne Lbackdrop_done
Lbackdrop_suffix:
    ldrh w16, [x0, #0xb8]
    cmp w16, #0x30
    b.ne Lbackdrop_done
    b Lbackdrop_size
Lbackdrop_texture_suffix:
    ldrb w16, [x0, #0xb8]
    cbnz w16, Lbackdrop_done
Lbackdrop_size:
    mov w11, wzr
    ldp s0, s1, [x0, #0x50]
    fcmp s0, #0.0
    b.le Lbackdrop_done
    fcmp s1, #0.0
    b.le Lbackdrop_done
    ldr x16, [x1, #0xc0]
    cbz x16, Lbackdrop_done
    ldp s2, s3, [x16, #0x28]
    fcmp s0, s2
    b.ne Lbackdrop_done
    fcmp s1, s3
    b.ne Lbackdrop_done
    ldr s4, [x2, #0x210]
    ldr s5, [x2, #0x214]
    fcmp s4, s0
    b.ne Lbackdrop_done
    fcmp s5, s1
    b.ne Lbackdrop_done
Lbackdrop_projection:
    ldr w16, [x1, #0x4c]
    cbnz w16, Lbackdrop_done
    ldr s2, [x1, #0x24]
    ldr s3, [x1, #0x10]
    fcmp s3, #0.0
    b.le Lbackdrop_done
    fdiv s2, s2, s3
    // Require this draw to use the requested screen projection. Local capture
    // cameras and an old matrix during a target handoff retain their own quad.
    ldr s4, [x15, #0x58]
    fsub s5, s2, s4
    fabs s5, s5
    ldr s6, Lepsilon
    fmul s6, s6, s4
    fcmp s5, s6
    b.hi Lbackdrop_done
    ldr x17, [x1, #0xc0]
    ldp s3, s4, [x17, #0x28]
    fdiv s3, s3, s4
    fdiv s2, s2, s3
    fcmp s2, #0.0
    b.le Lbackdrop_done
    fmov s3, #1.0
    fcmp s2, s3
    b.gt Lbackdrop_wide
    b.ge Lbackdrop_done
    fdiv s3, s3, s2
    fmul s4, s1, s3
    str s4, [x2, #0x214]
    fmov s5, #0.5
    fsub s4, s4, s1
    fmul s4, s4, s5
    ldr s6, [x1, #0x28]
    fmul s6, s6, s1
    fmul s6, s6, s3
    fmadd s4, s6, s5, s4
    ldr s5, [x2, #0x21c]
    fadd s4, s4, s5
    str s4, [x2, #0x21c]
    cbnz w11, Lbackdrop_done
    mov x16, #0xe4
    mov x9, #16
    mov x10, #32
    b Lbackdrop_uv_begin
Lbackdrop_wide:
    cbnz w11, Lwipe_wide
    fmov s3, s2
    fmul s4, s0, s3
    str s4, [x2, #0x210]
    fsub s4, s4, s0
    fmov s5, #0.5
    fmul s4, s4, s5
    ldr s5, [x2, #0x218]
    fsub s5, s5, s4
    str s5, [x2, #0x218]
    mov x16, #0xe0
    mov x9, #32
    mov x10, #16
    b Lbackdrop_uv_begin
Lwipe_wide:
    // Add half the newly visible screen width on the outside of each curtain.
    // The left curtain's right edge and right curtain's left edge stay fixed,
    // preserving the animated circular aperture and every UV coordinate.
    ldr s0, [x0, #0x50]
    ldr s4, [x2, #0x210]
    fcmp s0, s4
    b.ne Lbackdrop_done
    fmov s3, #1.0
    fsub s2, s2, s3
    ldr s3, [x17, #0x28]
    fmul s2, s2, s3
    fmov s3, #0.5
    fmul s2, s2, s3
    fadd s4, s4, s2
    str s4, [x2, #0x210]
    ldrb w16, [x0, #0xb2]
    cmp w16, #0x4c
    b.ne Lbackdrop_done
    ldr s4, [x2, #0x218]
    fsub s4, s4, s2
    str s4, [x2, #0x218]
    b Lbackdrop_done
Lbackdrop_uv_begin:
    // UV rows use {u,v,0,1}. Keep their midpoint while extending the
    // repeated picture, so dots and stripes retain their original shape.
    mov w14, #3
    mov x13, #0x188
    ldrb w12, [x0, #0xf1]
Lbackdrop_uv:
    // The uniform has three slots, but only active texture-coordinate
    // indices were populated by the original SDK call.
    ldrsb w17, [x1, x13]
    cmp w17, w12
    b.hs Lbackdrop_next_uv
    ldr s0, [x2, x16]
    add x17, x16, x10
    ldr s1, [x2, x17]
    fsub s1, s1, s0
    fmov s2, #0.5
    fmov s4, s3
    fmov s5, #1.0
    fsub s4, s4, s5
    fmul s4, s4, s2
    fmul s4, s4, s1
    fsub s0, s0, s4
    str s0, [x2, x16]
    add x17, x16, x9
    ldr s0, [x2, x17]
    fsub s0, s0, s4
    str s0, [x2, x17]
    add x17, x16, x10
    ldr s0, [x2, x17]
    fadd s0, s0, s4
    str s0, [x2, x17]
    add x17, x16, #48
    ldr s0, [x2, x17]
    fadd s0, s0, s4
    str s0, [x2, x17]
Lbackdrop_next_uv:
    add x13, x13, #1
    add x16, x16, #64
    subs w14, w14, #1
    b.ne Lbackdrop_uv
Lbackdrop_done:
    ret
.p2align 3
Lwipe_left_name: .asciz "P_L_00"
    .byte 0
Lwipe_right_name: .asciz "P_R_00"
    .byte 0
Lwipe_circle_name: .ascii "P_Circle"
Lwipe_inout_name: .ascii "N_InOut_"
Lwipe_root_name: .ascii "RootPane"
Lwipe_root_size: .float 1920.0, 1080.0
Lwipe_inout_size: .float 30.0, 30.0
.p2align 2
Lwipe_side:
    ldr x17, [x0, #0x18]
    cbz x17, Lbackdrop_done
    ldr x16, [x17, #0xb0]
    ldr x14, Lwipe_circle_name
    cmp x16, x14
    b.ne Lbackdrop_done
    // Require a complete matching _00/_01 suffix, including the terminator.
    ldr w16, [x17, #0xb8]
    ldrb w13, [x0, #0xb5]
    lsl w13, w13, #16
    mov w14, #0x305f
    orr w14, w14, w13
    cmp w16, w14
    b.ne Lbackdrop_done
    ldr x17, [x17, #0x18]
    cbz x17, Lbackdrop_done
    ldr x16, [x17, #0xb0]
    ldr x14, Lwipe_inout_name
    cmp x16, x14
    b.ne Lbackdrop_done
    ldr w16, [x17, #0xb8]
    ldrb w13, [x0, #0xb5]
    lsl w13, w13, #8
    mov w14, #0x30
    orr w14, w14, w13
    cmp w16, w14
    b.ne Lbackdrop_done
    ldr x16, [x17, #0x50]
    ldr x14, Lwipe_inout_size
    cmp x16, x14
    b.ne Lbackdrop_done
    ldr x17, [x17, #0x18]
    cbz x17, Lbackdrop_done
    ldr x16, [x17, #0xb0]
    ldr x14, Lwipe_root_name
    cmp x16, x14
    b.ne Lbackdrop_done
    ldrb w16, [x17, #0xb8]
    cbnz w16, Lbackdrop_done
    ldr x16, [x17, #0x50]
    ldr x14, Lwipe_root_size
    cmp x16, x14
    b.ne Lbackdrop_done
    ldr x17, [x1, #0xc0]
    cbz x17, Lbackdrop_done
    ldr x16, [x17, #0x28]
    cmp x16, x14
    b.ne Lbackdrop_done
    ldr s1, [x0, #0x54]
    ldr s2, [x17, #0x2c]
    fcmp s1, s2
    b.ne Lbackdrop_done
    ldr s2, [x2, #0x214]
    fcmp s1, s2
    b.ne Lbackdrop_done
    // These black coverage panes have no repeated-picture UV expansion.
    mov w11, #1
    b Lbackdrop_projection
Lbg_name: .ascii "P_BG"
Lbg_lower_name: .ascii "P_Bg"
Ldot_name: .ascii "P_Do"
Lcolor_bg_name: .ascii "P_Co"
Lall_bg_name: .ascii "P_Al"
Lstripe_name: .ascii "P_St"
.p2align 3
Ltexture_bg_name: .ascii "P_Tex_0"
    .byte 0x30
Lbase_bg_name: .ascii "P_Base_0"
Lfade_bg_name: .ascii "P_Fade_0"

// KeyGuide and VideogameKeyGuide share this verified operation-guide hierarchy:
// RootPane(1920x1080) -> N_InOut_00(30x30) -> N_Align_00(1810x100).
// Match all three shapes and both complete child names before changing its
// calculated translation. Original pane coordinates and animations stay intact.
.p2align 2
.globl _dm_view_footer_match
_dm_view_footer_match:
    mov w8, wzr
    ldr x14, [x0, #0xb0]
    ldr x15, Lfooter_inout
    cmp x14, x15
    b.ne Lfooter_match_done
    ldr w14, [x0, #0xb8]
    mov w15, #0x3030
    cmp w14, w15
    b.ne Lfooter_match_done
    ldr x14, [x0, #0x50]
    ldr x15, Lfooter_size
    cmp x14, x15
    b.ne Lfooter_match_done
    ldr x17, [x0, #0x18]
    cbz x17, Lfooter_match_done
    ldr x14, [x17, #0x50]
    ldr x15, Lfooter_root_size
    cmp x14, x15
    b.ne Lfooter_match_done
    ldr x17, [x0, #0x28]
    add x14, x0, #0x20
    cmp x17, x14
    b.eq Lfooter_match_done
    cbz x17, Lfooter_match_done
    sub x17, x17, #8
    ldr x14, [x17, #0xb0]
    ldr x15, Lfooter_align
    cmp x14, x15
    b.ne Lfooter_match_done
    ldr w14, [x17, #0xb8]
    mov w15, #0x3030
    cmp w14, w15
    b.ne Lfooter_match_done
    ldr x14, [x17, #0x50]
    ldr x15, Lfooter_align_size
    cmp x14, x15
    cset w8, eq
Lfooter_match_done:
    ret

// Recalculate this small subtree even when animations are idle, so folding,
// entering/exiting the inline keyboard and Fit restore the footer immediately.
.globl _dm_view_footer_recalc
_dm_view_footer_recalc:
    stp x8, x30, [sp, #-48]!
    stp x14, x15, [sp, #16]
    stp x16, x17, [sp, #32]
    mrs x16, nzcv
    adrp x15, _dm_view_state@PAGE
    add x15, x15, _dm_view_state@PAGEOFF
    ldr w17, [x15, #0x58]
    cbz w17, Lfooter_recalc_done
    bl _dm_view_footer_match
    cbz w8, Lfooter_recalc_done
    orr w3, w3, #1
Lfooter_recalc_done:
    msr nzcv, x16
    ldp x16, x17, [sp, #32]
    ldp x14, x15, [sp, #16]
    ldp x8, x30, [sp], #48
    str d8, [sp, #-0x50]!
    b _dm_return_0027d4c4

.globl _dm_view_footer_y
_dm_view_footer_y:
    stp x8, x30, [sp, #-64]!
    stp x14, x15, [sp, #16]
    stp x16, x17, [sp, #32]
    str x13, [sp, #48]
    stp q0, q1, [sp, #-32]!
    mrs x13, nzcv
    adrp x15, _dm_view_state@PAGE
    add x15, x15, _dm_view_state@PAGEOFF
    ldr w17, [x15, #0x58]
    cbz w17, Lfooter_y_done
    bl _dm_view_footer_match
    cbz w8, Lfooter_y_done
    adrp x15, _dm_view_state@PAGE
    add x15, x15, _dm_view_state@PAGEOFF
    ldr w16, [x15, #0x58]
    cbz w16, Lfooter_y_done
    ldr w16, [x15, #0x58]
    cbz w16, Lfooter_y_done
    ldr s0, Lbase
    fmov s1, w16
    fcmp s1, s0
    b.ge Lfooter_y_done
    bl Lrecent_keyboard
    cbnz w8, Lfooter_y_done
    fdiv s0, s0, s1
    fmov s1, #1.0
    fsub s0, s0, s1
    ldr s1, Lfooter_half_height
    fmsub s3, s0, s1, s3
Lfooter_y_done:
    msr nzcv, x13
    ldp q0, q1, [sp], #32
    ldr x13, [sp, #48]
    ldp x16, x17, [sp, #32]
    ldp x14, x15, [sp, #16]
    ldp x8, x30, [sp], #64
    lsr w10, w10, #6
    b _dm_return_0027d7c0
.p2align 3
Lfooter_inout: .ascii "N_InOut_"
Lfooter_align: .ascii "N_Align_"
Lfooter_size: .float 30.0, 30.0
Lfooter_root_size: .float 1920.0, 1080.0
Lfooter_align_size: .float 1810.0, 100.0
Lfooter_half_height: .float 540.0


// The keyboard owns an embedded dialog, outside ordinary game Page callbacks.
// Recognize its actually drawn default Window and complete authored hierarchy.
// The original draw arguments, floating registers and flags are preserved.
.globl _dm_view_keyboard_window_call
_dm_view_keyboard_window_call:
    stp x0, x30, [sp, #-16]!
    bl _dm_view_keyboard_window
    ldp x0, x30, [sp], #16
    ldrb w8, [x0, #0x10a]
    b _dm_return_0029a18c
.globl _dm_view_keyboard_window
_dm_view_keyboard_window:
    stp x8, x13, [sp, #-64]!
    stp x14, x15, [sp, #16]
    stp x16, x17, [sp, #32]
    stp x0, x30, [sp, #48]
    mrs x13, nzcv
    ldr x14, [x0]
    adrp x15, _dm_code_0474de58@PAGE
    add x15, x15, _dm_code_0474de58@PAGEOFF
    cmp x14, x15
    b.ne Lkeyboard_window_done
    ldr x14, [x0, #0xb0]
    ldr x15, Lkeyboard_window_name
    cmp x14, x15
    b.ne Lkeyboard_window_done
    ldr w14, [x0, #0xb8]
    mov w15, #0x3130
    cmp w14, w15
    b.ne Lkeyboard_window_done
    ldr x14, [x0, #0x50]
    ldr x15, Lkeyboard_window_size
    cmp x14, x15
    b.ne Lkeyboard_window_done
    ldr x17, [x0, #0x18]
    cbz x17, Lkeyboard_window_done
    ldr x14, [x17, #0xb0]
    ldr x15, Lkeyboard_scale_name
    cmp x14, x15
    b.ne Lkeyboard_window_done
    ldr w14, [x17, #0xb8]
    mov w15, #0x3130
    cmp w14, w15
    b.ne Lkeyboard_window_done
    ldr x14, [x17, #0x50]
    ldr x15, Lkeyboard_node_size
    cmp x14, x15
    b.ne Lkeyboard_window_done
    ldr x17, [x17, #0x18]
    cbz x17, Lkeyboard_window_done
    ldr x14, [x17, #0xb0]
    ldr x15, Lkeyboard_valid_name
    cmp x14, x15
    b.ne Lkeyboard_window_done
    ldr w14, [x17, #0xb8]
    mov w15, #0x3130
    cmp w14, w15
    b.ne Lkeyboard_window_done
    ldr x14, [x17, #0x50]
    ldr x15, Lkeyboard_node_size
    cmp x14, x15
    b.ne Lkeyboard_window_done
    ldr x17, [x17, #0x18]
    cbz x17, Lkeyboard_window_done
    ldr x14, [x17, #0xb0]
    ldr x15, Lkeyboard_part_name
    cmp x14, x15
    b.eq Lkeyboard_single_button
    ldr x15, Lkeyboard_part_pair_name
    cmp x14, x15
    b.ne Lkeyboard_window_done
    ldrb w14, [x17, #0xb8]
    cbnz w14, Lkeyboard_window_done
    ldr x17, [x17, #0x18]
    cbz x17, Lkeyboard_window_done
    b Lkeyboard_button_loop
Lkeyboard_single_button:
    ldrb w14, [x17, #0xb8]
    cbnz w14, Lkeyboard_window_done
    ldr x17, [x17, #0x18]
    cbz x17, Lkeyboard_window_done
    ldr x14, [x17, #0xb0]
    ldr x15, Lkeyboard_root_name
    cmp x14, x15
    b.eq Lkeyboard_button_root
Lkeyboard_button_loop:
    ldr x14, [x17, #0xb0]
    ldr x15, Lkeyboard_loop_name
    cmp x14, x15
    b.ne Lkeyboard_window_done
    ldr w14, [x17, #0xb8]
    mov w15, #0x30
    cmp w14, w15
    b.ne Lkeyboard_window_done
    ldr x14, [x17, #0x50]
    ldr x15, Lkeyboard_loop_size
    cmp x14, x15
    b.ne Lkeyboard_window_done
    ldr x17, [x17, #0x18]
    cbz x17, Lkeyboard_window_done
    ldr x14, [x17, #0xb0]
    ldr x15, Lkeyboard_root_name
    cmp x14, x15
    b.ne Lkeyboard_window_done
Lkeyboard_button_root:
    ldr x14, [x17, #0x50]
    ldr x15, Lkeyboard_root_size
    cmp x14, x15
    b.ne Lkeyboard_window_done
    adrp x15, _dm_view_state@PAGE
    add x15, x15, _dm_view_state@PAGEOFF
    ldr x16, [x15, #0xa8]
    cbz x16, Lkeyboard_window_done
    ldar x16, [x16]
    add x16, x16, #1
    add x15, x15, #56
    stlr x16, [x15]
Lkeyboard_window_done:
    msr nzcv, x13
    ldp x0, x30, [sp, #48]
    ldp x16, x17, [sp, #32]
    ldp x14, x15, [sp, #16]
    ldp x8, x13, [sp], #64
    ret
.p2align 3
Lkeyboard_window_name: .ascii "W_BaseL_"
Lkeyboard_scale_name: .ascii "N_Scale_"
Lkeyboard_valid_name: .ascii "N_Valid_"
Lkeyboard_part_name: .ascii "L_Btn_00"
Lkeyboard_part_pair_name: .ascii "L_Btn_01"
Lkeyboard_loop_name: .ascii "N_Loop_0"
Lkeyboard_root_name: .asciz "N_Root"
    .byte 0
Lkeyboard_window_size: .float 420,120
Lkeyboard_node_size: .float 30,40
Lkeyboard_root_size: .float 10,108
Lkeyboard_loop_size: .float 30,30

// Transition layouts are screen overlays. Extend only their outer coverage;
// retain the authored inner edges, animation, colours, alpha and texture UVs.
// Layout::Build stores its lyt1 name pointer at +0x30 (verified in the profile).
// The regular and colour-optimized Picture paths use different quad encodings.
.globl _dm_view_transition_fast_call
_dm_view_transition_fast_call:
    stp x1, x30, [sp, #-16]!
    bl _dm_code_00265220
    ldr x2, [sp]
    sub x2, x2, #0x40
    mov x0, x19
    mov x1, x20
    mov w3, #1
    bl _dm_view_transition
    ldp x1, x30, [sp], #16
    ret

// x0 pane, x1 DrawInfo, x2 private draw constants, w3 optimized-quad encoding.
// Return 1 for a transition layout, including refused local/malformed draws,
// so the older repeating-background adapter cannot process it a second time.
.globl _dm_view_transition
_dm_view_transition:
    adrp x15, _dm_view_state@PAGE
    add x15, x15, _dm_view_state@PAGEOFF
    ldr w14, [x15, #0x58]
    cbz w14, Ltransition_no
    ldr w14, [x15, #0x58]
    cbz w14, Ltransition_no
    fmov s0, w14
    ldr s1, Lbase
    fcmp s0, s1
    b.eq Ltransition_no
    ldr x16, [x1, #0xc0]
    cbz x16, Ltransition_no
    ldr x17, [x16, #0x30]
    cbz x17, Ltransition_no
    ldr w14, [x17]
    ldr w13, Ltransition_wipe
    cmp w14, w13
    b.eq Ltransition_layout
    ldr w13, Ltransition_fade
    cmp w14, w13
    b.ne Ltransition_video
    ldrb w14, [x17, #4]
    cbz w14, Ltransition_layout
    b Ltransition_no
Ltransition_video:
    ldr x14, [x17]
    ldr x13, Ltransition_video_name
    cmp x14, x13
    b.ne Ltransition_time
    ldr w14, [x17, #8]
    ldr w13, Ltransition_video_tail
    cmp w14, w13
    b.ne Ltransition_no
    ldrh w14, [x17, #12]
    cmp w14, #0x65
    b.eq Ltransition_layout
    b Ltransition_no
Ltransition_time:
    ldr x13, Ltransition_time_name
    cmp x14, x13
    b.ne Ltransition_cloud
    ldr w14, [x17, #8]
    ldr w13, Ltransition_time_tail
    cmp w14, w13
    b.ne Ltransition_no
    ldrh w14, [x17, #12]
    cmp w14, #0x74
    b.eq Ltransition_layout
    b Ltransition_no
Ltransition_cloud:
    ldr x13, Ltransition_cloud_name
    cmp x14, x13
    b.ne Ltransition_no
    ldr x14, [x17, #8]
    ldr x13, Ltransition_cloud_middle
    cmp x14, x13
    b.ne Ltransition_no
    ldr w14, [x17, #16]
    ldr w13, Ltransition_cloud_tail
    cmp w14, w13
    b.ne Ltransition_no
    ldrb w14, [x17, #20]
    cbnz w14, Ltransition_no
Ltransition_layout:
    ldr x14, [x16, #0x28]
    ldr x13, Ltransition_reference_size
    cmp x14, x13
    b.ne Ltransition_done
    ldr x10, [x0, #0xb0]
    adr x9, Ltransition_roles
    mov w11, #18
Ltransition_role:
    ldp x12, x13, [x9], #16
    and x14, x10, x13
    cmp x14, x12
    b.eq Ltransition_projection
    sub w11, w11, #1
    cbnz w11, Ltransition_role
    b Ltransition_done
Ltransition_projection:
    // The viewport must be the current screen perspective, not a capture.
    ldr w14, [x1, #0x4c]
    cbnz w14, Ltransition_done
    ldr w14, [x1, #0x48]
    mov w13, #0xbf800000
    cmp w14, w13
    b.ne Ltransition_done
    ldr x14, [x1, #0x40]
    and x14, x14, #0x7fffffff7fffffff
    cbnz x14, Ltransition_done
    ldr s0, [x1, #0x10]
    ldr s1, [x1, #0x24]
    fcmp s0, #0.0
    b.le Ltransition_done
    fcmp s1, #0.0
    b.le Ltransition_done
    fdiv s2, s1, s0
    ldr s3, [x15, #0x58]
    fsub s2, s2, s3
    fabs s2, s2
    ldr s30, Lepsilon
    fmul s3, s3, s30
    fcmp s2, s3
    b.hi Ltransition_done
    // Both matrices must be fresh. Reusing already expanded model constants
    // must be idempotent even if one material is submitted twice.
    mov x11, #0
Ltransition_projection_fresh:
    ldr x12, [x2, x11]
    add x10, x1, #0x10
    ldr x13, [x10, x11]
    cmp x12, x13
    b.ne Ltransition_done
    add x11, x11, #8
    cmp x11, #64
    b.ne Ltransition_projection_fresh
    mov x11, #0
    add x9, x2, #0x40
    add x10, x1, #0x80
Ltransition_model_fresh:
    ldr x12, [x9, x11]
    ldr x13, [x10, x11]
    cmp x12, x13
    b.ne Ltransition_done
    add x11, x11, #8
    cmp x11, #48
    b.ne Ltransition_model_fresh
    // Only an axis-aligned flat UI plane has separable screen outer edges.
    ldr s2, [x2, #0x44]
    fcmp s2, #0.0
    b.ne Ltransition_done
    ldr s2, [x2, #0x50]
    fcmp s2, #0.0
    b.ne Ltransition_done
    ldr s2, [x2, #0x60]
    fcmp s2, #0.0
    b.ne Ltransition_done
    ldr s2, [x2, #0x64]
    fcmp s2, #0.0
    b.ne Ltransition_done
    ldr s4, [x2, #0x40]
    ldr s5, [x2, #0x54]
    ldr s6, [x2, #0x4c]
    ldr s7, [x2, #0x5c]
    ldr s29, [x2, #0x6c]
    fcmp s4, #0.0
    b.eq Ltransition_done
    fcmp s5, #0.0
    b.eq Ltransition_done
    fcmp s29, #0.0
    b.ge Ltransition_done
    fneg s29, s29
    fdiv s26, s29, s0
    fdiv s27, s29, s1
    ldp s24, s25, [x16, #0x28]
    fmov s31, #0.5
    fmul s24, s24, s31
    fmul s25, s25, s31
    // Convert the current projected viewport into the same world UI units.
    ldr s28, [x1, #0x18]
    fmul s28, s28, s26
    ldr s29, [x1, #0x28]
    fmul s29, s29, s27
    // Clamp only negative extra coverage to zero: a smaller/capture viewport
    // cannot shrink authored coverage or change an existing aperture edge.
    fsub s20, s26, s28
    fsub s20, s20, s24
    fadd s21, s26, s28
    fsub s21, s21, s24
    fsub s22, s27, s29
    fsub s22, s22, s25
    fadd s23, s27, s29
    fsub s23, s23, s25
    fmov s31, wzr
    fmax s20, s20, s31
    fmax s21, s21, s31
    fmax s22, s22, s31
    fmax s23, s23, s31
    // Accept only the original complete quad encoding for this Picture path.
    ldp s0, s1, [x0, #0x50]
    ldr s2, [x2, #0x218]
    ldr s3, [x2, #0x21c]
    // Encoding mode lives in w3, whose floating register is independent.
    cbnz w3, Ltransition_fast_origin
    ldr s28, [x2, #0x210]
    ldr s29, [x2, #0x214]
    fcmp s28, s0
    b.ne Ltransition_done
    fcmp s29, s1
    b.ne Ltransition_done
    b Ltransition_bounds
Ltransition_fast_origin:
    ldr s28, [x2, #0x210]
    ldr s29, [x2, #0x214]
    fmov s31, #0.5
    fmul s30, s0, s31
    fcmp s28, s30
    b.ne Ltransition_done
    fmul s30, s1, s31
    fcmp s29, s30
    b.ne Ltransition_done
    fcmp s2, #0.0
    b.ne Ltransition_done
    fcmp s3, #0.0
    b.ne Ltransition_done
    ldrb w14, [x0, #0x5b]
    and w13, w14, #3
    cmp w13, #3
    b.eq Ltransition_done
    fmov s2, wzr
    cbnz w13, Ltransition_fast_x_edge
    fmul s2, s0, s31
    fneg s2, s2
    b Ltransition_fast_y
Ltransition_fast_x_edge:
    cmp w13, #2
    b.ne Ltransition_fast_y
    fneg s2, s0
Ltransition_fast_y:
    ubfx w13, w14, #2, #2
    cmp w13, #3
    b.eq Ltransition_done
    fmov s3, wzr
    cbnz w13, Ltransition_fast_y_edge
    fmul s3, s1, s31
    b Ltransition_bounds
Ltransition_fast_y_edge:
    cmp w13, #2
    b.ne Ltransition_bounds
    fmov s3, s1
Ltransition_bounds:
    fmadd s16, s4, s2, s6
    fadd s0, s2, s0
    fmadd s17, s4, s0, s6
    fmin s28, s16, s17
    fmax s17, s16, s17
    fmov s16, s28
    fmadd s18, s5, s3, s7
    fsub s1, s3, s1
    fmadd s19, s5, s1, s7
    fmin s28, s18, s19
    fmax s19, s18, s19
    fmov s18, s28
    // All decisions use finite bounds, including negative pane dimensions and
    // reflected animation scales. Decorative aperture pictures are excluded.
    ldr s31, Ltransition_infinity
    fabs s28, s16
    fcmp s28, s31
    b.hs Ltransition_done
    fabs s28, s17
    fcmp s28, s31
    b.hs Ltransition_done
    fabs s28, s18
    fcmp s28, s31
    b.hs Ltransition_done
    fabs s28, s19
    fcmp s28, s31
    b.hs Ltransition_done
    fadd s28, s20, s21
    fadd s29, s22, s23
    fadd s28, s28, s29
    fcmp s28, s31
    b.hs Ltransition_done
    ldr s30, Lepsilon
    fmul s28, s24, s30
    fmul s29, s25, s30
    fneg s30, s24
    fadd s30, s30, s28
    fcmp s16, s30
    b.gt Ltransition_keep_left
    fsub s20, s16, s20
    b Ltransition_right
Ltransition_keep_left:
    fmov s20, s16
Ltransition_right:
    fsub s30, s24, s28
    fcmp s17, s30
    b.lt Ltransition_keep_right
    fadd s21, s17, s21
    b Ltransition_bottom
Ltransition_keep_right:
    fmov s21, s17
Ltransition_bottom:
    fneg s30, s25
    fadd s30, s30, s29
    fcmp s18, s30
    b.gt Ltransition_keep_bottom
    fsub s22, s18, s22
    b Ltransition_top
Ltransition_keep_bottom:
    fmov s22, s18
Ltransition_top:
    fsub s30, s25, s29
    fcmp s19, s30
    b.lt Ltransition_keep_top
    fadd s23, s19, s23
    b Ltransition_commit
Ltransition_keep_top:
    fmov s23, s19
Ltransition_commit:
    fcmp s20, s16
    b.ne Ltransition_changed
    fcmp s21, s17
    b.ne Ltransition_changed
    fcmp s22, s18
    b.ne Ltransition_changed
    fcmp s23, s19
    b.eq Ltransition_done
Ltransition_changed:
    cbnz w3, Ltransition_commit_fast
    // Invert the original axes while retaining the signed vertex order.
    ldr s0, [x0, #0x50]
    fmul s0, s0, s4
    fmov w17, s0
    tbnz w17, #31, Ltransition_regular_x_reverse
    fmov s0, s20
    fsub s1, s21, s20
    b Ltransition_regular_x
Ltransition_regular_x_reverse:
    fmov s0, s21
    fsub s1, s20, s21
Ltransition_regular_x:
    fsub s0, s0, s6
    fdiv s0, s0, s4
    fdiv s1, s1, s4
    ldr s2, [x0, #0x54]
    fmul s2, s2, s5
    fmov w17, s2
    tbnz w17, #31, Ltransition_regular_y_reverse
    fmov s2, s23
    fsub s3, s23, s22
    b Ltransition_regular_y
Ltransition_regular_y_reverse:
    fmov s2, s22
    fsub s3, s22, s23
Ltransition_regular_y:
    fsub s2, s2, s7
    fdiv s2, s2, s5
    fdiv s3, s3, s5
    fabs s28, s0
    fcmp s28, s31
    b.hs Ltransition_done
    fabs s28, s1
    fcmp s28, s31
    b.hs Ltransition_done
    fabs s28, s2
    fcmp s28, s31
    b.hs Ltransition_done
    fabs s28, s3
    fcmp s28, s31
    b.hs Ltransition_done
    str s1, [x2, #0x210]
    str s3, [x2, #0x214]
    str s0, [x2, #0x218]
    str s2, [x2, #0x21c]
    b Ltransition_done
Ltransition_commit_fast:
    fsub s0, s17, s16
    fsub s1, s19, s18
    fcmp s0, #0.0
    b.le Ltransition_commit_fast_zero
    fcmp s1, #0.0
    b.le Ltransition_commit_fast_zero
    fsub s2, s21, s20
    fsub s3, s23, s22
    fdiv s2, s2, s0
    fdiv s3, s3, s1
    fsub s0, s6, s16
    fsub s1, s7, s18
    fmadd s0, s0, s2, s20
    fmadd s1, s1, s3, s22
    fmul s4, s4, s2
    fmul s5, s5, s3
    fabs s28, s4
    fcmp s28, s31
    b.hs Ltransition_done
    fabs s28, s5
    fcmp s28, s31
    b.hs Ltransition_done
    fabs s28, s0
    fcmp s28, s31
    b.hs Ltransition_done
    fabs s28, s1
    fcmp s28, s31
    b.hs Ltransition_done
    str s4, [x2, #0x40]
    str s5, [x2, #0x54]
    str s0, [x2, #0x4c]
    str s1, [x2, #0x5c]
    b Ltransition_done
Ltransition_commit_fast_zero:
    // A curtain can begin at zero width/height outside the reference screen.
    // Give its new outer strip real geometry without moving its aperture edge.
    fsub s0, s21, s20
    fsub s1, s23, s22
    fabs s2, s4
    fabs s3, s5
    fdiv s0, s0, s2
    fdiv s1, s1, s3
    fmov s28, #0.5
    fmul s0, s0, s28
    fmul s1, s1, s28
    ldr w17, [x0, #0x50]
    tbz w17, #31, Ltransition_zero_width_sign
    fneg s0, s0
Ltransition_zero_width_sign:
    ldr w17, [x0, #0x54]
    tbz w17, #31, Ltransition_zero_height_sign
    fneg s1, s1
Ltransition_zero_height_sign:
    fadd s2, s20, s21
    fadd s3, s22, s23
    fmul s2, s2, s28
    fmul s3, s3, s28
    ldrb w14, [x0, #0x5b]
    and w13, w14, #3
    cbz w13, Ltransition_zero_y_origin
    fmov s29, s0
    cmp w13, #2
    b.ne Ltransition_zero_x_origin
    fneg s29, s29
Ltransition_zero_x_origin:
    fmul s29, s4, s29
    fsub s2, s2, s29
Ltransition_zero_y_origin:
    ubfx w13, w14, #2, #2
    cbz w13, Ltransition_zero_finite
    fneg s29, s1
    cmp w13, #2
    b.ne Ltransition_zero_y_shift
    fneg s29, s29
Ltransition_zero_y_shift:
    fmul s29, s5, s29
    fsub s3, s3, s29
Ltransition_zero_finite:
    fabs s28, s0
    fcmp s28, s31
    b.hs Ltransition_done
    fabs s28, s1
    fcmp s28, s31
    b.hs Ltransition_done
    fabs s28, s2
    fcmp s28, s31
    b.hs Ltransition_done
    fabs s28, s3
    fcmp s28, s31
    b.hs Ltransition_done
    str s0, [x2, #0x210]
    str s1, [x2, #0x214]
    str s2, [x2, #0x4c]
    str s3, [x2, #0x5c]
Ltransition_done:
    mov w8, #1
    ret
Ltransition_no:
    mov w8, wzr
    ret
.p2align 3
Ltransition_wipe: .ascii "Wipe"
Ltransition_fade: .ascii "Fade"
Ltransition_video_name: .ascii "Videogam"
Ltransition_video_tail: .ascii "eFad"
Ltransition_time_name: .ascii "LWipeTim"
Ltransition_time_tail: .ascii "eEdi"
Ltransition_cloud_name: .ascii "CloudIsl"
Ltransition_cloud_middle: .ascii "andWipeD"
Ltransition_cloud_tail: .ascii "ream"
Ltransition_reference_size: .float 1920,1080
Ltransition_infinity: .word 0x7f800000
.p2align 3
Ltransition_roles:
    .quad 0x005f657361425f50, 0x00ffffffffffffff
    .quad 0x005f6b6361425f50, 0x00ffffffffffffff
    .quad 0x005f656461465f50, 0x00ffffffffffffff
    .quad 0x0000005f67425f50, 0x000000ffffffffff
    .quad 0x0000005f47425f50, 0x000000ffffffffff
    .quad 0x0000657069575f50, 0x0000ffffffffffff
    .quad 0x00000074754f5f50, 0x000000ffffffffff
    .quad 0x0000005f70555f50, 0x000000ffffffffff
    .quad 0x005f6e776f445f50, 0x00ffffffffffffff
    .quad 0x7265747461505f50, 0xffffffffffffffff
    .quad 0x000000005f4c5f50, 0x00000000ffffffff
    .quad 0x000000005f525f50, 0x00000000ffffffff
    .quad 0x000000005f545f50, 0x00000000ffffffff
    .quad 0x000000005f425f50, 0x00000000ffffffff
    .quad 0x616d726f4e4c5f50, 0xffffffffffffffff
    .quad 0x616d726f4e525f50, 0xffffffffffffffff
    .quad 0x616d726f4e545f50, 0xffffffffffffffff
    .quad 0x616d726f4e425f50, 0xffffffffffffffff
