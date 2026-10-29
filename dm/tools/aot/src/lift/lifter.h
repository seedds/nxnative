// The lifter (docs/design/aot-recompiler.md sections 9.1 and 9.2): dynarmic's A64 frontend translates guest code into dynarmic IR blocks
// (src/decode), and this lowering turns a region of them into one LLVM function `void name(dm_guest_state *st)` (include/nxaot/guest_state.h).
// Guest registers live in allocas that mem2reg promotes (loaded at entry, the written ones stored at exit); NZCV is four i1 values, compared directly
// against the operands of the subtraction or logical operation that set them where the block shows it, and stored to st->nzcv only when written;
// guest memory is one volatile access per IR access, exclusives a compare-and-swap against st->excl_* (section 6.5); svc, brk, udf and anything
// undefined leave the function with st->pc and st->exit_reason. Floating point and SIMD (section 7) follow the FPCR the region is specialised for
// (LiftOptions::fpcr, section 7.1): MXCSR holds its rounding mode and flush-to-zero while guest code runs (the host, or nxaot_vec, sets it), the
// cumulative exception flags accumulate in MXCSR and in st->fpsr (section 7.4), and NaN results follow Arm's rules through the NaN chains of
// section 7.3. A region that holds an IR opcode the lowering does not cover is refused whole (Lifted::unsupported), never lowered in part.
//
// Function mode (LiftOptions::plan, src/lift/lower_calls.cpp): one whole guest function becomes one LLVM function of the calling convention of
// section 2.3 that calls the others: bl as a call, ret as a return checked against the entry's lr (section 3.3), b and br out of the function as
// real tail calls, blr and unbounded br through a per-site inline cache and the dispatcher (section 4), svc as a host call (section 2.4), the
// registers stored and reloaded at calls as the interprocedural summaries say (or all of them in the correctness mode), and every address the code
// builds from its pc (adr, adrp, literal loads, return addresses) relative to the module's base, a run-time value (section 3.3).
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
class CallInst;
class DISubprogram;
class Function;
class FunctionType;
class GlobalVariable;
class LLVMContext;
class Module;
class StructType;
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

struct FunctionPlan;

struct LiftOptions {
    uint32_t fpcr = 0;            // the FPCR the region is specialised for; only its mode bits (AHP, DN, FZ, RMode, FZ16) matter
    bool fpsr_flags = true;       // model the cumulative exception flags exactly (strict floating point; section 7.4)
    NaNMode nan_mode = NaNMode::Chains;
    Target target;
    const FunctionPlan* plan = nullptr;  // function mode: one guest function lifted into plan->fn (the region names its entry and bounds)
};

// ---- Function mode: whole functions and the convention between them (aot-recompiler.md sections 2.3, 3.3, 4; src/build plans them) ----

// A set of guest registers
struct Regs {
    uint32_t x = 0;  // bit i: x_i (x0..x30)
    uint32_t v = 0;  // bit i: v_i (the whole 128-bit register)
    bool sp = false, nzcv = false;
    Regs operator|(const Regs& o) const { return {x | o.x, v | o.v, sp || o.sp, nzcv || o.nzcv}; }
    Regs operator&(const Regs& o) const { return {x & o.x, v & o.v, sp && o.sp, nzcv && o.nzcv}; }
    Regs operator-(const Regs& o) const { return {x & ~o.x, v & ~o.v, sp && !o.sp, nzcv && !o.nzcv}; }
    bool operator==(const Regs&) const = default;
    bool Empty() const { return !x && !v && !sp && !nzcv; }
    static Regs All() { return {0x7FFFFFFFu, 0xFFFFFFFFu, true, true}; }
    static Regs X(uint32_t bits) { return {bits, 0, false, false}; }
    static Regs V(uint32_t bits) { return {0, bits, false, false}; }
};

// The LLVM mapping of the convention between recompiled functions (section 2.3). Both pass the state block, the guest SP, lr (the guest's x30:
// the original return address, section 3.3) and x0..x3 in host registers, and return x0, x1 and v0..v3 as {i64, i64, <2 x i64> x 4}, which both
// lower to RAX, RDX and XMM0..XMM3 (no memory)
enum class CallConv : uint8_t {
    // swifttailcc with st as swiftself (R13): the callee pops its argument area, so musttail calls between different prototypes are allowed, and
    // every function has a signature of its own: x4..x7 and v0..v7 only when the callee reads them (sp, lr, x0..x3 in RDI, RSI, RDX, RCX, R8, R9;
    // x4..x7 on the stack; v0..v7 in XMM0..XMM7). RBX, RBP and R12..R15 are callee-saved, as in the C convention
    SwiftTail,
    // The design's preserve_none candidate. LLVM 18.1, which nxaot.lock pins, has no preserve_nonecc (it came with LLVM 19); ghccc, the convention
    // preserve_none was modelled on, is LLVM 18's stock convention with its defining property: every general register can carry an argument and
    // none is callee-saved. One uniform signature: st, sp, lr, x0..x6 in R13, RBP, R12, RBX, R14, RSI, RDI, R8, R9, R15 and v0..v5 in XMM1..XMM6;
    // x7, v6 and v7 always go through the state block. Frame pointers are off (RBP carries the guest SP)
    Ghc,
};
const char* CallConvName(CallConv cc);  // "swifttail", "ghc"

struct Convention {
    CallConv cc = CallConv::SwiftTail;
    Regs Passable() const;  // what may travel in host registers besides x0..x3: x4..x7 and v0..v7 (SwiftTail), x4..x6 and v0..v5 (Ghc)
    unsigned LlvmCC() const;
    llvm::StructType* RetType(llvm::LLVMContext& c) const;
    // The signature of a function that is passed `pass` (Ghc: every passable register, whatever pass says)
    llvm::FunctionType* Type(llvm::LLVMContext& c, const Regs& pass) const;
    // Argument numbers in that signature: 0 st, 1 sp, 2 lr, then x_i and v_i (-1 when the signature has no argument for it)
    int ArgX(const Regs& pass, unsigned i) const;
    int ArgV(const Regs& pass, unsigned i) const;
    void Apply(llvm::Function* f) const;     // the calling convention and the attributes of st (swiftself for SwiftTail, noalias)
    void Apply(llvm::CallInst* call) const;  // the same at a call
};

// A function as its callers see it
struct CalleeView {
    llvm::Function* fn = nullptr;  // the specialised body
    Regs pass;                     // what it takes in host registers: x0..x3, and the passable registers it reads
    Regs writes;                   // what may hold another value when it returns: reloaded from the block after a call unless returned
    uint32_t rv = 0;               // which of v0..v3 (bits 0..3) its return value holds; the others are poison
    bool returns = true;           // it has a return path (the analysis' summary)
    // writes is this function's own (a direct call). False for the indirect view, whose writes is a union over every function an indirect
    // call may reach: the one that runs stores only what it writes itself, so a register it is passed and the union says may change must be
    // current in the block before the call too
    bool exact = true;
};

// What function mode needs to lift one guest function (src/build/plan.h computes it from the analysis)
struct FunctionPlan {
    Convention conv;
    llvm::Function* fn = nullptr;   // the body to fill, declared with conv.Type(self.pass)
    CalleeView self;
    Regs live_after;                // LIVE_AFTER: what some caller may read after it returns (section 2.3)
    uint64_t entry = 0, limit = 0;  // module offsets: the function's code lies in [entry, limit)
    bool correctness = false;       // every register stored at every call and return and reloaded after every call: the reference mode
    llvm::GlobalVariable* base = nullptr;  // i64: the module's base address, written by the host before guest code runs
    std::function<const CalleeView*(uint64_t offset)> callee;  // the function that starts at a module offset; null when none does
    CalleeView indirect;            // blr and dispatched br: the standard entry (every passable register; the union over address-taken functions)
    std::function<std::vector<uint64_t>(uint64_t call_pc)> resume;    // the resume points of a call site (section 3.3), module offsets
    std::function<const std::vector<uint64_t>*(uint64_t br)> tables;  // the jump table of a br (Region::tables)
    std::vector<uint64_t> roots;    // block starts besides the entry that only a non-local transfer enters (landing pads)
    // Run-time entry points (src/build/pack.cpp declares them; the host defines what they call)
    llvm::Function* rt_dispatch = nullptr;     // preserve_all ptr (ptr st, i64 target, ptr slot, i64 site): an inline-cache miss
    llvm::Function* rt_svc = nullptr;          // void dm_aot_svc(ptr st, i32 number), the C convention
    llvm::Function* rt_trap = nullptr;         // noreturn void (ptr st, i64 offset, i32 reason): traps and the JIT's cases
    llvm::Function* rt_counter = nullptr;      // preserve_most i64 (ptr st): mrs cntpct_el0 (section 8)
    llvm::GlobalVariable* ic_empty = nullptr;  // the descriptor every inline cache starts with (it matches no guest address)
    std::string ic_prefix;                     // inline-cache slots are named <prefix><site offset>
    llvm::DISubprogram* debug = nullptr;       // debug locations, line = guest offset (section 9.2); null: none
    std::string line_section;                  // the !pcsections section of the line table (section 3.5); empty: none
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
    // Function mode: the sites lowered
    size_t calls = 0, indirect_calls = 0, tail_calls = 0, indirect_tails = 0, returns = 0, svcs = 0, traps = 0;
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
