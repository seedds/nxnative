// nxaot_funcdiff: the differential test per function of docs/design/aot-recompiler.md section 12, item 2, in its simple form. Every function of
// a module recompiled with --all-entries (each has a standard entry) runs from the same register and memory snapshot under the recompiled code
// (src/rt/host.h) and under dynarmic's A64 JIT on the same memory (one-to-one addresses, through callbacks that allow exactly what the native
// mapping allows), several times with registers drawn from a seed: pointers into the module's data, into a scratch area and into the stack,
// small integers, and values that are no valid address. Each run ends at the return to the host, at the first svc, at a trap or a fault, or at
// the time limit. When both end the same way, the final registers and every written page must match:
//   correctness mode (NXAOT_FLAG_CORRECTNESS)  every register: x0..x30, sp, NZCV, v0..v31 (the block holds them all at every return and svc)
//   summary mode                              what the convention gives a caller: x0, x1 and v0..v3 at a return, x0..x7 at an svc
// Runs that leave for the JIT (an indirect branch to a random address, a non-local transfer), time out, or end differently because a random
// pointer made one executor fault (both fault, possibly at different instructions) are counted apart. Exit 1 on any mismatch.
//   nxaot_funcdiff <library.so> <module.nro | name.aot.tsv> [--seeds N] [--timeout-ms T] [-v]
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "rt/host.h"

namespace {

using namespace Dynarmic;
using nxaot::rt::Host;

struct Regs {
    uint64_t x[31] = {}, sp = 0, pc = 0;
    uint32_t nzcv = 0;
    uint8_t v[32][16] = {};
};

enum class End { Returned, Svc, Trap, Fault, Jit, Timeout, Other };
const char* EndName(End e) {
    switch (e) {
    case End::Returned: return "returned";
    case End::Svc: return "svc";
    case End::Trap: return "trap";
    case End::Fault: return "fault";
    case End::Jit: return "jit";
    case End::Timeout: return "timeout";
    default: return "other";
    }
}

struct Outcome {
    End end = End::Other;
    Regs r;
    uint32_t svc = 0;
    uint64_t where = 0;  // trap: module offset; fault: address
    std::vector<uint8_t> mem;  // the writable regions after the run
};

uint64_t Mix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// The regions a guest run may write, in one order (snapshot, restore, compare): the module's data and bss, the stack, TLS and the scratch area
struct Region {
    uint64_t addr, size;
};

class Memory {
public:
    explicit Memory(const nxaot::module::Image& img) {
        const uint64_t base = nxaot::rt::kModuleBase;
        const uint64_t mapped = (img.bytes.size() + img.bss_size + 0xFFF) & ~0xFFFull;
        const uint64_t data = img.data_off & ~0xFFFull;
        text_end = base + data;  // text and read-only data: readable only
        regions = {{base + data, mapped - data}, {nxaot::rt::kStackBase, nxaot::rt::kStackSize}, {nxaot::rt::kTlsBase, nxaot::rt::kTlsSize},
                   {nxaot::rt::kScratchBase, nxaot::rt::kScratchSize}};
        module_lo = base;
        module_hi = base + mapped;
    }
    std::vector<uint8_t> Save() const {
        std::vector<uint8_t> out;
        for (const auto& r : regions) {
            const auto* p = reinterpret_cast<const uint8_t*>(r.addr);
            out.insert(out.end(), p, p + r.size);
        }
        return out;
    }
    void Restore(const std::vector<uint8_t>& snap) const {
        size_t at = 0;
        for (const auto& r : regions) {
            std::memcpy(reinterpret_cast<void*>(r.addr), snap.data() + at, r.size);
            at += r.size;
        }
    }
    bool Readable(uint64_t a, size_t n) const {
        if (a >= module_lo && a + n <= module_hi && a + n >= a) return true;
        return Writable(a, n);
    }
    bool Writable(uint64_t a, size_t n) const {
        for (const auto& r : regions) {
            if (a >= r.addr && a + n <= r.addr + r.size && a + n >= a) return true;
        }
        return false;
    }
    // The first differing 8-byte word of two saved images: its address and both values (address 0 when equal)
    uint64_t FirstDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint64_t* va, uint64_t* vb) const {
        size_t at = 0;
        for (const auto& r : regions) {
            for (uint64_t off = 0; off + 8 <= r.size; off += 8) {
                if (std::memcmp(a.data() + at + off, b.data() + at + off, 8) != 0) {
                    std::memcpy(va, a.data() + at + off, 8);
                    std::memcpy(vb, b.data() + at + off, 8);
                    return r.addr + off;
                }
            }
            at += r.size;
        }
        return 0;
    }
    std::vector<Region> regions;
    uint64_t text_end = 0, module_lo = 0, module_hi = 0;
};

// dynarmic on the same memory: guest addresses are host addresses, allowed where the native mapping allows them
struct Ref final : A64::UserCallbacks {
    Ref(const nxaot::module::Image& img, const Memory& m) : image(img), mem(m), monitor(1) {
        A64::UserConfig conf;
        conf.callbacks = this;
        conf.processor_id = 0;
        conf.global_monitor = &monitor;
        conf.hook_data_cache_operations = true;
        conf.cntfrq_el0 = 19'200'000;
        conf.ctr_el0 = 0x40000;
        conf.dczid_el0 = 4;
        conf.tpidr_el0 = &tpidr;
        conf.tpidrro_el0 = &tpidrro;
        conf.wall_clock_cntpct = true;
        conf.enable_cycle_counting = true;
        conf.define_unpredictable_behaviour = false;
        conf.code_cache_size = 64 * 1024 * 1024;
        conf.optimizations = all_safe_optimizations & ~OptimizationFlag::GetSetElimination;  // as nxaot_vec's reference (README, Known issues 1)
        jit = std::make_unique<A64::Jit>(conf);
    }
    void Stop(End e, uint64_t where_ = 0) {
        if (!done) {
            end = e;
            where = where_;
        }
        done = true;
        jit->HaltExecution(HaltReason::UserDefined1);
    }
    template<typename T>
    T Load(uint64_t a) {
        if (!mem.Readable(a, sizeof(T))) {
            Stop(End::Fault, a);
            return 0;
        }
        T v;
        std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T));
        return v;
    }
    template<typename T>
    void Store(uint64_t a, T v) {
        if (!mem.Writable(a, sizeof(T))) {
            Stop(End::Fault, a);
            return;
        }
        std::memcpy(reinterpret_cast<void*>(a), &v, sizeof(T));
    }
    template<typename T>
    bool Cas(uint64_t a, T v, T e) {
        if (!mem.Writable(a, sizeof(T))) {
            Stop(End::Fault, a);
            return false;
        }
        return __atomic_compare_exchange_n(reinterpret_cast<T*>(a), &e, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    std::optional<std::uint32_t> MemoryReadCode(A64::VAddr a) override {
        const uint64_t base = nxaot::rt::kModuleBase;
        if (a < base || a - base >= image.TextEnd() || (a & 3) || image.InData(a - base)) {
            return std::nullopt;
        }
        uint32_t w;
        std::memcpy(&w, reinterpret_cast<const void*>(a), 4);
        return w;
    }
    std::uint8_t MemoryRead8(A64::VAddr a) override { return Load<uint8_t>(a); }
    std::uint16_t MemoryRead16(A64::VAddr a) override { return Load<uint16_t>(a); }
    std::uint32_t MemoryRead32(A64::VAddr a) override { return Load<uint32_t>(a); }
    std::uint64_t MemoryRead64(A64::VAddr a) override { return Load<uint64_t>(a); }
    A64::Vector MemoryRead128(A64::VAddr a) override {
        if (!mem.Readable(a, 16)) {
            Stop(End::Fault, a);
            return {0, 0};
        }
        return {Load<uint64_t>(a), Load<uint64_t>(a + 8)};
    }
    void MemoryWrite8(A64::VAddr a, std::uint8_t v) override { Store(a, v); }
    void MemoryWrite16(A64::VAddr a, std::uint16_t v) override { Store(a, v); }
    void MemoryWrite32(A64::VAddr a, std::uint32_t v) override { Store(a, v); }
    void MemoryWrite64(A64::VAddr a, std::uint64_t v) override { Store(a, v); }
    void MemoryWrite128(A64::VAddr a, A64::Vector v) override {
        if (!mem.Writable(a, 16)) {
            Stop(End::Fault, a);
            return;
        }
        Store(a, v[0]);
        Store(a + 8, v[1]);
    }
    bool MemoryWriteExclusive8(A64::VAddr a, std::uint8_t v, std::uint8_t e) override { return Cas<uint8_t>(a, v, e); }
    bool MemoryWriteExclusive16(A64::VAddr a, std::uint16_t v, std::uint16_t e) override { return Cas<uint16_t>(a, v, e); }
    bool MemoryWriteExclusive32(A64::VAddr a, std::uint32_t v, std::uint32_t e) override { return Cas<uint32_t>(a, v, e); }
    bool MemoryWriteExclusive64(A64::VAddr a, std::uint64_t v, std::uint64_t e) override { return Cas<uint64_t>(a, v, e); }
    bool MemoryWriteExclusive128(A64::VAddr a, A64::Vector v, A64::Vector e) override {
        if (!mem.Writable(a, 16)) {
            Stop(End::Fault, a);
            return false;
        }
        // One thread: a plain compare and store (as nxaot_run's reference does)
        uint64_t cur[2];
        std::memcpy(cur, reinterpret_cast<const void*>(a), 16);
        if (cur[0] != e[0] || cur[1] != e[1]) {
            return false;
        }
        std::memcpy(reinterpret_cast<void*>(a), v.data(), 16);
        return true;
    }
    void InterpreterFallback(A64::VAddr, size_t) override { Stop(End::Other); }
    void CallSVC(std::uint32_t swi) override {
        svc = swi;
        Stop(End::Svc);
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
        case A64::Exception::NoExecuteFault:
            return Stop(pc == nxaot::rt::kHostReturn ? End::Returned : End::Jit, pc);
        default:
            return Stop(End::Trap, pc - nxaot::rt::kModuleBase);
        }
    }
    void DataCacheOperationRaised(A64::DataCacheOperation op, A64::VAddr value) override {
        if (op == A64::DataCacheOperation::ZeroByVA) {
            for (uint64_t off = 0; off < 64; off += 8) {
                Store<uint64_t>((value & ~63ull) + off, 0);
            }
        }
    }
    void AddTicks(std::uint64_t t) override { ticks = t > ticks ? 0 : ticks - t; }
    std::uint64_t GetTicksRemaining() override { return ticks; }
    std::uint64_t GetCNTPCT() override { return 0; }

    Outcome Run(const Regs& in, uint64_t tls, uint64_t budget) {
        done = false;
        end = End::Other;
        ticks = budget;
        tpidr = tls;
        tpidrro = 0;
        monitor.Clear();
        jit->ClearExclusiveState();
        for (unsigned i = 0; i < 31; ++i) jit->SetRegister(i, in.x[i]);
        jit->SetSP(in.sp);
        jit->SetPC(in.pc);
        jit->SetPstate(in.nzcv);
        jit->SetFpcr(0);
        jit->SetFpsr(0);
        for (unsigned i = 0; i < 32; ++i) {
            A64::Vector q;
            std::memcpy(q.data(), in.v[i], 16);
            jit->SetVector(i, q);
        }
        while (!done) {
            if (ticks == 0) {
                Stop(End::Timeout);
                break;
            }
            jit->ClearHalt(HaltReason::UserDefined1);
            jit->Run();
        }
        Outcome o;
        o.end = end;
        o.where = where;
        o.svc = svc;
        for (unsigned i = 0; i < 31; ++i) o.r.x[i] = jit->GetRegister(i);
        o.r.sp = jit->GetSP();
        o.r.pc = jit->GetPC();
        o.r.nzcv = jit->GetPstate() & 0xF0000000u;
        for (unsigned i = 0; i < 32; ++i) {
            const A64::Vector q = jit->GetVector(i);
            std::memcpy(o.r.v[i], q.data(), 16);
        }
        return o;
    }

    const nxaot::module::Image& image;
    const Memory& mem;
    ExclusiveMonitor monitor;
    std::unique_ptr<A64::Jit> jit;
    uint64_t tpidr = 0, tpidrro = 0, ticks = 0, where = 0;
    uint32_t svc = 0;
    bool done = false;
    End end = End::Other;
};

Regs Draw(uint64_t seed, uint64_t offset, const nxaot::module::Image& img) {
    uint64_t s = seed * 0x100000001B3ull ^ offset;
    const uint64_t base = nxaot::rt::kModuleBase;
    const uint64_t data_lo = base + img.data_off, data_hi = base + img.bytes.size() + img.bss_size;
    auto value = [&](bool argument) -> uint64_t {
        const uint64_t r = Mix(s);
        const unsigned kind = static_cast<unsigned>(r % 100);
        if (kind < (argument ? 35u : 25u)) return Mix(s) % 256;  // small integers: counts, indices, characters
        if (kind < (argument ? 65u : 40u)) return nxaot::rt::kScratchBase + (Mix(s) % (nxaot::rt::kScratchSize / 2) & ~7ull);
        if (kind < (argument ? 80u : 50u) && data_hi > data_lo + 64) return data_lo + ((Mix(s) % (data_hi - data_lo - 64)) & ~7ull);
        return Mix(s) | 0x8000000000000000ull;  // no valid address (non-canonical): an access faults at once in both executors
    };
    Regs r;
    for (unsigned i = 0; i < 31; ++i) r.x[i] = value(i < 8);
    r.x[29] = nxaot::rt::kStackBase + nxaot::rt::kStackSize - 0x100;
    r.x[30] = nxaot::rt::kHostReturn;
    r.sp = nxaot::rt::kStackBase + nxaot::rt::kStackSize - 0x1000;
    r.pc = base + offset;
    r.nzcv = static_cast<uint32_t>(Mix(s) & 0xF0000000u);
    for (auto& q : r.v) {
        const uint64_t a = Mix(s), b = Mix(s);
        std::memcpy(q, &a, 8);
        std::memcpy(q + 8, &b, 8);
    }
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    std::string library, path;
    unsigned seeds = 3, timeout_ms = 200;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seeds" && i + 1 < argc) {
            seeds = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 0));
        } else if (a == "--timeout-ms" && i + 1 < argc) {
            timeout_ms = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 0));
        } else if (a == "-v") {
            verbose = true;
        } else if (library.empty() && a[0] != '-') {
            library = a;
        } else if (path.empty() && a[0] != '-') {
            path = a;
        } else {
            library.clear();
            break;
        }
    }
    if (library.empty() || path.empty()) {
        std::fprintf(stderr, "usage: nxaot_funcdiff <library.so> <module.nro | name.aot.tsv> [--seeds N] [--timeout-ms T] [-v]\n");
        return 2;
    }
    std::string error;
    const auto image = nxaot::module::LoadModule(path, &error);
    if (!image) {
        std::fprintf(stderr, "nxaot_funcdiff: %s\n", error.c_str());
        return 2;
    }
    nxaot::rt::Options options;
    options.relocate = true;  // pointers in data are valid before any guest code runs
    options.quiet = true;
    // dynarmic first: its JIT installs a SIGSEGV handler of its own (for fastmem, unused here), which the host's then replaces, so that a
    // fault in recompiled code reaches the host's handler directly
    const Memory mem{*image};
    Ref ref{*image, mem};
    auto host = Host::Create(library, *image, options, &error);
    if (!host) {
        std::printf("[RED] nxaot_funcdiff: %s\n", error.c_str());
        return 2;
    }
    const nxaot_module_desc& d = host->Desc();
    if (!(d.flags & NXAOT_FLAG_ALL_ENTRIES)) {
        std::printf("[RED] nxaot_funcdiff: %s was not built with --all-entries\n", library.c_str());
        return 2;
    }
    const bool full = d.flags & NXAOT_FLAG_CORRECTNESS;
    host->ResetImage();
    host->Relocate();
    const std::vector<uint8_t> snapshot = mem.Save();
    std::map<std::string, uint64_t> counts;
    uint64_t compared = 0, mismatches = 0, functions = 0;
    for (uint64_t fi = 0; fi < d.fn_count; ++fi) {
        const uint64_t offset = d.fns[fi].offset;
        ++functions;
        for (unsigned seed = 0; seed < seeds; ++seed) {
            const Regs in = Draw(seed, offset, *image);
            // The recompiled code
            mem.Restore(snapshot);
            alignas(64) static dm_guest_state st;
            std::memset(&st, 0, sizeof st);
            std::memcpy(st.x, in.x, sizeof in.x);
            st.sp = in.sp;
            st.nzcv = in.nzcv;
            std::memcpy(st.v, in.v, sizeof in.v);
            st.tpidr_el0 = nxaot::rt::kTlsBase;
            const nxaot::rt::Result lr = host->Call(offset, &st, true, timeout_ms);
            Outcome l;
            switch (lr.status) {
            case nxaot::rt::kReturned: l.end = End::Returned; break;
            case nxaot::rt::kSvcStop: l.end = End::Svc; l.svc = lr.svc; break;
            case nxaot::rt::kTrap: l.end = End::Trap; l.where = lr.trap_offset; break;
            case nxaot::rt::kFault: l.end = End::Fault; l.where = lr.fault_address; break;
            case nxaot::rt::kJit: l.end = End::Jit; break;
            case nxaot::rt::kTimeout: l.end = End::Timeout; break;
            default: l.end = End::Other; break;
            }
            std::memcpy(l.r.x, st.x, sizeof st.x);
            l.r.sp = st.sp;
            l.r.pc = st.pc;
            l.r.nzcv = st.nzcv;
            std::memcpy(l.r.v, st.v, sizeof st.v);
            l.mem = mem.Save();
            // dynarmic
            mem.Restore(snapshot);
            Outcome r = ref.Run(in, nxaot::rt::kTlsBase, 20'000'000);
            r.mem = mem.Save();
            std::string key;
            if (l.end == End::Timeout || r.end == End::Timeout) {
                key = "timeout";
            } else if (l.end == End::Jit || r.end == End::Jit || l.end == End::Other || r.end == End::Other) {
                key = "left for the JIT";
            } else if (l.end == End::Fault && r.end == End::Fault) {
                key = "fault in both";
            } else if (l.end == End::Fault || r.end == End::Fault) {
                // A random pointer: one executor faulted where the other did not (a fault address the native mapping and the callbacks see
                // alike is not guaranteed for accesses that straddle a mapping's end); counted, not compared
                key = "fault in one";
            }
            if (!key.empty()) {
                ++counts[key];
                continue;
            }
            std::string diff;
            if (l.end != r.end) {
                diff = std::string("ended ") + EndName(l.end) + " natively, " + EndName(r.end) + " under dynarmic";
            } else if (l.end == End::Svc && l.svc != r.svc) {
                diff = "svc " + std::to_string(l.svc) + " natively, " + std::to_string(r.svc) + " under dynarmic";
            } else if (l.end == End::Trap && l.where != r.where) {
                diff = "trap at another instruction";
            } else {
                auto cmp = [&](const char* name, uint64_t a, uint64_t b) {
                    if (diff.empty() && a != b) {
                        char buf[160];
                        std::snprintf(buf, sizeof buf, "%s: native %#" PRIx64 ", dynarmic %#" PRIx64, name, a, b);
                        diff = buf;
                    }
                };
                const unsigned xs = full ? 31 : l.end == End::Svc ? 8 : 2;
                for (unsigned i = 0; i < xs; ++i) {
                    cmp(("x" + std::to_string(i)).c_str(), l.r.x[i], r.r.x[i]);
                }
                if (full) {
                    cmp("sp", l.r.sp, r.r.sp);
                    cmp("nzcv", l.r.nzcv, r.r.nzcv);
                }
                const unsigned vs = full ? 32 : l.end == End::Returned ? 4 : 0;
                for (unsigned i = 0; i < vs; ++i) {
                    uint64_t a[2], b[2];
                    std::memcpy(a, l.r.v[i], 16);
                    std::memcpy(b, r.r.v[i], 16);
                    cmp(("v" + std::to_string(i) + ".lo").c_str(), a[0], b[0]);
                    cmp(("v" + std::to_string(i) + ".hi").c_str(), a[1], b[1]);
                }
                uint64_t na = 0, da = 0;
                if (const uint64_t word = mem.FirstDiff(l.mem, r.mem, &na, &da); word && diff.empty()) {
                    char buf[160];
                    std::snprintf(buf, sizeof buf, "memory at %#" PRIx64 ": native %#" PRIx64 ", dynarmic %#" PRIx64, word, na, da);
                    diff = buf;
                }
            }
            ++compared;
            ++counts[std::string("compared, ended ") + EndName(l.end)];
            if (!diff.empty()) {
                ++mismatches;
                if (mismatches <= 20) {
                    std::printf("MISMATCH module+%#" PRIx64 " seed %u: %s\n", offset, seed, diff.c_str());
                }
            } else if (verbose) {
                std::printf("ok module+%#" PRIx64 " seed %u: %s\n", offset, seed, EndName(l.end));
            }
        }
    }
    for (const auto& [k, n] : counts) {
        std::printf("  %-28s %" PRIu64 "\n", k.c_str(), n);
    }
    std::printf("== nxaot_funcdiff %s: %" PRIu64 " functions x %u seeds, %" PRIu64 " compared (%s), %" PRIu64 " mismatches ==\n", image->name.c_str(),
                functions, seeds, compared, full ? "every register" : "the convention's results", mismatches);
    return mismatches || !compared ? 1 : 0;
}
