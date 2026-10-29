// nxaot_bench: the cost of the NaN-chain checks (aot-recompiler.md section 7.3; phase 2's "the cost of chain checks measured", section 14) and
// what lifted floating-point and SIMD code compiles to. Kernels of A64 code (a 4x4 single-precision matrix multiply with fmul / fmla by element,
// a chain of eight dependent vector fmla, a scalar fmadd / fadd / fmul chain with a store; each a loop counted down in x3) are lifted as one
// region each and run for many iterations:
//   chains     the NaN-chain fast path (NaNMode::Chains): one check per value that leaves a chain
//   per-op     every P operation checks its own result (NaNMode::PerOp, the way dynarmic's backend does)
//   unchecked  no NaN fix-up (NaNMode::Unchecked: inexact, the lower bound the checks are measured against)
// each with the cumulative exception flags modelled (strict floating point) and not; and the same code on dynarmic's JIT for comparison.
// It prints ns per iteration and the x86 instructions of the loop body lifted alone (one iteration, no loop), and the instructions each guest
// memory access of a load / store kernel became (section 6.4: one x86 instruction per access). Then the measurements of phase 1
// (src/bench/calls.h): the calling conventions, the dispatcher, a jump table, int_ops natively against dynarmic, the pack stage's throughput.
//
//   nxaot_bench [--cpu <llvm cpu>] [--iterations N] [--asm] [--sections nan,calls,dispatch,spills,int_ops,pack] [--call-iterations N]
//               [--rounds N] [--work <dir>] [--nxaot-run <path>]
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <immintrin.h>
#include <sys/mman.h>
#include <unistd.h>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "bench/calls.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "lift/jit.h"
#include "lift/lifter.h"
#include "nxaot/guest_state.h"
#include "nxaot_lock.h"

using namespace nxaot;

namespace {

constexpr uint64_t kGuestBase = 0x10000000, kImageSize = 0x10000, kCodeBase = kGuestBase + 0x8000;
constexpr uint64_t kA = kGuestBase + 0x1000, kB = kGuestBase + 0x1100, kOut = kGuestBase + 0x1200;

struct Kernel {
    const char* name;
    std::vector<uint32_t> loop;  // ends with subs x3, x3, #1; b.ne <start>
    std::vector<uint32_t> body;  // one iteration, for the instruction count
    const char* what;
};

const std::vector<Kernel>& Kernels() {
    static const std::vector<Kernel> k = {
        {"matmul4x4",
         {0x4c402800, 0x4c402824, 0x4f849010, 0x4fa41030, 0x4f841850, 0x4fa41870, 0x4f859011, 0x4fa51031, 0x4f851851, 0x4fa51871, 0x4f869012,
          0x4fa61032, 0x4f861852, 0x4fa61872, 0x4f879013, 0x4fa71033, 0x4f871853, 0x4fa71873, 0x4c002850, 0xf1000463, 0x54fffd81},
         {0x4c402800, 0x4c402824, 0x4f849010, 0x4fa41030, 0x4f841850, 0x4fa41870, 0x4f859011, 0x4fa51031, 0x4f851851, 0x4fa51871, 0x4f869012,
          0x4fa61032, 0x4f861852, 0x4fa61872, 0x4f879013, 0x4fa71033, 0x4f871853, 0x4fa71873, 0x4c002850},
         "ld1 {v0-v3}, [x0]; ld1 {v4-v7}, [x1]; 4 x (fmul + 3 fmla by element); st1 {v16-v19}, [x2]"},
        {"fmla_chain",
         {0x4e22cc20, 0x4e22cc20, 0x4e22cc20, 0x4e22cc20, 0x4e22cc20, 0x4e22cc20, 0x4e22cc20, 0x4e22cc20, 0xf1000463, 0x54fffee1},
         {0x4e22cc20, 0x4e22cc20, 0x4e22cc20, 0x4e22cc20, 0x4e22cc20, 0x4e22cc20, 0x4e22cc20, 0x4e22cc20},
         "8 x fmla v0.4s, v1.4s, v2.4s (dependent)"},
        {"scalar_chain",
         {0x1f010800, 0x1f010800, 0x1f010800, 0x1f010800, 0x1e232804, 0x1e240885, 0xbd000045, 0xf1000463, 0x54ffff01},
         {0x1f010800, 0x1f010800, 0x1f010800, 0x1f010800, 0x1e232804, 0x1e240885, 0xbd000045},
         "4 x fmadd s0, s0, s1, s2; fadd s4, s0, s3; fmul s5, s4, s4; str s5, [x2]"},
    };
    return k;
}

// ldr q8, [x0, #16]; str q8, [x1, #32]; ldp q9, q10, [x0, #64]; ld1 {v12.4s, v13.4s}, [x1]; st1 {v12.b}[0], [x2]; ldr d14, [x0, #24];
// str s14, [x1, #20]
const std::vector<uint32_t> kMemory = {0x3dc00408, 0x3d800828, 0xad412809, 0x4c40a82c, 0x4d00804c, 0xfd400c0e, 0xbd00142e};

struct Mode {
    const char* name;
    lift::NaNMode nan;
    bool flags;
};

const Mode kModes[] = {
    {"chains", lift::NaNMode::Chains, false}, {"per-op", lift::NaNMode::PerOp, false}, {"unchecked", lift::NaNMode::Unchecked, false},
    {"chains+flags", lift::NaNMode::Chains, true}, {"per-op+flags", lift::NaNMode::PerOp, true}, {"unchecked+flags", lift::NaNMode::Unchecked, true},
};

uint8_t* g_image = nullptr;

void FillImage() {
    float* a = reinterpret_cast<float*>(kA);
    float* b = reinterpret_cast<float*>(kB);
    for (int i = 0; i < 16; ++i) {
        a[i] = 0.25f * static_cast<float>(i + 1);
        b[i] = 1.0f / static_cast<float>(i + 3);
    }
}

void InitState(dm_guest_state& st, uint64_t iterations) {
    std::memset(&st, 0, sizeof st);
    st.x[0] = kA;
    st.x[1] = kB;
    st.x[2] = kOut;
    st.x[3] = iterations;
    const float v1[4] = {0.999f, 1.001f, 0.5f, 2.0f}, v2[4] = {1e-3f, -1e-3f, 0.25f, 0.0f};
    std::memcpy(st.v[1], v1, 16);
    std::memcpy(st.v[2], v2, 16);
    const float s[4] = {1.0f, 0.9999f, 1e-4f, 3.0f};
    std::memcpy(st.v[0], &s[0], 4);
    std::memcpy(st.v[3], &s[3], 4);
}

lift::Region RegionOf(const std::vector<uint32_t>& code) {
    lift::Region r;
    r.entry = kCodeBase;
    r.lo = kCodeBase;
    r.hi = kCodeBase + 4 * code.size();
    return r;
}

decode::CodeReader ReaderOf(const std::vector<uint32_t>* code) {
    return [code](uint64_t a) -> std::optional<uint32_t> {
        if (a < kCodeBase || a >= kCodeBase + 4 * code->size()) {
            return std::nullopt;
        }
        return (*code)[(a - kCodeBase) / 4];
    };
}

double TimeIt(void (*fn)(dm_guest_state*), uint64_t iterations, int repeats) {
    double best = 1e30;
    for (int r = 0; r < repeats; ++r) {
        alignas(64) static dm_guest_state st;
        InitState(st, iterations);
        const unsigned host = _mm_getcsr();
        const auto t0 = std::chrono::steady_clock::now();
        _mm_setcsr(lift::GuestMxcsr(0));
        fn(&st);
        _mm_setcsr(host);
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(iterations));
    }
    return best;
}

// dynarmic's JIT on the same code, for comparison
struct DynarmicRun final : Dynarmic::A64::UserCallbacks {
    const std::vector<uint32_t>* code = nullptr;
    uint64_t ticks = 0;
    bool done = false;
    std::unique_ptr<Dynarmic::A64::Jit> jit;
    DynarmicRun() {
        Dynarmic::A64::UserConfig conf;
        conf.callbacks = this;
        conf.optimizations = Dynarmic::all_safe_optimizations;
        jit = std::make_unique<Dynarmic::A64::Jit>(conf);
    }
    std::optional<std::uint32_t> MemoryReadCode(Dynarmic::A64::VAddr a) override {
        if (a < kCodeBase || a >= kCodeBase + 4 * code->size()) {
            return std::nullopt;
        }
        return (*code)[(a - kCodeBase) / 4];
    }
    template<typename T>
    T Load(uint64_t a) {
        T v;
        std::memcpy(&v, reinterpret_cast<void*>(a), sizeof v);
        return v;
    }
    template<typename T>
    void Store(uint64_t a, T v) {
        std::memcpy(reinterpret_cast<void*>(a), &v, sizeof v);
    }
    std::uint8_t MemoryRead8(Dynarmic::A64::VAddr a) override { return Load<uint8_t>(a); }
    std::uint16_t MemoryRead16(Dynarmic::A64::VAddr a) override { return Load<uint16_t>(a); }
    std::uint32_t MemoryRead32(Dynarmic::A64::VAddr a) override { return Load<uint32_t>(a); }
    std::uint64_t MemoryRead64(Dynarmic::A64::VAddr a) override { return Load<uint64_t>(a); }
    Dynarmic::A64::Vector MemoryRead128(Dynarmic::A64::VAddr a) override { return {Load<uint64_t>(a), Load<uint64_t>(a + 8)}; }
    void MemoryWrite8(Dynarmic::A64::VAddr a, std::uint8_t v) override { Store(a, v); }
    void MemoryWrite16(Dynarmic::A64::VAddr a, std::uint16_t v) override { Store(a, v); }
    void MemoryWrite32(Dynarmic::A64::VAddr a, std::uint32_t v) override { Store(a, v); }
    void MemoryWrite64(Dynarmic::A64::VAddr a, std::uint64_t v) override { Store(a, v); }
    void MemoryWrite128(Dynarmic::A64::VAddr a, Dynarmic::A64::Vector v) override {
        Store(a, v[0]);
        Store(a + 8, v[1]);
    }
    void InterpreterFallback(Dynarmic::A64::VAddr, size_t) override { Stop(); }
    void CallSVC(std::uint32_t) override { Stop(); }
    void ExceptionRaised(Dynarmic::A64::VAddr, Dynarmic::A64::Exception) override { Stop(); }
    void AddTicks(std::uint64_t t) override { ticks = t > ticks ? 0 : ticks - t; }
    std::uint64_t GetTicksRemaining() override { return ticks; }
    std::uint64_t GetCNTPCT() override { return 0; }
    void Stop() {
        done = true;
        jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
    }
    double Time(const std::vector<uint32_t>& c, uint64_t iterations, int repeats) {
        code = &c;
        jit->ClearCache();
        double best = 1e30;
        for (int r = 0; r < repeats; ++r) {
            alignas(64) static dm_guest_state st;
            InitState(st, iterations);
            jit->Reset();
            std::array<uint64_t, 31> x{};
            for (int i = 0; i < 31; ++i) {
                x[i] = st.x[i];
            }
            jit->SetRegisters(x);
            std::array<Dynarmic::A64::Vector, 32> v{};
            for (int i = 0; i < 32; ++i) {
                std::memcpy(v[i].data(), st.v[i], 16);
            }
            jit->SetVectors(v);
            jit->SetPC(kCodeBase);
            jit->SetFpcr(0);
            ticks = ~0ull >> 1;
            done = false;
            const auto t0 = std::chrono::steady_clock::now();
            while (!done) {
                jit->ClearHalt(Dynarmic::HaltReason::UserDefined1);
                jit->Run();
            }
            const auto t1 = std::chrono::steady_clock::now();
            best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(iterations));
        }
        return best;
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::string cpu;
    uint64_t iterations = 2'000'000;
    bool show_asm = false;
    std::string sections = "nan,calls,dispatch,spills,int_ops,pack";
    bench::CallsOptions co;
    co.corpus = NXAOT_CORPUS_DIR;
    {
        // nxaot_run beside this executable (the build folder)
        char self[4096] = {};
        const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        std::string dir = n > 0 ? std::string(self, static_cast<size_t>(n)) : std::string();
        dir = dir.substr(0, dir.find_last_of('/'));
        co.nxaot_run = dir + "/nxaot_run";
        co.work = dir + "/bench";
    }
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--cpu" && i + 1 < argc) {
            cpu = argv[++i];
        } else if (a == "--iterations" && i + 1 < argc) {
            iterations = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--asm") {
            show_asm = true;
        } else if (a == "--sections" && i + 1 < argc) {
            sections = argv[++i];
        } else if (a == "--call-iterations" && i + 1 < argc) {
            co.iterations = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--rounds" && i + 1 < argc) {
            co.rounds = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--work" && i + 1 < argc) {
            co.work = argv[++i];
        } else if (a == "--nxaot-run" && i + 1 < argc) {
            co.nxaot_run = argv[++i];
        } else {
            std::fprintf(stderr, "usage: nxaot_bench [--cpu <llvm cpu>] [--iterations N] [--asm] [--sections nan,calls,dispatch,spills,int_ops,pack]\n"
                                 "                   [--call-iterations N] [--rounds N] [--work <dir>] [--nxaot-run <path>]\n");
            return 2;
        }
    }
    const auto has = [&](const char* s) { return ("," + sections + ",").find(std::string(",") + s + ",") != std::string::npos; };
    co.cpu = cpu;
    co.calls = has("calls");
    co.dispatch = has("dispatch");
    co.spills = has("spills");
    co.int_ops = has("int_ops");
    co.pack = has("pack");
    if (!has("nan")) {
        return co.calls || co.dispatch || co.spills || co.int_ops || co.pack ? bench::RunCalls(co) : 0;
    }
    void* want = reinterpret_cast<void*>(kGuestBase);
    if (mmap(want, kImageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != want) {
        std::fprintf(stderr, "nxaot_bench: cannot map the guest image\n");
        return 2;
    }
    g_image = reinterpret_cast<uint8_t*>(kGuestBase);
    FillImage();

    std::string error;
    auto jit = lift::Jit::Create(cpu, &error);
    if (!jit) {
        std::fprintf(stderr, "nxaot_bench: %s\n", error.c_str());
        return 2;
    }
    std::printf("nxaot_bench: LLVM %s, dynarmic %.12s, target CPU %s, %" PRIu64 " iterations, best of 5\n", NXAOT_LOCK_LLVM, NXAOT_LOCK_DYNARMIC,
                jit->Cpu().c_str(), iterations);
    const lift::Target target = jit->TargetFeatures();

    auto lift_one = [&](const std::vector<uint32_t>& code, const Mode& m, const std::string& name, std::string* asm_out) -> void (*)(dm_guest_state*) {
        auto context = std::make_unique<llvm::LLVMContext>();
        auto module = jit->NewModule(*context, name);
        lift::LiftOptions o;
        o.fpsr_flags = m.flags;
        o.nan_mode = m.nan;
        o.target = target;
        const lift::Region region = RegionOf(code);
        const auto l = lift::LiftRegion(*module, name, region, ReaderOf(&code), o);
        if (!l.function) {
            std::fprintf(stderr, "nxaot_bench: %s: %s\n", name.c_str(), l.error.empty() ? "unsupported" : l.error.c_str());
            std::exit(1);
        }
        if (asm_out) {
            *asm_out = jit->Assembly(*module, &error);
        }
        if (!jit->Add(std::move(module), std::move(context), &error)) {
            std::fprintf(stderr, "nxaot_bench: %s\n", error.c_str());
            std::exit(1);
        }
        void* fn = jit->Lookup(name, &error);
        return reinterpret_cast<void (*)(dm_guest_state*)>(fn);
    };
    // x86 instructions of a function in an assembly listing: lines of a mnemonic, not labels or directives
    // hot_only: the blocks a normal run executes, not the cold ones the lowering marked (their names in the listing's block comments)
    auto count = [](const std::string& text, std::map<std::string, size_t>* mnemonics, bool hot_only = false) {
        static const char* const cold[] = {"%nan_chain", "%step_fallback", "%fma_fz", "%fmla_fz", "%fmla_lane"};
        size_t n = 0;
        size_t pos = 0;
        bool in_cold = false;
        while (pos < text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) {
                end = text.size();
            }
            std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            if (!line.empty() && line[0] != '\t' && line.find(':') != std::string::npos) {
                in_cold = false;  // a label: a new block, cold when its comment names a cold block
            }
            if (line.find("# %") != std::string::npos) {
                in_cold = false;
                for (const char* c : cold) {
                    if (line.find(c) != std::string::npos) {
                        in_cold = true;
                    }
                }
            }
            if (line.empty() || line[0] != '\t' || line.size() < 2 || line[1] == '.' || line[1] == '#') {
                continue;
            }
            if (hot_only && in_cold) {
                continue;
            }
            ++n;
            if (mnemonics) {
                const size_t sp = line.find_first_of(" \t", 1);
                ++(*mnemonics)[line.substr(1, sp == std::string::npos ? std::string::npos : sp - 1)];
            }
        }
        return n;
    };

    DynarmicRun dyn;
    int id = 0;
    for (const auto& k : Kernels()) {
        std::printf("\n%s: %s\n", k.name, k.what);
        std::printf("  %-18s %10s %14s %14s\n", "mode", "ns/iter", "x86 insns/body", "of them hot");
        for (const Mode& m : kModes) {
            std::string body_asm;
            lift_one(k.body, m, std::string("body") + std::to_string(id++), &body_asm);
            std::string loop_asm;
            void (*fn)(dm_guest_state*) = lift_one(k.loop, m, std::string("loop") + std::to_string(id++), &loop_asm);
            const double ns = TimeIt(fn, iterations, 5);
            std::printf("  %-18s %10.2f %14zu %14zu\n", m.name, ns, count(body_asm, nullptr, false), count(body_asm, nullptr, true));
            if (show_asm && !m.flags) {
                std::printf("---- %s, %s, one iteration\n%s\n---- %s, %s, the loop\n%s\n", k.name, m.name, body_asm.c_str(), k.name, m.name,
                            loop_asm.c_str());
            }
        }
        std::printf("  %-18s %10.2f\n", "dynarmic JIT", dyn.Time(k.loop, iterations, 5));
    }

    // Section 6.4: each guest memory access one x86 instruction
    std::string mem_asm;
    lift_one(kMemory, kModes[0], "memory", &mem_asm);
    std::map<std::string, size_t> mn;
    const size_t total = count(mem_asm, &mn);
    std::printf("\nmemory: ldr q, str q, ldp q, ld1 {2 x 4s}, st1 {b}[0], ldr d, str s (8 guest accesses: ldp and ld1 make 2 each, so 9; "
                "the 16-byte ones one each):\n  %zu x86 instructions in the function; mnemonics:", total);
    for (const auto& [name, n] : mn) {
        std::printf(" %s %zu", name.c_str(), n);
    }
    std::printf("\n");
    if (show_asm) {
        std::printf("---- memory kernel\n%s\n", mem_asm.c_str());
    }
    if (co.calls || co.dispatch || co.spills || co.int_ops || co.pack) {
        return bench::RunCalls(co);
    }
    return 0;
}
