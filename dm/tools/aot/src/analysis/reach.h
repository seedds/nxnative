// Which dynarmic IR opcodes the A64 frontend can emit: every encoding of dynarmic's A64 decoder table (frontend/A64/decoder/a64.inc) is filled with
// random field values, decoded, translated alone with nxaot's translation options and the lifter's passes (src/lift/lifter.cpp, Discover), under
// several FPCR values, and the opcodes of the resulting blocks are collected. `nxaot opcodes --reach` prints the result; its check (CTest
// `opcodes_reach`) fails when an opcode the A64 frontend emits is not lowered, which is what makes the a32 and other groups of src/lift/opcodes.cpp
// provably unreachable from A64 code rather than asserted.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "dynarmic/ir/opcodes.h"

namespace nxaot::analysis {

struct ReachedBy {
    size_t blocks = 0;                 // translated words whose block held the opcode
    std::vector<std::string> matchers;  // the decoder entries (a64.inc names) that emitted it, in table order, each once
};

struct ReachResult {
    std::map<Dynarmic::IR::Opcode, ReachedBy> opcodes;
    size_t patterns = 0;  // decoder entries tried
    size_t words = 0;     // words translated (every sample of every entry under every FPCR value)
    size_t skipped = 0;   // samples that a more specific decoder entry took
};

// samples: random fillings of each entry's variable bits (besides all-zero and all-one fillings); seed: the stream's seed
ReachResult Reach(size_t samples, uint64_t seed);

}  // namespace nxaot::analysis
