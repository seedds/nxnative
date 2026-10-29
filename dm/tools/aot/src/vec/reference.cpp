#include "vec/reference.h"

#include <cstdio>
#include <cstring>
#include <optional>

#include "decode/translate.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "lift/lifter.h"
#include "nxaot/guest_state.h"

namespace nxaot::vec {

using namespace Dynarmic;

namespace {
constexpr uint64_t kTickBudget = 10'000;  // instructions; a vector that has not left its code by then is a failure (no vector loops)
}

struct Reference::Impl final : A64::UserCallbacks {
    Impl() : monitor(1) {
        A64::UserConfig conf;
        conf.callbacks = this;
        conf.processor_id = 0;
        conf.global_monitor = &monitor;
        conf.hook_data_cache_operations = true;  // dc zva through DataCacheOperationRaised below (README.md, "Known issues": dynarmic's own pass)
        conf.cntfrq_el0 = lift::kCntfrq;
        conf.ctr_el0 = lift::kCtr;
        conf.dczid_el0 = lift::kDczid;
        conf.tpidr_el0 = &tpidr;
        conf.tpidrro_el0 = &tpidrro;
        conf.wall_clock_cntpct = true;
        conf.enable_cycle_counting = true;
        conf.define_unpredictable_behaviour = false;
        conf.code_cache_size = 16 * 1024 * 1024;
        // Without dynarmic's A64GetSetElimination pass: it forwards the value of msr nzcv, xN to a later mrs xM, nzcv of the block whole, where the
        // Arm ARM reads bits 27:0 of NZCV as 0 (README.md, "Known issues"). The pass is an optimisation: the frontend and backend semantics stay
        conf.optimizations = all_safe_optimizations & ~OptimizationFlag::GetSetElimination;
        jit = std::make_unique<A64::Jit>(conf);
    }

    // ---- Memory ----
    bool Inside(uint64_t vaddr, size_t size) const {
        return vaddr >= kGuestBase && vaddr - kGuestBase <= kImageSize - size;
    }
    uint64_t Load(uint64_t vaddr, size_t size) {
        if (!Inside(vaddr, size)) {
            Fail("reference: read of " + std::to_string(size) + " bytes outside the image at " + Hex(vaddr));
            return 0;
        }
        uint64_t v = 0;
        std::memcpy(&v, mem.data() + (vaddr - kGuestBase), size);
        return v;
    }
    void Store(uint64_t vaddr, size_t size, uint64_t v) {
        if (!Inside(vaddr, size)) {
            Fail("reference: write of " + std::to_string(size) + " bytes outside the image at " + Hex(vaddr));
            return;
        }
        std::memcpy(mem.data() + (vaddr - kGuestBase), &v, size);
    }
    static std::string Hex(uint64_t v) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%#llx", static_cast<unsigned long long>(v));
        return buf;
    }
    void Fail(const std::string& what) {
        if (failure.empty()) {
            failure = what;
        }
        Stop();
    }
    void Stop() {
        done = true;
        jit->HaltExecution(HaltReason::UserDefined1);
    }
    template<typename T>
    bool Cas(uint64_t vaddr, T value, T expected) {
        const T now = static_cast<T>(Load(vaddr, sizeof(T)));
        if (now != expected) {
            return false;
        }
        Store(vaddr, sizeof(T), value);
        return true;
    }

    std::optional<std::uint32_t> MemoryReadCode(A64::VAddr vaddr) override {
        if (!InCode(*vec, vaddr)) {
            return std::nullopt;
        }
        return vec->code[(vaddr - kCodeBase) / 4];
    }
    std::uint8_t MemoryRead8(A64::VAddr a) override { return static_cast<uint8_t>(Load(a, 1)); }
    std::uint16_t MemoryRead16(A64::VAddr a) override { return static_cast<uint16_t>(Load(a, 2)); }
    std::uint32_t MemoryRead32(A64::VAddr a) override { return static_cast<uint32_t>(Load(a, 4)); }
    std::uint64_t MemoryRead64(A64::VAddr a) override { return Load(a, 8); }
    A64::Vector MemoryRead128(A64::VAddr a) override { return {Load(a, 8), Load(a + 8, 8)}; }
    void MemoryWrite8(A64::VAddr a, std::uint8_t v) override { Store(a, 1, v); }
    void MemoryWrite16(A64::VAddr a, std::uint16_t v) override { Store(a, 2, v); }
    void MemoryWrite32(A64::VAddr a, std::uint32_t v) override { Store(a, 4, v); }
    void MemoryWrite64(A64::VAddr a, std::uint64_t v) override { Store(a, 8, v); }
    void MemoryWrite128(A64::VAddr a, A64::Vector v) override {
        Store(a, 8, v[0]);
        Store(a + 8, 8, v[1]);
    }
    bool MemoryWriteExclusive8(A64::VAddr a, std::uint8_t v, std::uint8_t e) override { return Cas<uint8_t>(a, v, e); }
    bool MemoryWriteExclusive16(A64::VAddr a, std::uint16_t v, std::uint16_t e) override { return Cas<uint16_t>(a, v, e); }
    bool MemoryWriteExclusive32(A64::VAddr a, std::uint32_t v, std::uint32_t e) override { return Cas<uint32_t>(a, v, e); }
    bool MemoryWriteExclusive64(A64::VAddr a, std::uint64_t v, std::uint64_t e) override { return Cas<uint64_t>(a, v, e); }
    bool MemoryWriteExclusive128(A64::VAddr a, A64::Vector v, A64::Vector e) override {
        if (Load(a, 8) != e[0] || Load(a + 8, 8) != e[1]) {
            return false;
        }
        Store(a, 8, v[0]);
        Store(a + 8, 8, v[1]);
        return true;
    }

    // ---- Exits ----
    void Leave(uint32_t why, uint64_t pc, uint32_t argument) {
        if (done) {
            return;
        }
        reason = why;
        exit_pc = pc;
        arg = argument;
        Stop();
    }
    void InterpreterFallback(A64::VAddr pc, size_t) override {
        // mrs xN, cntvct_el0, which dynarmic leaves to an interpreter: what the lifter's decode glue translates it to (src/decode/translate.cpp)
        const auto word = MemoryReadCode(pc);
        if (word && decode::IsMrsCntvct(*word)) {
            if ((*word & 31) != 31) {
                jit->SetRegister(*word & 31, cnt);
            }
            jit->SetPC(pc + 4);
            return;
        }
        Leave(DM_EXIT_UNTRANSLATED, pc, 0);
    }
    void CallSVC(std::uint32_t swi) override { Leave(DM_EXIT_SVC, jit->GetPC(), swi); }
    void ExceptionRaised(A64::VAddr pc, A64::Exception e) override {
        switch (e) {
        case A64::Exception::Yield:
        case A64::Exception::WaitForEvent:
        case A64::Exception::WaitForInterrupt:
        case A64::Exception::SendEvent:
        case A64::Exception::SendEventLocal:
            return;  // hints: execution goes on at the next instruction
        case A64::Exception::Breakpoint:
            return Leave(DM_EXIT_BRK, pc, 0);
        case A64::Exception::UnallocatedEncoding:
        case A64::Exception::ReservedValue:
            return Leave(DM_EXIT_UNDEFINED, pc, 0);
        case A64::Exception::UnpredictableInstruction:
            return Leave(DM_EXIT_UNPREDICTABLE, pc, 0);
        case A64::Exception::NoExecuteFault:
            return Leave(DM_EXIT_NO_CODE, pc, 0);
        }
        Fail("reference: unknown exception");
    }
    void DataCacheOperationRaised(A64::DataCacheOperation op, A64::VAddr value) override {
        if (op == A64::DataCacheOperation::ZeroByVA) {
            const uint64_t base = value & ~(lift::kZvaBytes - 1);
            for (uint64_t off = 0; off < lift::kZvaBytes; off += 8) {
                Store(base + off, 8, 0);
            }
        }
    }
    void AddTicks(std::uint64_t t) override { ticks = t > ticks ? 0 : ticks - t; }
    std::uint64_t GetTicksRemaining() override { return ticks; }
    std::uint64_t GetCNTPCT() override { return cnt; }

    ExclusiveMonitor monitor;
    std::unique_ptr<A64::Jit> jit;
    const Vector* vec = nullptr;
    std::vector<uint8_t> mem;
    uint64_t tpidr = 0, tpidrro = 0, cnt = 0, ticks = 0, exit_pc = 0;
    uint32_t reason = 0, arg = 0;
    bool done = false;
    std::string failure;
};

Reference::Reference() : impl(std::make_unique<Impl>()) {}
Reference::~Reference() = default;

Outcome Reference::Run(const Vector& v, const State& init, const std::vector<uint8_t>& image) {
    Impl& r = *impl;
    r.vec = &v;
    r.mem = image;
    r.tpidr = init.tpidr;
    r.tpidrro = init.tpidrro;
    r.cnt = init.cnt;
    r.ticks = kTickBudget;
    r.done = false;
    r.failure.clear();
    r.reason = r.arg = 0;
    r.exit_pc = 0;

    r.jit->Reset();       // registers, exclusive state, return stack buffer, halt requests
    r.jit->ClearCache();  // the code at kCodeBase is another vector's: compiled again at the next Run
    r.monitor.Clear();
    r.jit->SetRegisters(init.x);
    r.jit->SetSP(init.sp);
    r.jit->SetPC(init.pc);
    r.jit->SetPstate(init.nzcv & 0xF0000000u);
    r.jit->SetFpcr(init.fpcr);
    r.jit->SetFpsr(init.fpsr);
    while (!r.done) {
        if (r.ticks == 0) {
            r.Fail("reference: still in the vector's code after " + std::to_string(kTickBudget) + " instructions");
            break;
        }
        r.jit->ClearHalt(HaltReason::UserDefined1);
        r.jit->Run();
    }

    Outcome o;
    o.s.x = r.jit->GetRegisters();
    o.s.sp = r.jit->GetSP();
    o.s.pc = r.exit_pc;
    o.s.nzcv = r.jit->GetPstate() & 0xF0000000u;
    o.s.fpcr = r.jit->GetFpcr();
    o.s.fpsr = r.jit->GetFpsr();
    o.s.tpidr = r.tpidr;
    o.s.tpidrro = r.tpidrro;
    o.s.cnt = r.cnt;
    o.image = std::move(r.mem);
    o.reason = r.reason;
    o.arg = r.arg;
    o.failure = r.failure;
    return o;
}

}  // namespace nxaot::vec
