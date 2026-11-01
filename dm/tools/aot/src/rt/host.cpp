#include "rt/host.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <random>
#include <set>
#include <thread>

#include <dlfcn.h>
#include <immintrin.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#include "rt/jit.h"

namespace nxaot::rt {

// ---- A guest thread (section 6.1): a host thread, its state block and native stack outside the arena, its JIT instance ----

enum Paused : int { kRunning = 0, kPausedPoll = 1, kPausedSvc = 2, kPausedJit = 3 };
enum WaitKind : int { kNoWait = 0, kLockWait = 1, kAddrWait = 2 };

struct Thread {
    uint32_t handle = 0;
    unsigned index = 0;                 // 0: the thread Execute runs on (the main thread)
    dm_guest_state* st = nullptr;
    void* st_mem = nullptr;             // the state block's allocation (worker threads)
    std::atomic<dm_jit*> jit{nullptr};  // made at its first use (section 11)
    pthread_t pt{};
    sigjmp_buf base;
    uint64_t entry = 0;
    bool started = false, exited = false, closed = false, joinable = false;  // under the run's lock
    // The pause protocol of dm/host/hle_thread.c (SetThreadActivity): pause_req = requested, paused = stopped (where); svc_state bit 0 = inside an
    // SVC, bit 1 = a pause was requested (a thread counted as stopped in an SVC cannot leave it without the lock)
    std::atomic<int> pause_req{0}, svc_state{0};
    int paused = kRunning;
    // Waits (under the run's lock)
    int wait_kind = kNoWait;
    uint64_t wait_addr = 0, wait_seq = 0;
    uint32_t wait_tag = 0;
    bool woken = false;
    // A signal in guest code (no allocation in the handler: the message is made after the jump)
    volatile sig_atomic_t running = 0;
    int fault_signal = 0;
    uint64_t fault_address = 0, fault_pc = 0, fault_jit_pc = 0;
    std::vector<uint8_t> altstack;
};

namespace {

constexpr uint64_t kPage = 0x1000;
constexpr uint32_t kFirstHandle = 0x1000;     // thread handles: kFirstHandle + index
constexpr uint32_t kCurrentThread = 0xFFFF8000u;  // Horizon's pseudo-handle of the calling thread
constexpr uint32_t kWaitMask = 0x40000000u;   // Horizon's HandleWaitMask: the waiters bit of a mutex word
// Horizon result codes (module 1, the kernel: description << 9 | 1)
constexpr uint64_t kInvalidAddress = 0xCC01, kOutOfResource = 0xCE01, kInvalidCurrentMemory = 0xD401, kInvalidHandle = 0xE401,
                   kTimedOut = 0xEA01, kOutOfRange = 0xEE01, kInvalidEnumValue = 0xF001, kInvalidState = 0xFA01;

// What ended a run: the first stop request wins
struct StopInfo {
    int status = kReturned;
    std::string message;
    uint32_t svc = 0, trap = 0;
    uint64_t trap_offset = 0;
    int fault_signal = 0;
    uint64_t fault_address = 0, fault_pc = 0;
};

// One run at a time per process: the library's imports reach the host through these
struct RunState {
    Host* host = nullptr;
    Result* result = nullptr;
    bool svc_stops = false;
    std::mutex lock;                 // G of dm/host/hle_thread.c: every wait and the pause protocol
    std::condition_variable cv;      // one for every waiter (correctness first, as the host's)
    std::vector<std::unique_ptr<Thread>> threads;  // by index
    std::atomic<bool> stop{false};
    bool stopped = false;
    StopInfo why;
    uint64_t seq = 0;
    std::mutex out;                  // svcOutputDebugString lines
    std::set<uint64_t> jit_addresses;  // where dm_jit_run started (under lock)
};
RunState g_run;
thread_local Thread* t_self = nullptr;
const bool g_trace = std::getenv("NXAOT_JIT_TRACE") != nullptr;  // every JIT run and native call of the JIT driver on standard error
uint64_t g_clock_origin = 0;

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

bool InArena(uint64_t a, uint64_t n) {
    return a >= kArenaBase && n <= kArenaSize && a - kArenaBase <= kArenaSize - n;
}

// Every guest thread stops: the flag its polls, SVCs and waits read, its poll byte, its JIT halted, every wait woken
void StopAll() {
    g_run.stop = true;
    for (auto& t : g_run.threads) {
        __atomic_store_n(&t->st->poll, uint8_t{1}, __ATOMIC_RELAXED);
        if (dm_jit* j = t->jit.load()) {
            dm_jit_halt(j);
        }
    }
    g_run.cv.notify_all();
}

void RequestStop(StopInfo info) {
    std::lock_guard<std::mutex> guard{g_run.lock};
    if (!g_run.stopped) {
        g_run.stopped = true;
        g_run.why = std::move(info);
    }
    StopAll();
}

// The calling guest thread leaves: back to its base (Execute or the thread's body), which ends it
[[noreturn]] void LeaveThread() {
    siglongjmp(t_self->base, 1);
}

void CheckStop() {
    if (g_run.stop.load(std::memory_order_relaxed)) {
        LeaveThread();
    }
}

// ---- Signals ----

// A fault or the time limit inside guest code (compiled, or the JIT's memory callbacks): the facts only, the message is made after the jump
void OnSignal(int sig, siginfo_t* info, void* context) {
    Thread* t = t_self;
    if (!t || !t->running) {
        if (sig == SIGALRM) {
            return;
        }
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    t->running = 0;
    const auto* uc = static_cast<const ucontext_t*>(context);
    t->fault_signal = sig;
    t->fault_address = reinterpret_cast<uint64_t>(info->si_addr);
    t->fault_pc = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
    t->fault_jit_pc = dm_jit_access_pc();
    siglongjmp(t->base, 2);
}

// An alternate stack per thread, so that a native stack overflow in guest code (deep guest recursion) still reaches the handler
void InstallAltStack(Thread& t) {
    t.altstack.resize(256 * 1024);
    stack_t ss{};
    ss.ss_sp = t.altstack.data();
    ss.ss_size = t.altstack.size();
    sigaltstack(&ss, nullptr);
}

void InstallSignals() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    static std::vector<uint8_t> alt(256 * 1024);  // the alternate stack of the thread that creates the first host (the main thread)
    stack_t ss{};
    ss.ss_sp = alt.data();
    ss.ss_size = alt.size();
    sigaltstack(&ss, nullptr);
    // Before dynarmic's: its handler (installed with the first JIT instance) takes faults in its code cache and hands every other fault to
    // this one (dm/third_party/dynarmic/src/dynarmic/backend/exception_handler_posix.cpp)
    struct sigaction sa {};
    sa.sa_sigaction = OnSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (const int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGALRM}) {
        sigaction(sig, &sa, nullptr);
    }
}

void BlockAlarm() {
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &s, nullptr);
}

const char* SignalName(int sig) {
    switch (sig) {
    case SIGILL: return "SIGILL";
    case SIGTRAP: return "SIGTRAP";
    case SIGBUS: return "SIGBUS";
    case SIGFPE: return "SIGFPE";
    case SIGSEGV: return "SIGSEGV";
    default: return "a signal";
    }
}

// ---- The pause protocol (section 6.2; dm/host/hle_thread.c, "Pausing threads") ----

// The thread stops here while a pause is requested: at a poll (st holds the whole register file and pc), in the JIT (after dm_jit_run copied
// its state to st), at the exit of an SVC
void WaitWhilePaused(Thread& t, int where, std::unique_lock<std::mutex>& lk) {
    while (t.pause_req.load() && !g_run.stop) {
        t.paused = where;
        g_run.cv.notify_all();
        g_run.cv.wait(lk);
    }
    t.paused = kRunning;
}

void SafepointAt(Thread& t, int where) {
    CheckStop();
    {
        std::unique_lock<std::mutex> lk{g_run.lock};
        WaitWhilePaused(t, where, lk);
    }
    CheckStop();
}

void SvcEnter(Thread& t) {
    t.svc_state.fetch_or(1);
    if (t.pause_req.load()) {
        std::lock_guard<std::mutex> guard{g_run.lock};
        if (t.pause_req.load() && t.paused == kRunning) {
            t.paused = kPausedSvc;  // the block was complete when the SVC was entered
            g_run.cv.notify_all();
        }
    }
}

void SvcLeave(Thread& t) {
    int in = 1;
    if (t.svc_state.compare_exchange_strong(in, 0)) {
        return;  // no pause requested: the lock is not needed
    }
    std::unique_lock<std::mutex> lk{g_run.lock};
    WaitWhilePaused(t, kPausedSvc, lk);
    t.svc_state.fetch_and(~1);
}

// ---- Guest memory for the SVCs (the arena; 4-byte words read and written atomically, as the kernel does) ----

bool GuestWord(uint64_t addr) {
    return (addr & 3) == 0 && InArena(addr, 4);
}
uint32_t Load32(uint64_t addr) {
    return __atomic_load_n(reinterpret_cast<const uint32_t*>(addr), __ATOMIC_SEQ_CST);
}
void Store32(uint64_t addr, uint32_t v) {
    __atomic_store_n(reinterpret_cast<uint32_t*>(addr), v, __ATOMIC_SEQ_CST);
}

Thread* ByHandle(uint32_t h) {  // under the lock
    if (h == kCurrentThread) {
        return t_self;
    }
    if (h < kFirstHandle || h - kFirstHandle >= g_run.threads.size()) {
        return nullptr;
    }
    Thread* t = g_run.threads[h - kFirstHandle].get();
    return t->closed ? nullptr : t;
}

void* ThreadMain(void* p) {
    auto* t = static_cast<Thread*>(p);
    g_run.host->ThreadBody(*t);
    return nullptr;
}

// The JIT's view of the host (src/rt/jit.h)
int JitStopAt(void*, uint64_t pc) {
    return g_run.host ? g_run.host->StopKind(pc) : 0;
}
int JitSvc(void*, dm_guest_state* st, uint32_t number) {
    return g_run.host->Svc(st, number);
}
uint64_t Counter() {
    const uint64_t ns = NowNs() - g_clock_origin;
    return ns / 625 * 12 + ns % 625 * 12 / 625;  // section 8: 19.2 MHz ticks, guest_ns * 12 / 625 as svcGetSystemTick
}
uint64_t JitCounter(void*, dm_guest_state*) {
    return Counter();
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
    case kPauseFailed: return "pause-failed";
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
    // The function map and the line table (section 3.5; ABI 3: a run of {native - body, offset} per function), as absolute native addresses
    for (uint64_t i = 0; i < d.fn_count; ++i) {
        const nxaot_fn_entry& f = d.fns[i];
        const uint64_t body = reinterpret_cast<uint64_t>(&f.body) + static_cast<int64_t>(f.body);
        h->fn_by_native.emplace_back(body, f.offset);
        if (d.fn_lines && d.lines) {
            for (uint32_t k = 0; k < d.fn_lines[i].count; ++k) {
                const nxaot_line_entry& e = d.lines[d.fn_lines[i].first + k];
                h->lines.emplace_back(body + e.native, e.offset);
            }
        }
    }
    std::sort(h->lines.begin(), h->lines.end());
    std::sort(h->fn_by_native.begin(), h->fn_by_native.end());
    for (const auto& [addr, size] : {std::pair{kStackBase, kStackSize}, std::pair{kTlsBase, kTlsSize}, std::pair{kScratchBase, kScratchSize}}) {
        if (mprotect(reinterpret_cast<void*>(addr), size, PROT_READ | PROT_WRITE) != 0) {
            *error = std::string("mprotect: ") + std::strerror(errno);
            return nullptr;
        }
    }
    // The corpus's register-invariant loop (corpus/rt/pause.S), which the pause test checks paused threads against
    for (const auto& s : image.symbols) {
        if (s.name == "nxaot_pause_loop_head" && s.Defined()) {
            h->pause_head = kModuleBase + s.value;
        }
    }
    InstallSignals();
    if (!g_clock_origin) {
        g_clock_origin = NowNs();
    }
    // The JIT fallback (section 11): the module's text, its FPCR, the monitor's processors, 16 MB of code per instance
    dm_jit_host jh{};
    jh.code_lo = kModuleBase + image.text_off;
    jh.code_hi = kModuleBase + image.TextEnd();
    jh.fpcr = d.fpcr & 0x07C80000u;
    jh.max_threads = kMaxThreads;
    jh.code_cache = 16u * 1024 * 1024;
    jh.stop_at = JitStopAt;
    jh.svc = JitSvc;
    jh.cntpct = JitCounter;
    dm_jit_init(&jh);
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

const nxaot_lookup_entry* Host::Find(uint64_t offset) const {
    const uint32_t mask = desc->lookup_mask;
    for (uint32_t h = nxaot_lookup_hash(offset, mask), n = 0; n <= mask; h = (h + 1) & mask, ++n) {
        const nxaot_lookup_entry& e = desc->lookup[h];
        if (e.offset == offset) {
            return &e;
        }
        if (e.offset == NXAOT_LOOKUP_EMPTY) {
            return nullptr;
        }
    }
    return nullptr;
}

const void* Host::Lookup(uint64_t offset) const {
    const nxaot_lookup_entry* e = Find(offset);
    return e ? reinterpret_cast<const uint8_t*>(&e->entry) + e->entry : nullptr;
}

bool Host::IsReturn(uint64_t offset) const {
    return offset < UINT32_MAX && std::binary_search(desc->returns, desc->returns + desc->return_count, static_cast<uint32_t>(offset));
}

int Host::StopKind(uint64_t pc) const {
    // Where the JIT stops (src/rt/jit.h): compiled entries (continued natively) and compiled call sites' returns (a native frame may wait
    // there); none when everything runs in the JIT
    if (options.jit == JitMode::All || !InModule(pc)) {
        return 0;
    }
    const uint64_t off = pc - kModuleBase;
    if (Lookup(off)) {
        return DM_JIT_ENTRY;
    }
    return IsReturn(off) ? DM_JIT_RETURN : 0;
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

// ---- SVCs (section 2.4; the thread and synchronization SVCs of section 6.1 on pthreads) ----

int Host::Svc(dm_guest_state* st, uint32_t number) {
    Thread& t = *t_self;
    const uint64_t at = st->pc - 4 - kModuleBase;
    const auto bad = [&](const std::string& what) {
        StopInfo i;
        i.status = kBadSvc;
        i.message = "[RED] aot: svc " + Hex(number) + " at module+" + Hex(at) + ": " + what;
        RequestStop(std::move(i));
        return 1;
    };
    if (g_run.stop) {
        return 1;
    }
    if (g_run.svc_stops) {
        StopInfo i;
        i.status = kSvcStop;
        i.svc = number;
        RequestStop(std::move(i));
        return 1;
    }
    SvcEnter(t);
    int leave = 0;
    uint64_t* x = st->x;
    switch (number) {
    case 0x27: {  // OutputDebugString(string, length): one line, as nxaot_run and dm/host/hle_svc.c print it
        const uint64_t s = x[0], n = x[1];
        if (n > 4096 || !InArena(s, n ? n : 1)) {
            leave = bad("svcOutputDebugString with a string outside the arena");
            break;
        }
        {
            std::lock_guard<std::mutex> guard{g_run.out};
            ++g_run.result->lines;
            if (options.capture) {
                g_run.result->output.append(reinterpret_cast<const char*>(s), n);
                g_run.result->output.push_back('\n');
            } else if (!options.quiet) {
                std::fwrite(reinterpret_cast<const void*>(s), 1, n, stdout);
                std::fputc('\n', stdout);
            }
        }
        x[0] = 0;
        break;
    }
    case 0x07: {  // ExitProcess
        StopInfo i;
        i.status = kExited;
        RequestStop(std::move(i));
        leave = 1;
        break;
    }
    case 0x08: {  // CreateThread(x1 entry, x2 arg, x3 stack top, w4 priority, w5 core): w0 result, w1 handle
        const uint64_t entry = x[1], arg = x[2], top = x[3];
        if (!InModule(entry) || !image.InText(entry - kModuleBase) || (top & 15) || !InArena(top - 16, 16)) {
            x[0] = kInvalidCurrentMemory;
            break;
        }
        std::lock_guard<std::mutex> guard{g_run.lock};
        if (g_run.threads.size() >= kMaxThreads) {
            x[0] = kOutOfResource;
            break;
        }
        auto n = std::make_unique<Thread>();
        n->index = static_cast<unsigned>(g_run.threads.size());
        n->handle = kFirstHandle + n->index;
        n->entry = entry;
        n->st_mem = std::aligned_alloc(64, sizeof(dm_guest_state));
        n->st = static_cast<dm_guest_state*>(n->st_mem);
        std::memset(n->st, 0, sizeof *n->st);
        n->st->x[0] = arg;
        n->st->x[30] = kHostReturn;  // a thread's entry must end with svcExitThread (Horizon); a return comes back to the host
        n->st->sp = top;
        n->st->fpcr = desc->fpcr;
        x[0] = 0;
        x[1] = n->handle;
        ++g_run.result->threads;
        g_run.threads.push_back(std::move(n));
        break;
    }
    case 0x09: {  // StartThread(w0 handle)
        std::lock_guard<std::mutex> guard{g_run.lock};
        Thread* n = ByHandle(static_cast<uint32_t>(x[0]));
        if (!n || n == &t) {
            x[0] = kInvalidHandle;
            break;
        }
        if (n->started) {
            x[0] = kInvalidState;
            break;
        }
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 8u << 20);  // the native stack (section 6.1: at least 8 MB)
        const int rc = pthread_create(&n->pt, &attr, ThreadMain, n);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            x[0] = kOutOfResource;
            break;
        }
        n->started = true;
        n->joinable = true;
        x[0] = 0;
        break;
    }
    case 0x0A:  // ExitThread
        if (t.index == 0) {
            leave = bad("svcExitThread on the main thread: the corpus run time ends the process through svcExitProcess");
            break;
        }
        leave = 1;
        break;
    case 0x0B: {  // SleepThread(x0 nanoseconds; 0, -1, -2: yield)
        const int64_t ns = static_cast<int64_t>(x[0]);
        if (ns <= 0) {
            sched_yield();
        } else {
            std::unique_lock<std::mutex> lk{g_run.lock};
            g_run.cv.wait_for(lk, std::chrono::nanoseconds(ns), [] { return g_run.stop.load(); });
        }
        break;
    }
    case 0x16: {  // CloseHandle(w0)
        std::lock_guard<std::mutex> guard{g_run.lock};
        Thread* n = ByHandle(static_cast<uint32_t>(x[0]));
        if (!n || n == &t) {
            x[0] = kInvalidHandle;
            break;
        }
        n->closed = true;
        x[0] = 0;
        break;
    }
    case 0x18: {  // WaitSynchronization(x1 handles, w2 count, x3 timeout): w0 result, w1 index of the signalled one (threads: exited)
        const uint64_t handles = x[1];
        const int32_t count = static_cast<int32_t>(x[2]);
        const int64_t timeout = static_cast<int64_t>(x[3]);
        if (count < 0 || count > 0x40) {
            x[0] = kOutOfRange;
            break;
        }
        if (count && !InArena(handles, 4u * static_cast<uint64_t>(count))) {
            x[0] = kInvalidCurrentMemory;
            break;
        }
        std::unique_lock<std::mutex> lk{g_run.lock};
        std::vector<Thread*> ws;
        for (int32_t i = 0; i < count; ++i) {
            Thread* n = ByHandle(Load32(handles + 4u * static_cast<uint64_t>(i)));
            if (!n) {
                break;
            }
            ws.push_back(n);
        }
        if (ws.size() != static_cast<size_t>(count)) {
            x[0] = kInvalidHandle;
            break;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout > 0 ? timeout : 0);
        for (;;) {
            const auto it = std::find_if(ws.begin(), ws.end(), [](const Thread* n) { return n->exited; });
            if (it != ws.end()) {
                x[0] = 0;
                x[1] = static_cast<uint64_t>(it - ws.begin());
                break;
            }
            if (g_run.stop) {
                leave = 1;
                break;
            }
            if (timeout == 0 || (timeout > 0 && std::chrono::steady_clock::now() >= deadline)) {
                x[0] = kTimedOut;
                break;
            }
            if (timeout > 0) {
                g_run.cv.wait_until(lk, deadline);
            } else {
                g_run.cv.wait(lk);
            }
        }
        break;
    }
    case 0x1A: {  // ArbitrateLock(w0 owner handle, x1 address, w2 this thread's tag): wait until the owner hands the lock over
        const uint32_t owner = static_cast<uint32_t>(x[0]), tag = static_cast<uint32_t>(x[2]);
        const uint64_t addr = x[1];
        if (!GuestWord(addr)) {
            x[0] = kInvalidAddress;
            break;
        }
        std::unique_lock<std::mutex> lk{g_run.lock};
        if (Load32(addr) != (owner | kWaitMask)) {
            x[0] = 0;  // the lock changed meanwhile: the guest tries again
            break;
        }
        if (!ByHandle(owner)) {
            x[0] = kInvalidHandle;
            break;
        }
        t.wait_kind = kLockWait;
        t.wait_addr = addr;
        t.wait_tag = tag;
        t.wait_seq = ++g_run.seq;
        t.woken = false;
        while (!t.woken && !g_run.stop) {
            g_run.cv.wait(lk);
        }
        t.wait_kind = kNoWait;
        if (!t.woken) {
            leave = 1;
            break;
        }
        x[0] = 0;
        break;
    }
    case 0x1B: {  // ArbitrateUnlock(x0 address): the first waiter takes the lock (the waiters bit stays when more wait), else it is free
        const uint64_t addr = x[0];
        if (!GuestWord(addr)) {
            x[0] = kInvalidAddress;
            break;
        }
        std::lock_guard<std::mutex> guard{g_run.lock};
        Thread* first = nullptr;
        unsigned waiters = 0;
        for (auto& n : g_run.threads) {
            if (n->wait_kind == kLockWait && n->wait_addr == addr && !n->woken) {
                ++waiters;
                if (!first || n->wait_seq < first->wait_seq) {
                    first = n.get();
                }
            }
        }
        if (!first) {
            Store32(addr, 0);
        } else {
            Store32(addr, first->wait_tag | (waiters > 1 ? kWaitMask : 0));
            first->woken = true;
            g_run.cv.notify_all();
        }
        x[0] = 0;
        break;
    }
    case 0x34: {  // WaitForAddress(x0 address, w1 type, w2 value, x3 timeout)
        const uint64_t addr = x[0];
        const uint32_t type = static_cast<uint32_t>(x[1]);
        const int32_t value = static_cast<int32_t>(x[2]);
        const int64_t timeout = static_cast<int64_t>(x[3]);
        if (!GuestWord(addr)) {
            x[0] = kInvalidAddress;
            break;
        }
        std::unique_lock<std::mutex> lk{g_run.lock};
        const int32_t cur = static_cast<int32_t>(Load32(addr));
        if (type == 0 || type == 1) {  // WaitIfLessThan, DecrementAndWaitIfLessThan
            if (!(cur < value)) {
                x[0] = kInvalidState;
                break;
            }
            if (type == 1) {
                Store32(addr, static_cast<uint32_t>(cur - 1));
            }
        } else if (type == 2) {  // WaitIfEqual
            if (cur != value) {
                x[0] = kInvalidState;
                break;
            }
        } else {
            x[0] = kInvalidEnumValue;
            break;
        }
        if (timeout == 0) {
            x[0] = kTimedOut;
            break;
        }
        t.wait_kind = kAddrWait;
        t.wait_addr = addr;
        t.wait_seq = ++g_run.seq;
        t.woken = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout > 0 ? timeout : 0);
        while (!t.woken && !g_run.stop) {
            if (timeout > 0) {
                if (g_run.cv.wait_until(lk, deadline) == std::cv_status::timeout && !t.woken) {
                    break;
                }
            } else {
                g_run.cv.wait(lk);
            }
        }
        t.wait_kind = kNoWait;
        if (g_run.stop && !t.woken) {
            leave = 1;
            break;
        }
        x[0] = t.woken ? 0 : kTimedOut;
        break;
    }
    case 0x35: {  // SignalToAddress(x0 address, w1 type, w2 value, w3 count): wakes count waiters (all when count <= 0), first come first
        const uint64_t addr = x[0];
        const uint32_t type = static_cast<uint32_t>(x[1]);
        const int32_t value = static_cast<int32_t>(x[2]), count = static_cast<int32_t>(x[3]);
        if (!GuestWord(addr)) {
            x[0] = kInvalidAddress;
            break;
        }
        std::lock_guard<std::mutex> guard{g_run.lock};
        if (type == 1) {  // SignalAndIncrementIfEqual
            if (static_cast<int32_t>(Load32(addr)) != value) {
                x[0] = kInvalidState;
                break;
            }
            Store32(addr, static_cast<uint32_t>(value + 1));
        } else if (type != 0) {  // SignalAndModifyByWaiterCountIfEqual: not in the corpus's set
            leave = bad("SignalToAddress of type " + std::to_string(type) + ": the corpus run time answers types 0 and 1");
            break;
        }
        std::vector<Thread*> ws;
        for (auto& n : g_run.threads) {
            if (n->wait_kind == kAddrWait && n->wait_addr == addr && !n->woken) {
                ws.push_back(n.get());
            }
        }
        std::sort(ws.begin(), ws.end(), [](const Thread* a, const Thread* b) { return a->wait_seq < b->wait_seq; });
        for (size_t i = 0; i < ws.size() && (count <= 0 || i < static_cast<size_t>(count)); ++i) {
            ws[i]->woken = true;
        }
        g_run.cv.notify_all();
        x[0] = 0;
        break;
    }
    default:
        leave = bad("the corpus run time answers 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x16, 0x18, 0x1a, 0x1b, 0x27, 0x34 and 0x35");
        break;
    }
    SvcLeave(t);
    return leave || g_run.stop ? 1 : 0;
}

// ---- The dispatcher's miss path (section 4) ----

void* Host::Dispatch(dm_guest_state* st, uint64_t target, const nxaot_ic_desc** slot, uint64_t site) {
    // The module's lookup table, then a descriptor for the target published in the site's slot
    const bool inside = InModule(target);
    const void* native = inside && options.jit != JitMode::All ? Lookup(target - kModuleBase) : nullptr;
    if (!native) {
        if (options.jit == JitMode::None) {
            StopInfo i;
            i.status = kJit;
            i.message = "[RED] aot: the indirect branch at module+" + Hex(site) + " to " + Hex(target) +
                        (inside ? " (module+" + Hex(target - kModuleBase) + ")" : std::string(" (outside the module)")) +
                        " has no compiled entry, and the JIT fallback is off (--jit-none)";
            RequestStop(std::move(i));
            LeaveThread();
        }
        // No compiled entry: the JIT fallback through jit_entry (section 11, case 1); the slot keeps its descriptor, so the next call misses
        // again and finds the target in st->pc
        st->pc = target;
        st->exit_reason = NXAOT_TRAP_JIT_MISS;
        return const_cast<void*>(desc->jit_entry);
    }
    __atomic_fetch_add(&g_run.result->misses, 1, __ATOMIC_RELAXED);
    std::lock_guard<std::mutex> guard{g_run.lock};
    std::unique_ptr<nxaot_ic_desc>& d = descs[target];
    if (!d) {
        d = std::make_unique<nxaot_ic_desc>(nxaot_ic_desc{target, const_cast<void*>(native)});
    }
    __atomic_store_n(slot, d.get(), __ATOMIC_RELEASE);
    return const_cast<void*>(native);
}

void Host::Trap(dm_guest_state*, uint64_t offset, uint32_t reason) {
    StopInfo i;
    i.trap = reason;
    i.trap_offset = offset;
    const std::string at = "module+" + Hex(offset);
    switch (reason) {
    case NXAOT_TRAP_BRK:
        i.status = kTrap;
        i.message = "[RED] aot: brk at " + at;
        break;
    case NXAOT_TRAP_UNDEFINED:
    case NXAOT_TRAP_UNPREDICTABLE:
        i.status = kTrap;
        i.message = "[RED] aot: an undefined or unpredictable instruction at " + at;
        break;
    case NXAOT_TRAP_NORETURN:
        i.status = kReturned;
        i.message = "[RED] aot: the call at " + at + " returned, but the analysis says its callee does not return";
        break;
    case NXAOT_TRAP_UNTRANSLATED:
        i.status = kTrap;
        i.message = "[RED] aot: an encoding neither the lifter nor the JIT fallback translates at " + at;
        break;
    default:
        i.status = kTrap;
        i.message = "[RED] aot: trap " + std::to_string(reason) + " at " + at;
        break;
    }
    RequestStop(std::move(i));
    LeaveThread();
}

// ---- The JIT fallback (section 11) ----

namespace {

const char* JitWhy(uint32_t reason) {
    switch (reason) {
    case NXAOT_TRAP_JIT_MISS: return "an indirect branch to an address with no compiled entry";
    case NXAOT_TRAP_JIT_TARGET: return "a branch or call to an address that is no compiled entry";
    case NXAOT_TRAP_JIT_FPCR: return "msr fpcr left the FPCR the module was compiled for";
    case NXAOT_TRAP_JIT_REFUSED: return "a function the lifter refused";
    case NXAOT_TRAP_NO_CODE: return "code that runs into no compiled entry";
    case NXAOT_TRAP_UNTRANSLATED: return "an encoding the lifter does not translate";
    default: return "a continuation compiled code handed over";
    }
}

}  // namespace

void Host::NativeCall(Thread& t, const void* entry) {
    // A compiled entry the JIT reached: its standard entry with the block's registers (section 2.5), under the module's MXCSR (section 7.1;
    // FPSR's flags are in the block, dm_jit_run moved them there)
    __atomic_fetch_add(&g_run.result->jit_native_calls, 1, __ATOMIC_RELAXED);
    _mm_setcsr(GuestMxcsr(t.st->fpcr));
    t.st->pending = 0;
    desc->enter(t.st, entry);
}

// The JIT driver: st->pc is where the guest continues and st holds the whole register file. It runs until the guest returns to lr (the native
// frame that handed the JIT this continuation; lr = kHostReturn at the outermost level of a thread; sp is only traced), taking every compiled entry
// natively on the way (their normal return continues in the JIT at the lr they were entered with). A return address of compiled code that is
// not this frame's, reached with an SP above the one the JIT was entered with, is a non-local transfer (section 3.3): nested, it is propagated to
// the native frames further out (st->pending); at the outermost level nothing can own it, and the JIT runs it
void Host::JitLoop(Thread& t, uint64_t lr, uint64_t sp, bool outermost, const char* why) {
    dm_guest_state* st = t.st;
    for (;;) {
        CheckStop();
        const uint64_t pc = st->pc;
        const bool module_fpcr = (st->fpcr & 0x07C80000u) == (desc->fpcr & 0x07C80000u);
        // The return the native frame waits for: lr alone decides, as a compiled function's own return does (design 3.3: the target equal to
        // the entry's lr). SP is not compared: a branch through jit_entry from the middle of a function (an unbounded br) hands over the SP
        // of its body, while the return comes back with the SP of its entry
        if (pc == lr) {
            if (outermost || module_fpcr) {
                st->pending = 0;
                return;
            }
            // Back where the native frame waits, but under another FPCR than it was compiled for: it cannot go on, so this is a non-local
            // transfer its callers propagate; the JIT runs the continuation at the outermost level
            st->pending = pc;
            return;
        }
        if (pc == kHostReturn) {
            st->pending = pc;  // the guest returned to the host while native frames wait: the outermost level ends the thread
            return;
        }
        if (options.jit != JitMode::All && module_fpcr && InModule(pc)) {
            if (const nxaot_lookup_entry* slot = Find(pc - kModuleBase)) {
                const void* e = reinterpret_cast<const uint8_t*>(&slot->entry) + slot->entry;
                const uint64_t ret = st->x[30], sp_at_call = st->sp;
                if (g_trace) {
                    std::fprintf(stderr, "aot: native call module+%#llx returning to module+%#llx, sp %#llx\n", static_cast<unsigned long long>(pc - kModuleBase),
                                 static_cast<unsigned long long>(ret - kModuleBase), static_cast<unsigned long long>(st->sp));
                }
                NativeCall(t, e);
                if (g_trace) {
                    std::fprintf(stderr, "aot: native call back: pending %#llx, sp %#llx, x30 %#llx\n", static_cast<unsigned long long>(st->pending),
                                 static_cast<unsigned long long>(st->sp), static_cast<unsigned long long>(st->x[30]));
                }
                if (st->pending) {
                    st->pc = st->pending;  // a non-local transfer: the block holds the whole register file
                    st->pending = 0;
                } else {
                    // A normal return is a ret to the lr the entry was called with: x30 holds it; SP is the call's unless the function may
                    // change it, which then stored it (aot_module.h, NXAOT_LOOKUP_SP_KEPT)
                    st->pc = ret;
                    st->x[30] = ret;
                    if (slot->flags & NXAOT_LOOKUP_SP_KEPT) {
                        st->sp = sp_at_call;
                    }
                }
                continue;
            }
            if (!outermost && st->sp > sp && IsReturn(pc - kModuleBase)) {
                // A return address of compiled code above the frame that entered the JIT (its SP is higher than the SP it entered with): a
                // native frame further out may wait there, so it is propagated to them (section 3.3). At or below that SP it is code of the
                // continuation the JIT was given (a call site's return inside it), which the JIT runs on
                st->pending = pc;
                return;
            }
        }
        if (options.jit == JitMode::None) {
            StopInfo i;
            i.status = kJit;
            i.message = std::string("[RED] aot: ") + why + " at " + Hex(pc) +
                        (InModule(pc) ? " (module+" + Hex(pc - kModuleBase) + ")" : std::string(" (outside the module)")) +
                        ": the JIT fallback is off (--jit-none)";
            RequestStop(std::move(i));
            LeaveThread();
        }
        dm_jit* j = t.jit.load();
        if (!j) {
            j = dm_jit_create_stepping(st, options.jit_one_block ? 1 : 0);
            if (!j) {
                StopInfo i;
                i.status = kJit;
                i.message = "[RED] aot: no JIT instance for the thread " + Hex(t.handle) + " (more than " + std::to_string(kMaxThreads) + ")";
                RequestStop(std::move(i));
                LeaveThread();
            }
            t.jit.store(j);
            if (g_run.stop || t.pause_req.load()) {
                dm_jit_halt(j);  // a request made before the instance existed
            }
        }
        bool first = false;
        {
            std::lock_guard<std::mutex> guard{g_run.lock};
            first = g_run.jit_addresses.insert(pc).second;
        }
        if (first && options.jit == JitMode::Mixed && options.jit_log) {
            // Every JIT entry is a discovery gap or a continuation without a native frame (section 11): once per address, for the next pack
            std::fprintf(stderr, "[RED] aot: %s at %s%s runs in the JIT fallback (design section 11)\n", why, Hex(pc).c_str(),
                         InModule(pc) ? (" (module+" + Hex(pc - kModuleBase) + ")").c_str() : " (outside the module)");
        }
        const uint64_t t0 = NowNs();
        const uint64_t stop = dm_jit_run(j, pc);
        const double secs = static_cast<double>(NowNs() - t0) * 1e-9;
        if (g_trace) {
            std::fprintf(stderr, "aot: jit %s from module+%#llx to %#llx (reason %d), lr %#llx sp %#llx x30 %#llx\n", outermost ? "outer" : "nested",
                         static_cast<unsigned long long>(pc - kModuleBase), static_cast<unsigned long long>(stop - kModuleBase), dm_jit_reason(j),
                         static_cast<unsigned long long>(lr - kModuleBase), static_cast<unsigned long long>(sp), static_cast<unsigned long long>(st->x[30] - kModuleBase));
        }
        {
            std::lock_guard<std::mutex> guard{g_run.out};
            g_run.result->jit_seconds += secs;
        }
        why = "a non-local transfer to a continuation with no native frame";
        switch (dm_jit_reason(j)) {
        case DM_JIT_ENTRY:
        case DM_JIT_RETURN:
            break;  // st->pc = stop: the checks above decide
        case DM_JIT_HALTED:
            SafepointAt(t, kPausedJit);
            break;
        case DM_JIT_CALLBACK:
            LeaveThread();  // the svc ends the thread or the run
        case DM_JIT_NO_CODE:
            if (stop == kHostReturn || stop == lr) {
                break;
            }
            {
                StopInfo i;
                i.status = kFault;
                i.message = "[RED] aot: the JIT fallback reached " + Hex(stop) + ", which holds no guest code";
                i.fault_address = stop;
                RequestStop(std::move(i));
            }
            LeaveThread();
        case DM_JIT_TRAP:
        default: {
            const uint32_t reason = dm_jit_trap(j);
            Trap(st, InModule(stop) ? stop - kModuleBase : stop,
                 reason == DM_EXIT_BRK ? NXAOT_TRAP_BRK : reason == DM_EXIT_UNTRANSLATED ? NXAOT_TRAP_UNTRANSLATED : NXAOT_TRAP_UNDEFINED);
        }
        }
    }
}

void Host::Jit(dm_guest_state* st, uint64_t lr, uint64_t sp) {
    // nxaot_rt_jit: compiled code hands a continuation to the JIT (st->exit_reason says why; jit_entry stored what it was passed)
    Thread& t = *t_self;
    const uint32_t reason = st->exit_reason;
    st->exit_reason = 0;
    JitLoop(t, lr, sp, false, JitWhy(reason));
    // Back to compiled code under its specialisation (FPCR's mode equal: JitLoop returns pending otherwise)
    _mm_setcsr(GuestMxcsr(desc->fpcr));
}

void Host::Safepoint(dm_guest_state*) {
    SafepointAt(*t_self, kPausedPoll);
}

// The outermost level of a guest thread: pc with st's registers; a compiled entry runs natively, a non-local transfer that reaches this level
// is dispatched as a new call, and what has no compiled entry runs in the JIT (section 3.3). Returns when the guest returns to the host
void Host::RunGuest(Thread& t, uint64_t pc) {
    dm_guest_state* st = t.st;
    const char* why = options.jit == JitMode::All ? "the thread (--jit-all)" : "the start of a thread at an address with no compiled entry";
    for (;;) {
        CheckStop();
        if (pc == kHostReturn) {
            return;
        }
        st->pc = pc;
        const bool module_fpcr = (st->fpcr & 0x07C80000u) == (desc->fpcr & 0x07C80000u);
        if (options.jit != JitMode::All && module_fpcr && InModule(pc)) {
            if (const void* e = Lookup(pc - kModuleBase)) {
                _mm_setcsr(GuestMxcsr(st->fpcr));
                st->pending = 0;
                desc->enter(st, e);
                if (!st->pending) {
                    return;
                }
                pc = st->pending;
                st->pending = 0;
                why = "a non-local transfer to a continuation with no native frame";
                continue;
            }
        }
        JitLoop(t, kHostReturn, 0, true, why);
        st->pending = 0;
        return;
    }
}

void Host::ThreadBody(Thread& t) {
    t_self = &t;
    BlockAlarm();
    InstallAltStack(t);
    _mm_setcsr(GuestMxcsr(desc->fpcr));
    const int jumped = sigsetjmp(t.base, 1);
    if (jumped == 0) {
        t.running = 1;
        RunGuest(t, t.entry);
        t.running = 0;
        StopInfo i;
        i.status = kReturned;
        i.message = "[RED] aot: the guest thread " + Hex(t.handle) + " returned from its entry function (Horizon requires svcExitThread)";
        RequestStop(std::move(i));
    } else {
        t.running = 0;
        if (jumped == 2) {
            StopInfo i;
            i.status = kFault;
            i.fault_signal = t.fault_signal;
            i.fault_address = t.fault_address;
            i.fault_pc = t.fault_pc;
            i.message = std::string("[RED] aot: ") + SignalName(t.fault_signal) + " at address " + Hex(t.fault_address) + " in the guest thread " +
                        Hex(t.handle) + " (" +
                        (t.fault_jit_pc ? "a memory access of the JIT fallback near guest " + Hex(t.fault_jit_pc) : Where(t.fault_pc)) + ")";
            RequestStop(std::move(i));
        }
    }
    if (dm_jit* j = t.jit.exchange(nullptr)) {
        const dm_jit_counts c = dm_jit_get_counts(j);
        {
            std::lock_guard<std::mutex> guard{g_run.out};
            g_run.result->jit_runs += c.runs;
            g_run.result->jit_run_calls += c.run_calls;
            g_run.result->jit_steps += c.steps;
        }
        dm_jit_destroy(j);
    }
    std::lock_guard<std::mutex> guard{g_run.lock};
    t.exited = true;
    g_run.cv.notify_all();
}

// ---- The pause test (section 6.2, in the manner of dm/host/tests/t_preempt.c): stop the world, read every state block, resume ----

void Host::PauseTest() {
    BlockAlarm();
    std::mt19937_64 rng{options.pause_seed};
    Result& r = *g_run.result;
    unsigned shown = 0;
    while (!g_run.stop && r.pauses_threaded < options.pause_test) {
        {
            std::unique_lock<std::mutex> lk{g_run.lock};
            const unsigned lo = std::max(1u, options.pause_interval_us / 10);
            g_run.cv.wait_for(lk, std::chrono::microseconds(lo + rng() % (std::max(lo, options.pause_interval_us) - lo + 1)),
                              [] { return g_run.stop.load(); });
            if (g_run.stop) {
                break;
            }
            std::vector<Thread*> ts;
            for (auto& t : g_run.threads) {
                if (t->started && !t->exited) {
                    ts.push_back(t.get());
                }
            }
            const bool threaded = ts.size() > 1;
            // The request: the flag, the poll byte compiled code reads, the JIT halted (it polls nothing), and a thread inside an SVC counts
            // as stopped at once (its block was complete at the SVC's entry; it cannot leave the SVC until the resume)
            for (Thread* t : ts) {
                t->pause_req = 1;
                __atomic_store_n(&t->st->poll, uint8_t{1}, __ATOMIC_RELAXED);
                if (dm_jit* j = t->jit.load()) {
                    dm_jit_halt(j);
                }
                if (t->svc_state.fetch_or(2) & 1) {
                    t->paused = kPausedSvc;
                }
            }
            const uint64_t t0 = NowNs();
            bool warned = false;
            for (;;) {
                const bool all = std::all_of(ts.begin(), ts.end(), [](const Thread* t) { return t->paused != kRunning || t->exited; });
                if (all || g_run.stop) {
                    break;
                }
                g_run.cv.wait_for(lk, std::chrono::microseconds(100));
                for (Thread* t : ts) {
                    if (t->paused == kRunning) {
                        if (dm_jit* j = t->jit.load()) {
                            dm_jit_halt(j);  // made after the request, or entered after the halt: as the host resends every 100 us
                        }
                    }
                }
                if (!warned && NowNs() - t0 > 1'000'000'000ull) {
                    warned = true;
                    std::fprintf(stderr, "[RED] aot: pause test: a thread still runs more than 1 second after the request (no poll reached?)\n");
                }
            }
            const double us = static_cast<double>(NowNs() - t0) * 1e-3;
            r.pause_max_us = std::max(r.pause_max_us, us);
            if (g_trace) {
                std::fprintf(stderr, "aot: pause %llu: %.1f us,", static_cast<unsigned long long>(r.pauses), us);
                for (Thread* t : ts) {
                    std::fprintf(stderr, " %s:%d", Hex(t->handle).c_str(), t->exited ? -1 : t->paused);
                }
                std::fprintf(stderr, "\n");
            }
            ++r.pauses;
            if (threaded) {
                ++r.pauses_threaded;
            }
            // Every thread stopped: its state block holds its whole register file. What must hold of it, whatever it was doing
            for (Thread* t : ts) {
                if (t->exited || t->paused == kRunning) {
                    continue;
                }
                const dm_guest_state s = *t->st;
                const int where = t->paused;
                ++(where == kPausedPoll ? r.stops_poll : where == kPausedSvc ? r.stops_svc : r.stops_jit);
                std::string bad;
                const bool in_text = InModule(s.pc) && image.InText(s.pc - kModuleBase) && (s.pc & 3) == 0;
                if (!in_text && s.pc != kHostReturn) {
                    bad = "pc " + Hex(s.pc) + " is no guest instruction";
                } else if (!InArena(s.sp, 0) || (where == kPausedPoll && (s.sp & 15))) {
                    bad = "sp " + Hex(s.sp) + " is no guest stack address";
                } else if (pause_head && s.pc == pause_head) {
                    // corpus/rt/pause.S: at its loop head x20 = 2 * x19 and x21 = ~x19, whatever iteration it is in
                    ++r.invariant_checks;
                    if (s.x[20] != 2 * s.x[19] || s.x[21] != ~s.x[19]) {
                        bad = "x19 " + Hex(s.x[19]) + ", x20 " + Hex(s.x[20]) + ", x21 " + Hex(s.x[21]) + " at the loop head break x20 = 2 x19, x21 = ~x19";
                    }
                }
                if (!bad.empty()) {
                    ++r.inconsistent;
                    if (shown++ < 8) {
                        std::fprintf(stderr, "[RED] aot: pause test: the thread %s stopped %s: %s\n", Hex(t->handle).c_str(),
                                     where == kPausedPoll ? "at a poll" : where == kPausedSvc ? "in an SVC" : "in the JIT", bad.c_str());
                    }
                }
            }
            // The resume
            for (Thread* t : ts) {
                t->pause_req = 0;
                t->svc_state.fetch_and(~2);
                t->paused = kRunning;
                __atomic_store_n(&t->st->poll, static_cast<uint8_t>(g_run.stop ? 1 : 0), __ATOMIC_RELAXED);
            }
            g_run.cv.notify_all();
        }
    }
}

// ---- A run ----

Result Host::Execute(dm_guest_state* st, uint64_t pc, bool svc_stops, unsigned timeout_ms, bool returning_is_normal) {
    Result r;
    g_run.host = this;
    g_run.result = &r;
    g_run.svc_stops = svc_stops;
    g_run.stop = false;
    g_run.stopped = false;
    g_run.why = StopInfo{};
    g_run.jit_addresses.clear();
    g_run.threads.clear();
    auto main = std::make_unique<Thread>();
    main->index = 0;
    main->handle = kFirstHandle;
    main->st = st;
    main->started = true;
    Thread& t = *main;
    g_run.threads.push_back(std::move(main));
    t_self = &t;
    st->poll = 0;
    const unsigned host_mxcsr = _mm_getcsr();
    std::thread tester;
    if (options.pause_test) {
        tester = std::thread([this] { PauseTest(); });
    }
    const uint64_t t0 = NowNs();
    const int jumped = sigsetjmp(t.base, 1);
    if (jumped == 0) {
        if (timeout_ms) {
            itimerval it{};
            it.it_value.tv_sec = timeout_ms / 1000;
            it.it_value.tv_usec = static_cast<suseconds_t>(timeout_ms % 1000) * 1000;
            setitimer(ITIMER_REAL, &it, nullptr);
        }
        t.running = 1;
        _mm_setcsr(GuestMxcsr(desc->fpcr));  // section 7.1: guest code runs with the MXCSR of the FPCR it was compiled for
        RunGuest(t, pc);
        t.running = 0;
        StopInfo i;
        i.status = kReturned;
        if (!returning_is_normal) {
            i.message = "[RED] aot: guest code returned to the host (" + Hex(st->x[30]) + ")";
        }
        RequestStop(std::move(i));
    } else {
        t.running = 0;
        if (jumped == 2) {
            StopInfo i;
            i.status = t.fault_signal == SIGALRM ? kTimeout : kFault;
            i.fault_signal = t.fault_signal;
            i.fault_address = t.fault_address;
            i.fault_pc = t.fault_pc;
            if (i.status == kTimeout) {
                i.message = "[RED] aot: still running after " + std::to_string(timeout_ms) + " ms";
            } else {
                i.message = std::string("[RED] aot: ") + SignalName(t.fault_signal) + " at address " + Hex(t.fault_address) + " in guest code (" +
                            (t.fault_jit_pc ? "a memory access of the JIT fallback near guest " + Hex(t.fault_jit_pc) : Where(t.fault_pc)) + ")";
            }
            RequestStop(std::move(i));
        } else {
            RequestStop(StopInfo{});  // a stop the thread was asked for: the request that made it holds the reason
        }
    }
    if (timeout_ms) {
        itimerval it{};
        setitimer(ITIMER_REAL, &it, nullptr);
    }
    _mm_setcsr(host_mxcsr);
    // Every other thread stops at its next poll, SVC, JIT block boundary or wait; then they are joined
    {
        std::unique_lock<std::mutex> lk{g_run.lock};
        StopAll();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!std::all_of(g_run.threads.begin() + 1, g_run.threads.end(), [](const auto& n) { return !n->started || n->exited; })) {
            if (g_run.cv.wait_until(lk, deadline) == std::cv_status::timeout) {
                std::fprintf(stderr, "[RED] aot: a guest thread did not stop within 10 s of the end of the run (no poll reached?)\n");
                std::fflush(nullptr);
                std::_Exit(g_run.why.status ? g_run.why.status : kTimeout);
            }
        }
    }
    for (auto& n : g_run.threads) {
        if (n->joinable) {
            pthread_join(n->pt, nullptr);
        }
    }
    if (tester.joinable()) {
        tester.join();
    }
    r.seconds = static_cast<double>(NowNs() - t0) * 1e-9;
    if (dm_jit* j = t.jit.exchange(nullptr)) {
        const dm_jit_counts c = dm_jit_get_counts(j);
        r.jit_runs += c.runs;
        r.jit_run_calls += c.run_calls;
        r.jit_steps += c.steps;
        dm_jit_destroy(j);
    }
    r.jit_addresses = g_run.jit_addresses.size();
    const StopInfo& why = g_run.why;
    r.status = why.status;
    r.message = why.message;
    r.svc = why.svc;
    r.trap = why.trap;
    r.trap_offset = why.trap_offset;
    r.fault_signal = why.fault_signal;
    r.fault_address = why.fault_address;
    r.fault_pc = why.fault_pc;
    if (options.pause_test && r.status == kExited && (r.inconsistent || r.pauses_threaded < options.pause_test)) {
        r.status = kPauseFailed;
        r.message = "[RED] aot: pause test: " + std::to_string(r.pauses_threaded) + " of " + std::to_string(options.pause_test) +
                    " pauses with more than one thread running, " + std::to_string(r.inconsistent) + " inconsistent state blocks";
    }
    for (auto& n : g_run.threads) {
        std::free(n->st_mem);
    }
    g_run.threads.clear();
    g_run.result = nullptr;
    g_run.svc_stops = false;
    t_self = nullptr;
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
    st.fpcr = desc->fpcr;
    st.excl_valid = 0;                // the local monitor clear (section 6.5)
    if (options.jit != JitMode::All && !Lookup(desc->entry)) {
        Result r;
        r.status = kJit;
        r.message = "[RED] aot: the module's entry module+" + Hex(desc->entry) + " has no standard entry";
        return r;
    }
    return Execute(&st, kModuleBase + desc->entry, false, 0, false);
}

Result Host::Call(uint64_t offset, dm_guest_state* st, bool svc_stops, unsigned timeout_ms) {
    if (!Lookup(offset)) {
        Result r;
        r.status = kJit;
        r.message = "no standard entry at module+" + Hex(offset);
        return r;
    }
    return Execute(st, kModuleBase + offset, svc_stops, timeout_ms, true);
}

}  // namespace nxaot::rt

// ---- What the pack's library imports (include/nxaot/aot_module.h) ----

extern "C" {

__attribute__((visibility("default"))) void dm_aot_svc(dm_guest_state* st, uint32_t number) {
    if (nxaot::rt::g_run.host->Svc(st, number)) {
        nxaot::rt::LeaveThread();
    }
}

__attribute__((visibility("default"))) void* nxaot_rt_dispatch(dm_guest_state* st, uint64_t target, const nxaot_ic_desc** slot, uint64_t site) {
    return nxaot::rt::g_run.host->Dispatch(st, target, slot, site);
}

__attribute__((visibility("default"))) void nxaot_rt_trap(dm_guest_state* st, uint64_t offset, uint32_t reason) {
    nxaot::rt::g_run.host->Trap(st, offset, reason);
}

__attribute__((visibility("default"))) void nxaot_rt_jit(dm_guest_state* st, uint64_t lr, uint64_t sp) {
    nxaot::rt::g_run.host->Jit(st, lr, sp);
}

__attribute__((visibility("default"))) void nxaot_rt_safepoint(dm_guest_state* st) {
    nxaot::rt::g_run.host->Safepoint(st);
}

// Design 1.1: an observation gate or a spin wait's back-edge. This run time has no observers (dm/host's gate table, hle.c, has them) and runs only
// the corpus, whose NROs have no gate rows: a gate here is a pause point like a safepoint, and its observer returns after one pass, as Apple's gate
// returns with no callback attached
__attribute__((visibility("default"))) void nxaot_rt_gate(dm_guest_state* st, uint64_t) {
    nxaot::rt::g_run.host->Safepoint(st);
}

// Section 8: the guest counter in 19.2 MHz ticks (svcGetSystemTick's unit, guest_ns * 12 / 625)
__attribute__((visibility("default"))) uint64_t dm_aot_cntpct(dm_guest_state*) {
    return nxaot::rt::Counter();
}

}  // extern "C"
