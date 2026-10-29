// Jump tables recovered on the lifted IR by data flow (docs/design/aot-recompiler.md section 1.3), not by instruction shapes: for a br, the
// function's blocks from one of the br block's dominators to the br are lifted into one LLVM function (src/lift, Region::follow keeps the region
// to the blocks that reach the br; Region::calls continues past calls with what their summaries say they change), promoted to SSA and simplified
// (SROA, EarlyCSE), and the br's target (the operand of the nxaot.indirect marker, Region::mark_indirect) must reduce to a function
// of one guest load whose address is a function of one index; LLVM's LazyValueInfo bounds the index at the br (a dominating unsigned compare, a
// mask) and known bits narrow it; the entries are read from the image and every target must be a decoded instruction of the function.
// The region is sound only when it is closed: every block of it that reaches the br, except its first, is entered only from inside it.
#pragma once

#include <functional>
#include <optional>
#include <string>

#include "analysis/discover.h"
#include "lift/lifter.h"
#include "module/image.h"

namespace nxaot::analysis {

using CallEffects = std::function<std::optional<lift::CallEffect>(uint64_t call_pc)>;

struct TableResult {
    std::optional<JumpTable> table;
    std::string why;        // why not, when there is no table
    unsigned regions = 0;   // regions lifted
};

// fn: the function with its blocks (its known tables become switches in the regions); valid: whether an address may be a target (a decoded
// instruction of the function); calls: empty, or the call effects a region may continue past. optimistic: ignore the blocks only the function's
// speculative roots reach (Function::speculative), so that a switch whose cases loop back to it can be bounded before its cases are known; such
// a result is tentative until a closed region with the cases gives it again (discover.cpp, Verify)
TableResult ResolveJumpTable(const module::Image& image, const Function& fn, uint64_t br, const DiscoverOptions& options,
                             const std::function<bool(uint64_t)>& valid, const CallEffects& calls, bool optimistic);

}  // namespace nxaot::analysis
