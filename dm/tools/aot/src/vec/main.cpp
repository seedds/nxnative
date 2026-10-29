// nxaot_vec: the differential vector harness (docs/design/aot-recompiler.md section 12). Every vector runs on dynarmic's A64 JIT (the reference) and
// as lifted code compiled by LLVM ORC from the same initial state; the final x0..x30, sp, nzcv, fpcr, tpidr_el0, the exit (pc, reason, argument)
// and the 64 KiB memory image must be equal. A vector the lifter refuses (a floating-point or SIMD IR opcode) is counted as
// unsupported, not as a failure. README.md has the usage.
//
//   nxaot_vec [--cpu <llvm cpu>] [--filter <text>] [--dump] [--batch <n>] [--max-report <n>] [-v] <vectors.tsv or folder>...
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

namespace {

std::string Hex(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%#" PRIx64, v);
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

// Leaving the vector's code is how a block ends in the harness: dynarmic reports it as NoExecuteFault at the next address, the lifter either the same
// way (straight-line code reads past its end) or as a normal exit at the branch target. Both mean "continue at pc"
uint32_t Normalised(const Vector& v, const Outcome& o) {
    if (o.reason == DM_EXIT_NO_CODE && !InCode(v, o.s.pc)) {
        return DM_EXIT_NONE;
    }
    return o.reason;
}

std::vector<std::string> Compare(const Vector& v, const Outcome& ref, const Outcome& got) {
    std::vector<std::string> d;
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
    auto field = [&](const char* name, uint64_t a, uint64_t b) {
        if (a != b) {
            d.push_back(std::string(name) + ": reference " + Hex(a) + ", lifted " + Hex(b));
        }
    };
    field("sp", ref.s.sp, got.s.sp);
    field("nzcv", ref.s.nzcv, got.s.nzcv);
    field("fpcr", ref.s.fpcr, got.s.fpcr);
    // The final FPSR is not compared: dynarmic compiles a block the dispatcher misses with the guest's MXCSR loaded, so the host's own floating
    // point (its hash maps' load factors) can leave IXC in the guest's cumulative flags (README.md, "Known issues"). An mrs fpsr result is compared,
    // in its register
    field("tpidr_el0", ref.s.tpidr, got.s.tpidr);
    if (ref.image.size() == got.image.size()) {
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
    return d;
}

void Usage() {
    std::fprintf(stderr, "usage: nxaot_vec [--cpu <llvm cpu>] [--filter <text>] [--dump] [--batch <n>] [--max-report <n>] [-v] <vectors.tsv or folder>...\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // the log shows progress when stdout is a file
    std::string cpu, filter;
    bool dump = false, verbose = false;
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
        } else if (a == "-v") {
            verbose = true;
        } else if (a == "-h" || a == "--help") {
            Usage();
            return 0;
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

    std::vector<Vector> all;
    for (const auto& p : paths) {
        std::string error;
        if (!LoadVectors(p, all, &error)) {
            std::fprintf(stderr, "nxaot_vec: %s\n", error.c_str());
            return 2;
        }
    }
    std::vector<const Vector*> vectors;
    for (const auto& v : all) {
        if (filter.empty() || v.id.find(filter) != std::string::npos || v.cls.find(filter) != std::string::npos) {
            vectors.push_back(&v);
        }
    }

    std::string error;
    auto lifted = Lifted::Create(cpu, &error);
    if (!lifted) {
        std::fprintf(stderr, "nxaot_vec: %s\n", error.c_str());
        return 2;
    }
    Reference reference;
    std::printf("nxaot_vec: LLVM %s, dynarmic %.12s, target CPU %s\n", NXAOT_LOCK_LLVM, NXAOT_LOCK_DYNARMIC, lifted->Cpu().c_str());
    std::printf("nxaot_vec: %zu vectors from", vectors.size());
    for (const auto& p : paths) {
        std::printf(" %s", p.c_str());
    }
    std::printf("\n");

    size_t mismatches = 0, unsupported = 0, passed = 0, reported = 0;
    std::map<std::string, size_t> per_class, per_class_fail;
    std::map<uint32_t, size_t> exits;            // reference exit reasons, normalised
    std::map<std::string, size_t> per_class_normal;  // vectors whose reference run ended normally (not an exception or an escape)
    std::map<std::string, size_t> unsupported_class;
    std::map<std::string, std::map<std::string, size_t>> unsupported_ops;  // class -> opcode -> vectors
    std::map<Dynarmic::IR::Opcode, size_t> exercised;                      // lowered opcode -> passing vectors whose code holds it
    for (size_t start = 0; start < vectors.size(); start += batch_size) {
        const std::vector<const Vector*> batch(vectors.begin() + start, vectors.begin() + std::min(vectors.size(), start + batch_size));
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
                std::printf("---- %s: dynarmic IR\n%s", v->id.c_str(), lift::DumpRegion(region, read).c_str());
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
            std::vector<std::string> diffs;
            if (!p.error.empty() || !p.fn) {
                diffs.push_back("lifter: " + (p.error.empty() ? std::string("no function") : p.error));
            } else {
                const Outcome ref = reference.Run(v, init, image);
                const Outcome got = lifted->Run(p, init, image);
                diffs = Compare(v, ref, got);
                ++exits[Normalised(v, ref)];
                if (Normalised(v, ref) == DM_EXIT_NONE) {
                    ++per_class_normal[v.cls];
                }
            }
            if (diffs.empty()) {
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
                std::printf("\n");
                for (const auto& d : diffs) {
                    std::printf("  %s\n", d.c_str());
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
    std::printf("\nper class: vectors, mismatches, unsupported, ended normally\n");
    for (const auto& [cls, n] : per_class) {
        const auto f = per_class_fail.find(cls);
        const auto u = unsupported_class.find(cls);
        const auto ok = per_class_normal.find(cls);
        std::printf("  %-24s %6zu %6zu %6zu %6zu\n", cls.c_str(), n, f == per_class_fail.end() ? 0 : f->second, u == unsupported_class.end() ? 0 : u->second,
                    ok == per_class_normal.end() ? 0 : ok->second);
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
    // Which lowered opcodes the passing vectors exercised (a lowering no vector reaches is checked by nothing)
    size_t lowered = 0, hit = 0;
    std::string unexercised;
    for (size_t i = 0; i < Dynarmic::IR::OpcodeCount; ++i) {
        const auto op = static_cast<Dynarmic::IR::Opcode>(i);
        if (!lift::IsLowered(op) || op == Dynarmic::IR::Opcode::Void) {
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
    std::printf("\n%zu passed: the reference and the lifted code agree on registers, flags, exit and memory\n", passed);
    std::printf("== nxaot_vec: %zu vectors, %zu mismatches, %zu unsupported (listed) ==\n", vectors.size(), mismatches, unsupported);
    return mismatches ? 1 : 0;
}
