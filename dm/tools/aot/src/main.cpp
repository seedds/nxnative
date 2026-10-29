// nxaot: the ahead-of-time ARM64 -> x86-64 recompiler (docs/design/aot-recompiler.md). Subcommands today:
//   nxaot census <raw ARM64 code file> --base <hex>   phase 0's census of a code blob, as key<TAB>count lines (src/analysis/census.h)
//   nxaot opcodes                                     every dynarmic IR opcode with whether the lifter lowers it (group<TAB>opcode)
//   nxaot opcodes --reach [--samples N] [--check]     the opcodes the A64 frontend emits (src/analysis/reach.h), with the decoder entries that emit
//                                                     them; --check exits 1 when one of them is not lowered
//   nxaot version                                     the toolchain of nxaot.lock this build was configured with
// The lifter (src/lift) is exercised by nxaot_vec until the analysis and the pack stages exist (README.md, "What is not done yet").
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "analysis/census.h"
#include "analysis/reach.h"
#include "dynarmic/ir/opcodes.h"
#include "lift/opcodes.h"
#include "nxaot_lock.h"

namespace {

int Usage() {
    std::fprintf(stderr,
                 "usage: nxaot census <raw ARM64 code file> --base <hex>\n"
                 "       nxaot opcodes [--reach [--samples N] [--check]]\n"
                 "       nxaot version\n");
    return 2;
}

int Census(int argc, char** argv) {
    std::string path;
    uint64_t base = 0;
    bool have_base = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--base" && i + 1 < argc) {
            char* end = nullptr;
            base = std::strtoull(argv[++i], &end, 16);
            if (!end || *end) {
                std::fprintf(stderr, "nxaot census: bad --base %s\n", argv[i]);
                return 2;
            }
            have_base = true;
        } else if (path.empty() && !a.empty() && a[0] != '-') {
            path = a;
        } else {
            return Usage();
        }
    }
    if (path.empty() || !have_base) {
        return Usage();
    }
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        std::fprintf(stderr, "nxaot census: cannot read %s\n", path.c_str());
        return 1;
    }
    const std::vector<uint8_t> data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const auto counts = nxaot::analysis::Census(data.data(), data.size(), base);
    for (const auto& [key, n] : counts) {
        std::printf("%s\t%llu\n", key.c_str(), static_cast<unsigned long long>(n));
    }
    return 0;
}

int Opcodes(int argc, char** argv) {
    using Dynarmic::IR::Opcode;
    bool reach = false, check = false;
    size_t samples = 256;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--reach") {
            reach = true;
        } else if (a == "--check") {
            check = true;
        } else if (a == "--samples" && i + 1 < argc) {
            samples = std::strtoull(argv[++i], nullptr, 10);
        } else {
            return Usage();
        }
    }
    size_t per_group[4] = {};
    if (!reach) {
        for (size_t i = 0; i < Dynarmic::IR::OpcodeCount; ++i) {
            const auto op = static_cast<Opcode>(i);
            const auto group = nxaot::lift::GroupOf(op);
            ++per_group[static_cast<size_t>(group)];
            std::printf("%s\t%s\n", nxaot::lift::GroupName(group), nxaot::lift::OpcodeName(op));
        }
        std::printf("# %zu opcodes: %zu lowered, %zu fp_simd, %zu a32, %zu other\n", Dynarmic::IR::OpcodeCount, per_group[0], per_group[1], per_group[2],
                    per_group[3]);
        return 0;
    }
    // Every opcode with the A64 decoder entries that emitted it (at most four named), group<TAB>opcode<TAB>blocks<TAB>entries
    const auto r = nxaot::analysis::Reach(samples, 0x6E78616F74726561ull);
    size_t reached[4] = {}, not_lowered = 0;
    std::string missing;
    for (size_t i = 0; i < Dynarmic::IR::OpcodeCount; ++i) {
        const auto op = static_cast<Opcode>(i);
        const auto group = nxaot::lift::GroupOf(op);
        ++per_group[static_cast<size_t>(group)];
        const auto it = r.opcodes.find(op);
        if (it == r.opcodes.end()) {
            std::printf("%s\t%s\t0\t-\n", nxaot::lift::GroupName(group), nxaot::lift::OpcodeName(op));
            continue;
        }
        ++reached[static_cast<size_t>(group)];
        std::string names;
        for (size_t k = 0; k < it->second.matchers.size() && k < 4; ++k) {
            names += (k ? "," : "") + it->second.matchers[k];
        }
        if (it->second.matchers.size() > 4) {
            names += ",+" + std::to_string(it->second.matchers.size() - 4);
        }
        std::printf("%s\t%s\t%zu\t%s\n", nxaot::lift::GroupName(group), nxaot::lift::OpcodeName(op), it->second.blocks, names.c_str());
        if (group != nxaot::lift::OpcodeGroup::Lowered) {
            ++not_lowered;
            missing += std::string(" ") + nxaot::lift::OpcodeName(op);
        }
    }
    std::printf("# reach: %zu decoder entries, %zu words translated (%zu samples taken by a more specific entry); %zu opcodes emitted by the A64 "
                "frontend: %zu lowered, %zu fp_simd, %zu a32, %zu other\n",
                r.patterns, r.words, r.skipped, r.opcodes.size(), reached[0], reached[1], reached[2], reached[3]);
    if (check && not_lowered) {
        std::printf("[RED] nxaot opcodes --reach: %zu opcodes the A64 frontend emits are not lowered:%s\n", not_lowered, missing.c_str());
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return Usage();
    }
    const std::string cmd = argv[1];
    if (cmd == "census") {
        return Census(argc, argv);
    }
    if (cmd == "opcodes") {
        return Opcodes(argc, argv);
    }
    if (cmd == "version") {
        std::printf("llvm\t%s\ndynarmic\t%s\n", NXAOT_LOCK_LLVM, NXAOT_LOCK_DYNARMIC);
        return 0;
    }
    return Usage();
}
