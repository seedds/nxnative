// Horizon's user exception delivery. A fault in guest code (a data abort, an instruction abort, an undefined instruction) that no fault hook takes goes to the handler the game
// set with nn::os::SetUserExceptionHandler, as the kernel delivers it (Atmosphère kern_k_exception / KProcess::EnterUserException): the kernel writes the faulting x0–x8, lr, sp,
// pc, pstate, esr and far to the user exception context area (svcGetInfo InfoType 20) and enters the process with x0 = the exception type and x1 = that area, every other
// register as it was. The SDK's entry is nn::os::detail::UserExceptionHandler: it saves x8–x29 and the FPU registers, builds the UserExceptionInfo on the handler stack (or 0x340
// bytes under the faulting sp), calls the SDK handler and then the game's, and ends with svcReturnFromException (0xf801 when nobody handled it).
// .NET NativeAOT (NativeAOT-NX) throws its NullReferenceException this way: its explicit null checks are loads through the reference (ldr wzr, [x0]), and its handler resumes the
// thread in the runtime's throw with the context it rewrote, never returning. MARVEL Cosmic Invasion's EOS platform is null (the host refuses Epic Online Services) and the game
// reaches it in a managed call. Without a handler the fault is the usual crash report; a handler that returns (unhandled) ends in one as well, with the fault it was given.
#include "hle.h"
#include "dm.h"
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

static _Alignas(0x1000) uint8_t g_ctx[0x1000];
uint8_t *dm_user_exc_ctx(void) { return g_ctx; }

// The faulting context as the kernel writes it (svc::aarch64::ExceptionInfo; the SDK's entry reads these offsets)
typedef struct { uint64_t r[9], lr, sp, pc; uint32_t pstate, afsr0, afsr1, esr; uint64_t far; } exc_info;
_Static_assert(sizeof(exc_info) == 0x78, "ExceptionInfo ABI size");
// svc::ExceptionType
enum { EXC_INSTRUCTION_ABORT = 0x100, EXC_DATA_ABORT = 0x101, EXC_UNALIGNED_PC = 0x102, EXC_UNALIGNED_DATA = 0x103, EXC_UNDEFINED = 0x104 };

static void *g_entry;                    // nn::os::detail::UserExceptionHandler
static uint64_t *g_handler, *g_sdk_handler;   // nn::os::detail::g_UserExceptionHandler / g_SdkUserExceptionHandler: set by SetUserExceptionHandler / SetSdkUserExceptionHandler
static atomic_int g_looked;
static atomic_uint g_delivered;

// What the thread's last delivery was given: a handler that returns ends the run with it (svc ReturnFromException runs on the faulting thread)
static __thread struct { uint64_t pc, lr, fp, sp, far, ns; int sig, type, n; } t_last;

static void look_up(void) {
    if (atomic_load(&g_looked)) return;
    g_entry = guest_sym("_ZN2nn2os6detail20UserExceptionHandlerEv");
    g_handler = guest_sym("_ZN2nn2os6detail22g_UserExceptionHandlerE");
    g_sdk_handler = guest_sym("_ZN2nn2os6detail25g_SdkUserExceptionHandlerE");
    atomic_store(&g_looked, 1);
}

static int exc_type(int sig, uint32_t esr) {
    if (sig == SIGILL) return EXC_UNDEFINED;
    switch (esr >> 26) {
    case 0x20: case 0x21: return EXC_INSTRUCTION_ABORT;
    case 0x22: return EXC_UNALIGNED_PC;
    case 0x26: return EXC_UNALIGNED_DATA;   // sp alignment
    case 0x24: case 0x25: return (esr & 0x3f) == 0x21 ? EXC_UNALIGNED_DATA : EXC_DATA_ABORT;   // DFSC 0x21: alignment fault
    default: return EXC_DATA_ABORT;
    }
}

static const char *at(uint64_t a, char *b, size_t n) {
    module_t *m = module_of((void *)a);
    if (m) snprintf(b, n, "%s+%#llx", m->name, (unsigned long long)(a - (uint64_t)m->base)); else snprintf(b, n, "%#llx", (unsigned long long)a);
    return b;
}

static int deliver(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    uint64_t pc = __darwin_arm_thread_state64_get_pc(*ss), sp = __darwin_arm_thread_state64_get_sp(*ss);
    if (!module_of((void *)pc)) return 0;   // host code: the crash report
    look_up();
    if (!g_entry || !((g_handler && *g_handler) || (g_sdk_handler && *g_sdk_handler))) return 0;
    // A fault again at the same place right away is a loop (the handler resumed into the fault, or the entry itself faults on an overflowed stack): the crash report
    uint64_t ns = now_ns();
    if (t_last.n && t_last.pc == pc && ns - t_last.ns < 10000000ull) return 0;
    if ((uint8_t *)pc >= (uint8_t *)g_entry && (uint8_t *)pc < (uint8_t *)g_entry + 0x1000) return 0;
    uint32_t esr = uc->uc_mcontext->__es.__esr;
    int type = exc_type(sig, esr);
    exc_info *e = (exc_info *)g_ctx;
    memset(e, 0, sizeof *e);
    for (int i = 0; i < 9; i++) e->r[i] = ss->__x[i];
    e->lr = __darwin_arm_thread_state64_get_lr(*ss); e->sp = sp; e->pc = pc;
    e->pstate = ss->__cpsr & 0xf00003ffu; e->esr = esr; e->far = (uint64_t)si->si_addr;
    t_last.pc = pc; t_last.lr = e->lr; t_last.fp = __darwin_arm_thread_state64_get_fp(*ss); t_last.sp = sp; t_last.far = e->far; t_last.ns = ns; t_last.sig = sig; t_last.type = type; t_last.n++;
    unsigned n = atomic_fetch_add(&g_delivered, 1) + 1;
    if (n <= 8 || n % 1000 == 0) {
        char a[64], b[64];
        LOG("user exception #%u: type %#x at %s (address %#llx, lr %s) → the game's handler", n, type, at(pc, a, sizeof a), (unsigned long long)e->far, at(e->lr, b, sizeof b));
    }
    ss->__x[0] = (uint64_t)type; ss->__x[1] = (uint64_t)(uintptr_t)g_ctx;
    ss->__pc = (uint64_t)(uintptr_t)g_entry;
    return 1;
}

void dm_user_exc_return(uint64_t result) {
    char a[64], b[64];
    LOG("[RED] the game's user exception handler returned (svc ReturnFromException %#llx): unhandled, type %#x at %s (address %#llx, lr %s, sp %#llx)", (unsigned long long)result,
        t_last.type, at(t_last.pc, a, sizeof a), (unsigned long long)t_last.far, at(t_last.lr, b, sizeof b), (unsigned long long)t_last.sp);
    dm_backtrace(t_last.fp);
    _exit(128 + (t_last.sig ? t_last.sig : SIGSEGV));
}

__attribute__((constructor)) static void userexc_init(void) { crash_set_exc_hook(deliver); }
