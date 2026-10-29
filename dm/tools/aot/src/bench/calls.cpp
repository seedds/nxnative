#include "bench/calls.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <x86intrin.h>

#include "build/pack.h"
#include "module/image.h"
#include "rt/host.h"

namespace nxaot::bench {

namespace {

// ---- A few A64 encodings (the Arm ARM's), with labels ----
class Asm {
public:
    size_t Here() const { return w.size() * 4; }
    void Emit(uint32_t x) { w.push_back(x); }
    void Label(const std::string& l) { labels[l] = Here(); }
    void B(const std::string& l) { Fix(l, 0x14000000u, 26); }
    void Bl(const std::string& l) { Fix(l, 0x94000000u, 26); }
    void BNe(const std::string& l) { Fix(l, 0x54000001u, 19); }
    void Adr(unsigned rd, const std::string& l) { fixups.push_back({Here(), l, 0x10000000u | rd, 21}); Emit(0); }
    void Ret() { Emit(0xD65F03C0u); }
    void Blr(unsigned n) { Emit(0xD63F0000u | n << 5); }
    void Br(unsigned n) { Emit(0xD61F0000u | n << 5); }
    void AddI(unsigned d, unsigned n, unsigned imm) { Emit(0x91000000u | imm << 10 | n << 5 | d); }
    void SubsI(unsigned d, unsigned n, unsigned imm) { Emit(0xF1000000u | imm << 10 | n << 5 | d); }
    void AddR(unsigned d, unsigned n, unsigned m, unsigned lsl = 0) { Emit(0x8B000000u | m << 16 | lsl << 10 | n << 5 | d); }
    void EorR(unsigned d, unsigned n, unsigned m) { Emit(0xCA000000u | m << 16 | n << 5 | d); }
    void Movz(unsigned d, unsigned imm) { Emit(0xD2800000u | imm << 5 | d); }
    void FrameIn() { Emit(0xA9BF7BFDu); Emit(0x910003FDu); }   // stp x29, x30, [sp, #-16]!; mov x29, sp
    void FrameOut() { Emit(0xA8C17BFDu); }                        // ldp x29, x30, [sp], #16
    void PushPair(unsigned a, unsigned b) { Emit(0xA9800000u | 0x7Eu << 15 | b << 10 | 31u << 5 | a); }  // stp xa, xb, [sp, #-16]!
    void PopPair(unsigned a, unsigned b) { Emit(0xA8C00000u | 2u << 15 | b << 10 | 31u << 5 | a); }      // ldp xa, xb, [sp], #16
    void And7(unsigned d, unsigned n) { Emit(0x12000800u | n << 5 | d); }        // and wd, wn, #7
    void LdrbR(unsigned t, unsigned n, unsigned m) { Emit(0x38606800u | m << 16 | n << 5 | t); }  // ldrb wt, [xn, xm]
    std::vector<uint32_t> Finish() {
        for (const auto& f : fixups) {
            const int64_t off = static_cast<int64_t>(labels.at(f.label)) - static_cast<int64_t>(f.at);
            uint32_t& word = w[f.at / 4];
            if (f.bits == 21) {
                word = f.base | (static_cast<uint32_t>(off) & 3u) << 29 | (static_cast<uint32_t>(off >> 2) & 0x7FFFFu) << 5;
            } else if (f.bits == 26) {
                word = f.base | (static_cast<uint32_t>(off / 4) & 0x3FFFFFFu);
            } else {
                word = f.base | (static_cast<uint32_t>(off / 4) & 0x7FFFFu) << 5;
            }
        }
        return w;
    }
    std::map<std::string, size_t> labels;

private:
    struct Fixup {
        size_t at;
        std::string label;
        uint32_t base;
        unsigned bits;
    };
    void Fix(const std::string& l, uint32_t base, unsigned bits) {
        fixups.push_back({Here(), l, base, bits});
        Emit(0);
    }
    std::vector<uint32_t> w;
    std::vector<Fixup> fixups;
};

// A module image of the code at offset 0 and read-only data on the next page (no symbols, no unwind entries: discovery finds the functions
// from the entry, the bl targets and the adr targets)
module::Image ImageOf(const std::string& name, const std::vector<uint32_t>& code, const std::vector<uint8_t>& rodata) {
    module::Image m;
    m.path = name;
    m.name = name;
    m.nro = false;
    m.text_off = 0;
    m.text_size = code.size() * 4;
    m.ro_off = (m.text_size + 0xFFF) & ~0xFFFull;
    m.ro_size = rodata.size();
    m.data_off = m.ro_off + ((m.ro_size + 0xFFF) & ~0xFFFull) + 0x1000;
    m.data_size = 0x1000;
    m.bytes.assign(m.data_off + m.data_size, 0);
    std::memcpy(m.bytes.data(), code.data(), m.text_size);
    std::memcpy(m.bytes.data() + m.ro_off, rodata.data(), rodata.size());
    for (size_t i = 0; i < 32; ++i) {
        m.sha256[i] = static_cast<uint8_t>(name.size() * 131 + i);
    }
    return m;
}

struct Program {
    std::string name, what;
    module::Image image;
    unsigned ops = 1;          // the operations one iteration measures (calls and returns, blr, table dispatches)
    std::string baseline;      // the program whose time per iteration is subtracted
};

// The loop every program runs: x3 iterations of a body at offset 0, returning to the host; the body is between the frame and the loop end
void Loop(Asm& a, const std::function<void(Asm&)>& body) {
    a.FrameIn();
    a.Label("loop");
    body(a);
    a.SubsI(3, 3, 1);
    a.BNe("loop");
    a.FrameOut();
    a.Ret();
}

std::vector<Program> Programs() {
    std::vector<Program> ps;
    {
        Asm a;
        Loop(a, [](Asm& b) { b.AddR(0, 0, 0); });  // a doubling: no closed form, so the loop is not folded away
        ps.push_back({"loop", "the loop alone (the baseline): add x0, x0, x0", ImageOf("bench_loop", a.Finish(), {}), 1, ""});
    }
    {
        Asm a;
        Loop(a, [](Asm& b) { b.Bl("leaf"); });
        a.Label("leaf");
        a.AddR(0, 0, 0);
        a.Ret();
        ps.push_back({"leaf", "bl to a leaf (add x0, x0, x0; ret): one call and return", ImageOf("bench_leaf", a.Finish(), {}), 1, "loop"});
    }
    {
        // f1..f7 with frame records, f8 a leaf: eight calls and returns per iteration
        Asm a;
        Loop(a, [](Asm& b) { b.Bl("f1"); });
        for (int i = 1; i <= 7; ++i) {
            a.Label("f" + std::to_string(i));
            a.FrameIn();
            a.Bl("f" + std::to_string(i + 1));
            a.AddI(0, 0, 1);
            a.FrameOut();
            a.Ret();
        }
        a.Label("f8");
        a.AddI(0, 0, 1);
        a.Ret();
        ps.push_back({"chain8", "a chain of 8 calls, frame records in each (stp / ldp x29, x30)", ImageOf("bench_chain8", a.Finish(), {}), 8, "loop"});
    }
    {
        // Four callee-saved registers live across a call to a function that saves and uses two of them
        Asm a;
        a.FrameIn();
        a.PushPair(19, 20);
        a.PushPair(21, 22);
        a.Movz(19, 1);
        a.Movz(20, 2);
        a.Movz(21, 3);
        a.Movz(22, 4);
        a.Label("loop");
        a.Bl("g");
        a.AddR(19, 19, 0);
        a.AddR(20, 20, 19);
        a.AddR(21, 21, 20);
        a.AddR(22, 22, 21);
        a.SubsI(3, 3, 1);
        a.BNe("loop");
        a.AddR(0, 22, 19);
        a.PopPair(21, 22);
        a.PopPair(19, 20);
        a.FrameOut();
        a.Ret();
        a.Label("g");
        a.PushPair(19, 20);
        a.AddI(19, 0, 3);
        a.AddI(20, 19, 5);
        a.AddR(0, 20, 19);
        a.PopPair(19, 20);
        a.Ret();
        ps.push_back({"saved", "x19..x22 live across a call to a function that saves and uses x19, x20", ImageOf("bench_saved", a.Finish(), {}), 1,
                      ""});
    }
    {
        // Tail calls: t1 -> t2 -> t3 -> t4 by b, t4 returns
        Asm a;
        Loop(a, [](Asm& b) { b.Bl("t1"); });
        for (int i = 1; i <= 3; ++i) {
            a.Label("t" + std::to_string(i));
            a.AddI(0, 0, 1);
            a.B("t" + std::to_string(i + 1));
        }
        a.Label("t4");
        a.AddI(0, 0, 1);
        a.Ret();
        ps.push_back({"tail4", "a call into a chain of 3 tail calls (b) and a return", ImageOf("bench_tail4", a.Finish(), {}), 1, "loop"});
    }
    {
        Asm a;
        a.Adr(8, "leaf");
        Loop(a, [](Asm& b) { b.Blr(8); });
        a.Label("leaf");
        a.AddR(0, 0, 0);
        a.Ret();
        ps.push_back({"blr_hit", "blr to the same leaf: the inline cache hits", ImageOf("bench_blr_hit", a.Finish(), {}), 1, "leaf"});
    }
    {
        // Two targets alternating at one site: every call misses the site's cache, which holds one descriptor
        Asm a;
        a.Adr(8, "leaf_a");
        a.Adr(9, "leaf_b");
        a.EorR(10, 8, 9);
        Loop(a, [](Asm& b) {
            b.Blr(8);
            b.EorR(8, 8, 10);
        });
        a.Label("leaf_a");
        a.AddI(0, 0, 1);
        a.Ret();
        a.Label("leaf_b");
        a.AddI(0, 0, 2);
        a.Ret();
        ps.push_back({"blr_miss", "blr alternating between two targets: every call misses and goes through the dispatcher",
                      ImageOf("bench_blr_miss", a.Finish(), {}), 1, "blr_hit"});
    }
    {
        // A switch of 8 cases through a byte table (the shape GCC writes): and w9, w3, #7; ldrb; add x12, base, entry, lsl #2; br x12. The
        // table lives on the read-only page after the code (ImageOf puts it at 0x1000: the code is shorter than a page)
        const size_t ro = 0x1000;
        std::vector<uint8_t> table = {0, 2, 4, 6, 8, 10, 12, 14};
        Asm b;
        b.FrameIn();
        b.Label("loop");
        b.And7(9, 3);
        b.labels["table"] = ro;
        const size_t adr_at = b.Here();
        b.Emit(0x10000000u | 10u | (static_cast<uint32_t>(ro - adr_at) & 3u) << 29 | (static_cast<uint32_t>((ro - adr_at) >> 2) & 0x7FFFFu) << 5);
        b.LdrbR(11, 10, 9);
        b.Adr(12, "cases");
        b.AddR(12, 12, 11, 2);
        b.Br(12);
        b.Label("cases");
        for (unsigned k = 0; k < 8; ++k) {
            b.AddI(0, 0, k + 1);
            b.B("join");
        }
        b.Label("join");
        b.SubsI(3, 3, 1);
        b.BNe("loop");
        b.FrameOut();
        b.Ret();
        ps.push_back({"switch8", "a switch of 8 cases through a byte table (br), lowered as an LLVM switch", ImageOf("bench_switch8", b.Finish(), table), 1,
                      "loop"});
    }
    return ps;
}

double TscPerNs() {
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t c0 = __rdtsc();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(100)) {
    }
    const uint64_t c1 = __rdtsc();
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
    return static_cast<double>(c1 - c0) / ns;
}

struct Built {
    std::string so;
    build::BuildStats stats;
};

bool BuildSo(const module::Image& image, const build::BuildOptions& bo, const std::string& stem, Built* out, std::string* error) {
    const std::string obj = stem + ".o";
    out->so = stem + ".so";
    if (!build::BuildObject(image, bo, obj, &out->stats, error)) {
        return false;
    }
    return build::LinkShared({obj}, out->so, stem.substr(stem.find_last_of('/') + 1) + ".so", error);
}

// Runs the program at offset 0 with x3 = iterations: the best time of repeats, in seconds
double TimeProgram(const std::string& so, const module::Image& image, uint64_t iterations, int repeats, std::string* error) {
    rt::Options ro;
    ro.quiet = true;
    auto host = rt::Host::Create(so, image, ro, error);
    if (!host) {
        return -1;
    }
    host->ResetImage();
    double best = 1e30;
    for (int r = -1; r < repeats; ++r) {
        alignas(64) static dm_guest_state st;
        std::memset(&st, 0, sizeof st);
        st.x[3] = r < 0 ? 1000 : iterations;  // one warm-up run: the inline caches and the pages
        st.x[30] = rt::kHostReturn;
        st.sp = rt::kStackBase + rt::kStackSize;
        const rt::Result res = host->Call(0, &st, false, 0);
        if (res.status != rt::kReturned) {
            *error = res.message;
            return -1;
        }
        if (r >= 0) {
            best = std::min(best, res.seconds);
        }
    }
    return best;
}

std::string Join(const std::string& dir, const std::string& name) {
    return dir + "/" + name;
}

}  // namespace

int RunCalls(const CallsOptions& o) {
    mkdir(o.work.c_str(), 0755);
    std::string error;
    const double tsc = TscPerNs();
    std::printf("\ncalls, dispatch, jump table (nxaot build into a shared object, run by the native run time): %" PRIu64
                " iterations, best of %d; TSC %.3f cycles per ns\n",
                o.iterations, o.repeats, tsc);
    const auto programs = Programs();
    struct Config {
        const char* name;
        lift::CallConv cc;
        build::Tier tier;
        bool no_inline;
    };
    const Config configs[] = {
        {"swifttail W", lift::CallConv::SwiftTail, build::Tier::W, true}, {"ghc W", lift::CallConv::Ghc, build::Tier::W, true},
        {"swifttail H+", lift::CallConv::SwiftTail, build::Tier::HPlus, false}, {"ghc H+", lift::CallConv::Ghc, build::Tier::HPlus, false},
    };
    if (o.calls || o.dispatch) {
        std::printf("  calls are kept out of line (noinline) in the W rows, to measure the convention; the H+ rows are built as the max mode builds\n");
        std::printf("  %-10s %-14s %12s %12s %14s %8s %8s  %s\n", "program", "build", "ns/iter", "net ns/op", "net cycles/op", "spills", "reloads",
                    "what");
        for (const Config& c : configs) {
            std::map<std::string, double> per_iter;
            for (const Program& p : programs) {
                build::BuildOptions bo;
                bo.cc = c.cc;
                bo.tier = c.tier;
                bo.mode = c.tier == build::Tier::HPlus ? "max" : "balanced";
                bo.no_inline = c.no_inline;
                bo.count_spills = true;
                bo.cpu = o.cpu;
                Built b;
                const std::string stem = Join(o.work, p.name + "." + lift::CallConvName(c.cc) + "." + build::TierName(c.tier));
                if (!BuildSo(p.image, bo, stem, &b, &error)) {
                    std::printf("[RED] nxaot_bench: %s: %s\n", p.name.c_str(), error.c_str());
                    return 1;
                }
                const double s = TimeProgram(b.so, p.image, o.iterations, o.repeats, &error);
                if (s < 0) {
                    std::printf("[RED] nxaot_bench: %s: %s\n", p.name.c_str(), error.c_str());
                    return 1;
                }
                const double ns = s * 1e9 / static_cast<double>(o.iterations);
                per_iter[p.name] = ns;
                const double base = p.baseline.empty() ? 0.0 : per_iter[p.baseline];
                const double net = (ns - base) / p.ops;
                std::printf("  %-10s %-14s %12.3f %12.3f %14.2f %8" PRIu64 " %8" PRIu64 "  %s\n", p.name.c_str(), c.name, ns, p.baseline.empty() ? ns : net,
                            (p.baseline.empty() ? ns : net) * tsc, b.stats.spills, b.stats.reloads, p.what.c_str());
            }
        }
        std::printf("  net: minus the baseline program's time per iteration (leaf, chain8, tail4, switch8: the loop; blr_hit: leaf; blr_miss: blr_hit),"
                    " divided by the operations per iteration\n");
    }

    const std::vector<std::string> corpus = {"int_ops-gcc", "int_ops-clang", "switch-gcc", "switch-clang", "fnptr-gcc", "fnptr-clang", "fp-gcc", "fp-clang",
                                             "simd-gcc", "simd-clang", "sjlj-gcc", "sjlj-clang", "excl-gcc", "excl-clang"};
    if (o.spills) {
        std::printf("\nspills and reloads of the register allocator on the call-heavy corpus programs (tier W, the balanced mode):\n");
        std::printf("  %-14s %-10s %8s %8s %10s %12s %8s\n", "nro", "convention", "spills", "reloads", "text", "calls+tails", "run ms");
        for (const char* n : {"fnptr-gcc", "fnptr-clang", "switch-gcc", "switch-clang"}) {
            std::string err;
            const auto image = module::LoadModule(o.corpus + "/nro/" + n + ".nro", &err);
            if (!image) {
                std::printf("[RED] nxaot_bench: %s\n", err.c_str());
                return 1;
            }
            for (const auto cc : {lift::CallConv::SwiftTail, lift::CallConv::Ghc}) {
                build::BuildOptions bo;
                bo.cc = cc;
                bo.count_spills = true;
                bo.cpu = o.cpu;
                Built b;
                if (!BuildSo(*image, bo, Join(o.work, std::string(n) + ".spills." + lift::CallConvName(cc)), &b, &error)) {
                    std::printf("[RED] nxaot_bench: %s: %s\n", n, error.c_str());
                    return 1;
                }
                rt::Options ro;
                ro.quiet = true;
                double best = 1e30;
                {
                    auto host = rt::Host::Create(b.so, *image, ro, &error);
                    for (int r = 0; host && r < o.repeats; ++r) {
                        const rt::Result res = host->RunStart();
                        if (res.status == rt::kExited) {
                            best = std::min(best, res.seconds);
                        }
                    }
                }
                std::printf("  %-14s %-10s %8" PRIu64 " %8" PRIu64 " %10" PRIu64 " %12" PRIu64 " %8.3f\n", n, lift::CallConvName(cc), b.stats.spills,
                            b.stats.reloads, b.stats.text_bytes, b.stats.calls + b.stats.indirect_calls + b.stats.tail_calls + b.stats.indirect_tails,
                            best * 1e3);
            }
        }
    }

    if (o.int_ops) {
        // Integer throughput: int_ops with int_ops_rounds = rounds, natively and under dynarmic (nxaot_run, memory callbacks and fastmem)
        const std::string nro = o.corpus + "/nro/int_ops-gcc.nro";
        std::string err;
        const auto image = module::LoadModule(nro, &err);
        if (!image) {
            std::printf("[RED] nxaot_bench: %s\n", err.c_str());
            return 1;
        }
        std::printf("\ninteger throughput: int_ops-gcc.nro with int_ops_rounds = %" PRIu64 " (best of %d):\n", o.rounds, o.repeats);
        double native[2] = {0, 0};
        int ci = 0;
        for (const auto cc : {lift::CallConv::SwiftTail, lift::CallConv::Ghc}) {
            build::BuildOptions bo;
            bo.cc = cc;
            bo.cpu = o.cpu;
            Built b;
            if (!BuildSo(*image, bo, Join(o.work, std::string("int_ops.") + lift::CallConvName(cc)), &b, &error)) {
                std::printf("[RED] nxaot_bench: int_ops: %s\n", error.c_str());
                return 1;
            }
            rt::Options ro;
            ro.quiet = true;
            ro.sets.emplace_back("int_ops_rounds", std::to_string(o.rounds));
            auto host = rt::Host::Create(b.so, *image, ro, &error);
            double best = 1e30;
            for (int r = 0; host && r < o.repeats; ++r) {
                const rt::Result res = host->RunStart();
                if (res.status != rt::kExited) {
                    std::printf("[RED] nxaot_bench: int_ops: %s\n", res.message.c_str());
                    return 1;
                }
                best = std::min(best, res.seconds);
            }
            native[ci++] = best;
            std::printf("  %-34s %10.4f s\n", (std::string("native, ") + lift::CallConvName(cc) + ", balanced").c_str(), best);
        }
        double dyn[2] = {0, 0};
        uint64_t executed = 0;
        for (int fm = 0; fm < 2 && !o.nxaot_run.empty(); ++fm) {
            double best = 1e30;
            for (int r = 0; r < o.repeats; ++r) {
                const std::string cmd = o.nxaot_run + " " + nro + " --quiet --max-instructions 100000000000 --set int_ops_rounds=" +
                                        std::to_string(o.rounds) + (fm ? " --fastmem" : "") + " 2>&1";
                FILE* p = popen(cmd.c_str(), "r");
                char line[512] = {};
                std::string all;
                while (p && std::fgets(line, sizeof line, p)) {
                    all += line;
                }
                if (p) {
                    pclose(p);
                }
                unsigned long long n = 0;
                double s = 0;
                const auto at = all.find("after ");
                if (at == std::string::npos || std::sscanf(all.c_str() + at, "after %llu instructions in %lf s", &n, &s) != 2) {
                    std::printf("[RED] nxaot_bench: nxaot_run: %s\n", all.c_str());
                    return 1;
                }
                if (!fm) {
                    executed = n;
                }
                best = std::min(best, s);
            }
            dyn[fm] = best;
            std::printf("  %-34s %10.4f s\n", fm ? "dynarmic, fastmem" : "dynarmic, memory callbacks", best);
        }
        if (executed) {
            std::printf("  %" PRIu64 " guest instructions: native %.0f M/s (swifttail), %.0f M/s (ghc); dynarmic %.0f M/s (callbacks), %.0f M/s (fastmem); "
                        "native / dynarmic fastmem: %.2fx\n",
                        executed, executed / native[0] * 1e-6, executed / native[1] * 1e-6, executed / dyn[0] * 1e-6, executed / dyn[1] * 1e-6,
                        dyn[1] / std::min(native[0], native[1]));
        }
    }

    if (o.pack) {
        // Section 10: guest instructions per second per core of each stage, over the whole corpus, per tier
        std::printf("\npack throughput over the corpus (%zu NROs, one thread, swifttail; guest instructions per second):\n", corpus.size());
        std::printf("  %-4s %10s %12s %12s %14s %12s %10s %10s\n", "tier", "words", "analysis", "lifting", "opt+codegen", "link s/NRO", "text KiB",
                    "peak MiB");
        for (const auto tier : {build::Tier::C, build::Tier::W, build::Tier::H, build::Tier::HPlus}) {
            uint64_t words = 0, text = 0, peak = 0;
            double analyse = 0, lift_s = 0, compile = 0, link = 0;
            for (const auto& n : corpus) {
                std::string err;
                const auto image = module::LoadModule(o.corpus + "/nro/" + n + ".nro", &err);
                if (!image) {
                    std::printf("[RED] nxaot_bench: %s\n", err.c_str());
                    return 1;
                }
                build::BuildOptions bo;
                bo.tier = tier;
                bo.cpu = o.cpu;
                const std::string stem = Join(o.work, n + ".pack." + build::TierName(tier));
                build::BuildStats s;
                if (!build::BuildObject(*image, bo, stem + ".o", &s, &error)) {
                    std::printf("[RED] nxaot_bench: %s: %s\n", n.c_str(), error.c_str());
                    return 1;
                }
                const auto t0 = std::chrono::steady_clock::now();
                if (!build::LinkShared({stem + ".o"}, stem + ".so", n + ".so", &error)) {
                    std::printf("[RED] nxaot_bench: %s: %s\n", n.c_str(), error.c_str());
                    return 1;
                }
                link += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                words += s.words;
                text += s.text_bytes;
                analyse += s.t_discover + s.t_plan;
                lift_s += s.t_lift;
                compile += s.t_opt + s.t_codegen;
                peak = std::max(peak, s.peak_rss_kib);
            }
            std::printf("  %-4s %10" PRIu64 " %12.0f %12.0f %14.0f %12.4f %10" PRIu64 " %10" PRIu64 "\n", build::TierName(tier), words, words / analyse,
                        words / lift_s, words / compile, link / static_cast<double>(corpus.size()), text / 1024, peak / 1024);
        }
    }
    return 0;
}

}  // namespace nxaot::bench
