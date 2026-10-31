// Guest thread environment: gate slot = host gate entry; x18 slot cleared; tpidr_el0 slot = ELF TLS block; tpidrro_el0 slot = Horizon ThreadLocalRegion (0x200).
// The SDK's internal critical section takes the current thread object pointer from TLR[0x1f8]; here each thread gets a zero-initialized object allocated and written in.
#include "dm.h"
#define TLS_SIZE 8192
#define TLR_SIZE 0x200
#define THREAD_OBJ_SIZE 0x400
extern void dm_gate(void);            // entry.S: common gate entry
#if !DM_NATIVE_EXEC
// The x86-64 execution engine (aot.h): the TLS block and the thread-local region are guest memory in its arena below 2^39 (design 5.2), and the thread's
// state block holds them as tpidr_el0 / tpidrro_el0 (2.4), which recompiled code and the JIT read; the TSD slots are the Apple rewriter's scheme
void *dm_guest_alloc(size_t n);
void dm_guest_free(void *p, size_t n);
void dm_aot_set_tls(uint64_t tpidr_el0, uint64_t tpidrro_el0);
#endif
static pthread_key_t g_keyown;   // raw pointers of the TLS block / thread-local region this thread allocated (see the end of guest_thread_init)
static pthread_once_t g_keyown_once = PTHREAD_ONCE_INIT;
#if DM_NATIVE_EXEC
static void own_free(void *p) { void **own = p; free(own[0]); free(own[1]); free(own); }
#else
static void own_free(void *p) { void **own = p; dm_guest_free(own[0], TLS_SIZE); dm_guest_free(own[1], TLR_SIZE + THREAD_OBJ_SIZE); free(own); }
#endif
static void own_key_init(void) { if (pthread_key_create(&g_keyown, own_free)) die("cannot create the free key for thread-local blocks"); }
void guest_thread_init(void) {
#if DM_NATIVE_EXEC
    pthread_setspecific(g_keygate, (void *)dm_gate);
    pthread_setspecific(g_key18, 0);
#endif
    // The TLS block and the thread-local region are freed when the thread exits. The guest overwrites the slots of the TLS / thread-local-region keys (it sets its own thread pointer), so they cannot be freed by the current value in the key:
    // the raw pointers that were allocated are recorded separately on this file's own key g_keyown and freed by its destructor (key destructors run at pthread_exit, like crash.c's signal alternate stack; the destructor only calls free, which is safe on a guest thread's small stack too).
    // They used not to be freed: every short-lived thread the game started leaked 8192 + 0x200 + 0x400 = 9728 bytes. The thread-local region and the current thread object are one block (the SDK may also overwrite the TLR[0x1f8] slot)
    pthread_once(&g_keyown_once, own_key_init);
#if DM_NATIVE_EXEC
    void **own = calloc(2, sizeof *own); uint8_t *tlr = calloc(1, TLR_SIZE + THREAD_OBJ_SIZE);
#else
    void **own = calloc(2, sizeof *own); uint8_t *tlr = dm_guest_alloc(TLR_SIZE + THREAD_OBJ_SIZE);   // zero, as calloc's
#endif
    if (!own || !tlr) die("guest thread-local block allocation failed");
#if DM_NATIVE_EXEC
    own[0] = calloc(1, TLS_SIZE); own[1] = tlr;
#else
    own[0] = dm_guest_alloc(TLS_SIZE); own[1] = tlr;
#endif
    if (!own[0]) die("guest thread TLS block allocation failed");
    *(void **)(tlr + 0x1f8) = tlr + TLR_SIZE;               // current thread object
#if DM_NATIVE_EXEC
    pthread_setspecific(g_keytls, own[0]); pthread_setspecific(g_keytlr, tlr);
#else
    dm_aot_set_tls((uint64_t)(uintptr_t)own[0], (uint64_t)(uintptr_t)tlr);   // into the calling thread's state block (aot_thread.c)
#endif
    void **prev = pthread_getspecific(g_keyown); if (prev) own_free(prev);   // re-initialised on the same thread: free the previous set first (the new one is what the slots now point to)
    pthread_setspecific(g_keyown, own);
}
