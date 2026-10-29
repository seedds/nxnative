// nxaot: the ahead-of-time ARM64 -> x86-64 recompiler (docs/design/aot-recompiler.md). Subcommands today:
//   nxaot census <raw ARM64 code file> --base <hex>   phase 0's census of a code blob, as key<TAB>count lines (src/analysis/census.h)
//   nxaot opcodes                                     every dynarmic IR opcode with whether the lifter lowers it (group<TAB>opcode)
//   nxaot version                                     the toolchain of nxaot.lock this build was configured with
// The lifter (src/lift) is exercised by nxaot_vec until the analysis and the pack stages exist (README.md, "What is not done yet").
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "analysis/census.h"
#include "dynarmic/ir/opcodes.h"
#include "lift/opcodes.h"
#include "nxaot_lock.h"

namespace {

int Usage() {
    std::fprintf(stderr,
                 "usage: nxaot census <raw ARM64 code file> --base <hex>\n"
                 "       nxaot opcodes\n"
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

int Opcodes() {
    using Dynarmic::IR::Opcode;
    size_t per_group[4] = {};
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
        return Opcodes();
    }
    if (cmd == "version") {
        std::printf("llvm\t%s\ndynarmic\t%s\n", NXAOT_LOCK_LLVM, NXAOT_LOCK_DYNARMIC);
        return 0;
    }
    return Usage();
}
