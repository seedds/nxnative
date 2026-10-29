// The lifter (docs/design/aot-recompiler.md sections 9.1 and 9.2): dynarmic's A64 frontend translates guest code into dynarmic IR blocks
// (src/decode), and this lowering turns a region of them into one LLVM function `void name(dm_guest_state *st)` (include/nxaot/guest_state.h).
// Guest registers live in allocas that mem2reg promotes (loaded at entry, the written ones stored at exit); NZCV is four i1 values, compared directly
// against the operands of the subtraction or logical operation that set them where the block shows it, and stored to st->nzcv only when written;
// guest memory is one volatile access per IR access, exclusives a compare-and-swap against st->excl_* (section 6.5); svc, brk, udf and anything
// undefined leave the function with st->pc and st->exit_reason. Floating point and SIMD (section 7) follow the FPCR the region is specialised for
// (LiftOptions::fpcr, section 7.1): MXCSR holds its rounding mode and flush-to-zero while guest code runs (the host, or nxaot_vec, sets it), the
// cumulative exception flags accumulate in MXCSR and in st->fpsr (section 7.4), and NaN results follow Arm's rules through the NaN chains of
// section 7.3. A region that holds an IR opcode the lowering does not cover is refused whole (Lifted::unsupported), never lowered in part.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
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

// What the lowering may use of the x86-64 target (lift/jit.h reads it from the target machine). The baseline is x86-64-v3 (FMA3, AVX2, F16C:
// migration-status.md, CPU baseline); AES-NI, SHA and PCLMULQDQ are outside the x86-64 levels and have software forms
struct Target {
    bool fma = true, f16c = true, avx2 = true;
    bool aes = false, sha = false, pclmul = false;
};

// How NaN results are made exact (section 7.3)
enum class NaNMode {
    Chains,     // the fast path: a check on each value that leaves a chain of P operations, the chain recomputed with Arm's rules when it is a NaN
    PerOp,      // every operation checks its own result (dynarmic's way): the reference the chains are measured against
    Unchecked,  // no NaN fix-up at all: inexact (x86 NaNs), for measuring the cost of the checks only (nxaot_bench); never in a pack
};

struct LiftOptions {
    uint32_t fpcr = 0;            // the FPCR the region is specialised for; only its mode bits (AHP, DN, FZ, RMode, FZ16) matter
    bool fpsr_flags = true;       // model the cumulative exception flags exactly (strict floating point; section 7.4)
    NaNMode nan_mode = NaNMode::Chains;
    Target target;
};

// The FPCR bits a block is specialised on: dynarmic's location descriptor keeps the same (frontend/A64/a64_location_descriptor.h, fpcr_mask)
constexpr uint32_t kFpcrModeMask = 0x07C80000;

// What a call may change, for a region that continues past it (Region::calls): the registers that may hold another value when the callee
// returns (its interprocedural summary, src/analysis/summary.h). Everything else keeps its value across the call
struct CallEffect {
    uint32_t x = 0x7FFFFFFFu;  // bit i: x_i
    uint32_t v = 0xFFFFFFFFu;  // bit i: v_i
    bool sp = true, nzcv = true;
};

struct Region {
    uint64_t entry = 0;
    uint64_t lo = 0, hi = 0;  // blocks that start in [lo, hi) are lifted into the function; a transfer anywhere else leaves it with st->pc set
    size_t max_blocks = 64;
    // For the analyses (src/analysis/jumptable.cpp); the defaults lift a region as the vector harness and the benchmark need it.
    // follow: the block starts the region may enter besides the entry (empty: every one in [lo, hi))
    std::function<bool(uint64_t pc)> follow;
    // calls: for a block that ends in bl or blr at call_pc, what the call may change; the region then goes on at call_pc + 4 with those
    // registers unknown (opaque values) instead of leaving at the call. nullopt, or no hook: the call leaves the function as before
    std::function<std::optional<CallEffect>(uint64_t call_pc)> calls;
    // mark_indirect: before each exit to a computed target, `call void @nxaot.indirect(i64 <pc of the branch>, i64 <target>)`, so that an
    // analysis finds the target's value in the optimised IR
    bool mark_indirect = false;
    // tables: the targets of a br at br_pc that a jump table gives (null: none known); the region follows them, and the br becomes an LLVM
    // switch over them that leaves the function for any other value (aot-recompiler.md section 4)
    std::function<const std::vector<uint64_t>*(uint64_t br_pc)> tables;
};

// Where dynarmic's x86-64 backend, the reference of nxaot_vec, raises other cumulative FPSR bits than the Arm ARM for an operation the lowering
// follows the Arm ARM in (README.md, "Known issues": the numbers there). The lowering notes the ones a region meets, in the forms dynarmic takes
// them; nxaot_vec relaxes only those FPSR bits, only in those directions, for such regions (src/vec/main.cpp, KnownIssues)
enum DynarmicIssue : int {
    kIssueRoundIntInexact = 8,      // frintn / p / m / z / i: Inexact from roundss (Arm raises none when exact = FALSE)
    kIssueToFixedNative = 9,        // fcvt[npmz][su] / fcvtzs fixed: the clamps of the native conversions drop or swap Invalid Operation and Inexact
    kIssueToFixedSoftware = 10,     // fcvta[su], half precision: a negative value rounding to 0 in an unsigned conversion gives IOC, not IXC
    kIssueMulAddNaNProduct = 11,    // fmadd family, DN = 0: the NaN path's test multiply adds Inexact / Overflow / Underflow
    kIssueNoInputDenormal = 12,     // FZ: denormal inputs flushed by DAZ raise no Input Denormal in native operations
    kIssueVectorMinMaxQNaN = 13,    // fmax / fmin / fmaxnm / fminnm (vector): maxps / minps raise Invalid Operation for quiet NaNs
    kIssueRSqrtEstimateNative = 14, // frsqrte (single, double): the native estimate raises Inexact (sqrtss / divss) or, through rsqrtss, nothing
    kIssueVectorUpperLanes = 15,    // 64-bit vector forms: dynarmic computes the zero upper lanes too (fdiv 0 / 0: IOC; frecpe, frsqrte of 0: DZC)
    kIssueMulAddInfZero = 16,       // fmla / fmls, and the fmadd family with DN: a quiet NaN addend with infinity times zero raises no Invalid Operation
};

struct Lifted {
    llvm::Function* function = nullptr;    // null when the region was refused
    std::vector<std::string> unsupported;  // the dynarmic IR opcodes it was refused for: their names, sorted, each once
    std::string error;                     // an internal error of the lowering (the region is refused too)
    std::vector<Dynarmic::IR::Opcode> opcodes;  // every IR opcode of the region's blocks (after dead-code elimination), each once
    std::vector<int> dynarmic_issues;      // DynarmicIssue values the region's floating point meets, sorted, each once
    size_t blocks = 0;
};

// Lifts the region into module as a function called name. The module's data layout should be the target's (lift/jit.h sets it)
Lifted LiftRegion(llvm::Module& module, const std::string& name, const Region& region, const decode::CodeReader& read, const LiftOptions& options = {});

// dynarmic's text dump of the region's IR blocks, as the lifter sees them (nxaot_vec --dump)
std::string DumpRegion(const Region& region, const decode::CodeReader& read, uint32_t fpcr = 0);

// The MXCSR value guest code runs with for an FPCR (dynarmic's A64JitState::SetFpcr): every exception masked, RMode as the rounding control, FZ
// as flush-to-zero and denormals-are-zero, the flags clear
uint32_t GuestMxcsr(uint32_t fpcr);

// MXCSR exception flags as FPSR cumulative bits (dynarmic's A64JitState::GetFpsr): IE -> IOC, ZE -> DZC, OE -> OFC, UE -> UFC, PE -> IXC
uint32_t FpsrFromMxcsr(uint32_t mxcsr);

}  // namespace nxaot::lift
