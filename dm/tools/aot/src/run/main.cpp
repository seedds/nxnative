// nxaot_run: runs a corpus NRO (dm/tools/aot/corpus) under dynarmic's A64 JIT, the reference executor of the vector harness and of the corpus,
// with the supervisor calls the corpus makes: svcOutputDebugString (0x27, x0 = string, x1 = length), whose lines it prints, svcExitProcess
// (0x07), which ends the run, and the thread and synchronization SVCs of corpus/rt/thread.h (0x08 CreateThread, 0x09 StartThread, 0x0A
// ExitThread, 0x0B SleepThread, 0x16 CloseHandle, 0x18 WaitSynchronization, 0x1A ArbitrateLock, 0x1B ArbitrateUnlock, 0x34 WaitForAddress,
// 0x35 SignalToAddress), with the semantics src/rt/host.cpp gives them. Guest threads are contexts on this one JIT, switched round robin at a
// blocking SVC, at svcSleepThread and every 20,000 instructions, so the reference interleaving is deterministic (the corpus's threaded programs
// print only totals, which every interleaving must give). A native run on ARM64 hardware and the recompiled code must print the same lines.
//   nxaot_run <module.nro | name.aot.tsv> [--max-instructions N] [--set <symbol>=<value>]... [--quiet] [--fastmem]
// --set writes a 64-bit value at a symbol of the image before the run (int_ops_rounds: the throughput measurement); --quiet counts the lines
// instead of printing them; --fastmem lets dynarmic's JIT reach the module's memory directly (its fastmem arena, as an emulator configures it,
// for the throughput comparison with the recompiled code) instead of through the callbacks. The run's time goes to standard error.
// Exit status: 0 when the program left through svcExitProcess, 1 on a fault, an unknown SVC or the instruction budget, 2 on a usage error.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <sys/mman.h>

#include "decode/translate.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "lift/lifter.h"
#include "module/image.h"

namespace {

using namespace Dynarmic;

constexpr uint64_t kBase = 0x80000000ull;  // where the module is mapped: any address works, dynarmic reaches memory through the callbacks
constexpr unsigned kFastmemBits = 36;      // dynarmic's default fastmem arena: 2^36 bytes reserved, the module mapped at kBase inside it

// The module's memory: an anonymous mapping, or with fastmem the part of dynarmic's arena that holds the module
struct Memory {
    uint8_t* p = nullptr;
    size_t n = 0;
    uint8_t* arena = nullptr;
    uint8_t* data() const { return p; }
    size_t size() const { return n; }
};

struct Runner final : A64::UserCallbacks {
    Runner(const nxaot::module::Image& m, bool fastmem) : image(m), monitor(1) {
        const uint64_t size = (m.bytes.size() + m.bss_size + 0xFFF) & ~0xFFFull;
        mem.n = size;
        if (fastmem) {
            mem.arena = static_cast<uint8_t*>(mmap(nullptr, size_t{1} << kFastmemBits, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
            mem.p = mem.arena + kBase;
            mprotect(mem.p, size, PROT_READ | PROT_WRITE);
        } else {
            mem.p = static_cast<uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        }
        std::memcpy(mem.data(), m.bytes.data(), m.bytes.size());
        if (fastmem) {
            // Text and read-only data read-only, so that a store there faults and takes the callbacks' path, which reports it
            mprotect(mem.p, m.data_off & ~0xFFFull, PROT_READ);
        }
        A64::UserConfig conf;
        if (fastmem) {
            conf.fastmem_pointer = reinterpret_cast<uintptr_t>(mem.arena);
            conf.fastmem_address_space_bits = kFastmemBits;
            conf.silently_mirror_fastmem = false;
            conf.recompile_on_fastmem_failure = true;
            conf.fastmem_exclusive_access = true;
        }
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
        conf.enable_cycle_counting = !fastmem;  // the fastmem configuration is the design's (section 11): no cycle counting
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

    // ---- Guest threads: contexts on this one JIT, switched round robin (Schedule) ----
    enum class TState { Created, Ready, Blocked, Exited };
    enum class Wait { None, Join, Lock, Addr };
    struct GThread {
        uint32_t handle = 0;
        std::array<uint64_t, 31> x{};
        uint64_t sp = 0, pc = 0, tpidr = 0;
        std::array<A64::Vector, 32> v{};
        uint32_t fpcr = 0, fpsr = 0, pstate = 0;
        TState state = TState::Created;
        Wait wait = Wait::None;
        uint64_t addr = 0, seq = 0;
        uint32_t tag = 0;
        std::vector<uint32_t> handles;
    };
    static constexpr uint64_t kSlice = 20000;      // instructions a thread runs before the next one gets the JIT
    static constexpr uint32_t kFirstHandle = 0x1000, kWaitMask = 0x40000000u;
    static constexpr uint64_t kInvalidHandle = 0xE401, kInvalidState = 0xFA01, kInvalidEnumValue = 0xF001, kOutOfRange = 0xEE01;
    std::vector<GThread> threads = MainThread();
    size_t cur = 0;
    bool switch_needed = false;
    uint64_t seq = 0;

    static std::vector<GThread> MainThread() {
        std::vector<GThread> v(1);
        v[0].handle = kFirstHandle;
        v[0].state = TState::Ready;  // its context lives in the JIT while it runs
        return v;
    }
    GThread* ByHandle(uint32_t h) {
        return h >= kFirstHandle && h - kFirstHandle < threads.size() ? &threads[h - kFirstHandle] : nullptr;
    }
    void Yield() {
        switch_needed = true;
        jit->HaltExecution(HaltReason::UserDefined2);
    }
    void Block(Wait w) {
        threads[cur].state = TState::Blocked;
        threads[cur].wait = w;
        threads[cur].seq = ++seq;
        Yield();
    }
    void Wake(GThread& g, uint64_t x0) {
        g.state = TState::Ready;
        g.wait = Wait::None;
        g.x[0] = x0;
    }
    void WakeJoiners() {
        for (GThread& g : threads) {
            if (g.state != TState::Blocked || g.wait != Wait::Join) {
                continue;
            }
            for (size_t i = 0; i < g.handles.size(); ++i) {
                const GThread* w = ByHandle(g.handles[i]);
                if (w && w->state == TState::Exited) {
                    Wake(g, 0);
                    g.x[1] = i;
                    break;
                }
            }
        }
    }
    void Save(GThread& g) {
        g.x = jit->GetRegisters();
        g.sp = jit->GetSP();
        g.pc = jit->GetPC();
        g.v = jit->GetVectors();
        g.fpcr = jit->GetFpcr();
        g.fpsr = jit->GetFpsr();
        g.pstate = jit->GetPstate();
        g.tpidr = tpidr;
    }
    void Load(const GThread& g) {
        jit->SetRegisters(g.x);
        jit->SetSP(g.sp);
        jit->SetPC(g.pc);
        jit->SetVectors(g.v);
        jit->SetFpcr(g.fpcr);
        jit->SetFpsr(g.fpsr);
        jit->SetPstate(g.pstate);
        tpidr = g.tpidr;
        jit->ClearExclusiveState();  // a context switch clears the local monitor
    }
    // After every Run(): the next ready thread, round robin, at the end of a slice or when the running one blocked, yielded or exited
    void Schedule() {
        if (threads.size() == 1 && !switch_needed) {
            return;
        }
        switch_needed = false;
        Save(threads[cur]);
        for (size_t k = 1; k <= threads.size(); ++k) {
            const size_t i = (cur + k) % threads.size();
            if (threads[i].state == TState::Ready) {
                if (i != cur) {
                    cur = i;
                    Load(threads[i]);
                }
                return;
            }
        }
        Fail("every guest thread waits (a deadlock)");
    }
    bool Word(uint64_t addr) { return (addr & 3) == 0 && Inside(addr, 4); }

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
            ++lines;
            if (!quiet) {
                std::fwrite(mem.data() + (s - kBase), 1, n, stdout);
                std::fputc('\n', stdout);
            }
            jit->SetRegister(0, 0);
            return;
        }
        case 0x07:  // ExitProcess
            exited = true;
            Stop();
            return;
        case 0x08: {  // CreateThread(x1 entry, x2 argument, x3 stack top, w4 priority, w5 core): w0 result, w1 handle
            GThread g;
            g.handle = kFirstHandle + static_cast<uint32_t>(threads.size());
            g.x[0] = jit->GetRegister(2);
            g.sp = jit->GetRegister(3);
            g.pc = jit->GetRegister(1);  // x30 = 0: a return from the entry runs into no code (Horizon requires svcExitThread)
            g.fpcr = jit->GetFpcr();
            threads.push_back(g);
            jit->SetRegister(0, 0);
            jit->SetRegister(1, g.handle);
            return;
        }
        case 0x09: {  // StartThread(w0 handle)
            GThread* g = ByHandle(static_cast<uint32_t>(jit->GetRegister(0)));
            if (!g || g->state != TState::Created) {
                jit->SetRegister(0, g ? kInvalidState : kInvalidHandle);
                return;
            }
            g->state = TState::Ready;
            jit->SetRegister(0, 0);
            return;
        }
        case 0x0A:  // ExitThread
            if (cur == 0) {
                Fail("svcExitThread on the main thread");
                return;
            }
            threads[cur].state = TState::Exited;
            WakeJoiners();
            Yield();
            return;
        case 0x0B:  // SleepThread: a yield to the next ready thread
            Yield();
            return;
        case 0x16:  // CloseHandle
            jit->SetRegister(0, ByHandle(static_cast<uint32_t>(jit->GetRegister(0))) ? 0 : kInvalidHandle);
            return;
        case 0x18: {  // WaitSynchronization(x1 handles, w2 count, x3 timeout): w0 result, w1 index of the exited thread
            const uint64_t handles = jit->GetRegister(1);
            const int32_t count = static_cast<int32_t>(jit->GetRegister(2));
            if (count < 1 || count > 0x40) {
                jit->SetRegister(0, kOutOfRange);
                return;
            }
            GThread& me = threads[cur];
            me.handles.clear();
            for (int32_t i = 0; i < count; ++i) {
                const uint32_t h = static_cast<uint32_t>(Load(handles + 4u * static_cast<uint64_t>(i), 4));
                if (!ByHandle(h)) {
                    jit->SetRegister(0, kInvalidHandle);
                    return;
                }
                me.handles.push_back(h);
            }
            for (size_t i = 0; i < me.handles.size(); ++i) {
                if (ByHandle(me.handles[i])->state == TState::Exited) {
                    jit->SetRegister(0, 0);
                    jit->SetRegister(1, i);
                    return;
                }
            }
            Block(Wait::Join);
            return;
        }
        case 0x1A: {  // ArbitrateLock(w0 owner handle, x1 address, w2 tag)
            const uint32_t owner = static_cast<uint32_t>(jit->GetRegister(0));
            const uint64_t addr = jit->GetRegister(1);
            if (!Word(addr) || static_cast<uint32_t>(Load(addr, 4)) != (owner | kWaitMask)) {
                jit->SetRegister(0, 0);
                return;
            }
            threads[cur].addr = addr;
            threads[cur].tag = static_cast<uint32_t>(jit->GetRegister(2));
            Block(Wait::Lock);
            return;
        }
        case 0x1B: {  // ArbitrateUnlock(x0 address): the first waiter takes the lock
            const uint64_t addr = jit->GetRegister(0);
            GThread* first = nullptr;
            unsigned waiters = 0;
            for (GThread& g : threads) {
                if (g.state == TState::Blocked && g.wait == Wait::Lock && g.addr == addr) {
                    ++waiters;
                    if (!first || g.seq < first->seq) {
                        first = &g;
                    }
                }
            }
            Store(addr, 4, first ? (first->tag | (waiters > 1 ? kWaitMask : 0)) : 0);
            if (first) {
                Wake(*first, 0);
            }
            jit->SetRegister(0, 0);
            return;
        }
        case 0x34: {  // WaitForAddress(x0 address, w1 type, w2 value, x3 timeout)
            const uint64_t addr = jit->GetRegister(0);
            const uint32_t type = static_cast<uint32_t>(jit->GetRegister(1));
            const int32_t value = static_cast<int32_t>(jit->GetRegister(2));
            const int32_t v = Word(addr) ? static_cast<int32_t>(Load(addr, 4)) : 0;
            if (type > 2) {
                jit->SetRegister(0, kInvalidEnumValue);
                return;
            }
            if ((type == 2 && v != value) || (type < 2 && !(v < value))) {
                jit->SetRegister(0, kInvalidState);
                return;
            }
            if (type == 1) {
                Store(addr, 4, static_cast<uint32_t>(v - 1));
            }
            threads[cur].addr = addr;
            Block(Wait::Addr);
            return;
        }
        case 0x35: {  // SignalToAddress(x0 address, w1 type, w2 value, w3 count)
            const uint64_t addr = jit->GetRegister(0);
            const uint32_t type = static_cast<uint32_t>(jit->GetRegister(1));
            const int32_t value = static_cast<int32_t>(jit->GetRegister(2)), count = static_cast<int32_t>(jit->GetRegister(3));
            if (type == 1) {
                if (static_cast<int32_t>(Load(addr, 4)) != value) {
                    jit->SetRegister(0, kInvalidState);
                    return;
                }
                Store(addr, 4, static_cast<uint32_t>(value + 1));
            } else if (type != 0) {
                Fail("SignalToAddress of type " + std::to_string(type));
                return;
            }
            std::vector<GThread*> ws;
            for (GThread& g : threads) {
                if (g.state == TState::Blocked && g.wait == Wait::Addr && g.addr == addr) {
                    ws.push_back(&g);
                }
            }
            std::sort(ws.begin(), ws.end(), [](const GThread* a, const GThread* b) { return a->seq < b->seq; });
            for (size_t i = 0; i < ws.size() && (count <= 0 || i < static_cast<size_t>(count)); ++i) {
                Wake(*ws[i], 0);
            }
            jit->SetRegister(0, 0);
            return;
        }
        default:
            Fail("svc " + Hex(swi) + " at module+" + Hex(jit->GetPC() - 4 - kBase) + ": the corpus run time does not answer it");
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
    std::uint64_t GetTicksRemaining() override { return std::min(ticks, kSlice); }  // a slice at most: then the next thread
    std::uint64_t GetCNTPCT() override { return 0; }

    const nxaot::module::Image& image;
    ExclusiveMonitor monitor;
    std::unique_ptr<A64::Jit> jit;
    Memory mem;
    uint64_t tpidr = 0, tpidrro = 0, ticks = 0, executed = 0, lines = 0;
    bool done = false, exited = false, quiet = false;
    std::string failure;
};

int Usage() {
    std::fprintf(stderr, "usage: nxaot_run <module.nro | name.aot.tsv> [--max-instructions N] [--set <symbol>=<value>]... [--quiet] [--fastmem]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    uint64_t budget = 2'000'000'000ull;
    bool quiet = false, fastmem = false;
    std::vector<std::pair<std::string, uint64_t>> sets;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--max-instructions" && i + 1 < argc) {
            budget = std::strtoull(argv[++i], nullptr, 0);
        } else if (a == "--set" && i + 1 < argc) {
            const std::string kv = argv[++i];
            const auto eq = kv.find('=');
            if (eq == std::string::npos) {
                return Usage();
            }
            sets.emplace_back(kv.substr(0, eq), std::strtoull(kv.c_str() + eq + 1, nullptr, 0));
        } else if (a == "--quiet") {
            quiet = true;
        } else if (a == "--fastmem") {
            fastmem = true;
        } else if (path.empty() && !a.empty() && a[0] != '-') {
            path = a;
        } else {
            return Usage();
        }
    }
    if (path.empty()) {
        return Usage();
    }
    std::string error;
    const auto image = nxaot::module::LoadModule(path, &error);
    if (!image) {
        std::fprintf(stderr, "nxaot_run: %s\n", error.c_str());
        return 2;
    }
    Runner r{*image, fastmem};
    r.quiet = quiet;
    for (const auto& [name, value] : sets) {
        const nxaot::module::Symbol* sym = nullptr;
        for (const auto& s : image->symbols) {
            if (s.name == name && s.Defined()) {
                sym = &s;
            }
        }
        if (!sym || sym->value + 8 > r.mem.size()) {
            std::fprintf(stderr, "nxaot_run: --set: no symbol %s in the image\n", name.c_str());
            return 2;
        }
        std::memcpy(r.mem.data() + sym->value, &value, 8);
    }
    r.ticks = budget;
    r.jit->SetPC(kBase + image->text_off);
    r.jit->SetRegister(0, 0);           // the homebrew ABI's environment pointer: none
    r.jit->SetRegister(1, ~0ull);       // and main thread handle (libnx's crt0: x1 = -1 on NRO entry)
    r.jit->SetSP(0);                    // crt0 sets its own stack
    r.jit->SetFpcr(0);
    const auto t0 = std::chrono::steady_clock::now();
    while (!r.done) {
        if (r.ticks == 0 && !fastmem) {
            r.Fail("still running after " + std::to_string(budget) + " instructions");
            break;
        }
        r.jit->ClearHalt(HaltReason::UserDefined1);
        r.jit->ClearHalt(HaltReason::UserDefined2);
        r.jit->Run();
        if (!r.done) {
            r.Schedule();
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::fflush(stdout);
    if (!r.exited) {
        std::printf("[RED] nxaot_run: %s\n", r.failure.c_str());
        return 1;
    }
    std::fprintf(stderr, "nxaot_run: %s left through svcExitProcess after %llu instructions in %.6f s (%s), %llu lines\n", path.c_str(),
                 static_cast<unsigned long long>(r.executed), seconds, fastmem ? "fastmem" : "memory callbacks",
                 static_cast<unsigned long long>(r.lines));
    return 0;
}
