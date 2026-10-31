// Guest threads of the x86-64 execution engine (docs/design/aot-recompiler.md 6.1, 6.2; aot.h): one host thread per guest thread, as on Apple. Each gets, at
// its first guest code, a state block (64-byte aligned, outside the arena: no guest pointer reaches it) and its alternate signal stack; its native stack is the
// host thread's own, 8 MB for the threads hle_thread.c creates (the guest's code keeps x86 return addresses and spills there, design 3.2), its guest stack the
// one the SDK gives it in guest memory. The live threads are kept in a list, so that a process stop can poke every one (the poll byte compiled code reads at
// function entries and loop back-edges, and its JIT instance halted) and wait until they have left
#include "aot.h"
#if NXHOST_JIT
#include "rt/jit.h"   // dm/tools/aot/src/rt: dm_jit_halt, dm_jit_destroy
#endif

static _Thread_local dm_aot_thread *t_self;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;   // the list; taken after hle_thread.c's G when both are held, never before it
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static dm_aot_thread *g_threads;   // the live threads (g_lock)
static int g_live;
static _Atomic uint64_t g_made;    // threads that ran guest code
// thread.c guest_thread_init's TLS block and thread-local region, kept until the thread's block exists (a guest thread makes it when it starts running)
static _Thread_local uint64_t t_tls, t_tlr;

dm_aot_thread *dm_aot_self(void) { return t_self; }
dm_guest_state *dm_aot_self_state(void) { return t_self ? &t_self->st : NULL; }
uint64_t dm_aot_threads_made(void) { return atomic_load(&g_made); }

dm_guest_state *dm_aot_thread_begin(void) {
    if (t_self) return &t_self->st;
    void *mem = malloc(sizeof(dm_aot_thread) + 63);
    if (!mem) die("aot: no memory for a guest thread's state block");
    dm_aot_thread *t = (dm_aot_thread *)(((uintptr_t)mem + 63) & ~(uintptr_t)63);
    memset(t, 0, sizeof *t);
    t->mem = mem;
    t->st.x[30] = DM_AOT_HOST_RETURN;
    t->st.fpcr = dm_aot_module_fpcr();   // the FPCR the packs are specialised for (7.1); guest code changes it with msr fpcr
    t->st.tpidr_el0 = t_tls; t->st.tpidrro_el0 = t_tlr;   // the TLS registers' initial values (thread.c), 2.4
    crash_thread_init();                 // aot_fault.c: the alternate signal stack, so that a native stack overflow in deep guest recursion still reaches the handler
    pthread_mutex_lock(&g_lock);
    t->next = g_threads; g_threads = t; g_live++;
    if (dm_aot_stop_status() >= 0) __atomic_store_n(&t->st.poll, (uint8_t)1, __ATOMIC_SEQ_CST);   // a stop requested before this thread began
    pthread_mutex_unlock(&g_lock);
    atomic_fetch_add(&g_made, 1);
    t_self = t;
    return &t->st;
}

void dm_aot_thread_end(void) {
    dm_aot_thread *t = t_self;
    if (!t) return;
    if (t->depth) die("aot: a guest thread ended with guest code on its stack");
    pthread_mutex_lock(&g_lock);
    for (dm_aot_thread **p = &g_threads; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; }
    g_live--;
#if NXHOST_JIT
    // Under g_lock: a poke (which finds the instance under it) cannot reach an instance being destroyed
    if (t->jit) { dm_jit_destroy(t->jit); t->jit = NULL; }
#endif
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_lock);
    t_self = NULL;
    if (t->stack_lo) dm_guest_free((void *)(uintptr_t)t->stack_lo, t->stack_size);
    free(t->mem);
}

int dm_aot_thread_run(uint64_t entry, uint64_t arg, uint64_t sp) {
    dm_guest_state *st = dm_aot_thread_begin();
    dm_aot_thread *t = t_self;
    st->sp = sp;
    t->left = DM_AOT_LEFT_NONE;
    dm_guest_call(st, entry, 1, arg);
    if (t->left != DM_AOT_LEFT_NONE) return t->left;
    // The entry returned to the host: Horizon requires svcExitThread (on Apple, dm_thread_returned dies)
    char name[64], why[200];
    dm_aot_guest_name(entry, name, sizeof name);
    snprintf(why, sizeof why, "[RED] aot: the guest thread that started at %s returned from its entry (Horizon requires svcExitThread)", name);
    dm_aot_stop(DM_AOT_RETURNED, why);
    return DM_AOT_LEFT_STOP;
}

void dm_aot_set_tls(uint64_t tpidr_el0, uint64_t tpidrro_el0) {
    t_tls = tpidr_el0; t_tlr = tpidrro_el0;
    if (t_self) { t_self->st.tpidr_el0 = tpidr_el0; t_self->st.tpidrro_el0 = tpidrro_el0; }
}

void dm_aot_thread_exiting(void) { if (t_self) atomic_store(&t_self->exiting, 1); }

void dm_aot_thread_set_jit(void *jit) {
    dm_aot_thread *t = t_self;
    pthread_mutex_lock(&g_lock);
    t->jit = jit;
#if NXHOST_JIT
    if (dm_aot_stop_status() >= 0 || __atomic_load_n(&t->st.poll, __ATOMIC_SEQ_CST)) dm_jit_halt(jit);   // a request made before the instance existed
#endif
    pthread_mutex_unlock(&g_lock);
}

static void poke(dm_aot_thread *t) {   // g_lock held
    __atomic_store_n(&t->st.poll, (uint8_t)1, __ATOMIC_SEQ_CST);
#if NXHOST_JIT
    if (t->jit) dm_jit_halt(t->jit);   // the JIT polls nothing: dm_jit_run returns at the next block boundary
#endif
}

// The caller keeps the block alive (hle_thread.c: under G, which thread_exit holds while it clears thr_t.st before the block goes)
void dm_aot_thread_poke(dm_guest_state *st) {
    if (!st) return;
    pthread_mutex_lock(&g_lock);
    poke((dm_aot_thread *)st);
    pthread_mutex_unlock(&g_lock);
}

int dm_aot_threads_poke_all(void) {
    int n = 0;
    pthread_mutex_lock(&g_lock);
    for (dm_aot_thread *t = g_threads; t; t = t->next) { poke(t); n += t != t_self; }
    pthread_mutex_unlock(&g_lock);
    return n;
}

int dm_aot_threads_live(void) {
    pthread_mutex_lock(&g_lock);
    int n = g_live - (t_self != NULL);
    pthread_mutex_unlock(&g_lock);
    return n;
}

// Wait until the caller is the last live guest thread, poking the others again every 100 ms (one that entered the JIT after its halt, or whose poll byte a
// pause's resume cleared after the stop was requested, sees it then)
int dm_aot_threads_wait(unsigned ms) {
    uint64_t deadline = plat_now_ns() + (uint64_t)ms * 1000000u;
    pthread_mutex_lock(&g_lock);
    int others;
    while ((others = g_live - (t_self != NULL)) > 0 && plat_now_ns() < deadline) {
        for (dm_aot_thread *t = g_threads; t; t = t->next) if (t != t_self) poke(t);
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&g_cv, &g_lock, &ts);
    }
    pthread_mutex_unlock(&g_lock);
    return others > 0 ? others : 0;
}

int dm_aot_main_stack(uint64_t *lo, uint64_t *size) {
    if (!t_self || !t_self->stack_lo) return 0;
    *lo = t_self->stack_lo; *size = t_self->stack_size;
    return 1;
}
