"""Execute Ocarina of Time's dynamic-resolution level stub (drs_controller) on ARM64, without game files.

Checks off mode (state +0x40 == 0), threshold scaling by 60 / target_fps when 0 < target_fps < 60
(DM_FPS_TARGET=30: 83 -> 166), unchanged threshold at target 0 and 60, and step-down / climb-up rules.
"""
import importlib.util
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
spec = importlib.util.spec_from_file_location('ocarina_patches', Path(__file__).resolve().parents[2] / 'games/0400486024DEC000/patches.py')
assert spec is not None and spec.loader is not None
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)


class OcarinaDrsStub(unittest.TestCase):
    def test_off_entry_prefix(self):
        # First 4 words are the off-check returning 0 (ldr w17,[x16,#0x40]; cbnz w17,+0x10; mov w0,wzr; ret).
        self.assertEqual(patch.drs_controller[:4], (0xb9404211, 0x35000071, 0x2a1f03e0, 0xd65f03c0))

    @unittest.skipUnless(platform.system() == 'Darwin' and platform.machine() == 'arm64' and shutil.which('clang'), 'needs macOS arm64 and clang')
    def test_drs_controller_threshold_scaling_and_steps(self):
        words = '\n'.join(f'    .long 0x{w:08x}' for w in patch.drs_controller)
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
{words}
'''
        c_src = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int call_drs(void *ctrl, void *state);

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

int main(void) {
    uint8_t ctrl[0x200], state[0x400];

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
