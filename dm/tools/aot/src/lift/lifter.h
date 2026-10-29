// The lifter (docs/design/aot-recompiler.md sections 9.1 and 9.2): dynarmic's A64 frontend translates guest code into dynarmic IR blocks
// (src/decode), and this lowering turns a region of them into one LLVM function `void name(dm_guest_state *st)` (include/nxaot/guest_state.h).
// Guest registers live in allocas that mem2reg promotes (loaded at entry, the written ones stored at exit); NZCV is four i1 values, compared directly
// against the operands of the subtraction or logical operation that set them where the block shows it, and stored to st->nzcv only when written;
// guest memory is one volatile access per IR access, exclusives a compare-and-swap against st->excl_* (section 6.5); svc, brk, udf and anything
// undefined leave the function with st->pc and st->exit_reason. Scope today: the integer, memory, branch, system-register and exclusive instruction
// classes. A region that holds a floating-point or SIMD IR opcode is refused whole (Lifted::unsupported), never lowered in part.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "decode/translate.h"
#include "dynarmic/ir/opcodes.h"

namespace llvm {
class Function;
class Module;
}  // namespace llvm

namespace nxaot::lift {

// The values recompiled code reads for the system registers that are fixed on the Switch (sections 1.1, 8 and 9.2); nxaot_vec configures dynarmic with
// the same values
constexpr uint32_t kCntfrq = 19'200'000;  // cntfrq_el0: the guest counter's frequency, 19.2 MHz
constexpr uint32_t kCtr = 0x40000;        // ctr_el0: the constant the Apple rewriter writes (DminLine 4: 64-byte data cache lines)
constexpr uint32_t kDczid = 4;            // dczid_el0: dc zva zeroes 4 << 4 = 64 bytes
constexpr uint64_t kZvaBytes = 64;
// The bits msr fpcr / msr fpsr keep. FPSR: QC and the cumulative exception flags (as the Cortex-A57 has them). FPCR: as dynarmic keeps them
// (backend/x64/a64_jitstate.cpp, FPCR_MASK), which includes FZ16 and the trap enables the A57 does not implement (README.md, "Known issues")
constexpr uint32_t kFpcrMask = 0x07C89F00;
constexpr uint32_t kFpsrMask = 0x0800009F;

struct Region {
    uint64_t entry = 0;
    uint64_t lo = 0, hi = 0;  // blocks that start in [lo, hi) are lifted into the function; a transfer anywhere else leaves it with st->pc set
    size_t max_blocks = 64;
};

struct Lifted {
    llvm::Function* function = nullptr;    // null when the region was refused
    std::vector<std::string> unsupported;  // the dynarmic IR opcodes it was refused for: their names, sorted, each once
    std::string error;                     // an internal error of the lowering (the region is refused too)
    std::vector<Dynarmic::IR::Opcode> opcodes;  // every IR opcode of the region's blocks (after dead-code elimination), each once
    size_t blocks = 0;
};

// Lifts the region into module as a function called name. The module's data layout should be the target's (lift/jit.h sets it)
Lifted LiftRegion(llvm::Module& module, const std::string& name, const Region& region, const decode::CodeReader& read);

// dynarmic's text dump of the region's IR blocks, as the lifter sees them (nxaot_vec --dump)
std::string DumpRegion(const Region& region, const decode::CodeReader& read);

}  // namespace nxaot::lift
