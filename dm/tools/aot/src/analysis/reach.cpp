#include "analysis/reach.h"

#include <algorithm>
#include <optional>

#include "decode/translate.h"
#include "dynarmic/frontend/A64/decoder/a64.h"
#include "dynarmic/frontend/A64/translate/impl/impl.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opt/passes.h"

namespace nxaot::analysis {

namespace {

using namespace Dynarmic;

struct Pattern {
    const char* fn;
    const char* name;
    const char* bits;
};

// The decoder table itself, so that every entry dynarmic decodes is visited (commented-out entries are not decoded and are not in it)
const Pattern kPatterns[] = {
#define INST(fn, name, bitstring) {#fn, name, bitstring},
#include "dynarmic/frontend/A64/decoder/a64.inc"
#undef INST
};

// FPCR values a block is translated under: the translators read only RMode (rounding of the int-to-float conversions and frinti / frintx), but
// the set is cheap and keeps the census honest if a later dynarmic reads more
constexpr uint32_t kFpcrs[] = {0x00000000, 0x00C00000, 0x03000000, 0x04080000};

struct Stream {
    uint64_t state;
    uint64_t Next() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
};

}  // namespace

ReachResult Reach(size_t samples, uint64_t seed) {
    ReachResult out;
    Stream rng{seed};
    constexpr uint64_t kPc = 0x10008000;
    for (const Pattern& p : kPatterns) {
        ++out.patterns;
        uint32_t mask = 0, expect = 0;
        for (int i = 0; i < 32; ++i) {
            const char c = p.bits[i];
            const uint32_t bit = 1u << (31 - i);
            if (c == '0' || c == '1') {
                mask |= bit;
                if (c == '1') {
                    expect |= bit;
                }
            }
        }
        for (size_t s = 0; s < samples + 2; ++s) {
            const uint32_t fill = s == 0 ? 0u : s == 1 ? ~0u : static_cast<uint32_t>(rng.Next());
            const uint32_t word = expect | (fill & ~mask);
            const auto matcher = A64::Decode<A64::TranslatorVisitor>(word);
            if (!matcher || matcher->get().GetMask() != mask || matcher->get().GetExpected() != expect) {
                ++out.skipped;  // a more specific entry decodes this word; it is visited as that entry
                continue;
            }
            for (const uint32_t fpcr : kFpcrs) {
                const decode::CodeReader read = [word](uint64_t a) -> std::optional<uint32_t> {
                    if (a != kPc) {
                        return std::nullopt;
                    }
                    return word;
                };
                IR::Block block = decode::TranslateBlock(kPc, read, fpcr);
                Optimization::IdentityRemovalPass(block);
                Optimization::DeadCodeElimination(block);
                ++out.words;
                std::vector<IR::Opcode> seen;
                for (const auto& inst : block) {
                    if (inst.GetOpcode() != IR::Opcode::Void && std::find(seen.begin(), seen.end(), inst.GetOpcode()) == seen.end()) {
                        seen.push_back(inst.GetOpcode());
                    }
                }
                for (const IR::Opcode op : seen) {
                    ReachedBy& r = out.opcodes[op];
                    ++r.blocks;
                    if (std::find(r.matchers.begin(), r.matchers.end(), p.fn) == r.matchers.end()) {
                        r.matchers.push_back(p.fn);
                    }
                }
            }
        }
    }
    return out;
}

}  // namespace nxaot::analysis
