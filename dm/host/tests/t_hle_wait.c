// Real blocking-hook assembly, big stack and thread pause/context SVCs. No game, socket, UI or device is used.
#include "../hle.h"
#include "../hle_thread.c"
#include <mach/mach_time.h>
#include <unistd.h>
struct dm_flags g_flag;
uint64_t g_t0_ns;
__thread _Atomic int dm_host_work;
pthread_key_t g_key18, g_keygate;
extern uint64_t dm_gate_top[1024];
extern const unsigned char dm_gate_stubs[];
void dm_gate_nested(uint64_t sp) { (void)sp; abort(); }
void guest_thread_set_id(uint64_t id) { (void)id; }
void guest_gate_stack(int own) { (void)own; }
void crash_thread_init(void) {}
void guest_thread_init(void) {}
void *guest_sym(const char *name) { (void)name; return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
uint64_t now_ns(void) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return mach_absolute_time() * tb.numer / tb.denom; }
uint64_t guest_ns(void) { return now_ns(); }
int dm_detclock_enabled(void) { return 0; }
void unimpl_c(void) { abort(); }
uint64_t gate_dispatch(uint64_t pc, uint64_t *registers) { (void)pc; (void)registers; abort(); }

static uintptr_t guest_bottom, guest_top;
int dm_guest_as_contains(uintptr_t p) { return p >= guest_bottom && p < guest_top; }
uintptr_t g_wait_test_host_sp;   // used only by the assembly worker before/after its simulated guest call
extern void wait_test_invoke(const uint64_t *, uint64_t *, uintptr_t);
extern const char wait_test_return[];
static uint64_t before[100], after[100], arguments[8];
static thr_t worker;
static pthread_mutex_t M = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t C = PTHREAD_COND_INITIALIZER;
static int entered, release_nested, nested_left, finish_body;
static _Atomic int body_returning, guest_progress, on_big_stack;
static uint64_t native_x18;
static const uint64_t result = UINT64_C(0xfedcba9876543210);
static void await_flag(int *flag) {
    struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += 10;
    pthread_mutex_lock(&M);
    while (!*flag) assert(!pthread_cond_timedwait(&C, &M, &deadline));
    pthread_mutex_unlock(&M);
}
static void set_flag(int *flag) { pthread_mutex_lock(&M); *flag = 1; pthread_cond_broadcast(&C); pthread_mutex_unlock(&M); }

HLE_FORCE_WAIT("test::Blocking", "exercise the production blocking hook boundary", uint64_t, wait_probe,
               uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
    // More than twice the whole guest stack: this must run on the production big stack.
    volatile unsigned char heavy[20000]; heavy[0] = 19; heavy[sizeof heavy - 1] = 23;
    atomic_store(&on_big_stack, !dm_guest_as_contains((uintptr_t)&heavy[0]));
    uint64_t received[] = {a,b,c,d,e,f,g,h}; memcpy(arguments, received, sizeof arguments);
    uint64_t nested[100] = {UINT64_C(0xbadbadbad)};
    thr_svc_enter(nested, UINT64_C(0xbad000));
    set_flag(&entered); await_flag(&release_nested);
    thr_svc_leave();   // a nested exit must wait while paused, without unpublishing the outer guest frame
    set_flag(&nested_left); await_flag(&finish_body);
    assert(heavy[0] == 19 && heavy[sizeof heavy - 1] == 23);
    atomic_store(&body_returning, 1);
    // Exercise restoration even for SIMD registers the host's current libc happens not to use.
    __asm__ volatile(
        "movi v0.16b, #0\n movi v1.16b, #0\n movi v2.16b, #0\n movi v3.16b, #0\n"
        "movi v4.16b, #0\n movi v5.16b, #0\n movi v6.16b, #0\n movi v7.16b, #0\n"
        "movi v8.16b, #0\n movi v9.16b, #0\n movi v10.16b, #0\n movi v11.16b, #0\n"
        "movi v12.16b, #0\n movi v13.16b, #0\n movi v14.16b, #0\n movi v15.16b, #0\n"
        "movi v16.16b, #0\n movi v17.16b, #0\n movi v18.16b, #0\n movi v19.16b, #0\n"
        "movi v20.16b, #0\n movi v21.16b, #0\n movi v22.16b, #0\n movi v23.16b, #0\n"
        "movi v24.16b, #0\n movi v25.16b, #0\n movi v26.16b, #0\n movi v27.16b, #0\n"
        "movi v28.16b, #0\n movi v29.16b, #0\n movi v30.16b, #0\n movi v31.16b, #0\n"
        ::: "v0","v1","v2","v3","v4","v5","v6","v7","v8","v9","v10","v11","v12","v13","v14","v15",
            "v16","v17","v18","v19","v20","v21","v22","v23","v24","v25","v26","v27","v28","v29","v30","v31");
    return result;
}
static void *run_worker(void *unused) {
    (void)unused;
    pthread_setspecific(g_keythr, &worker);
    pthread_setspecific(g_key18, (void *)(uintptr_t)UINT64_C(0x1818181818181818));
    __asm__ volatile("mov %0, x18" : "=r"(native_x18));
    void *gate = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); assert(gate != MAP_FAILED);
    dm_gate_top[0] = (uintptr_t)gate + (1 << 20);
    pthread_setspecific(g_keygate, dm_gate_stubs);
    wait_test_invoke(before, after, guest_top);
    munmap(gate, 1 << 20);
    atomic_store(&guest_progress, 1);
    return NULL;
}
static void activity(int paused) { uint64_t r[32] = {worker.handle, (uint64_t)paused}; assert(svc_thread(0x32, r) && !r[0]); }
static hz_ctx_t context(void) {
    hz_ctx_t out; memset(&out, 0xcc, sizeof out);
    uint64_t r[32] = {(uint64_t)&out, worker.handle}; assert(svc_thread(0x33, r) && !r[0]); return out;
}
static void check_context(const hz_ctx_t *c) {
    for (int i = 0; i < 29; i++) assert(c->r[i] == (i == 18 ? UINT64_C(0x1818181818181818) : before[i]));
    assert(c->fp == before[29] && c->lr == (uintptr_t)wait_test_return && c->pc == (uintptr_t)wait_test_return);
    assert(c->sp == guest_top && c->pstate == before[96] && c->fpsr == before[97] && c->fpcr == before[98]);
    assert(!memcmp(c->v, before + 32, sizeof c->v));
}
int main(void) {
    // A bounded test watchdog only; the implementation has no artificial timeout or forced resume.
    alarm(20);
    pthread_key_create(&g_key18, NULL); pthread_key_create(&g_keygate, NULL); svc_thread_main_init();
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *memory = mmap(NULL, page * 2, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0); assert(memory != MAP_FAILED);
    assert(!mprotect(memory + page, page, PROT_READ | PROT_WRITE));
    memset(memory + page, 0xa5, page);
    guest_bottom = (uintptr_t)memory + page; guest_top = guest_bottom + 8192;   // exactly 8 KiB above the guard
    for (int i = 0; i < 31; i++) before[i] = UINT64_C(0x1111000000000000) + i;
    for (int i = 32; i < 96; i++) before[i] = UINT64_C(0x9999000000000000) + i;
    before[96] = UINT64_C(0xa0000000); before[97] = 8; before[98] = UINT64_C(0x04000000);
    pthread_cond_init(&worker.cv, NULL); pthread_cond_init(&worker.pcv, NULL);
    worker.started = 1; worker.prio = 44; worker.core = 1; worker.mask = 2;
    pthread_mutex_lock(&G); worker.handle = h_new(O_THREAD, &worker); worker.all = g_all; g_all = &worker; pthread_mutex_unlock(&G);
    assert(!pthread_create(&worker.pt, NULL, run_worker, NULL));
    await_flag(&entered);
    assert(atomic_load(&on_big_stack) && !memcmp(arguments, before, sizeof arguments));
    for (uintptr_t p = guest_bottom; p < guest_top - 32; p++) assert(*(unsigned char *)p == 0xa5);
    activity(1); hz_ctx_t first = context(); check_context(&first);
    pthread_mutex_lock(&G); assert(worker.svc_depth == 2 && atomic_load(&worker.svc_regs)); pthread_mutex_unlock(&G);
    set_flag(&release_nested); usleep(20000);
    pthread_mutex_lock(&M); assert(!nested_left); pthread_mutex_unlock(&M);
    hz_ctx_t second = context(); assert(!memcmp(&first, &second, sizeof first));
    activity(0); await_flag(&nested_left);
    activity(1); second = context(); check_context(&second);
    set_flag(&finish_body);
    while (!atomic_load(&body_returning)) sched_yield();
    usleep(20000); assert(!atomic_load(&guest_progress));
    second = context(); assert(!memcmp(&first, &second, sizeof first));   // returning x0 must not replace the captured first argument while paused
    activity(0); assert(!pthread_join(worker.pt, NULL));
    assert(atomic_load(&guest_progress));
    assert(after[0] == result);
    assert(after[18] == native_x18);   // never write Apple's platform register; the guest's x18 is reported through its separate TSD slot
    for (int i = 1; i < 30; i++) if (i != 18) assert(after[i] == before[i]);
    assert(after[30] == (uintptr_t)wait_test_return && after[31] == guest_top);
    assert(!memcmp(after + 32, before + 32, 64 * sizeof(uint64_t)));
    assert(after[96] == before[96] && after[97] == before[97] && after[98] == before[98]);
    assert(!worker.svc_depth && !atomic_load(&worker.svc_regs) && !atomic_load(&worker.svc_state));
    munmap(memory, page * 2);
    puts("PASS: blocking hook context, nested pause/resume, immutable call roots, full register/SIMD ABI, 64-bit return and guarded 8 KiB guest stack");
    return 0;
}
