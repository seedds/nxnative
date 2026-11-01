// The calling-convention facts of a whole module (docs/design/aot-recompiler.md section 2.3), from discovery's functions and summaries
// (src/analysis): per function what it is passed in host registers (PASS), what its callers reload after it returns (WRITES, the high halves of
// v8..v15 included: a d8 restore zeroes them), what it returns in host registers (x0, x1 and RV, the v0..v3 that every function of its tail-call
// component writes), what some caller may read after it returns (LIVE_AFTER, a fixed point over every call site of the program, indirect ones
// for every address-taken function), whether the host or an indirect call may enter it (a standard entry), and the resume points of its call
// sites (section 3.3: setjmp continuations by symbol, LSDA landing pads). Function mode of the lifter (lift::FunctionPlan) is filled from it.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "analysis/discover.h"
#include "lift/lifter.h"
#include "module/image.h"

namespace nxaot::build {

struct FnFacts {
    uint64_t entry = 0, limit = 0, size = 0;
    std::string name;            // the symbol, when the module has one
    lift::Regs pass;             // PASS
    lift::Regs reads;            // may be read before written (the summary)
    lift::Regs writes;           // WRITES: the summary's, with the v8..v15 the function writes at all (their high halves)
    lift::Regs raw;              // every register it or its callees write
    lift::Regs live_after;       // LIVE_AFTER
    uint32_t rv = 0;             // v0..v3 returned in host registers (its component's)
    bool returns = true;
    bool host_callable = false;  // a standard entry and a lookup slot: address-taken, the module's entry, exported (or every function, all_entries)
    int component = 0;           // its tail-call component (RV)
    std::vector<uint64_t> continuations;      // offsets after its calls to setjmp: resume points of every call site of the function
    std::map<uint64_t, uint64_t> pads;        // call pc -> the landing pad of the LSDA call site covering it
    std::map<uint64_t, std::vector<uint64_t>> tables;  // br pc -> jump-table targets
    std::vector<uint64_t> roots;              // landing pads inside the function
    std::vector<std::pair<uint64_t, uint64_t>> extra;  // its blocks outside [entry, limit): the aux region's (design 1.1), [start, end) each
    size_t words = 0;
};

struct Plan {
    std::map<uint64_t, FnFacts> fns;
    lift::Regs indirect_pass;    // the standard entries' one signature, the same in every pack of the convention: StdPass
    lift::Regs indirect_writes;  // the union of WRITES over the functions an indirect call may reach (the standard entries), AAPCS64's results
    uint32_t indirect_rv = 0;    // RV of their component: kStdRv
    size_t live_after_rounds = 0;
};

// The standard entries' fixed signature (design 2.3; include/nxaot/aot_module.h, ABI 3): x0..x3 and the convention's passable registers in
// host registers (swifttailcc: x4..x7 and v0..v7; ghccc: x4..x6 and v0..v5), x0 and x1 returned in host registers and v0..v3 through the block
lift::Regs StdPass(const lift::Convention& conv);
constexpr uint32_t kStdRv = 0;

lift::Regs ToRegs(const analysis::RegSet& r);

// The facts for every discovered function; all_entries gives every function a standard entry (nxaot_funcdiff calls any of them). threads: the
// threads its per-function passes run on (0: one per CPU); the plan is the same with any number (README.md, "Phase 5c")
Plan MakePlan(const module::Image& image, const analysis::Discovery& d, const lift::Convention& conv, bool all_entries, unsigned threads = 1);

}  // namespace nxaot::build
