#include "rt/host.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <dlfcn.h>
#include <immintrin.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

namespace nxaot::rt {

namespace {

// One run at a time per process: the library's imports reach the host through these, and a run ends by a jump back to Execute
Host* g_host = nullptr;
Result* g_result = nullptr;
sigjmp_buf g_run;
volatile sig_atomic_t g_running = 0;
bool g_svc_stops = false;
uint64_t g_clock_origin = 0;

constexpr uint64_t kPage = 0x1000;

uint64_t NowNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
}

std::string Hex(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%#llx", static_cast<unsigned long long>(v));
    return v ? buf : "0x0";
}

[[noreturn]] void Finish(int status, std::string message) {
    g_running = 0;
    g_result->status = status;
    g_result->message = std::move(message);
    siglongjmp(g_run, 1);
}

// A fault or the time limit inside guest code: the facts only (no allocation in a handler), the message is made after the jump
void OnSignal(int sig, siginfo_t* info, void* context) {
    if (!g_running) {
        if (sig == SIGALRM) {
            return;
        }
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    g_running = 0;
    const auto* uc = static_cast<const ucontext_t*>(context);
    g_result->fault_signal = sig;
    g_result->fault_address = reinterpret_cast<uint64_t>(info->si_addr);
    g_result->fault_pc = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
    g_result->status = sig == SIGALRM ? kTimeout : kFault;
    siglongjmp(g_run, 1);
}

void InstallSignals() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    // An alternate stack, so that a native stack overflow in guest code (deep guest recursion) still reaches the handler
    static std::vector<uint8_t> alt(256 * 1024);
    stack_t ss{};
    ss.ss_sp = alt.data();
    ss.ss_size = alt.size();
    sigaltstack(&ss, nullptr);
    struct sigaction sa {};
    sa.sa_sigaction = OnSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (const int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGALRM}) {
        sigaction(sig, &sa, nullptr);
    }
}

bool InArena(uint64_t a, uint64_t n) {
    return a >= kArenaBase && n <= kArenaSize && a - kArenaBase <= kArenaSize - n;
}

}  // namespace

uint32_t GuestMxcsr(uint32_t fpcr) {
    static const uint32_t rc[4] = {0x0000, 0x4000, 0x2000, 0x6000};
    uint32_t m = 0x1F80 | rc[(fpcr >> 22) & 3];
    if ((fpcr >> 24) & 1) {
        m |= 0x8040;
    }
    return m;
}

const char* StatusName(int status) {
    switch (status) {
    case kExited: return "exited";
    case kTrap: return "trap";
    case kUsage: return "usage";
    case kJit: return "jit-needed";
    case kFault: return "fault";
    case kBadSvc: return "bad-svc";
    case kReturned: return "returned";
    case kTimeout: return "timeout";
    case kSvcStop: return "svc";
    default: return "?";
    }
}

std::unique_ptr<Host> Host::Create(const std::string& library, const module::Image& image, const Options& options, std::string* error) {
    std::unique_ptr<Host> h{new Host(image, options)};
    // The arena (section 5.2): reserved, nothing accessible until mapped
    void* want = reinterpret_cast<void*>(kArenaBase);
    void* got = mmap(want, kArenaSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (got != want) {
        *error = "cannot reserve the arena at " + Hex(kArenaBase) + ": " + std::strerror(errno);
        if (got != MAP_FAILED) {
            munmap(got, kArenaSize);
        }
        return nullptr;
    }
    h->module_bytes = (image.bytes.size() + image.bss_size + kPage - 1) & ~(kPage - 1);
    if (kModuleBase + h->module_bytes > kArenaBase + kArenaSize) {
        *error = "the module does not fit the arena";
        return nullptr;
    }
    h->handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h->handle) {
        *error = std::string("dlopen: ") + dlerror();
        return nullptr;
    }
    h->desc = static_cast<const nxaot_module_desc*>(dlsym(h->handle, "nxaot_module"));
    if (!h->desc) {
        *error = library + " has no nxaot_module descriptor";
        return nullptr;
    }
    const nxaot_module_desc& d = *h->desc;
    // The library's identity against the image (section 9.4): compiled from these bytes, for this layout
    if (d.abi != NXAOT_MODULE_ABI) {
        *error = "the library's descriptor has ABI " + std::to_string(d.abi) + ", this host reads " + std::to_string(NXAOT_MODULE_ABI);
        return nullptr;
    }
    if (std::memcmp(d.sha256, image.sha256.data(), 32) != 0 || d.image_size != image.bytes.size() || d.text_end != image.TextEnd() ||
        d.bss_size != image.bss_size) {
        *error = "the library was compiled from another image than " + image.path + " (SHA-256 or layout differ)";
        return nullptr;
    }
    *d.base = kModuleBase;
    for (const nxaot_line_entry* e = d.lines; e && e < d.lines_end; ++e) {
        h->lines.emplace_back(reinterpret_cast<uint64_t>(&e->native) + static_cast<int64_t>(e->native), e->offset);
    }
    std::sort(h->lines.begin(), h->lines.end());
    for (uint64_t i = 0; i < d.fn_count; ++i) {
        const nxaot_fn_entry& f = d.fns[i];
        h->fn_by_native.emplace_back(reinterpret_cast<uint64_t>(&f.body) + static_cast<int64_t>(f.body), f.offset);
    }
    std::sort(h->fn_by_native.begin(), h->fn_by_native.end());
    for (const auto& [addr, size] : {std::pair{kStackBase, kStackSize}, std::pair{kTlsBase, kTlsSize}, std::pair{kScratchBase, kScratchSize}}) {
        if (mprotect(reinterpret_cast<void*>(addr), size, PROT_READ | PROT_WRITE) != 0) {
            *error = std::string("mprotect: ") + std::strerror(errno);
            return nullptr;
        }
    }
    InstallSignals();
    if (!g_clock_origin) {
        g_clock_origin = NowNs();
    }
    return h;
}

Host::~Host() {
    if (handle) {
        dlclose(handle);
    }
    munmap(reinterpret_cast<void*>(kArenaBase), kArenaSize);
}

void Host::ResetImage() {
    auto* m = reinterpret_cast<uint8_t*>(kModuleBase);
    mprotect(m, module_bytes, PROT_READ | PROT_WRITE);
    std::memcpy(m, image.bytes.data(), image.bytes.size());
    std::memset(m + image.bytes.size(), 0, module_bytes - image.bytes.size());
    // Text and read-only data readable and never executable (section 5.3: a host jump into guest text faults at once); data and bss writable
    const uint64_t ro_end = (image.data_off ? image.data_off : image.ro_off + image.ro_size) & ~(kPage - 1);
    if (ro_end) {
        mprotect(m, std::min<uint64_t>(ro_end, module_bytes), PROT_READ);
    }
    std::memset(reinterpret_cast<void*>(kStackBase), 0, kStackSize);
    std::memset(reinterpret_cast<void*>(kTlsBase), 0, kTlsSize);
    std::memset(reinterpret_cast<void*>(kScratchBase), 0, kScratchSize);
}

void Host::Relocate() {
    // As dm/host/loader.c relocates every module it maps: each slot to base + the module offset it holds once relocated (RELA, JMPREL, RELR)
    for (const auto& r : image.relocs) {
        if (r.has_target) {
            const uint64_t v = kModuleBase + r.target;
            std::memcpy(reinterpret_cast<void*>(kModuleBase + r.offset), &v, 8);
        }
    }
}

bool Host::ApplySets(std::string* error) {
    for (const auto& [name, value] : options.sets) {
        const module::Symbol* sym = nullptr;
        for (const auto& s : image.symbols) {
            if (s.name == name && s.Defined()) {
                sym = &s;
            }
        }
        if (!sym) {
            *error = "--set: no symbol " + name + " in " + image.name;
            return false;
        }
        const bool rel = !value.empty() && value[0] == '@';
        char* end = nullptr;
        const uint64_t n = std::strtoull(value.c_str() + (rel ? 1 : 0), &end, 0);
        if (!end || *end) {
            *error = "--set: bad value " + value;
            return false;
        }
        const uint64_t v = rel ? kModuleBase + n : n;
        if (sym->value + 8 > module_bytes) {
            *error = "--set: " + name + " lies outside the image";
            return false;
        }
        std::memcpy(reinterpret_cast<void*>(kModuleBase + sym->value), &v, 8);
    }
    return true;
}

const void* Host::Lookup(uint64_t offset) const {
    const uint32_t mask = desc->lookup_mask;
    for (uint32_t h = nxaot_lookup_hash(offset, mask), n = 0; n <= mask; h = (h + 1) & mask, ++n) {
        const nxaot_lookup_entry& e = desc->lookup[h];
        if (e.offset == offset) {
            return reinterpret_cast<const uint8_t*>(&e.entry) + e.entry;
        }
        if (e.offset == NXAOT_LOOKUP_EMPTY) {
            return nullptr;
        }
    }
    return nullptr;
}

std::vector<uint64_t> Host::LookupOffsets() const {
    std::vector<uint64_t> out;
    for (uint32_t i = 0; i <= desc->lookup_mask; ++i) {
        if (desc->lookup[i].offset != NXAOT_LOOKUP_EMPTY) {
            out.push_back(desc->lookup[i].offset);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string Host::Where(uint64_t pc) const {
    std::string s = "host pc " + Hex(pc);
    auto f = std::upper_bound(fn_by_native.begin(), fn_by_native.end(), std::make_pair(pc, UINT64_MAX));
    if (f == fn_by_native.begin()) {
        return s + ", not in the module's code";
    }
    --f;
    s += ", in the function at module+" + Hex(f->second);
    // The line table: the guest instruction of the nearest entry at or before pc inside that function (exact for memory accesses and calls)
    auto l = std::upper_bound(lines.begin(), lines.end(), std::make_pair(pc, UINT32_MAX));
    if (l != lines.begin()) {
        --l;
        if (l->first >= f->first) {
            s += ", guest instruction module+" + Hex(l->second) + (l->first == pc ? "" : " (nearest line entry before it)");
        }
    }
    return s;
}

void Host::Svc(dm_guest_state* st, uint32_t number) {
    const uint64_t at = st->pc - 4 - kModuleBase;
    if (g_svc_stops) {
        g_result->svc = number;
        Finish(kSvcStop, "");
    }
    switch (number) {
    case 0x27: {  // OutputDebugString(string, length): one line, as nxaot_run and dm/host/hle_svc.c print it
        const uint64_t s = st->x[0], n = st->x[1];
        if (n > 4096 || !InArena(s, n ? n : 1)) {
            Finish(kBadSvc, "[RED] aot: svcOutputDebugString at module+" + Hex(at) + " with a string outside the arena");
        }
        ++g_result->lines;
        if (options.capture) {
            g_result->output.append(reinterpret_cast<const char*>(s), n);
            g_result->output.push_back('\n');
        } else if (!options.quiet) {
            std::fwrite(reinterpret_cast<const void*>(s), 1, n, stdout);
            std::fputc('\n', stdout);
        }
        st->x[0] = 0;
        return;
    }
    case 0x07:  // ExitProcess
        Finish(kExited, "");
    default:
        Finish(kBadSvc, "[RED] aot: svc " + Hex(number) + " at module+" + Hex(at) + ": the corpus run time answers only 0x27 and 0x07");
    }
}

void* Host::Dispatch(dm_guest_state*, uint64_t target, const nxaot_ic_desc** slot, uint64_t site) {
    // The miss path of section 4: the module's lookup table, then a descriptor for the target published in the site's slot
    const bool inside = target >= kModuleBase && target - kModuleBase < image.bytes.size();
    const void* native = inside ? Lookup(target - kModuleBase) : nullptr;
    if (!native) {
        Finish(kJit, "[RED] aot: the indirect branch at module+" + Hex(site) + " to " + Hex(target) +
                         (inside ? " (module+" + Hex(target - kModuleBase) + ")" : std::string(" (outside the module)")) +
                         " has no compiled entry: the JIT fallback (design section 11) is phase 3");
    }
    ++g_result->misses;
    std::unique_ptr<nxaot_ic_desc>& d = descs[target];
    if (!d) {
        d = std::make_unique<nxaot_ic_desc>(nxaot_ic_desc{target, const_cast<void*>(native)});
    }
    __atomic_store_n(slot, d.get(), __ATOMIC_RELEASE);
    return const_cast<void*>(native);
}

void Host::Trap(dm_guest_state*, uint64_t offset, uint32_t reason) {
    g_result->trap = reason;
    g_result->trap_offset = offset;
    const std::string at = "module+" + Hex(offset);
    switch (reason) {
    case NXAOT_TRAP_BRK:
        Finish(kTrap, "[RED] aot: brk at " + at);
    case NXAOT_TRAP_UNDEFINED:
    case NXAOT_TRAP_UNPREDICTABLE:
        Finish(kTrap, "[RED] aot: an undefined or unpredictable instruction at " + at);
    case NXAOT_TRAP_NORETURN:
        Finish(kReturned, "[RED] aot: the call at " + at + " returned, but the analysis says its callee does not return");
    case NXAOT_TRAP_NO_CODE:
        Finish(kJit, "[RED] aot: execution ran into " + at + ", which is no compiled function's entry: the JIT fallback (design section 11) is phase 3");
    case NXAOT_TRAP_UNTRANSLATED:
        Finish(kJit, "[RED] aot: an encoding dynarmic does not translate at " + at + ": the JIT fallback (design section 11) is phase 3");
    case NXAOT_TRAP_JIT_TARGET:
        Finish(kJit, "[RED] aot: a branch to " + at + ", which is no compiled function's entry: the JIT fallback (design section 11) is phase 3");
    case NXAOT_TRAP_JIT_FPCR:
        Finish(kJit, "[RED] aot: msr fpcr at " + at + " left the FPCR the module was compiled for: the JIT fallback (design section 11) is phase 3");
    case NXAOT_TRAP_JIT_REFUSED:
        Finish(kJit, "[RED] aot: the function at " + at + " was refused by the lifter: the JIT fallback (design section 11) is phase 3");
    default:
        Finish(kTrap, "[RED] aot: trap " + std::to_string(reason) + " at " + at);
    }
}

Result Host::Execute(dm_guest_state* st, const void* entry, bool svc_stops, unsigned timeout_ms, bool returning_is_normal) {
    Result r;
    g_host = this;
    g_result = &r;
    g_svc_stops = svc_stops;
    const unsigned host_mxcsr = _mm_getcsr();
    const uint64_t t0 = NowNs();
    if (sigsetjmp(g_run, 1) == 0) {
        if (timeout_ms) {
            itimerval it{};
            it.it_value.tv_sec = timeout_ms / 1000;
            it.it_value.tv_usec = static_cast<suseconds_t>(timeout_ms % 1000) * 1000;
            setitimer(ITIMER_REAL, &it, nullptr);
        }
        g_running = 1;
        _mm_setcsr(GuestMxcsr(desc->fpcr));  // section 7.1: guest code runs with the MXCSR of the FPCR it was compiled for
        desc->enter(st, entry);
        // Section 3.3: a non-local transfer no native frame took reaches the outermost one, which dispatches it as a new call
        while (st->pending) {
            const uint64_t target = st->pending;
            st->pending = 0;
            const bool inside = target >= kModuleBase && target - kModuleBase < image.bytes.size();
            const void* next = inside ? Lookup(target - kModuleBase) : nullptr;
            if (!next) {
                Finish(kJit, "[RED] aot: a non-local transfer to " + Hex(target) +
                                 (inside ? " (module+" + Hex(target - kModuleBase) + ")" : std::string(" (outside the module)")) +
                                 " has neither a native frame to resume nor a compiled entry: the JIT fallback (design section 11) is phase 3");
            }
            desc->enter(st, next);
        }
        g_running = 0;
        r.status = kReturned;
        if (!returning_is_normal) {
            r.message = "[RED] aot: guest code returned to the host (" + Hex(st->x[30]) + ")";
        }
    }
    if (timeout_ms) {
        itimerval it{};
        setitimer(ITIMER_REAL, &it, nullptr);
    }
    _mm_setcsr(host_mxcsr);
    r.seconds = static_cast<double>(NowNs() - t0) * 1e-9;
    if (r.status == kFault) {
        static const char* const names[] = {"", "", "", "", "SIGILL", "SIGTRAP", "", "SIGBUS", "SIGFPE", "", "", "SIGSEGV"};
        const char* name = r.fault_signal > 0 && r.fault_signal < 12 ? names[r.fault_signal] : "a signal";
        r.message = std::string("[RED] aot: ") + name + " at address " + Hex(r.fault_address) + " in guest code (" + Where(r.fault_pc) + ")";
    } else if (r.status == kTimeout) {
        r.message = "[RED] aot: still running after " + std::to_string(timeout_ms) + " ms";
    }
    g_result = nullptr;
    g_svc_stops = false;
    return r;
}

Result Host::RunStart() {
    ResetImage();
    if (options.relocate) {
        Relocate();
    }
    std::string error;
    if (!ApplySets(&error)) {
        Result r;
        r.status = kUsage;
        r.message = "[RED] aot: " + error;
        return r;
    }
    alignas(64) static dm_guest_state st;
    std::memset(&st, 0, sizeof st);
    st.x[0] = 0;        // the homebrew ABI's environment pointer: none (as nxaot_run)
    st.x[1] = ~0ull;    // and the main thread handle
    st.x[30] = kHostReturn;
    st.sp = kStackBase + kStackSize;  // the main thread's guest stack (section 2.4; crt0 sets one of its own)
    st.tpidr_el0 = kTlsBase;          // its TLS block (guest_thread_init's job; crt0 points tpidr_el0 at its own)
    st.excl_valid = 0;                // the local monitor clear (section 6.5)
    const void* start = Lookup(desc->entry);
    if (!start) {
        Result r;
        r.status = kJit;
        r.message = "[RED] aot: the module's entry module+" + Hex(desc->entry) + " has no standard entry";
        return r;
    }
    return Execute(&st, start, false, 0, false);
}

Result Host::Call(uint64_t offset, dm_guest_state* st, bool svc_stops, unsigned timeout_ms) {
    const void* entry = Lookup(offset);
    if (!entry) {
        Result r;
        r.status = kJit;
        r.message = "no standard entry at module+" + Hex(offset);
        return r;
    }
    return Execute(st, entry, svc_stops, timeout_ms, true);
}

}  // namespace nxaot::rt

// ---- What the pack's library imports (include/nxaot/aot_module.h) ----

extern "C" {

__attribute__((visibility("default"))) void dm_aot_svc(dm_guest_state* st, uint32_t number) {
    nxaot::rt::g_host->Svc(st, number);
}

__attribute__((visibility("default"))) void* nxaot_rt_dispatch(dm_guest_state* st, uint64_t target, const nxaot_ic_desc** slot, uint64_t site) {
    return nxaot::rt::g_host->Dispatch(st, target, slot, site);
}

__attribute__((visibility("default"))) void nxaot_rt_trap(dm_guest_state* st, uint64_t offset, uint32_t reason) {
    nxaot::rt::g_host->Trap(st, offset, reason);
}

// Section 8: the guest counter in 19.2 MHz ticks (svcGetSystemTick's unit, guest_ns * 12 / 625)
__attribute__((visibility("default"))) uint64_t dm_aot_cntpct(dm_guest_state*) {
    const uint64_t ns = nxaot::rt::NowNs() - nxaot::rt::g_clock_origin;
    return ns / 625 * 12 + ns % 625 * 12 / 625;
}

}  // extern "C"
