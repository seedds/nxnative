#include "rt/jit.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <immintrin.h>

#include "decode/translate.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/interface/halt_reason.h"
#include "dynarmic/interface/optimization_flags.h"
#include "lift/lifter.h"

namespace {

using namespace Dynarmic;

dm_jit_host g_host{};
ExclusiveMonitor* g_monitor = nullptr;  // never freed: an instance made ahead may still use it while the process exits (dm_jit_prepare)
std::mutex g_ids_lock;
std::vector<bool> g_ids;  // the monitor's processor ids in use

// The shared monitor for max_threads processors, made once (under g_ids_lock): never replaced while an instance holds an id of it
void EnsureMonitor(unsigned max_threads) {
    if (g_monitor && (g_ids.size() == max_threads || std::find(g_ids.begin(), g_ids.end(), true) != g_ids.end())) {
        return;
    }
    delete g_monitor;
    g_monitor = new ExclusiveMonitor(max_threads);
    g_ids.assign(max_threads, false);
}

// The code ranges (dm_jit_set_code): published whole, read by every instance at translation; a list replaced stays allocated (an instance may be
// reading it, and lists are replaced only when modules are loaded), and g_code_gen tells instances to drop what they translated before
struct CodeRanges {
    std::vector<dm_jit_range> ranges;
};
std::atomic<const CodeRanges*> g_code{nullptr};
std::atomic<uint64_t> g_code_gen{0};
std::mutex g_code_lock;
std::vector<std::unique_ptr<CodeRanges>> g_code_lists;  // every list published (under g_code_lock)

bool InCode(uint64_t pc) {
    const CodeRanges* c = g_code.load(std::memory_order_acquire);
    if (!c) {
        return false;
    }
    for (const dm_jit_range& r : c->ranges) {
        if (pc - r.lo < r.hi - r.lo) {
            return true;
        }
    }
    return false;
}

void SetCode(const dm_jit_range* code, unsigned count) {
    auto c = std::make_unique<CodeRanges>();
    if (code) {
        c->ranges.assign(code, code + count);
    }
    std::lock_guard<std::mutex> guard{g_code_lock};
    g_code.store(c.get(), std::memory_order_release);
    g_code_lists.push_back(std::move(c));
    g_code_gen.fetch_add(1, std::memory_order_release);
}

// The guest access a memory callback is making on this thread (a fault there is reported at it, with the guest's registers there: src/rt/host.cpp,
// dm/host/aot_run.c, dm_jit_fault_state)
thread_local uint64_t t_access_pc = 0, t_access_address = 0;
thread_local int t_access_exact = 0;

constexpr HaltReason kStopped = HaltReason::UserDefined1;  // a callback stopped the run (dm_jit::Stop)
constexpr HaltReason kHalted = HaltReason::UserDefined2;   // dm_jit_halt

uint32_t FpsrFromMxcsr(uint32_t mxcsr) {
    return (mxcsr & 0x1) | ((mxcsr & 0x3C) >> 1);  // IE -> IOC, ZE -> DZC, OE -> OFC, UE -> UFC, PE -> IXC (lift::FpsrFromMxcsr)
}

}  // namespace

struct dm_jit final : A64::UserCallbacks {
    // state may be null: an instance made ahead (dm_jit_prepare), bound to a thread's block when that thread first needs one (dm_jit_create)
    dm_jit(dm_guest_state* state, bool one_block_per_run) : st(state), one_block(one_block_per_run) {
        {
            std::lock_guard<std::mutex> guard{g_ids_lock};
            for (id = 0; id < g_ids.size() && g_ids[id]; ++id) {
            }
            if (id == g_ids.size()) {
                return;  // more instances than the monitor has processors: dm_jit_create returns null
            }
            g_ids[id] = true;
        }
        A64::UserConfig conf;
        conf.callbacks = this;
        conf.processor_id = id;
        conf.global_monitor = g_monitor;
        // Section 11: one-to-one addresses (guest address = host address) below 2^39, the arena's bound; a fault takes the callbacks below
        conf.fastmem_pointer = 0;
        conf.fastmem_address_space_bits = 39;
        conf.silently_mirror_fastmem = false;
        conf.recompile_on_fastmem_failure = true;
        conf.fastmem_exclusive_access = true;  // lock cmpxchg on the same memory as compiled code's exclusives (section 6.5)
        conf.recompile_on_exclusive_fastmem_failure = true;
        conf.hook_data_cache_operations = true;  // dc zva zeroes the aligned block (README.md, "Known issues", 2)
        // A callback access (fastmem's fallback once its access faulted) halts at its exact instruction (MemoryAbort), which Run() then steps
        // with the access made (Retry): dynarmic checks this halt only after a callback, so fastmem accesses cost nothing more
        conf.check_halt_on_memory_access = true;
        conf.cntfrq_el0 = nxaot::lift::kCntfrq;
        conf.ctr_el0 = nxaot::lift::kCtr;
        conf.dczid_el0 = nxaot::lift::kDczid;
        // The instance's own copies, which the hand-over moves from and to the block (an instance may be made before it has a thread)
        conf.tpidr_el0 = &tpidr_el0;
        conf.tpidrro_el0 = &tpidrro_el0;
        conf.wall_clock_cntpct = true;
        conf.enable_cycle_counting = one_block;  // off (section 11), except in the measurement mode of one block per Run()
        conf.define_unpredictable_behaviour = true;  // a store-exclusive with Rs == Rt is the store (README.md, "Store-exclusives whose status register is a data register"); the same in every user of the frontend
        conf.code_cache_size = CodeCacheSize();
        // Without A64GetSetElimination, as the reference runs (README.md, "Known issues", 1: it reads msr nzcv back wrong)
        conf.optimizations = all_safe_optimizations & ~OptimizationFlag::GetSetElimination;
        jit = std::make_unique<A64::Jit>(conf);
        code_gen = g_code_gen.load(std::memory_order_acquire);
    }
    ~dm_jit() override {
        if (jit) {
            g_monitor->ClearProcessor(id);
            jit.reset();
            std::lock_guard<std::mutex> guard{g_ids_lock};
            g_ids[id] = false;
        }
    }

    // ---- The hand-over (sections 2.5 and 11) ----
    void LoadFrom(uint64_t pc) {
        tpidr_el0 = st->tpidr_el0;
        tpidrro_el0 = st->tpidrro_el0;
        std::array<uint64_t, 31> x;
        std::memcpy(x.data(), st->x, sizeof st->x);
        jit->SetRegisters(x);
        jit->SetSP(st->sp);
        jit->SetPC(pc);
        std::array<A64::Vector, 32> v;
        std::memcpy(v.data(), st->v, sizeof st->v);
        jit->SetVectors(v);
        jit->SetPstate(st->nzcv & 0xF0000000u);
        jit->SetFpcr(st->fpcr);
        // The cumulative flags compiled code left in MXCSR belong to FPSR (section 7.4): into the JIT's FPSR, and out of MXCSR
        const uint32_t m = _mm_getcsr();
        jit->SetFpsr(st->fpsr | FpsrFromMxcsr(m));
        _mm_setcsr(m & ~0x3Fu);
        jit->ClearExclusiveState();  // as an exception return clears the local monitor
    }
    void SaveTo() {
        const std::array<uint64_t, 31> x = jit->GetRegisters();
        std::memcpy(st->x, x.data(), sizeof st->x);
        st->sp = jit->GetSP();
        const std::array<A64::Vector, 32> v = jit->GetVectors();
        std::memcpy(st->v, v.data(), sizeof st->v);
        st->nzcv = jit->GetPstate() & 0xF0000000u;
        st->fpcr = jit->GetFpcr();
        st->fpsr = jit->GetFpsr();
        st->excl_valid = 0;
        st->tpidr_el0 = tpidr_el0;  // msr tpidr_el0 in JIT code
    }

    // Compiled code runs under FPCR fpcr: its specialisation, FZ left out (design 7.1: FZ is a run-time state of the same code)
    static bool Under(uint64_t fpcr) { return (fpcr & nxaot::lift::kFpcrSpecMask) == (g_host.fpcr & nxaot::lift::kFpcrSpecMask); }
    // Where the JIT stops under FPCR fpcr: DM_JIT_ENTRY or DM_JIT_RETURN where compiled code goes on (only under an FPCR it runs under, Under;
    // it calls a gate there itself), else DM_JIT_GATE at a gate site (under any FPCR), else 0
    static int StopKind(uint64_t pc, uint64_t fpcr) {
        if (!g_host.stop_at || !InCode(pc)) {
            return 0;
        }
        const int k = g_host.stop_at(g_host.ctx, pc);
        if ((k & 0xFF) && Under(fpcr)) {
            return k & 0xFF;
        }
        return (k & DM_JIT_GATE_AT) ? DM_JIT_GATE : 0;
    }
    int StopKind(uint64_t pc) const { return StopKind(pc, jit->GetFpcr()); }
    void Stop(int why, uint64_t pc, uint32_t trap_reason = 0) {
        if (!stopped) {
            stopped = true;
            reason = why;
            stop_pc = pc;
            trap = trap_reason;
        }
        jit->HaltExecution(kStopped);
    }

    // A callback access halted with MemoryAbort (see Access): dynarmic left at its exact instruction with nothing of it written back. That
    // instruction alone again, as a single-step block whose callback accesses are made at retry_pc: a fault there is the guest's, at that instruction
    void Retry() {
        const uint64_t pc = jit->GetPC();
        ++counts.aborts;
        retry_pc = pc;
        allow = pc;
        const HaltReason hr = jit->Step();
        retry_pc = 0;
        allow = UINT64_MAX;
        if (Has(hr, kHalted) && !stopped) {
            Stop(DM_JIT_HALTED, jit->GetPC());
        }
    }
    // pc's one instruction as a single-step block (dynarmic caches it apart from the block that stops there), even where the code reader stops
    void StepAt(uint64_t pc) {
        allow = pc;
        ++counts.steps;
        const HaltReason hr = jit->Step();
        allow = UINT64_MAX;
        if (Has(hr, HaltReason::MemoryAbort) && !stopped) {
            Retry();
        }
        if (Has(hr, kHalted) && !stopped) {
            Stop(DM_JIT_HALTED, jit->GetPC());
        }
    }

    // past < 0: dm_jit_run; 0 and 1: dm_jit_run_past (the gate at pc served), 1 returning after pc's instruction
    uint64_t Run(uint64_t pc, int past) {
        ++counts.runs;
        stopped = false;
        reason = 0;
        // One stop_at query per run (as before gates), under the block's FPCR and before anything is copied: a gate site is served before its
        // instruction runs, so the run returns at once with the block as the host handed it over
        const int k = past < 0 ? StopKind(pc, st->fpcr) : 0;
        if (k == DM_JIT_GATE) {
            ++counts.gates;
            reason = DM_JIT_GATE;
            stop_pc = pc;
            st->pc = pc;
            return pc;
        }
        if (code_gen != g_code_gen.load(std::memory_order_acquire)) {
            // The code ranges grew (dm_jit_set_code): a block translated before may end where there was no code then. An instance that has run
            // nothing has no block (one made ahead, the ranges given after it was made): dynarmic's ClearCache would only write its dispatch table again
            if (ran) {
                jit->ClearCache();
            }
            code_gen = g_code_gen.load(std::memory_order_acquire);
        }
        ran = true;
        LoadFrom(pc);
        if (past >= 0 || (k && !one_block)) {
            // A stop address the host wants run, or a gate site the host served: its instruction, then on (or back, with one)
            StepAt(pc);
            if (past == 1 && !stopped) {
                Stop(DM_JIT_STEPPED, jit->GetPC());
            }
        }
        while (!stopped) {
            const HaltReason hr = jit->Run();
            ++counts.run_calls;
            if (Has(hr, HaltReason::MemoryAbort) && !stopped) {
                Retry();
            }
            if (Has(hr, kHalted) && !stopped) {
                Stop(DM_JIT_HALTED, jit->GetPC());
            }
            if (one_block && !stopped) {
                // The alternative of design 11: one block per Run(), the next pc checked by the host (only at block boundaries)
                const uint64_t next = jit->GetPC();
                if (const int sk = StopKind(next)) {
                    if (sk == DM_JIT_GATE) {
                        ++counts.gates;
                    }
                    Stop(sk, next);
                } else if (!InCode(next)) {
                    Stop(DM_JIT_NO_CODE, next);
                }
            }
        }
        jit->ClearHalt(kStopped);
        SaveTo();
        st->pc = stop_pc;
        return stop_pc;
    }

    // ---- Memory (the fallback of fastmem: an access that faulted there once, and addresses at or above 2^39) ----
    // Outside Retry a callback access only halts (MemoryAbort: dynarmic leaves at the exact instruction, which Retry steps) and gives the
    // instruction nothing it keeps; inside Retry the access is made, and a fault there is reported at retry_pc, exactly (dm_jit_access_exact)
    bool Access(uint64_t a) {
        if (!retry_pc) {
            jit->HaltExecution(HaltReason::MemoryAbort);
            return false;
        }
        t_access_pc = retry_pc;
        t_access_address = a;
        t_access_exact = 1;
        return true;
    }
    template<typename T>
    T Load(uint64_t a) {
        if (!Access(a)) {
            return 0;
        }
        T v;
        std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof v);
        t_access_pc = 0;
        return v;
    }
    template<typename T>
    void Store(uint64_t a, T v) {
        if (!Access(a)) {
            return;
        }
        std::memcpy(reinterpret_cast<void*>(a), &v, sizeof v);
        t_access_pc = 0;
    }
    template<typename T>
    bool Cas(uint64_t a, T value, T expected) {
        if (!Access(a)) {
            return false;
        }
        const bool ok = __atomic_compare_exchange_n(reinterpret_cast<T*>(a), &expected, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        t_access_pc = 0;
        return ok;
    }

    std::optional<std::uint32_t> MemoryReadCode(A64::VAddr vaddr) override {
        if ((vaddr & 3) || !InCode(vaddr)) {
            return std::nullopt;
        }
        if (vaddr != allow) {
            // A compiled entry, a compiled call site's return or a gate site (also in the one-block mode, which checks the others per block):
            // the block ends before it, and the block from it is a NoExecuteFault
            const int k = StopKind(vaddr);
            if (k && (!one_block || k == DM_JIT_GATE)) {
                return std::nullopt;
            }
        }
        uint32_t w;
        std::memcpy(&w, reinterpret_cast<const void*>(vaddr), 4);
        return w;
    }
    std::uint8_t MemoryRead8(A64::VAddr a) override { return Load<uint8_t>(a); }
    std::uint16_t MemoryRead16(A64::VAddr a) override { return Load<uint16_t>(a); }
    std::uint32_t MemoryRead32(A64::VAddr a) override { return Load<uint32_t>(a); }
    std::uint64_t MemoryRead64(A64::VAddr a) override { return Load<uint64_t>(a); }
    A64::Vector MemoryRead128(A64::VAddr a) override { return {Load<uint64_t>(a), Load<uint64_t>(a + 8)}; }
    void MemoryWrite8(A64::VAddr a, std::uint8_t v) override { Store(a, v); }
    void MemoryWrite16(A64::VAddr a, std::uint16_t v) override { Store(a, v); }
    void MemoryWrite32(A64::VAddr a, std::uint32_t v) override { Store(a, v); }
    void MemoryWrite64(A64::VAddr a, std::uint64_t v) override { Store(a, v); }
    void MemoryWrite128(A64::VAddr a, A64::Vector v) override {
        Store(a, v[0]);
        Store(a + 8, v[1]);
    }
    bool MemoryWriteExclusive8(A64::VAddr a, std::uint8_t v, std::uint8_t e) override { return Cas<uint8_t>(a, v, e); }
    bool MemoryWriteExclusive16(A64::VAddr a, std::uint16_t v, std::uint16_t e) override { return Cas<uint16_t>(a, v, e); }
    bool MemoryWriteExclusive32(A64::VAddr a, std::uint32_t v, std::uint32_t e) override { return Cas<uint32_t>(a, v, e); }
    bool MemoryWriteExclusive64(A64::VAddr a, std::uint64_t v, std::uint64_t e) override { return Cas<uint64_t>(a, v, e); }
    bool MemoryWriteExclusive128(A64::VAddr a, A64::Vector v, A64::Vector e) override {
        if (!Access(a)) {
            return false;
        }
        uint64_t lo = e[0], hi = e[1];
        bool ok;
        __asm__ volatile("lock cmpxchg16b %1" : "=@ccz"(ok), "+m"(*reinterpret_cast<volatile unsigned __int128*>(a)), "+a"(lo), "+d"(hi)
                         : "b"(v[0]), "c"(v[1])
                         : "memory");
        t_access_pc = 0;
        return ok;
    }

    // ---- Exceptions, SVCs, the counter ----
    void InterpreterFallback(A64::VAddr pc, size_t) override {
        // mrs xN, cntvct_el0, which dynarmic leaves to an interpreter (README.md, "Known issues", 7); anything else is untranslated here too
        const auto word = MemoryReadCode(pc);
        if (word && nxaot::decode::IsMrsCntvct(*word)) {
            if ((*word & 31) != 31) {
                jit->SetRegister(*word & 31, g_host.cntpct ? g_host.cntpct(g_host.ctx, st) : 0);
            }
            jit->SetPC(pc + 4);
            return;
        }
        Stop(DM_JIT_TRAP, pc, DM_EXIT_UNTRANSLATED);
    }
    void CallSVC(std::uint32_t swi) override {
        // The host's SVC path on the whole register file, as at an svc of compiled code (section 2.4); the general registers come back (the
        // SVC's outputs, x0..x8). FPCR and FPSR stay with the JIT: dynarmic calls back with the guest's MXCSR live, so they are not copied here
        SaveTo();
        st->pc = jit->GetPC();
        if (g_host.svc && g_host.svc(g_host.ctx, st, swi) != 0) {
            Stop(DM_JIT_CALLBACK, st->pc);
            return;
        }
        std::array<uint64_t, 31> x;
        std::memcpy(x.data(), st->x, sizeof st->x);
        jit->SetRegisters(x);
        jit->SetSP(st->sp);
        tpidr_el0 = st->tpidr_el0;  // as the general registers: the block holds the thread's state again
        tpidrro_el0 = st->tpidrro_el0;
        jit->ClearExclusiveState();  // an svc returns through an exception return, which clears the local monitor
    }
    void ExceptionRaised(A64::VAddr pc, A64::Exception e) override {
        switch (e) {
        case A64::Exception::Yield:
        case A64::Exception::WaitForEvent:
        case A64::Exception::WaitForInterrupt:
            _mm_pause();  // as compiled code lowers them (section 6.3)
            jit->SetPC(pc + 4);
            return;
        case A64::Exception::SendEvent:
        case A64::Exception::SendEventLocal:
            jit->SetPC(pc + 4);
            return;
        case A64::Exception::NoExecuteFault: {
            // No instruction at pc: a stop the code reader made (a compiled entry, a call site's return, a gate site), or no code at all
            const int k = StopKind(pc);
            if (k == DM_JIT_GATE) {
                ++counts.gates;
            }
            Stop(k ? k : DM_JIT_NO_CODE, pc);
            return;
        }
        case A64::Exception::Breakpoint:
            Stop(DM_JIT_TRAP, pc, DM_EXIT_BRK);
            return;
        case A64::Exception::UnpredictableInstruction:
            Stop(DM_JIT_TRAP, pc, DM_EXIT_UNPREDICTABLE);
            return;
        default:
            Stop(DM_JIT_TRAP, pc, DM_EXIT_UNDEFINED);
            return;
        }
    }
    void DataCacheOperationRaised(A64::DataCacheOperation op, A64::VAddr value) override {
        if (op == A64::DataCacheOperation::ZeroByVA) {
            // Made here (no MemoryAbort check follows this hook): a fault is reported near the pc dynarmic last stored, not exactly
            const uint64_t base = value & ~(nxaot::lift::kZvaBytes - 1);
            t_access_pc = jit->GetPC();
            t_access_address = base;
            t_access_exact = 0;
            std::memset(reinterpret_cast<void*>(base), 0, nxaot::lift::kZvaBytes);
            t_access_pc = 0;
        }
    }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return one_block ? 1 : UINT64_MAX; }
    std::uint64_t GetCNTPCT() override { return g_host.cntpct ? g_host.cntpct(g_host.ctx, st) : 0; }

    dm_guest_state* st;
    bool one_block;
    size_t id = 0;
    std::unique_ptr<A64::Jit> jit;
    bool stopped = false;
    int reason = 0;
    uint32_t trap = 0;
    uint64_t stop_pc = 0;
    uint64_t allow = UINT64_MAX;  // the stop address being single-stepped (MemoryReadCode hands its instruction over)
    uint64_t retry_pc = 0;        // the instruction Retry steps: its callback accesses are made, and reported at it
    uint64_t code_gen = 0;        // g_code_gen when this instance last dropped its translations
    bool ran = false;             // it has run: blocks to drop when the code ranges change
    uint64_t tpidr_el0 = 0, tpidrro_el0 = 0;  // the guest's TPIDR_EL0 / TPIDRRO_EL0 while it runs here (dynarmic reads them through the configuration)
    dm_jit_counts counts{};

    static size_t CodeCacheSize();
};

namespace {

// ---- Instances made ahead of their threads' first use (dm_jit_prepare) ----
// What a first use cost (dm/tools/aot/README.md, "The JIT fallback", "The first use"): making an instance (about 35 ms: dynarmic's emitter writes its
// 16 MiB fast-dispatch table, whose entries have member initializers (a64_emit_x64.h FastDispatchEntry), and emits its prelude and fastmem thunks)
// and, once per process, dynarmic's A64 decoder table (frontend/A64/decoder/a64.h GetDecodeTable, a function-local static built at the first
// translation). A helper thread pays both ahead: it builds the decoder table, then makes one instance bound to no thread, the spare, which the next
// dm_jit_create binds to its block; a take starts the next spare (a pool of one), and an instance whose thread ends comes back as the spare with its
// translations, which belong to no thread. Pool::lock guards the spare and the helper's state. A first use that finds the spare being made waits for
// it (it started earlier, so the wait is shorter than making one, which would also compete with it for the CPU); a second first use meanwhile makes
// its own rather than queue behind the next spare (a burst of threads that miss at once pays about what it paid before). One spare, not one per
// thread: an instance holds about 20 MiB resident (the dispatch table, the prelude), and only the threads that miss need one
struct Pool {
    std::mutex lock;
    std::condition_variable cv;
    dm_jit* spare = nullptr;  // made ahead or given back, bound to no thread
    bool making = false;      // a helper thread is making the spare
    bool waiting = false;     // a first use waits for it
    bool on = false;          // dm_jit_prepare was called: a take starts the next spare, an instance given back is kept
    size_t code_cache = 16u * 1024 * 1024;
    void (*made)(void) = nullptr;  // on the thread that made an instance, right after
    dm_jit_pool_stats stats{};
};
Pool& ThePool() {
    static Pool* p = new Pool;  // never destroyed: a helper thread may use it while the process exits (exit waits for it: AtExit)
    return *p;
}

void WarmDecoder() {
    // One instruction (a nop) through the frontend: dynarmic's decoder table is built at its first use, once per process (decode/translate.cpp's
    // Decode<TranslatorVisitor>, the instantiation every translation of the JIT uses)
    static std::once_flag once;
    std::call_once(once, [] {
        const auto read = [](uint64_t vaddr) -> std::optional<uint32_t> {
            return vaddr == 0 ? std::optional<uint32_t>{0xD503201Fu} : std::nullopt;
        };
        (void)nxaot::decode::TranslateBlock(0, read);
    });
}

dm_jit* MakeInstance(dm_guest_state* st, bool one_block) {
    auto* j = new dm_jit(st, one_block);
    if (!j->jit) {
        delete j;
        return nullptr;
    }
    if (void (*made)(void) = ThePool().made) {
        made();
    }
    return j;
}

void Helper() {
    Pool& p = ThePool();
    const auto t0 = std::chrono::steady_clock::now();
    WarmDecoder();
    dm_jit* j = MakeInstance(nullptr, false);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::lock_guard<std::mutex> guard{p.lock};
    p.making = false;
    if (j) {
        p.stats.made_ahead++;
        p.stats.ahead_seconds += secs;
        if (p.spare) {
            delete j;  // an instance came back meanwhile (dm_jit_destroy)
        } else {
            p.spare = j;
        }
    }
    p.cv.notify_all();
}

// With p.lock held: a helper thread makes the spare when there is none and none is being made
void Refill(Pool& p) {
    if (!p.on || p.spare || p.making) {
        return;
    }
    p.making = true;
    try {
        std::thread(Helper).detach();
    } catch (...) {
        p.making = false;  // no thread: the next first use makes its instance itself
    }
}

// exit() while a helper makes an instance: wait for it, before the static objects it uses are destroyed
void AtExit() {
    Pool& p = ThePool();
    std::unique_lock<std::mutex> guard{p.lock};
    p.cv.wait(guard, [&] { return !p.making; });
}

}  // namespace

size_t dm_jit::CodeCacheSize() {
    return g_host.code_cache ? g_host.code_cache : ThePool().code_cache;
}

extern "C" {

void dm_jit_init(const dm_jit_host* host) {
    g_host = *host;
    SetCode(host->code, host->code_count);
    g_host.code = nullptr;  // the copy in g_code is the one read
    std::lock_guard<std::mutex> guard{g_ids_lock};
    EnsureMonitor(host->max_threads);
}

void dm_jit_prepare(const dm_jit_ahead* a) {
    {
        std::lock_guard<std::mutex> guard{g_ids_lock};
        EnsureMonitor(a->max_threads);
    }
    Pool& p = ThePool();
    std::lock_guard<std::mutex> guard{p.lock};
    if (p.on) {
        return;
    }
    p.on = true;
    p.code_cache = a->code_cache ? a->code_cache : p.code_cache;
    p.made = a->made;
    std::atexit(AtExit);
#if defined(_WIN32)
    // On Windows the decoder table is built here, not first in the helper: building it registers its destructor with atexit, and Windows' C runtime holds its exit
    // lock while it runs the atexit functions, AtExit among them. A helper that registered one while the process exited waited for that lock while AtExit waited for
    // the helper: a program that ended within the table's build time (63 to 197 ms: README.md, "The first use") hung at exit. The cost is that time at the
    // host's start, on Windows only: elsewhere exit() runs its functions without holding a lock a registration needs, and the helper builds the table.
    // Open: another first use in the helper of a static object with a destructor (MakeInstance) would wait the same way; none hung in the Wine runs
    WarmDecoder();
#endif
    Refill(p);
}

dm_jit_pool_stats dm_jit_get_pool_stats(void) {
    Pool& p = ThePool();
    std::lock_guard<std::mutex> guard{p.lock};
    dm_jit_pool_stats s = p.stats;
    s.spare = p.spare != nullptr;
    s.making = p.making;
    return s;
}

void dm_jit_set_code(const dm_jit_range* code, unsigned count) {
    SetCode(code, count);
}

dm_jit* dm_jit_create_stepping(dm_guest_state* st, int one_block_per_run) {
    Pool& p = ThePool();
    if (!one_block_per_run) {
        std::unique_lock<std::mutex> guard{p.lock};
        if (!p.spare && p.making && !p.waiting) {
            // Being made ahead: wait for it, which takes less than making one here (it started earlier). One thread waits for it; another first
            // use meanwhile makes its own, in parallel, rather than queue behind the next spare
            const auto t0 = std::chrono::steady_clock::now();
            p.waiting = true;
            p.cv.wait(guard, [&] { return p.spare || !p.making; });
            p.waiting = false;
            p.stats.waits++;
            p.stats.wait_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }
        if (dm_jit* j = p.spare) {
            p.spare = nullptr;
            p.stats.taken++;
            Refill(p);  // the next thread's
            guard.unlock();
            j->st = st;
            j->counts = dm_jit_counts{};
            return j;
        }
        p.stats.made_on_use++;
    }
    WarmDecoder();  // here rather than at the first translation, so that the instance is ready when it returns (waits while the helper builds it)
    return MakeInstance(st, one_block_per_run != 0);
}

dm_jit* dm_jit_create(dm_guest_state* st) {
    return dm_jit_create_stepping(st, 0);
}

void dm_jit_destroy(dm_jit* j) {
    if (!j) {
        return;
    }
    if (!j->one_block) {
        Pool& p = ThePool();
        std::lock_guard<std::mutex> guard{p.lock};
        if (p.on && !p.spare) {
            // Back as the spare with its translations (they belong to no thread): unbound, its monitor reservation and any halt cleared
            g_monitor->ClearProcessor(j->id);
            j->jit->ClearHalt(kHalted);
            j->jit->ClearHalt(kStopped);
            j->st = nullptr;
            j->stopped = false;
            p.spare = j;
            p.stats.given_back++;
            p.cv.notify_all();
            return;
        }
    }
    delete j;
}

uint64_t dm_jit_run(dm_jit* j, uint64_t pc) {
    return j->Run(pc, -1);
}

uint64_t dm_jit_run_past(dm_jit* j, uint64_t pc, int one) {
    return j->Run(pc, one ? 1 : 0);
}

int dm_jit_reason(const dm_jit* j) {
    return j->reason;
}

uint32_t dm_jit_trap(const dm_jit* j) {
    return j->trap;
}

void dm_jit_halt(dm_jit* j) {
    j->jit->HaltExecution(kHalted);
}

dm_jit_counts dm_jit_get_counts(const dm_jit* j) {
    return j->counts;
}

double dm_jit_measure_handover(dm_jit* j, unsigned n) {
    const uint64_t pc = j->st->pc;
    const auto t0 = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < n; ++i) {
        j->LoadFrom(pc);
        j->SaveTo();
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return n ? s / n : 0;
}

uint64_t dm_jit_access_pc(void) {
    return t_access_pc;
}

uint64_t dm_jit_access_address(void) {
    return t_access_address;
}

int dm_jit_access_exact(void) {
    return t_access_exact;
}

int dm_jit_fault_state(dm_jit* j) {
    // Called from the host's fault report after the fault left the run (no callback returns to dynarmic, nothing changes its state any more): dynarmic's
    // state is only read, the block written
    if (!j || !j->st || !t_access_pc) {
        return 0;
    }
    const std::array<uint64_t, 31> x = j->jit->GetRegisters();
    std::memcpy(j->st->x, x.data(), sizeof j->st->x);
    j->st->sp = j->jit->GetSP();
    j->st->nzcv = j->jit->GetPstate() & 0xF0000000u;
    j->st->pc = t_access_pc;
    return 1;
}

}  // extern "C"
