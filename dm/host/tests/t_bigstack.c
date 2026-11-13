// Big-stack entry (hle_bigstack.c, hle.h HLE_FORCE): a take-over called on a small guest stack runs on the thread's host stack with its register and stack arguments intact, and dm_bigstack_caller
// gives back the game's call site both after a switch and straight through off the guest block (the audio Start / Stop / Close log it, PR #721)
#include "../hle.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static uintptr_t g_lo, g_hi;   // the fake guest stack: 16 KB above a guard page
int dm_guest_as_contains(uintptr_t a) { return a >= g_lo && a < g_hi; }

static uintptr_t g_fp;
static void *g_site;
// 64 KB of locals: four times the fake guest stack, so running on it would fault in the guard page
HLE_FORCE("_ZN4test5probeEmmd", "unit test", uint64_t, probe, uint64_t a, uint64_t b, double d) {
    volatile char big[64 << 10];
    big[0] = 1; big[sizeof big - 1] = 2;
    g_fp = (uintptr_t)__builtin_frame_address(0);
    g_site = dm_bigstack_caller(__builtin_return_address(0));
    return a * 3 + b + (uint64_t)d + (uint64_t)(big[0] + big[sizeof big - 1]);
}

// a take-over on the guest stack (HLE_FORCE_DIRECT) that logs, with integer, string and floating-point arguments
HLE_FORCE_DIRECT("_ZN4test4logsEm", "unit test", uint64_t, logs, uint64_t a) {
    g_fp = (uintptr_t)__builtin_frame_address(0);
    LOG("big-stack test line %llu from a %s stack %.2f", (unsigned long long)a, "guest", 2.5);
    return a + 1;
}

// 12 parameters: the last 4 are on the caller's stack (as the video decoder's take-overs), and the entry copies those 32 bytes to the host stack
HLE_FORCE("_ZN4test4manyEmmmmmmmmmmmm", "unit test", uint64_t, many, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8,
          uint64_t a9, uint64_t a10, uint64_t a11, uint64_t a12) {
    g_fp = (uintptr_t)__builtin_frame_address(0);
    return a1 * 1 + a2 * 2 + a3 * 3 + a4 * 4 + a5 * 5 + a6 * 6 + a7 * 7 + a8 * 8 + a9 * 9 + a10 * 10 + a11 * 11 + a12 * 12;
}
// call_args(top, fn): calls fn(1, 2, ..., 12) with its last 4 arguments right below top, so nothing above them belongs to the caller
uint64_t call_args(uintptr_t top, const void *fn);
__asm__(".text\n.p2align 2\n.globl _call_args\n_call_args:\n"
        "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n"
        "    sub x9, x0, #32\n    mov sp, x9\n    mov x16, x1\n"
        "    mov x10, #9\n    mov x11, #10\n    stp x10, x11, [sp]\n    mov x10, #11\n    mov x11, #12\n    stp x10, x11, [sp, #16]\n"
        "    mov x0, #1\n    mov x1, #2\n    mov x2, #3\n    mov x3, #4\n    mov x4, #5\n    mov x5, #6\n    mov x6, #7\n    mov x7, #8\n"
        "    blr x16\n"
        "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n");

// call_on(a, b, d, top, fn): calls fn(a, b, d) with sp = top (0: on this stack) and x17 left as garbage; _call_on_site is the return address the call leaves, the "game's call site"
uint64_t call_on(uint64_t a, uint64_t b, double d, uintptr_t top, const void *fn);
extern const char call_on_site[];
__asm__(".text\n.p2align 2\n.globl _call_on\n_call_on:\n"
        "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n"
        "    cbz x2, 1f\n    mov sp, x2\n"
        "1:  mov x17, #0x7ff0\n"            // garbage in x17: the NVN, Vulkan and GL layers enter without setting it, and a plain entry must carry no stack arguments
        "    blr x3\n"
        ".globl _call_on_site\n_call_on_site:\n"
        "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n");

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

int main(void) {
    size_t guard = 16384, size = 16384;
    uint8_t *m = mmap(NULL, guard + size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }
    mprotect(m, guard, PROT_NONE);
    g_lo = (uintptr_t)m; g_hi = (uintptr_t)m + guard + size;

    // on the guest stack: switched to the host stack, arguments kept, call site given back
    uint64_t r = call_on(5, 7, 2.0, g_hi, probe_bs);
    CHECK(r == 5 * 3 + 7 + 2 + 3, "arguments carried over through the switch");
    CHECK(g_fp && !dm_guest_as_contains(g_fp), "the implementation ran off the guest stack");
    CHECK(g_site == (void *)call_on_site, "dm_bigstack_caller gives the call site back after a switch");

    // a second call on the same thread reuses the thread's host stack
    g_site = NULL; r = call_on(1, 1, 1.0, g_hi, probe_bs);
    CHECK(r == 3 + 1 + 1 + 3 && g_site == (void *)call_on_site, "second switched call");

    // off the guest block: jumped to straight, the return address is already the caller's
    g_site = NULL; r = call_on(2, 0, 0.0, 0, probe_bs);
    CHECK(r == 6 + 3, "arguments on the direct path");
    CHECK(g_site == (void *)call_on_site, "dm_bigstack_caller passes the call site through on the direct path");

    // stack arguments: called from the very top of the guest stack, the 4 on the stack arrive and nothing above them is read
    g_fp = 0; r = call_args(g_hi, many_bs);
    CHECK(r == 650, "stack arguments carried over through the switch");
    CHECK(g_fp && !dm_guest_as_contains(g_fp), "the 12-parameter implementation ran off the guest stack");

    // a log line from a take-over registered through HLE_FORCE_DIRECT: 1 KB of guest stack is left, less than one line's formatting needs (about 2.7 KB), and the line
    // still comes out whole, written on the host stack (dm_log)
    int saved = dup(2), fds[2];
    if (saved < 0 || pipe(fds) || dup2(fds[1], 2) < 0) { perror("pipe"); return 1; }
    r = call_on(41, 0, 0.0, g_lo + guard + 1024, logs);
    fflush(stderr); dup2(saved, 2); close(saved); close(fds[1]);
    char got[256]; ssize_t n = read(fds[0], got, sizeof got - 1); close(fds[0]);
    got[n > 0 ? n : 0] = 0;
    CHECK(r == 42, "the logging take-over returned on its own stack");
    CHECK(g_fp && dm_guest_as_contains(g_fp), "the logging take-over itself ran on the guest stack");
    CHECK(!strcmp(got, "[dm] big-stack test line 41 from a guest stack 2.50\n"), "the log line written from 1 KB of guest stack");

    printf("big-stack entry: %d/%d checks %s\n", checks - fails, checks, fails ? "✗" : "✓");
    return fails != 0;
}
