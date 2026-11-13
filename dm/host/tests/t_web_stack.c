// Offline HTML entry points on an 8 KB guest stack (issue #2541). The viewer is fake: no browser, game files or UI.
#define DM_TEST_WEB_FRONT
#include "../hle_web.c"
#include <assert.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>

static uintptr_t guest_lo, guest_hi;
int dm_guest_as_contains(uintptr_t a) { return a >= guest_lo && a < guest_hi; }
static int held, signals, closes, expect_hidden, on_host;
static atomic_int opened;
static char root[] = "/tmp/nxnative-web-stack-XXXXXX";
void dm_input_hold(int on) { held = on; }
void hle_bind_system_event(void *g, int autoclear) { assert(g == g_ev_web && !autoclear); }
void hle_clear_system_event(void *g) { assert(g == g_ev_web); }
void hle_signal_system_event(void *g) { assert(g == g_ev_web); signals++; }
int front_web_open(const char *dir, const char *path, const char *callback, int hidden, const dm_web_opts *opts) {
    assert(!strcmp(dir, root) && !strcmp(path, "info.html?tip=1"));
    assert(!strcmp(callback, "http://localhost/") && hidden == expect_hidden);
    assert(opts->repeat_delay == 0 && opts->repeat_pulse == 0);
    on_host = !dm_guest_as_contains((uintptr_t)__builtin_frame_address(0));
    atomic_store(&opened, 1);
    return 0;
}
void front_web_show(void) {}
void front_web_send(const char *msg, size_t n) { (void)msg; (void)n; }
void front_web_close(void) { closes++; }
void front_web_shot(const char *path) { (void)path; }

// Call the registered entry, preserving its three register arguments, with only 8 KB above the guard page.
Result call_web(void *a, void *b, void *c, uintptr_t top, const void *fn);
__asm__(".text\n.p2align 2\n.globl _call_web\n_call_web:\n"
        "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n"
        "    mov sp, x3\n    blr x4\n"
        "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n");
static _Alignas(16) uint8_t arg[ARG_SIZE], result[0x1010];
static void *end_page(void *unused) {
    (void)unused;
    while (!atomic_load(&opened)) usleep(1000);
    dm_web_ended(3, "http://localhost/done");
    return NULL;
}
int main(void) {
    size_t page = (size_t)getpagesize();
    uint8_t *m = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(m != MAP_FAILED && !mprotect(m, page, PROT_NONE));
    guest_lo = (uintptr_t)m; guest_hi = guest_lo + page + 8192;
    assert(mkdtemp(root) && !setenv("DM_HTML", root, 1));
    char file[sizeof root + 16]; snprintf(file, sizeof file, "%s/info.html", root);
    FILE *f = fopen(file, "w"); assert(f); fputs("<p>synthetic info page</p>", f); fclose(f);
    *(uint16_t *)arg = 2;
    const char path[16] = "info.html?tip=1";
    *(uint16_t *)(arg + 8) = TLV_DOCUMENT_PATH; *(uint16_t *)(arg + 10) = sizeof path;
    memcpy(arg + 16, path, sizeof path);
    size_t at = 16 + sizeof path;
    uint16_t type = TLV_BOOT_MODE, size = 4; uint32_t mode = BOOT_INITIALLY_HIDDEN;
    memcpy(arg + at, &type, 2); memcpy(arg + at + 2, &size, 2); memcpy(arg + at + 8, &mode, 4);

    // Session Start must carry s, ev and arg intact across the stack switch; it stays hidden until Appear.
    void *ev = NULL; expect_hidden = 1;
    assert(call_web(NULL, &ev, arg, guest_hi, hle_force_h_webs_Start.fn) == RES_OK);
    assert(on_host && ev == g_ev_web && atomic_load(&opened) && !held);
    assert(h_webs_Appear(NULL) == 1 && held);
    h_webs_ReqExit(NULL); assert(!held && signals == 1);
    assert(h_webs_Wait(NULL, result) == RES_OK && *(uint64_t *)result == 2 && closes == 1);

    // Synchronous Show ignores the hidden boot mode and waits for the fake front end's exit result.
    atomic_store(&opened, 0); expect_hidden = 0; on_host = 0;
    pthread_t thread; assert(!pthread_create(&thread, NULL, end_page, NULL));
    assert(call_web(result, arg, NULL, guest_hi, hle_force_h_web_Offline.fn) == RES_OK);
    assert(!pthread_join(thread, NULL));
    assert(on_host && !held && signals == 2 && closes == 2);
    assert(*(uint64_t *)result == 3 && !strcmp((char *)result + 8, "http://localhost/done"));
    assert(*(uint64_t *)(result + 0x1008) == strlen("http://localhost/done") + 1);
    // Missing pages keep the established failure result, including on a small guest stack.
    assert(!unlink(file));
    assert(call_web(result, arg, NULL, guest_hi, hle_force_h_web_Offline.fn) == RES_FAIL);
    assert(!g_web_state && !held);
    assert(!rmdir(root) && !munmap(m, page * 2));
    puts("t_web_stack: session, synchronous result and missing-page paths passed");
}
