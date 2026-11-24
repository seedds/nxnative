"""Execute Ocarina of Time's dynamic-resolution and render-scale stubs on ARM64, without game files.

Checks:
- drs_controller: off mode (state +0x40 == 0), threshold scaling by 60 / target_fps when 0 < target_fps < 60
  (DM_FPS_TARGET=30: 83 -> 166), unchanged threshold at target 0 and 60, and step-down / climb-up rules.
- drs_render_scale_guard + drs_controller: render scale in (0, 1) holds level 0 even with DRS on; 0, >= 1, negative, NaN fall through.
- render_scale status/store stubs: recompute trigger, scale multiply, scene-reset re-apply, status 1 preservation, and register/flag preservation.
"""
import importlib.util
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import types
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from nso_asm import MOVX   # noqa: E402
spec = importlib.util.spec_from_file_location('ocarina_patches', Path(__file__).resolve().parents[2] / 'games/0400486024DEC000/patches.py')
assert spec is not None and spec.loader is not None
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)


class OcarinaDrsStub(unittest.TestCase):
    def test_off_entry_prefix(self):
        # First 4 words are the off-check returning 0 (ldr w17,[x16,#0x40]; cbnz w17,+0x10; mov w0,wzr; ret).
        self.assertEqual(patch.drs_controller[:4], (0xb9404211, 0x35000071, 0x2a1f03e0, 0xd65f03c0))

    def test_guard_omitted_without_render_scale_match(self):
        pos, words = [0x6000000], []
        def emit(w):
            words.append(w)
            pos[0] += 4
        ctx = types.SimpleNamespace(
            here=lambda: pos[0], emit=emit, site=lambda off, orig: None, put=lambda off, ws, why: None,
            state=0x9000000, render_scale=0, step_max=0, base=0x100000, capabilities=set(),
            others={'dynamic resolution': 0x8000000, 'dynamic resolution update': 0x8200000, 'render scale': None},
        )
        patch.dynamic_resolution(ctx)
        patch.render_scale(ctx)
        self.assertEqual(ctx.render_scale, 0)
        self.assertEqual(tuple(words[1:]), patch.drs_controller)

    @unittest.skipUnless(platform.system() == 'Darwin' and platform.machine() == 'arm64' and shutil.which('clang'), 'needs macOS arm64 and clang')
    def test_drs_controller_threshold_scaling_and_steps(self):
        drs_words = '\n'.join(f'    .long 0x{w:08x}' for w in patch.drs_controller)
        guarded_words = '\n'.join(f'    .long 0x{w:08x}' for w in (*patch.drs_render_scale_guard, *patch.drs_controller))
        status_words = '\n'.join(f'    .long 0x{w:08x}' for w in patch.render_scale_status_body(MOVX(16, 1)))
        store_words = '\n'.join(f'    .long 0x{w:08x}' for w in patch.render_scale_store_body(MOVX(16, 1)))
        asm_src = f'''.text
.p2align 2
.globl _call_drs
_call_drs:
    stp x29, x30, [sp, #-16]!
    mov x16, x1
    bl _drs_stub
    ldp x29, x30, [sp], #16
    ret
_drs_stub:
{drs_words}

.p2align 2
.globl _call_drs_guarded
_call_drs_guarded:
    stp x29, x30, [sp, #-16]!
    mov x16, x1
    bl _drs_guarded_stub
    ldp x29, x30, [sp], #16
    ret
_drs_guarded_stub:
{guarded_words}

// uint64_t call_rs_status(uint64_t x0_ret, void *state, void *view, uint64_t *nzcv_io, uint64_t *x16_17_io)
.p2align 2
.globl _call_rs_status
_call_rs_status:
    stp x29, x30, [sp, #-32]!
    stp x19, x20, [sp, #16]
    mov x20, x2
    ldr x9, [x3]
    msr nzcv, x9
    ldp x16, x17, [x4]
{status_words}
    mrs x9, nzcv
    str x9, [x3]
    stp x16, x17, [x4]
    mov x0, x8
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

// float call_rs_store(float s0_in, void *state, void *view, uint64_t *nzcv_io, uint64_t *x16_17_io, void *q30_31_io)
.p2align 2
.globl _call_rs_store
_call_rs_store:
    stp x29, x30, [sp, #-32]!
    stp x19, x20, [sp, #16]
    mov x20, x1
    mov x1, x0
    ldr x9, [x2]
    msr nzcv, x9
    ldp x16, x17, [x3]
    ldp q30, q31, [x4]
{store_words}
    mrs x9, nzcv
    str x9, [x2]
    stp x16, x17, [x3]
    stp q30, q31, [x4]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
'''
        c_src = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int call_drs(void *ctrl, void *state);
extern int call_drs_guarded(void *ctrl, void *state);
extern uint64_t call_rs_status(uint64_t x0_ret, void *state, void *view, uint64_t *nzcv_io, uint64_t *x16_17_io);
extern float call_rs_store(float s0_in, void *state, void *view, uint64_t *nzcv_io, uint64_t *x16_17_io, uint64_t *q30_31_io);

static void set_f32(uint8_t *buf, size_t off, float v) { memcpy(buf + off, &v, 4); }
static void set_u32(uint8_t *buf, size_t off, uint32_t v) { memcpy(buf + off, &v, 4); }
static float get_f32(const uint8_t *buf, size_t off) { float v; memcpy(&v, buf + off, 4); return v; }
static uint32_t get_u32(const uint8_t *buf, size_t off) { uint32_t v; memcpy(&v, buf + off, 4); return v; }

static void init_ctrl(uint8_t *ctrl, float load, float thresh, int level, int count) {
    memset(ctrl, 0, 0x200);
    set_f32(ctrl, 0x08, load);
    set_f32(ctrl, 0x10, thresh);
    set_u32(ctrl, 0x174, (uint32_t)level);
    set_u32(ctrl, 0x1a0, (uint32_t)count);
}

static void init_state(uint8_t *state, float target_fps, uint32_t enabled) {
    memset(state, 0, 0x400);
    set_f32(state, 0x00, target_fps);
    set_u32(state, 0x40, enabled);
}

static uint64_t pack_ret(float scale, uint8_t status) {
    uint32_t bits; memcpy(&bits, &scale, 4);
    return (uint64_t)bits | ((uint64_t)status << 32);
}

int main(void) {
    uint8_t ctrl[0x200], state[0x400], view[0x1000];

    // 1. Off (DM_DYNAMIC_RESOLUTION=0): always returns 0.
    init_ctrl(ctrl, 200.0f, 83.0f, 5, 10);
    init_state(state, 30.0f, 0);
    assert(call_drs(ctrl, state) == 0);
    assert(get_u32(state, 0x200) == 0);

    // 2. 60 fps target: threshold stays 83.0; load 90 steps down after 8 over-budget frames.
    init_ctrl(ctrl, 90.0f, 83.0f, 0, 10);
    init_state(state, 60.0f, 1);
    int r = -1;
    for (int i = 0; i < 14; i++) { r = call_drs(ctrl, state); assert(r == 0); }   // 7 settling + 7 over
    r = call_drs(ctrl, state);
    assert(r == 1);
    assert(get_f32(state, 0x210) == 90.0f);
    // At level 1 with load 80 (fits 83): stays at 1 while level 0's 90 is fresh, climbs after 600 calls.
    set_u32(ctrl, 0x174, 1);
    set_f32(ctrl, 0x08, 80.0f);
    for (int i = 0; i < 600; i++) { r = call_drs(ctrl, state); assert(r == 1); }
    r = call_drs(ctrl, state);
    assert(r == 0);

    // 3. 30 fps target: threshold scales to 166.0; load 134 stays at level 0 and climbs from level 2.
    init_ctrl(ctrl, 134.0f, 83.0f, 0, 10);
    init_state(state, 30.0f, 1);
    for (int i = 0; i < 40; i++) { assert(call_drs(ctrl, state) == 0); }
    assert(get_f32(state, 0x210) == 134.0f);

    init_ctrl(ctrl, 134.0f, 83.0f, 2, 10);
    init_state(state, 30.0f, 1);
    for (int i = 0; i < 8; i++) { assert(call_drs(ctrl, state) == 2); }
    assert(call_drs(ctrl, state) == 1);

    // 30 fps target: load 170 (> 166) steps down after 8 over-budget calls, clamped at floor (count - 1 = 9).
    init_ctrl(ctrl, 170.0f, 83.0f, 0, 10);
    init_state(state, 30.0f, 1);
    for (int i = 0; i < 14; i++) { assert(call_drs(ctrl, state) == 0); }
    assert(call_drs(ctrl, state) == 1);
    // Level 1 fits at 150 (< 166), stays at 1 while level 0's 170 (>= 166) is fresh.
    set_u32(ctrl, 0x174, 1);
    set_f32(ctrl, 0x08, 150.0f);
    for (int i = 0; i < 20; i++) { assert(call_drs(ctrl, state) == 1); }

    init_ctrl(ctrl, 170.0f, 83.0f, 9, 10);
    init_state(state, 30.0f, 1);
    for (int i = 0; i < 30; i++) { assert(call_drs(ctrl, state) == 9); }

    // 4. Target 0 (game's own frame step): threshold stays 83.0, no divide-by-zero.
    init_ctrl(ctrl, 100.0f, 83.0f, 0, 10);
    init_state(state, 0.0f, 1);
    for (int i = 0; i < 14; i++) { assert(call_drs(ctrl, state) == 0); }
    assert(call_drs(ctrl, state) == 1);

    // 5. Render scale guard in dynamic_resolution: scale in (0, 1) forces level 0 even with DRS on.
    init_ctrl(ctrl, 200.0f, 83.0f, 3, 10);
    init_state(state, 60.0f, 1);
    set_f32(state, 0x10, 0.75f);
    assert(call_drs_guarded(ctrl, state) == 0);
    assert(get_u32(state, 0x200) == 0);
    set_f32(state, 0x10, 0.5f);
    assert(call_drs_guarded(ctrl, state) == 0);
    // Unset (0), 1.0, >1.0, negative, NaN fall through to drs_controller (stays at level 3 during settling).
    const float passthrough[] = {0.0f, 1.0f, 1.5f, -0.5f, NAN};
    for (size_t i = 0; i < sizeof passthrough / sizeof *passthrough; i++) {
        init_ctrl(ctrl, 200.0f, 83.0f, 3, 10);
        init_state(state, 60.0f, 1);
        set_f32(state, 0x10, passthrough[i]);
        assert(call_drs_guarded(ctrl, state) == 3);
        assert(get_u32(state, 0x200) == 1);
    }

    // 6. Render scale status + store stubs: preserve registers/flags, apply once, survive scene reset.
    memset(view, 0, sizeof view);
    set_f32(view, 0xe5c, 0.75f);
    for (size_t i = 0; i < sizeof passthrough / sizeof *passthrough; i++) {
        init_state(state, 60.0f, 0);
        set_f32(state, 0x10, passthrough[i]);
        uint64_t nzcv = 0xa0000000ULL, r16[2] = {0x1111222233334444ULL, 0x5555666677778888ULL};
        uint64_t q30[4] = {1, 2, 3, 4};
        assert(call_rs_status(pack_ret(0.75f, 2), state, view, &nzcv, r16) == 2);
        assert(nzcv == 0xa0000000ULL && r16[0] == 0x1111222233334444ULL && r16[1] == 0x5555666677778888ULL);
        float out = call_rs_store(0.75f, state, view, &nzcv, r16, q30);
        assert(out == 0.75f && get_f32(view, 0xe5c) == 0.75f && get_u32(state, 0x290) == 0);
        assert(nzcv == 0xa0000000ULL && r16[0] == 0x1111222233334444ULL && r16[1] == 0x5555666677778888ULL);
        assert(q30[0] == 1 && q30[1] == 2 && q30[2] == 3 && q30[3] == 4);
    }

    init_state(state, 60.0f, 0);
    set_f32(state, 0x10, 0.75f);
    set_f32(view, 0xe5c, 0.75f);
    uint64_t nzcv = 0x50000000ULL, r16[2] = {0xdeadbeef00000001ULL, 0xcafebabe00000002ULL};
    uint64_t q30[4] = {10, 20, 30, 40};
    // Status 1 (game's other request) is kept even before applied.
    assert(call_rs_status(pack_ret(0.75f, 1), state, view, &nzcv, r16) == 1);
    assert(nzcv == 0x50000000ULL && r16[0] == 0xdeadbeef00000001ULL && r16[1] == 0xcafebabe00000002ULL);
    // Status 2 -> 0 (not applied yet).
    assert(call_rs_status(pack_ret(0.75f, 2), state, view, &nzcv, r16) == 0);
    assert(nzcv == 0x50000000ULL && r16[0] == 0xdeadbeef00000001ULL && r16[1] == 0xcafebabe00000002ULL);
    // Store stub multiplies s0 by 0.75 and records it at state +0x290 and view +0xe5c.
    float scaled = call_rs_store(0.75f, state, view, &nzcv, r16, q30);
    assert(scaled == 0.5625f && get_f32(view, 0xe5c) == 0.5625f && get_f32(state, 0x290) == 0.5625f);
    assert(nzcv == 0x50000000ULL && r16[0] == 0xdeadbeef00000001ULL && r16[1] == 0xcafebabe00000002ULL);
    assert(q30[0] == 10 && q30[1] == 20 && q30[2] == 30 && q30[3] == 40);
    // Next frame: [view + 0xe5c] == state +0x290, so status 2 stays 2 (no extra recompute).
    assert(call_rs_status(pack_ret(0.75f, 2), state, view, &nzcv, r16) == 2);
    // If a scene reset overwrites [view + 0xe5c] back to unscaled 0.75, status 2 -> 0 re-triggers once.
    set_f32(view, 0xe5c, 0.75f);
    assert(call_rs_status(pack_ret(0.75f, 2), state, view, &nzcv, r16) == 0);
    assert(call_rs_store(0.75f, state, view, &nzcv, r16, q30) == 0.5625f);
    assert(call_rs_status(pack_ret(0.75f, 2), state, view, &nzcv, r16) == 2);

    puts("ok");
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='dm-oot-drs-') as tmp:
            work = Path(tmp)
            asm_path, c_path, exe = work / 'stub.s', work / 'test.c', work / 'test'
            asm_path.write_text(asm_src)
            c_path.write_text(c_src)
            comp = subprocess.run(['clang', '-arch', 'arm64', '-O1', '-Wall', '-Wextra', '-Werror',
                                   str(c_path), str(asm_path), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(comp.returncode, 0, comp.stdout + comp.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertEqual(run.stdout.strip(), 'ok')


if __name__ == '__main__':
    unittest.main()
