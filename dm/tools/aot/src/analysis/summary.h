// The interprocedural facts of docs/design/aot-recompiler.md section 2.3, over discovery's functions (no code generation): per function the
// registers it may read before writing them, may change, must write, and whether it is preserving (x19..x29, SP and the low halves of v8..v15
// back at every return, from the slots its own prologue saved them to, SP at its entry value), as a fixed point over the call graph (direct calls
// by their callee; blr and unresolved br by the union over the address-taken functions; svc by the SVC convention). Also the exclusive windows
// of section 1.3 under the rewriter's rules, classified as in section 6.5. The register effects of each instruction come from dynarmic's IR of it.
#pragma once

#include <map>

#include "analysis/discover.h"
#include "module/image.h"

namespace nxaot::analysis {

// Fills Function::summary for every function; address_taken must be set already
void Summarise(const module::Image& image, std::map<uint64_t, Function>& functions);

// Fills Function::windows and unpaired_exclusive
void FindWindows(const module::Image& image, Function& fn);

// The effects of one instruction on the registers (uses before its own writes, writes), from dynarmic's IR of it
struct RegFx {
    RegSet use, def;
};
RegFx InstructionFx(uint64_t pc, uint32_t word);

}  // namespace nxaot::analysis
