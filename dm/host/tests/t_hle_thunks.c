// The host-call thunks and the host-function page of the x86-64 execution engine (dm/host/tools/gen_hle_thunks.py, dm/host/aot_hostfn.c; docs/design/aot-recompiler.md
// 2.4): the generator's output for the synthetic entries of t_hle_thunks_entries.c, called the way the dispatcher will call it, dm_aot_hostcall(st, dm_guest_fn(fn)),
// with the arguments in the state block as AAPCS64 passes them. Death tests (a missing function, a call outside the slots, a second placement, a write to the page)
// run in a forked child: Linux only, skipped on Windows
#include "gen/t_hle_thunks.c"   // the generated thunks and table: also the canonical declarations of the entries used below
#include "dm.h"
#include <math.h>
#if !defined(_WIN32)
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

extern uint64_t g_rx[9];
extern double g_rv[8];
extern int g_calls;

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

static dm_guest_state st;
static void put_f(int n, float f) { memset(st.v[n], 0, 16); memcpy(st.v[n], &f, sizeof f); }
static void put_d(int n, double d) { memset(st.v[n], 0, 16); memcpy(st.v[n], &d, sizeof d); }
static float get_f(int n) { float f; memcpy(&f, st.v[n], sizeof f); return f; }
static double get_d(int n) { double d; memcpy(&d, st.v[n], sizeof d); return d; }
static int v0_upper_zero(size_t from) { for (size_t i = from; i < 16; i++) if (st.v[0][i]) return 0; return 1; }
// Garbage in every register and lane first: a thunk must read only what its parameters say (the low 32 bits of a w argument, the low lane of a v register)
static void reset(void) {
    memset(&st, 0xA5, sizeof st); memset(g_rx, 0, sizeof g_rx); memset(g_rv, 0, sizeof g_rv);
    st.pc = 0x7100001234ull;
}
static int call(const void *fn) {
    int before = g_calls; uint64_t a = dm_guest_fn(fn);
    dm_aot_hostcall(&st, a);
    return g_calls == before + 1;
}

static void noentry(void) {}
#if !defined(_WIN32)
// 1 when fn(arg) dies with SIGABRT in a child (die() aborts)
static int dies(void (*fn)(uint64_t), uint64_t arg) {
    fflush(stdout); fflush(stderr);
    pid_t p = fork();
    if (p < 0) return 0;
    if (!p) { alarm(10); fn(arg); _exit(0); }
    int ws = 0; waitpid(p, &ws, 0);
    return WIFSIGNALED(ws) && WTERMSIG(ws) == SIGABRT;
}
static void die_guest_fn(uint64_t a) { (void)a; dm_guest_fn((const void *)noentry); }
static void die_hostcall(uint64_t a) { dm_aot_hostcall(&st, a); }
static void die_init_twice(uint64_t a) { (void)a; dm_aot_hostfn_init(dm_aot_hostfn_base(), 4096); }
static void write_page(uint64_t a) { *(volatile uint32_t *)(uintptr_t)a = 0; }
// A placement through dm_aot_hostfn_init before any use: slot i of the given page is hle_thunks[i]. Exit 0 when it holds
static int placed_explicitly(void) {
    fflush(stdout); fflush(stderr);
    pid_t p = fork();
    if (p < 0) return 0;
    if (!p) {
        alarm(10);
        size_t n = plat_page_size() * 2; uint8_t *page = plat_vm_alloc(n);
        if (!page) _exit(2);
        dm_aot_hostfn_init((uint64_t)(uintptr_t)page, n);
        int ok = dm_aot_hostfn_base() == (uint64_t)(uintptr_t)page;
        for (size_t i = 0; i < hle_thunks_count; i++) ok &= dm_guest_fn(hle_thunks[i].fn) == (uint64_t)(uintptr_t)page + 16 * i;
        uint32_t w; memcpy(&w, page + n - 4, 4); ok &= w == DM_AOT_HOSTFN_POISON;   // the whole mapping is poison, not only the slots in use
        _exit(ok ? 0 : 1);
    }
    int ws = 0; waitpid(p, &ws, 0);
    return WIFEXITED(ws) && WEXITSTATUS(ws) == 0;
}
#endif

int main(void) {
#if !defined(_WIN32)
    CHECK(placed_explicitly(), "dm_aot_hostfn_init places the page: slot i at base + 16 i, the whole mapping poisoned");
#endif
    CHECK(hle_thunks_count == 19, "the generator found the 19 synthetic entries (the #if 0 one left out)");
    for (size_t i = 1; i < hle_thunks_count; i++) CHECK(strcmp(hle_thunks[i - 1].mangled, hle_thunks[i].mangled) < 0, "table sorted by mangled name");
    CHECK(dm_aot_hostfn_lookup(0x1000) == NULL, "lookup before any use: no page, NULL");

    // The page: one slot per entry, poison words, the address names the entry
    uint64_t base = dm_aot_hostfn_base();
    CHECK(base && base % plat_page_size() == 0, "page placed on a page boundary");
    CHECK(dm_aot_hostfn_size() == hle_thunks_count * 16, "16 bytes per slot");
    int poison = 1; for (size_t i = 0; i < hle_thunks_count * 4; i++) poison &= ((const uint32_t *)(uintptr_t)base)[i] == 0xD43E1E00u;
    CHECK(poison, "slots hold brk #0xF0F0");
    int slots = 1;
    for (size_t i = 0; i < hle_thunks_count; i++) {
        uint64_t a = dm_guest_fn(hle_thunks[i].fn);
        slots &= a == base + 16 * i && dm_aot_hostfn_lookup(a) == &hle_thunks[i] && !strcmp(dm_aot_hostfn_name(a + 7), hle_thunks[i].cname);
    }
    CHECK(slots, "dm_guest_fn gives slot i to entry i; lookup and name map it back");
    CHECK(dm_aot_hostfn_lookup(base - 16) == NULL && dm_aot_hostfn_lookup(base + 16 * hle_thunks_count) == NULL, "lookup outside the page: NULL");
    CHECK(dm_aot_hostfn_lookup(base + 4) == NULL, "lookup inside a slot, not at its start: NULL");
    CHECK(dm_aot_hostfn_lookup(0) == NULL && dm_aot_hostfn_name(0) == NULL && dm_aot_hostfn_name(base + 16 * hle_thunks_count) == NULL, "name outside the page: NULL");
    CHECK(!strcmp(dm_aot_hostfn_name(dm_guest_fn((const void *)t_int8)), "t_int8"), "crash-report name of a slot");

    // void(void)
    reset(); CHECK(call((const void *)t_void0), "void(void) runs");
    CHECK(st.x[0] == 0xA5A5A5A5A5A5A5A5ull, "void return leaves x0 alone");
    // uint64_t(uint64_t, const void *)
    reset(); st.x[0] = 0x123456789ABCDEF0ull; st.x[1] = (uint64_t)(uintptr_t)&st;
    CHECK(call((const void *)t_u64_ptr), "uint64_t(uint64_t, const void *) runs");
    CHECK(g_rx[0] == 0x123456789ABCDEF0ull && g_rx[1] == (uint64_t)(uintptr_t)&st, "x0 and x1 arrive whole");
    CHECK(st.x[0] == (0x123456789ABCDEF0ull ^ 0xFFFF000000000000ull), "uint64_t return in x0");
    // Result(uint32_t, int): the upper halves of x0 / x1 are garbage on the way in, and x0's upper half is zero on the way out
    reset(); st.x[0] = 0xDEADBEEF00000005ull; st.x[1] = 0x77777777FFFFFFFDull;
    CHECK(call((const void *)t_res_u32_int), "Result(uint32_t, int) runs");
    CHECK(g_rx[0] == 5 && (int64_t)g_rx[1] == -3, "32-bit arguments are the low halves (w registers)");
    CHECK(st.x[0] == 0x0000000080000005ull, "32-bit return zero-extended into x0");
    // void(float *, float *)
    float fa = 0, fb = 0; reset(); st.x[0] = (uint64_t)(uintptr_t)&fa; st.x[1] = (uint64_t)(uintptr_t)&fb;
    CHECK(call((const void *)t_fptr2), "void(float *, float *) runs");
    CHECK(fa == 1.5f && fb == -2.25f, "float pointers in x0 and x1");
    // void(float, float): the low 32 bits of v0 and v1
    reset(); put_f(0, 0.5f); put_f(1, -8.0f); st.v[0][8] = 0x11; st.v[1][4] = 0x22;   // garbage above the float
    CHECK(call((const void *)t_f2), "void(float, float) runs");
    CHECK(g_rv[0] == 0.5 && g_rv[1] == -8.0, "floats from the low lanes of v0 and v1");
    // double(double, int, double): v0, x0, v1; return in v0, the rest of q0 zero
    reset(); put_d(0, 1.25); put_d(1, 4.0); st.x[0] = 0xFFFFFFFF00000003ull;
    CHECK(call((const void *)t_d_d_i_d), "double(double, int, double) runs");
    CHECK(g_rv[0] == 1.25 && g_rx[0] == 3 && g_rv[1] == 4.0, "classes counted apart: v0, x0, v1");
    CHECK(get_d(0) == 1.25 * 4.0 + 3 && v0_upper_zero(8), "double return in v0, upper 64 bits cleared");
    // Eight integer arguments of mixed widths
    reset();
    uint64_t in8[8] = {0x1111111111111111ull, 0xAAAAAAAAFFFFFF85ull, 0xBBBBBBBB80000000ull, (uint64_t)(uintptr_t)"s", 0x8000000000000001ull, 0xCCCCCCCC00000007ull, 0x7766554433221100ull, 0xDDDDDDDD00000042ull};
    memcpy(st.x, in8, sizeof in8);
    CHECK(call((const void *)t_int8), "eight integer arguments run");
    CHECK(g_rx[0] == in8[0] && (int64_t)g_rx[1] == -123 && g_rx[2] == 0x80000000u && g_rx[3] == in8[3] && g_rx[4] == in8[4] && g_rx[5] == 7 && g_rx[6] == in8[6] && g_rx[7] == 0x42,
          "x0..x7, 32-bit ones truncated (a my_handle typedef among them)");
    CHECK(st.x[0] == in8[0] + in8[6], "uint64_t return");
    // Eight floating-point arguments, float and double alternating
    reset(); for (int i = 0; i < 8; i++) { if (i % 2) put_d(i, i + 0.5); else put_f(i, (float)i + 0.25f); st.v[i][12] = 0x5A; }
    CHECK(call((const void *)t_flt8), "eight floating-point arguments run");
    int f8 = 1; for (int i = 0; i < 8; i++) f8 &= g_rv[i] == (i % 2 ? i + 0.5 : i + 0.25);
    CHECK(f8, "v0..v7, float from the low 32 bits, double from the low 64");
    CHECK(get_f(0) == 0.25f + 2.25f && v0_upper_zero(4), "float return in v0, the other 96 bits cleared");
    // (int, float, uint64_t, double)
    reset(); st.x[0] = 0x00000001FFFFFFFFull; put_f(0, 3.5f); st.x[1] = 0xFEDCBA9876543210ull; put_d(1, -0.125);
    CHECK(call((const void *)t_mixed), "(int, float, uint64_t, double) runs");
    CHECK((int64_t)g_rx[0] == -1 && g_rv[0] == 3.5 && g_rx[1] == 0xFEDCBA9876543210ull && g_rv[1] == -0.125, "x0, v0, x1, v1");
    CHECK(st.x[0] == 0x00000000FFFFFFFEull, "negative int return zero-extended");
    // Nine integer arguments: the ninth from the guest stack's first slot
    uint64_t stack[2] = {0x9999999900000008ull, 0}; reset();
    for (int i = 0; i < 8; i++) st.x[i] = 0xEEEEEEEE00000000ull | (uint64_t)i;
    st.sp = (uint64_t)(uintptr_t)stack;
    CHECK(call((const void *)t_int9), "nine integer arguments run");
    int i9 = 1; for (int i = 0; i < 9; i++) i9 &= g_rx[i] == (uint64_t)i;
    CHECK(i9, "x0..x7 and [sp] (8-byte stack slot)");
    // long: the value sign-extended
    reset(); st.x[0] = 5;
    CHECK(call((const void *)t_long), "long(unsigned long) runs");
    CHECK(g_rx[0] == 5 && st.x[0] == (uint64_t)-5ll, "long return keeps its value (-5 in all 64 bits)");
    // A pointer return, pointer typedefs
    reset(); st.x[0] = (uint64_t)(uintptr_t)&st; st.x[1] = 0x7100004000ull;
    CHECK(call((const void *)t_ptr_ret), "void *(my_pair *, my_cb) runs");
    CHECK(st.x[0] == (uint64_t)(uintptr_t)&st && g_rx[0] == 0x7100004000ull, "pointer return in x0, callback typedef as a pointer");
    // A bool return: zero-extended from 8 bits
    reset(); st.x[0] = 0xFFFFFFFF00000000ull;
    CHECK(call((const void *)t_bool_ret) && st.x[0] == 0, "bool false from w0 = 0 (upper half garbage)");
    reset(); st.x[0] = 2;
    CHECK(call((const void *)t_bool_ret) && st.x[0] == 1, "bool true: x0 = 1");
    // HLE, HLE_FORCE_BIG, the alias, the wrapper macros
    reset(); st.x[0] = 41; CHECK(call((const void *)t_hle_plain) && st.x[0] == 42, "HLE entry");
    reset(); st.x[0] = 21; CHECK(call((const void *)t_big) && st.x[0] == 42, "HLE_FORCE_BIG entry");
    reset(); st.x[0] = 14; CHECK(call((const void *)t_aliased) && st.x[0] == 42, "entry through #define HLE_FORCE HLE_FORCE_BIG");
    reset(); st.x[0] = 40; CHECK(call((const void *)t_numbered_2) && st.x[0] == 42, "entry through a wrapper with # and ##");
    CHECK(dm_aot_hostfn_lookup(dm_guest_fn((const void *)t_numbered_1)) && !strcmp(dm_aot_hostfn_lookup(dm_guest_fn((const void *)t_numbered_1))->mangled, "t_numbered1E"),
          "stringized mangled name");
    reset(); st.x[0] = 50; st.x[1] = 8; CHECK(call((const void *)t_wrapped) && st.x[0] == 42, "entry through a wrapper with __VA_ARGS__");

#if !defined(_WIN32)
    CHECK(dies(die_guest_fn, 0), "dm_guest_fn of a function that is no entry dies");
    CHECK(dies(die_hostcall, base + 16 * hle_thunks_count), "host call past the last slot dies");
    CHECK(dies(die_hostcall, base + 8), "host call into the middle of a slot dies");
    CHECK(dies(die_hostcall, 0x1000), "host call outside the page dies");
    CHECK(dies(die_init_twice, 0), "a second placement dies");
    fflush(stdout); fflush(stderr);
    pid_t p = fork();
    if (p == 0) { alarm(10); signal(SIGSEGV, SIG_DFL); signal(SIGBUS, SIG_DFL); write_page(base); _exit(0); }
    int ws = 0; if (p > 0) waitpid(p, &ws, 0);
    CHECK(p > 0 && WIFSIGNALED(ws) && (WTERMSIG(ws) == SIGSEGV || WTERMSIG(ws) == SIGBUS), "the page is read-only");
#else
    printf("death tests skipped on Windows (no fork)\n");
#endif
    printf("== hle_thunks: %d checks, %d failed ==\n", checks, fails);
    return fails != 0;
}
