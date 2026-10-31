#include "rt/jit.h"

#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
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
std::unique_ptr<ExclusiveMonitor> g_monitor;
std::mutex g_ids_lock;
std::vector<bool> g_ids;  // the monitor's processor ids in use

// The guest access a memory callback is making on this thread (a fault there is reported at it: src/rt/host.cpp)
thread_local uint64_t t_access_pc = 0, t_access_address = 0;

constexpr HaltReason kStopped = HaltReason::UserDefined1;  // a callback stopped the run (dm_jit::Stop)
constexpr HaltReason kHalted = HaltReason::UserDefined2;   // dm_jit_halt

uint32_t FpsrFromMxcsr(uint32_t mxcsr) {
    return (mxcsr & 0x1) | ((mxcsr & 0x3C) >> 1);  // IE -> IOC, ZE -> DZC, OE -> OFC, UE -> UFC, PE -> IXC (lift::FpsrFromMxcsr)
}

}  // namespace

struct dm_jit final : A64::UserCallbacks {
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
        conf.global_monitor = g_monitor.get();
        // Section 11: one-to-one addresses (guest address = host address) below 2^39, the arena's bound; a fault takes the callbacks below
        conf.fastmem_pointer = 0;
        conf.fastmem_address_space_bits = 39;
        conf.silently_mirror_fastmem = false;
        conf.recompile_on_fastmem_failure = true;
        conf.fastmem_exclusive_access = true;  // lock cmpxchg on the same memory as compiled code's exclusives (section 6.5)
        conf.recompile_on_exclusive_fastmem_failure = true;
        conf.hook_data_cache_operations = true;  // dc zva zeroes the aligned block (README.md, "Known issues", 2)
        conf.cntfrq_el0 = nxaot::lift::kCntfrq;
        conf.ctr_el0 = nxaot::lift::kCtr;
        conf.dczid_el0 = nxaot::lift::kDczid;
        conf.tpidr_el0 = &st->tpidr_el0;
        conf.tpidrro_el0 = &st->tpidrro_el0;
        conf.wall_clock_cntpct = true;
        conf.enable_cycle_counting = one_block;  // off (section 11), except in the measurement mode of one block per Run()
        conf.define_unpredictable_behaviour = false;
        conf.code_cache_size = g_host.code_cache ? g_host.code_cache : 16u * 1024 * 1024;
        // Without A64GetSetElimination, as the reference runs (README.md, "Known issues", 1: it reads msr nzcv back wrong)
        conf.optimizations = all_safe_optimizations & ~OptimizationFlag::GetSetElimination;
        jit = std::make_unique<A64::Jit>(conf);
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
    }

    bool Under(uint64_t fpcr) const { return (fpcr & nxaot::lift::kFpcrModeMask) == g_host.fpcr; }
    int StopKind(uint64_t pc) const {
        if (pc < g_host.code_lo || pc >= g_host.code_hi || !g_host.stop_at || !Under(jit->GetFpcr())) {
            return 0;
        }
        return g_host.stop_at(g_host.ctx, pc);
    }
    void Stop(int why, uint64_t pc, uint32_t trap_reason = 0) {
        if (!stopped) {
            stopped = true;
            reason = why;
            stop_pc = pc;
            trap = trap_reason;
        }
        jit->HaltExecution(kStopped);
    }

    uint64_t Run(uint64_t pc) {
        ++counts.runs;
        LoadFrom(pc);
        stopped = false;
        reason = 0;
        if (!one_block && StopKind(pc)) {
            // A stop address the host wants run: its one instruction as a single-step block (cached apart from the stopping block), then on
            allow = pc;
            ++counts.steps;
            const HaltReason hr = jit->Step();
            allow = UINT64_MAX;
            if (Has(hr, kHalted) && !stopped) {
                Stop(DM_JIT_HALTED, jit->GetPC());
            }
        }
        while (!stopped) {
            const HaltReason hr = jit->Run();
            ++counts.run_calls;
            if (Has(hr, kHalted) && !stopped) {
                Stop(DM_JIT_HALTED, jit->GetPC());
            }
            if (one_block && !stopped) {
                // The alternative of design 11: one block per Run(), the next pc checked by the host (only at block boundaries)
                const uint64_t next = jit->GetPC();
                if (const int k = StopKind(next)) {
                    Stop(k, next);
                } else if (next < g_host.code_lo || next >= g_host.code_hi) {
                    Stop(DM_JIT_NO_CODE, next);
                }
            }
        }
        jit->ClearHalt(kStopped);
        SaveTo();
        st->pc = stop_pc;
        return stop_pc;
    }

    // ---- Memory (the fallback of fastmem: faults and addresses at or above 2^39) ----
    template<typename T>
    T Load(uint64_t a) {
        t_access_pc = jit->GetPC();
        t_access_address = a;
        T v;
        std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof v);
        t_access_pc = 0;
        return v;
    }
    template<typename T>
    void Store(uint64_t a, T v) {
        t_access_pc = jit->GetPC();
        t_access_address = a;
        std::memcpy(reinterpret_cast<void*>(a), &v, sizeof v);
        t_access_pc = 0;
    }
    template<typename T>
    bool Cas(uint64_t a, T value, T expected) {
        t_access_pc = jit->GetPC();
        t_access_address = a;
        const bool ok = __atomic_compare_exchange_n(reinterpret_cast<T*>(a), &expected, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        t_access_pc = 0;
        return ok;
    }

    std::optional<std::uint32_t> MemoryReadCode(A64::VAddr vaddr) override {
        if (vaddr < g_host.code_lo || vaddr >= g_host.code_hi || (vaddr & 3)) {
            return std::nullopt;
        }
        if (vaddr != allow && !one_block && StopKind(vaddr)) {
            return std::nullopt;  // a compiled entry or a compiled call site's return: the block ends in NoExecuteFault there
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
        t_access_pc = jit->GetPC();
        t_access_address = a;
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
            // No instruction at pc: a stop the code reader made (a compiled entry or a call site's return), or no code at all
            const int k = StopKind(pc);
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
            const uint64_t base = value & ~(nxaot::lift::kZvaBytes - 1);
            for (uint64_t off = 0; off < nxaot::lift::kZvaBytes; off += 8) {
                Store<uint64_t>(base + off, 0);
            }
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
    dm_jit_counts counts{};
};

extern "C" {

void dm_jit_init(const dm_jit_host* host) {
    g_host = *host;
    std::lock_guard<std::mutex> guard{g_ids_lock};
    if (!g_monitor || g_ids.size() != host->max_threads) {
        g_monitor = std::make_unique<ExclusiveMonitor>(host->max_threads);
        g_ids.assign(host->max_threads, false);
    }
}

dm_jit* dm_jit_create_stepping(dm_guest_state* st, int one_block_per_run) {
    auto* j = new dm_jit(st, one_block_per_run != 0);
    if (!j->jit) {
        delete j;
        return nullptr;
    }
    return j;
}

dm_jit* dm_jit_create(dm_guest_state* st) {
    return dm_jit_create_stepping(st, 0);
}

void dm_jit_destroy(dm_jit* j) {
    delete j;
}

uint64_t dm_jit_run(dm_jit* j, uint64_t pc) {
    return j->Run(pc);
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

}  // extern "C"
