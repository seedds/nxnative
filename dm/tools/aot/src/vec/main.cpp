// nxaot_vec: the differential vector harness (docs/design/aot-recompiler.md section 12). Every vector runs on dynarmic's A64 JIT (the reference) and
// as lifted code compiled by LLVM ORC from the same initial state; the final x0..x30, sp, nzcv, fpcr, fpsr, tpidr_el0, v0..v31, the exit (pc,
// reason, argument) and the 64 KiB memory image must be equal. A vector the lifter refuses (an IR opcode it does not lower) is counted as
// unsupported, not as a failure. FPSR differences that a known dynarmic issue explains (kKnownIssues, README.md "Known issues") are counted per
// issue instead of as mismatches, and only when the FPSR is all that differs. README.md has the usage.
//
//   nxaot_vec [--cpu <llvm cpu>] [--filter <text>] [--dump] [--batch <n>] [--max-report <n>] [--nan-mode chains|perop|unchecked]
//             [--no-fpsr-model] [--ignore-fpsr-flags] [--no-filters] [--state] [-v] <vectors.tsv, folder or - for standard input>...
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "decode/translate.h"
#include "lift/lifter.h"
#include "lift/opcodes.h"
#include "nxaot/guest_state.h"
#include "nxaot_lock.h"
#include "vec/lifted.h"
#include "vec/reference.h"
#include "vec/vectors.h"

using namespace nxaot;
using namespace nxaot::vec;
using Dynarmic::IR::Opcode;

namespace {

std::string Hex(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%#" PRIx64, v);
    return buf;
}

std::string Hex128(const V128& q) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%016" PRIx64 "%016" PRIx64, q[1], q[0]);
    return buf;
}

const char* ReasonName(uint32_t r) {
    switch (r) {
    case DM_EXIT_NONE:
        return "none";
    case DM_EXIT_SVC:
        return "svc";
    case DM_EXIT_BRK:
        return "brk";
    case DM_EXIT_UNDEFINED:
        return "undefined";
    case DM_EXIT_UNPREDICTABLE:
        return "unpredictable";
    case DM_EXIT_NO_CODE:
        return "no-code";
    case DM_EXIT_UNTRANSLATED:
        return "untranslated";
    default:
        return "?";
    }
}

// Where dynarmic's own FPSR differs from the Arm ARM's and the lifted code follows the Arm ARM (README.md, "Known issues"). The lifter notes the
// issues a region meets (lift::DynarmicIssue, in the forms dynarmic takes them); the final FPSR of such a vector may then have these bits in the
// reference only, or in the lifted run only. Values, QC and every other register are still compared exactly
struct KnownIssue {
    int number;  // README.md, "Known issues"
    const char* what;
    uint32_t reference_only;  // bits the reference may have and the lifted run not
    uint32_t lifted_only;     // bits the lifted run may have and the reference not
};

constexpr uint32_t kIOC = 0x01, kDZC = 0x02, kOFC = 0x04, kUFC = 0x08, kIXC = 0x10, kIDC = 0x80;

const std::vector<KnownIssue>& KnownIssues() {
    static const std::vector<KnownIssue> issues = {
        {lift::kIssueRoundIntInexact, "frintn / frintp / frintm / frintz / frinti: dynarmic's roundss raises Inexact; Arm's FPRoundInt with exact FALSE does not",
         kIXC, 0},
        {lift::kIssueToFixedNative,
         "fcvt[npmz][su], fcvtzs / fcvtzu fixed point: dynarmic's native conversion clamps before converting and so drops Invalid Operation "
         "(saturation, negative to unsigned, quiet NaN) or raises Inexact where Arm raises Invalid Operation; its scaling by 2^fbits in the "
         "source precision can overflow (Overflow, Inexact) where Arm scales exactly",
         kIOC | kIXC | kOFC, kIOC | kIXC},
        {lift::kIssueToFixedSoftware, "fcvta[su] and half-precision conversions to unsigned: dynarmic's FPToFixed raises Invalid Operation for a "
                                      "negative value that rounds to 0; Arm's raises Inexact", kIOC, kIXC},
        {lift::kIssueMulAddNaNProduct, "fmadd / fmsub / fnmadd / fnmsub, DN = 0: dynarmic's NaN path multiplies the two factors to test for "
                                       "infinity times zero, which raises Inexact / Overflow / Underflow", kIXC | kOFC | kUFC, 0},
        {lift::kIssueNoInputDenormal, "FZ = 1: dynarmic's native operations flush denormal inputs through DAZ, which raises nothing; Arm raises Input Denormal",
         0, kIDC},
        {lift::kIssueVectorMinMaxQNaN, "fmax / fmin / fmaxnm / fminnm (vector): dynarmic's maxps / minps raise Invalid Operation for quiet NaNs; Arm only "
                                       "for signalling ones", kIOC, 0},
        {lift::kIssueRSqrtEstimateNative, "frsqrte (single, double): dynarmic's native estimate (sqrtss / divss on the truncated operand) raises Inexact; "
                                          "its rsqrtss path for negative, zero and NaN operands raises nothing where Arm raises Invalid "
                                          "Operation or Divide by Zero", kIXC | kIOC, kIOC | kDZC | kIDC},
        {lift::kIssueVectorUpperLanes, "fdiv / frecpe / frsqrte of 64-bit vectors (2S): dynarmic computes the zero upper lanes as well, 0 / 0 raising "
                                       "Invalid Operation and the estimates of 0 Divide by Zero; Arm computes two lanes", kIOC | kDZC, 0},
        {lift::kIssueMulAddInfZero, "fmla / fmls (vector, by element), and fmadd / fmsub / fnmadd / fnmsub with DN = 1: a quiet NaN addend with "
                                    "infinity times zero gives the default NaN with Invalid Operation in Arm's FPMulAdd; dynarmic's vector NaN "
                                    "correction returns the default NaN without it, and its DN paths are vfmadd alone, which raises nothing",
         0, kIOC},
    };
    return issues;
}

// Leaving the vector's code is how a block ends in the harness: dynarmic reports it as NoExecuteFault at the next address, the lifter either the same
// way (straight-line code reads past its end) or as a normal exit at the branch target. Both mean "continue at pc"
uint32_t Normalised(const Vector& v, const Outcome& o) {
    if (o.reason == DM_EXIT_NO_CODE && !InCode(v, o.s.pc)) {
        return DM_EXIT_NONE;
    }
    return o.reason;
}

struct Diff {
    std::vector<std::string> lines;  // everything that differs but the FPSR
    uint32_t fpsr_xor = 0;           // the FPSR bits that differ (within the compared mask)
};

Diff Compare(const Vector& v, const Outcome& ref, const Outcome& got, uint32_t fpsr_mask) {
    Diff diff;
    auto& d = diff.lines;
    if (!ref.failure.empty()) {
        d.push_back("reference failed: " + ref.failure);
    }
    if (!got.failure.empty()) {
        d.push_back("lifted failed: " + got.failure);
    }
    const uint32_t rr = Normalised(v, ref), gr = Normalised(v, got);
    if (rr != gr) {
        d.push_back(std::string("exit: reference ") + ReasonName(rr) + ", lifted " + ReasonName(gr));
    }
    if (ref.s.pc != got.s.pc) {
        d.push_back("pc: reference " + Hex(ref.s.pc) + ", lifted " + Hex(got.s.pc));
    }
    if (rr == DM_EXIT_SVC && ref.arg != got.arg) {
        d.push_back("svc number: reference " + Hex(ref.arg) + ", lifted " + Hex(got.arg));
    }
    for (size_t i = 0; i < 31; ++i) {
        if (ref.s.x[i] != got.s.x[i]) {
            d.push_back("x" + std::to_string(i) + ": reference " + Hex(ref.s.x[i]) + ", lifted " + Hex(got.s.x[i]));
        }
    }
    for (size_t i = 0; i < 32; ++i) {
        if (ref.s.v[i] != got.s.v[i]) {
            d.push_back("v" + std::to_string(i) + ": reference " + Hex128(ref.s.v[i]) + ", lifted " + Hex128(got.s.v[i]));
        }
    }
    auto field = [&](const char* name, uint64_t a, uint64_t b) {
        if (a != b) {
            d.push_back(std::string(name) + ": reference " + Hex(a) + ", lifted " + Hex(b));
        }
    };
    field("sp", ref.s.sp, got.s.sp);
    field("nzcv", ref.s.nzcv, got.s.nzcv);
    field("fpcr", ref.s.fpcr, got.s.fpcr);
    field("tpidr_el0", ref.s.tpidr, got.s.tpidr);
    diff.fpsr_xor = (ref.s.fpsr ^ got.s.fpsr) & fpsr_mask;
    if (ref.image.size() == got.image.size() && std::memcmp(ref.image.data(), got.image.data(), ref.image.size()) == 0) {
        // equal: the usual case, without the byte loop below
    } else if (ref.image.size() == got.image.size()) {
        size_t shown = 0;
        for (size_t i = 0; i < ref.image.size() && shown < 4;) {
            if (ref.image[i] == got.image[i]) {
                ++i;
                continue;
            }
            size_t j = i;
            while (j < ref.image.size() && ref.image[j] != got.image[j] && j - i < 16) {
                ++j;
            }
            std::string a, b;
            char buf[4];
            for (size_t k = i; k < j; ++k) {
                std::snprintf(buf, sizeof buf, "%02x", ref.image[k]);
                a += buf;
                std::snprintf(buf, sizeof buf, "%02x", got.image[k]);
                b += buf;
            }
            d.push_back("memory " + Hex(kGuestBase + i) + ": reference " + a + ", lifted " + b);
            ++shown;
            i = j;
        }
    } else {
        d.push_back("memory image sizes differ");
    }
    return diff;
}

void Usage() {
    std::fprintf(stderr,
                 "usage: nxaot_vec [--cpu <llvm cpu>] [--filter <text>] [--dump] [--batch <n>] [--max-report <n>] [--nan-mode chains|perop|unchecked]\n"
                 "                 [--no-fpsr-model] [--ignore-fpsr-flags] [--no-filters] [--state] [-v] <vectors.tsv, folder or - for standard input>...\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // the log shows progress when stdout is a file
    std::string cpu, filter;
    bool dump = false, verbose = false, ignore_flags = false, no_filters = false, fpsr_model = true, show_state = false;
    lift::NaNMode nan_mode = lift::NaNMode::Chains;
    size_t batch_size = 256, max_report = 50;
    std::vector<std::string> paths;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                Usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--cpu") {
            cpu = next();
        } else if (a == "--filter") {
            filter = next();
        } else if (a == "--dump") {
            dump = true;
        } else if (a == "--batch") {
            batch_size = std::max<size_t>(1, std::strtoull(next().c_str(), nullptr, 10));
        } else if (a == "--max-report") {
            max_report = std::strtoull(next().c_str(), nullptr, 10);
        } else if (a == "--nan-mode") {
            const std::string m = next();
            if (m == "chains") {
                nan_mode = lift::NaNMode::Chains;
            } else if (m == "perop") {
                nan_mode = lift::NaNMode::PerOp;
            } else if (m == "unchecked") {
                nan_mode = lift::NaNMode::Unchecked;
            } else {
                Usage();
                return 2;
            }
        } else if (a == "--no-fpsr-model") {
            fpsr_model = false;
            ignore_flags = true;
        } else if (a == "--ignore-fpsr-flags") {
            ignore_flags = true;
        } else if (a == "--state") {
            show_state = true;
        } else if (a == "--no-filters") {
            no_filters = true;
        } else if (a == "-v") {
            verbose = true;
        } else if (a == "-h" || a == "--help") {
            Usage();
            return 0;
        } else if (a == "-") {
            paths.push_back(a);
        } else if (!a.empty() && a[0] == '-') {
            Usage();
            return 2;
        } else {
            paths.push_back(a);
        }
    }
    if (paths.empty()) {
        Usage();
        return 2;
    }

    std::string error;
    auto lifted = Lifted::Create(cpu, nan_mode, fpsr_model, &error);
    if (!lifted) {
        std::fprintf(stderr, "nxaot_vec: %s\n", error.c_str());
        return 2;
    }
    Reference reference;
    const lift::Target t = lifted->Target();
    std::printf("nxaot_vec: LLVM %s, dynarmic %.12s, target CPU %s\n", NXAOT_LOCK_LLVM, NXAOT_LOCK_DYNARMIC, lifted->Cpu().c_str());
    std::printf("nxaot_vec: lowering uses fma %d f16c %d avx2 %d aes %d sha %d pclmul %d; NaN mode %s; FPSR flags %s, compared %s\n", t.fma, t.f16c, t.avx2,
                t.aes, t.sha, t.pclmul, nan_mode == lift::NaNMode::Chains ? "chains" : nan_mode == lift::NaNMode::PerOp ? "per operation" : "unchecked",
                fpsr_model ? "modelled" : "not modelled", ignore_flags ? "without the cumulative exception bits" : no_filters ? "whole, no filters" : "whole");
    std::printf("nxaot_vec: vectors from");
    for (const auto& p : paths) {
        std::printf(" %s", p.c_str());
    }
    std::printf("\n");
    const uint32_t fpsr_mask = ignore_flags ? 0x08000000u : 0xFFFFFFFFu;

    size_t total = 0, mismatches = 0, unsupported = 0, passed = 0, reported = 0, filtered = 0;
    std::map<std::string, size_t> per_class, per_class_fail, per_class_filtered;
    std::map<uint32_t, size_t> exits;                // reference exit reasons, normalised
    std::map<std::string, size_t> per_class_normal;  // vectors whose reference run ended normally (not an exception or an escape)
    std::map<std::string, size_t> unsupported_class;
    std::map<std::string, std::map<std::string, size_t>> unsupported_ops;  // class -> opcode -> vectors
    std::map<Opcode, size_t> exercised;                                     // lowered opcode -> passing vectors whose code holds it
    std::map<int, size_t> per_issue;
    VectorReader reader{paths};
    std::vector<Vector> batch_storage;
    bool more = true;
    while (more) {
        batch_storage.clear();
        while (batch_storage.size() < batch_size) {
            Vector v;
            if (!reader.Next(v, &error)) {
                if (!error.empty()) {
                    std::fprintf(stderr, "nxaot_vec: %s\n", error.c_str());
                    return 2;
                }
                more = false;
                break;
            }
            if (filter.empty() || v.id.find(filter) != std::string::npos || v.cls.find(filter) != std::string::npos) {
                batch_storage.push_back(std::move(v));
            }
        }
        if (batch_storage.empty()) {
            break;
        }
        std::vector<const Vector*> batch;
        for (const auto& v : batch_storage) {
            batch.push_back(&v);
        }
        total += batch.size();
        std::string ir;
        const auto prepared = lifted->Prepare(batch, dump, &ir);
        if (dump) {
            for (const Vector* v : batch) {
                lift::Region region{kCodeBase, kCodeBase, kCodeBase + 4 * v->code.size()};
                const decode::CodeReader read = [v](uint64_t a) -> std::optional<uint32_t> {
                    if (!InCode(*v, a)) {
                        return std::nullopt;
                    }
                    return v->code[(a - kCodeBase) / 4];
                };
                std::printf("---- %s: dynarmic IR\n%s", v->id.c_str(), lift::DumpRegion(region, read, v->fpcr).c_str());
            }
            std::printf("---- optimised LLVM IR of the batch\n%s\n", ir.c_str());
        }
        for (size_t i = 0; i < batch.size(); ++i) {
            const Vector& v = *batch[i];
            const Prepared& p = prepared[i];
            ++per_class[v.cls];
            if (!p.unsupported.empty()) {
                ++unsupported;
                ++unsupported_class[v.cls];
                for (const auto& op : p.unsupported) {
                    ++unsupported_ops[v.cls][op];
                }
                if (verbose) {
                    std::printf("UNSUPPORTED %s (%s): unsupported:", v.id.c_str(), v.cls.c_str());
                    for (const auto& op : p.unsupported) {
                        std::printf(" %s", op.c_str());
                    }
                    std::printf("\n");
                }
                continue;
            }
            State init;
            std::vector<uint8_t> image;
            SeedState(v, init, image);
            Diff diff;
            if (!p.error.empty() || !p.fn) {
                diff.lines.push_back("lifter: " + (p.error.empty() ? std::string("no function") : p.error));
            } else {
                const Outcome ref = reference.Run(v, init, image);
                const Outcome got = lifted->Run(v, p, init, image);
                diff = Compare(v, ref, got, fpsr_mask);
                ++exits[Normalised(v, ref)];
                if (Normalised(v, ref) == DM_EXIT_NONE) {
                    ++per_class_normal[v.cls];
                }
                if (diff.fpsr_xor) {
                    std::string line = "fpsr: reference " + Hex(ref.s.fpsr) + ", lifted " + Hex(got.s.fpsr);
                    // Known dynarmic FPSR issues the region meets explain it when only the FPSR differs, in their bits and directions
                    int issue = 0;
                    if (!no_filters && diff.lines.empty()) {
                        const uint32_t ref_extra = ref.s.fpsr & ~got.s.fpsr & fpsr_mask;
                        const uint32_t got_extra = got.s.fpsr & ~ref.s.fpsr & fpsr_mask;
                        uint32_t ref_allowed = 0, got_allowed = 0;
                        for (const auto& k : KnownIssues()) {
                            const bool met = std::find(p.dynarmic_issues.begin(), p.dynarmic_issues.end(), k.number) != p.dynarmic_issues.end() ||
                                             std::find(got.issues.begin(), got.issues.end(), k.number) != got.issues.end();
                            if (!met) {
                                continue;
                            }
                            ref_allowed |= k.reference_only;
                            got_allowed |= k.lifted_only;
                            if (!issue && ((ref_extra & k.reference_only) || (got_extra & k.lifted_only))) {
                                issue = k.number;
                            }
                        }
                        if ((ref_extra & ~ref_allowed) || (got_extra & ~got_allowed)) {
                            issue = 0;
                        }
                    }
                    if (issue) {
                        ++filtered;
                        ++per_issue[issue];
                        ++per_class_filtered[v.cls];
                        if (verbose) {
                            std::printf("FILTERED %s (%s): known issue %d: %s\n", v.id.c_str(), v.cls.c_str(), issue, line.c_str());
                        }
                        diff.fpsr_xor = 0;
                    } else {
                        diff.lines.push_back(line);
                    }
                }
            }
            if (diff.lines.empty()) {
                ++passed;
                for (const auto op : p.opcodes) {
                    ++exercised[op];
                }
                if (verbose) {
                    std::printf("PASS %s (%s)\n", v.id.c_str(), v.cls.c_str());
                }
                continue;
            }
            ++mismatches;
            ++per_class_fail[v.cls];
            if (reported++ < max_report) {
                std::printf("MISMATCH %s (%s) %s: %s\n  code:", v.id.c_str(), v.cls.c_str(), v.where.c_str(), v.text.c_str());
                for (uint32_t w : v.code) {
                    std::printf(" %08x", w);
                }
                std::printf("  fpcr: %#x\n", v.fpcr);
                for (const auto& d : diff.lines) {
                    std::printf("  %s\n", d.c_str());
                }
                if (show_state) {
                    std::printf("  initial: fpcr %#x fpsr %#x nzcv %#x\n", init.fpcr, init.fpsr, init.nzcv);
                    for (size_t k = 0; k < 32; ++k) {
                        std::printf("  initial v%zu %s%s", k, Hex128(init.v[k]).c_str(), k % 4 == 3 ? "\n" : "");
                    }
                    for (size_t k = 0; k < 31; ++k) {
                        std::printf("  initial x%zu %s%s", k, Hex(init.x[k]).c_str(), k % 4 == 3 || k == 30 ? "\n" : "");
                    }
                }
            }
        }
    }
    if (reported > max_report) {
        std::printf("(%zu more mismatches not shown: --max-report)\n", reported - max_report);
    }

    std::printf("\nexits of the reference runs (normalised as compared):");
    for (const auto& [reason, n] : exits) {
        std::printf(" %s %zu", ReasonName(reason), n);
    }
    std::printf("\n");
    std::printf("\nper class: vectors, mismatches, unsupported, FPSR filtered, ended normally\n");
    for (const auto& [cls, n] : per_class) {
        auto get = [&](const std::map<std::string, size_t>& m) {
            const auto it = m.find(cls);
            return it == m.end() ? size_t{0} : it->second;
        };
        std::printf("  %-24s %8zu %6zu %6zu %6zu %8zu\n", cls.c_str(), n, get(per_class_fail), get(unsupported_class), get(per_class_filtered),
                    get(per_class_normal));
    }
    if (unsupported) {
        std::printf("\nunsupported vectors, by class, with the IR opcodes the lifter refused (vectors per opcode):\n");
        for (const auto& [cls, n] : unsupported_class) {
            std::printf("  %s: %zu:", cls.c_str(), n);
            for (const auto& [op, k] : unsupported_ops[cls]) {
                std::printf(" %s(%zu)", op.c_str(), k);
            }
            std::printf("\n");
        }
    }
    if (filtered) {
        std::printf("\nFPSR differences explained by known dynarmic issues (README.md, \"Known issues\"):\n");
        for (const auto& [issue, n] : per_issue) {
            const auto& list = KnownIssues();
            const auto it = std::find_if(list.begin(), list.end(), [&](const KnownIssue& k) { return k.number == issue; });
            std::printf("  issue %d: %zu vectors: %s\n", issue, n, it == list.end() ? "?" : it->what);
        }
    }
    // Which lowered opcodes the passing vectors exercised (a lowering no vector reaches is checked by nothing)
    size_t lowered = 0, hit = 0;
    std::string unexercised;
    for (size_t i = 0; i < Dynarmic::IR::OpcodeCount; ++i) {
        const auto op = static_cast<Opcode>(i);
        if (!lift::IsLowered(op) || op == Opcode::Void) {
            continue;
        }
        ++lowered;
        if (exercised.count(op)) {
            ++hit;
        } else {
            unexercised += std::string(" ") + lift::OpcodeName(op);
        }
    }
    std::printf("\nlowered opcodes exercised by passing vectors: %zu of %zu; not exercised:%s\n", hit, lowered, unexercised.empty() ? " none" : unexercised.c_str());
    std::printf("continuations lifted (a pc inside the code after a normal exit: msr fpcr, computed branches): %zu\n", lifted->Continuations());
    std::printf("\n%zu passed: the reference and the lifted code agree on registers, vector registers, flags, FPSR, exit and memory\n", passed);
    std::printf("== nxaot_vec: %zu vectors, %zu mismatches, %zu unsupported (listed), %zu FPSR differences of known dynarmic issues ==\n", total, mismatches,
                unsupported, filtered);
    return mismatches ? 1 : 0;
}
