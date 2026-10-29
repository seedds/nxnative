// Synthetic HLE entries for t_hle_thunks.c: one of every argument and return shape the generator supports (dm/host/tools/gen_hle_thunks.py), and the
// ways sources spell entries (HLE, HLE_FORCE, HLE_FORCE_BIG, an alias as in hle_fs.c, a wrapper macro with # and ## as in hle_net.c / hle_hid.c, a typedef, #if 0).
// The test's CMake rule runs the generator over this file into gen/t_hle_thunks.c. Each entry records what it received in the globals below
#include "../hle.h"
#include <stdbool.h>

uint64_t g_rx[9];      // integer arguments as received, widened
double g_rv[8];        // floating-point arguments as received
int g_calls;           // entries run

typedef uint32_t my_handle;              // a scalar typedef: an x register
typedef void (*my_cb)(int);              // a function pointer typedef: a pointer
typedef struct { int a, b; } my_pair;    // a struct: only by pointer

HLE_FORCE("t_void0", "void(void)", void, t_void0, void) { g_calls++; }
HLE_FORCE("t_u64_ptr", "uint64_t(uint64_t, const void *)", uint64_t, t_u64_ptr, uint64_t a, const void *p) {
    g_calls++; g_rx[0] = a; g_rx[1] = (uint64_t)(uintptr_t)p; return a ^ 0xFFFF000000000000ull;
}
HLE_FORCE("t_res_u32_int", "Result(uint32_t, int): zero-extended return", Result, t_res_u32_int, uint32_t a, int b) {
    g_calls++; g_rx[0] = a; g_rx[1] = (uint64_t)(int64_t)b; return 0x80000000u | a;
}
HLE_FORCE("t_fptr2", "void(float *, float *)", void, t_fptr2, float *a, float *b) { g_calls++; *a = 1.5f; *b = -2.25f; }
HLE_FORCE("t_f2", "void(float, float)", void, t_f2, float a, float b) { g_calls++; g_rv[0] = a; g_rv[1] = b; }
HLE_FORCE("t_d_d_i_d", "double(double, int, double)", double, t_d_d_i_d, double a, int i, double b) {
    g_calls++; g_rv[0] = a; g_rx[0] = (uint64_t)(int64_t)i; g_rv[1] = b; return a * b + i;
}
HLE_FORCE("t_int8", "eight integer arguments", uint64_t, t_int8, uint64_t a0, int a1, uint32_t a2, const char *a3, int64_t a4, unsigned a5, size_t a6, my_handle a7) {
    g_calls++; g_rx[0] = a0; g_rx[1] = (uint64_t)(int64_t)a1; g_rx[2] = a2; g_rx[3] = (uint64_t)(uintptr_t)a3; g_rx[4] = (uint64_t)a4; g_rx[5] = a5; g_rx[6] = a6; g_rx[7] = a7;
    return a0 + a6;
}
HLE_FORCE("t_flt8", "eight floating-point arguments", float, t_flt8, float f0, double f1, float f2, double f3, float f4, double f5, float f6, double f7) {
    g_calls++; g_rv[0] = f0; g_rv[1] = f1; g_rv[2] = f2; g_rv[3] = f3; g_rv[4] = f4; g_rv[5] = f5; g_rv[6] = f6; g_rv[7] = f7; return f0 + f2;
}
HLE_FORCE("t_mixed", "(int, float, uint64_t, double)", int, t_mixed, int a, float b, uint64_t c, double d) {
    g_calls++; g_rx[0] = (uint64_t)(int64_t)a; g_rv[0] = b; g_rx[1] = c; g_rv[1] = d; return -2;   // zero-extended: 0x00000000fffffffe
}
// Nine integer arguments: the ninth in the guest stack's first 8-byte slot (AAPCS64), as hle_codec.c's h_codec_HwOpusMsInit receives its option
HLE_FORCE("t_int9", "nine integer arguments", void, t_int9, int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8) {
    g_calls++; int v[9] = {a0, a1, a2, a3, a4, a5, a6, a7, a8}; for (int i = 0; i < 9; i++) g_rx[i] = (uint64_t)(int64_t)v[i];
}
HLE_FORCE("t_long", "long: sign-extended from the C value", long, t_long, unsigned long n) { g_calls++; g_rx[0] = n; return -(long)n; }
HLE_FORCE("t_ptr_ret", "a pointer return; a struct pointer and a callback typedef", void *, t_ptr_ret, my_pair *p, my_cb cb) { g_calls++; g_rx[0] = (uint64_t)(uintptr_t)cb; return p; }
HLE_FORCE("t_bool_ret", "a sub-32-bit return: zero-extended", bool, t_bool_ret, int v) { g_calls++; return v != 0; }
HLE("t_hle_plain", int, t_hle_plain, int v) { g_calls++; return v + 1; }
HLE_FORCE_BIG("t_big", "HLE_FORCE_BIG", int, t_big, int v) { g_calls++; return v * 2; }

// hle_fs.c's alias
#undef HLE_FORCE
#define HLE_FORCE HLE_FORCE_BIG
HLE_FORCE("t_aliased", "through #define HLE_FORCE HLE_FORCE_BIG", int, t_aliased, int v) { g_calls++; return v * 3; }
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) HLE_FORCE_BIG(mangled, why, ret, cname, __VA_ARGS__)

// A wrapper with # and ##, as hle_hid.c's TOUCH
#define NUMBERED(N) HLE_FORCE("t_numbered" #N "E", "wrapper macro", int, t_numbered_##N, int v) { g_calls++; return v + N; }
NUMBERED(1) NUMBERED(2)
// A wrapper with __VA_ARGS__, as hle_net.c's SKF
#define WRAP(m, cname, ...) HLE_FORCE(m, "wrapper macro with __VA_ARGS__", int, cname, __VA_ARGS__)
WRAP("t_wrapped", t_wrapped, int a, int b) { g_calls++; return a - b; }

#if 0
HLE_FORCE("t_disabled", "in #if 0: not compiled, not generated", int, t_disabled, short s) { return s; }
#endif
