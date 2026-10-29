// nxaot_run: runs a corpus NRO (dm/tools/aot/corpus) under dynarmic's A64 JIT, the reference executor of the vector harness and the JIT fallback
// of design section 11, with the two supervisor calls the corpus makes: svcOutputDebugString (0x27, x0 = string, x1 = length), whose lines it
// prints, and svcExitProcess (0x07), which ends the run. It is the corpus's reference output on x86 until the recompiled path exists (the
// dispatcher and the host run time, phase 1 of section 14): a native run on ARM64 hardware and the recompiled code must print the same lines.
//   nxaot_run <module.nro | name.aot.tsv> [--max-instructions N]
// Exit status: 0 when the program left through svcExitProcess, 1 on a fault, an unknown SVC or the instruction budget, 2 on a usage error.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "decode/translate.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "lift/lifter.h"
#include "module/image.h"

namespace {

using namespace Dynarmic;

constexpr uint64_t kBase = 0x80000000ull;  // where the module is mapped: any address works, dynarmic reaches memory through the callbacks

struct Runner final : A64::UserCallbacks {
    explicit Runner(const nxaot::module::Image& m) : image(m), monitor(1) {
        const uint64_t size = (m.bytes.size() + m.bss_size + 0xFFF) & ~0xFFFull;
        mem.assign(size, 0);
        std::memcpy(mem.data(), m.bytes.data(), m.bytes.size());
        A64::UserConfig conf;
        conf.callbacks = this;
        conf.processor_id = 0;
        conf.global_monitor = &monitor;
        conf.hook_data_cache_operations = true;
        conf.cntfrq_el0 = nxaot::lift::kCntfrq;
        conf.ctr_el0 = nxaot::lift::kCtr;
        conf.dczid_el0 = nxaot::lift::kDczid;
        conf.tpidr_el0 = &tpidr;
        conf.tpidrro_el0 = &tpidrro;
        conf.wall_clock_cntpct = true;
        conf.enable_cycle_counting = true;
        conf.define_unpredictable_behaviour = false;
        conf.code_cache_size = 64 * 1024 * 1024;
        // As nxaot_vec's reference: without A64GetSetElimination (README.md, "Known issues", 1)
        conf.optimizations = all_safe_optimizations & ~OptimizationFlag::GetSetElimination;
        jit = std::make_unique<A64::Jit>(conf);
    }

    bool Inside(uint64_t vaddr, size_t size) const { return vaddr >= kBase && vaddr - kBase <= mem.size() - size; }
    uint64_t Load(uint64_t vaddr, size_t size) {
        if (!Inside(vaddr, size)) {
            Fail("read of " + std::to_string(size) + " bytes outside the module at " + Hex(vaddr) + " (pc " + Hex(jit->GetPC()) + ")");
            return 0;
        }
        uint64_t v = 0;
        std::memcpy(&v, mem.data() + (vaddr - kBase), size);
        return v;
    }
    void Store(uint64_t vaddr, size_t size, uint64_t v) {
        if (!Inside(vaddr, size)) {
            Fail("write of " + std::to_string(size) + " bytes outside the module at " + Hex(vaddr) + " (pc " + Hex(jit->GetPC()) + ")");
            return;
        }
        if (vaddr - kBase < image.ro_off + image.ro_size) {
            Fail("write to the module's text or read-only data at " + Hex(vaddr) + " (pc " + Hex(jit->GetPC()) + ")");
            return;
        }
        std::memcpy(mem.data() + (vaddr - kBase), &v, size);
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
        if (static_cast<T>(Load(vaddr, sizeof(T))) != expected) {
            return false;
        }
        Store(vaddr, sizeof(T), value);
        return true;
    }

    std::optional<std::uint32_t> MemoryReadCode(A64::VAddr vaddr) override {
        if (vaddr < kBase || vaddr - kBase >= image.TextEnd() || (vaddr & 3)) {
            return std::nullopt;
        }
        uint32_t w;
        std::memcpy(&w, mem.data() + (vaddr - kBase), 4);
        return w;
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

    void InterpreterFallback(A64::VAddr pc, size_t) override {
        const auto word = MemoryReadCode(pc);
        if (word && nxaot::decode::IsMrsCntvct(*word)) {
            if ((*word & 31) != 31) {
                jit->SetRegister(*word & 31, 0);
            }
            jit->SetPC(pc + 4);
            return;
        }
        Fail("an instruction dynarmic does not translate at module+" + Hex(pc - kBase));
    }
    void CallSVC(std::uint32_t swi) override {
        switch (swi) {
        case 0x27: {  // OutputDebugString(string, length): one line
            const uint64_t s = jit->GetRegister(0), n = jit->GetRegister(1);
            if (n > 4096 || !Inside(s, n ? n : 1)) {
                Fail("svcOutputDebugString with a string outside the module");
                return;
            }
            std::fwrite(mem.data() + (s - kBase), 1, n, stdout);
            std::fputc('\n', stdout);
            jit->SetRegister(0, 0);
            return;
        }
        case 0x07:  // ExitProcess
            exited = true;
            Stop();
            return;
        default:
            Fail("svc " + Hex(swi) + " at module+" + Hex(jit->GetPC() - 4 - kBase) + ": the corpus makes only 0x27 and 0x07");
        }
    }
    void ExceptionRaised(A64::VAddr pc, A64::Exception e) override {
        switch (e) {
        case A64::Exception::Yield:
        case A64::Exception::WaitForEvent:
        case A64::Exception::WaitForInterrupt:
        case A64::Exception::SendEvent:
        case A64::Exception::SendEventLocal:
            jit->SetPC(pc + 4);
            return;
        case A64::Exception::Breakpoint:
            return Fail("brk at module+" + Hex(pc - kBase));
        case A64::Exception::NoExecuteFault:
            return Fail("execution outside the text at " + Hex(pc));
        default:
            return Fail("an undefined or unpredictable instruction at module+" + Hex(pc - kBase));
        }
    }
    void DataCacheOperationRaised(A64::DataCacheOperation op, A64::VAddr value) override {
        if (op == A64::DataCacheOperation::ZeroByVA) {
            const uint64_t base = value & ~(nxaot::lift::kZvaBytes - 1);
            for (uint64_t off = 0; off < nxaot::lift::kZvaBytes; off += 8) {
                Store(base + off, 8, 0);
            }
        }
    }
    void AddTicks(std::uint64_t t) override {
        executed += t;
        ticks = t > ticks ? 0 : ticks - t;
    }
    std::uint64_t GetTicksRemaining() override { return ticks; }
    std::uint64_t GetCNTPCT() override { return 0; }

    const nxaot::module::Image& image;
    ExclusiveMonitor monitor;
    std::unique_ptr<A64::Jit> jit;
    std::vector<uint8_t> mem;
    uint64_t tpidr = 0, tpidrro = 0, ticks = 0, executed = 0;
    bool done = false, exited = false;
    std::string failure;
};

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    uint64_t budget = 2'000'000'000ull;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--max-instructions" && i + 1 < argc) {
            budget = std::strtoull(argv[++i], nullptr, 0);
        } else if (path.empty() && !a.empty() && a[0] != '-') {
            path = a;
        } else {
            std::fprintf(stderr, "usage: nxaot_run <module.nro | name.aot.tsv> [--max-instructions N]\n");
            return 2;
        }
    }
    if (path.empty()) {
        std::fprintf(stderr, "usage: nxaot_run <module.nro | name.aot.tsv> [--max-instructions N]\n");
        return 2;
    }
    std::string error;
    const auto image = nxaot::module::LoadModule(path, &error);
    if (!image) {
        std::fprintf(stderr, "nxaot_run: %s\n", error.c_str());
        return 2;
    }
    Runner r{*image};
    r.ticks = budget;
    r.jit->SetPC(kBase + image->text_off);
    r.jit->SetRegister(0, 0);           // the homebrew ABI's environment pointer: none
    r.jit->SetRegister(1, ~0ull);       // and main thread handle (libnx's crt0: x1 = -1 on NRO entry)
    r.jit->SetSP(0);                    // crt0 sets its own stack
    r.jit->SetFpcr(0);
    while (!r.done) {
        if (r.ticks == 0) {
            r.Fail("still running after " + std::to_string(budget) + " instructions");
            break;
        }
        r.jit->ClearHalt(HaltReason::UserDefined1);
        r.jit->Run();
    }
    std::fflush(stdout);
    if (!r.exited) {
        std::printf("[RED] nxaot_run: %s\n", r.failure.c_str());
        return 1;
    }
    std::fprintf(stderr, "nxaot_run: %s left through svcExitProcess after %llu instructions\n", path.c_str(),
                 static_cast<unsigned long long>(r.executed));
    return 0;
}
