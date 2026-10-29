// Which dynarmic IR opcodes the lifter lowers (src/dynarmic/ir/opcodes.inc is the list). A block that holds any other opcode is refused as a whole
// (LiftRegion reports "unsupported: <opcode>"), never lowered in part.
#pragma once

#include "dynarmic/ir/opcodes.h"

namespace nxaot::lift {

// The groups of `nxaot opcodes` (README.md, "Lifter coverage")
enum class OpcodeGroup {
    Lowered,   // lowered by lifter.cpp
    FpSimd,    // floating point, Advanced SIMD and the cryptographic extension: phase 2 of the plan (aot-recompiler.md section 14)
    A32,       // the AArch32 frontend's opcodes, and the shared ones only it emits (packed arithmetic, ssat / usat, GE flags): never in an A64 block
    Other,     // dynarmic-internal opcodes the A64 translator does not emit: Breakpoint, CallHostFunction, GetNZFromOp (an ARM64-backend pass)
};

OpcodeGroup GroupOf(Dynarmic::IR::Opcode op);

inline bool IsLowered(Dynarmic::IR::Opcode op) {
    return GroupOf(op) == OpcodeGroup::Lowered;
}

const char* GroupName(OpcodeGroup group);

// The opcode's name as opcodes.inc spells it, with the A32 / A64 prefix that Dynarmic::IR::GetNameOf leaves out (A64GetCFlag, A32GetCFlag)
const char* OpcodeName(Dynarmic::IR::Opcode op);

}  // namespace nxaot::lift
