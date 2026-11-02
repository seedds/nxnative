#include "build/pack.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <thread>

#include <sched.h>
#include <sys/resource.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#endif
#include <unistd.h>

#include <lld/Common/Driver.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticPrinter.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/IntrinsicsX86.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassTimingInfo.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Pass.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/StandardInstrumentations.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Timer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/SubtargetFeature.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/BDCE.h>
#include <llvm/Transforms/Scalar/DCE.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/InstSimplifyPass.h>
#include <llvm/Transforms/Scalar/SCCP.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>

#include "analysis/census.h"
#include "analysis/discover.h"
#include "analysis/insn.h"
#include "analysis/memory.h"
#include "analysis/summary.h"
#include "build/analysis_cache.h"
#include "build/coff.h"
#include "build/plan.h"
#include "cache/cache.h"
#include "nxaot/aot_module.h"
#include "nxaot/guest_state.h"
#include "nxaot_lock.h"

LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(coff)

namespace nxaot::build {

namespace {

using Clock = std::chrono::steady_clock;
double Since(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

// What --workers 0 sizes the compile's pool by (CompileWorkers; README.md, "Memory at scale"), measured on the game-shaped benchmark of
// gen_bench.py --direct with 150,000 functions (7.8 million instructions): a worker's share of the compile's peak with 8 and 4 workers, the
// tables' object and what the compile starts from aside (H+: 98 to 164 MiB with shards of up to 64,000 instructions, its malloc arena's
// leftovers included, given 192 at H and H+; C and W: about 20 MiB with groups of up to 1,024, given 64 for the parts of split functions of up
// to 4,096, which that module has none of); and the tables' object, which one worker compiles beside the units and which grows with the
// module's functions (2,401 bytes per function: its function map's and lookup table's constants, their code, the declarations);
// the analysis's own peak per text word (52 to 57 bytes at 150,000 and 600,000 functions, the image and 128 MiB of libraries besides), for the
// warning when the machine has less than that
constexpr uint64_t kWorkerPeak[4] = {64ull << 20, 64ull << 20, 192ull << 20, 192ull << 20};  // C, W, H, H+
constexpr uint64_t kTablesFixed = 32ull << 20, kTablesPerFunction = 2560;
constexpr uint64_t kAnalysisPerWord = 64, kAnalysisFixed = 128ull << 20;

// The cache key's format: changed whenever what a key covers changes (UnitKey), or what a unit's object holds for the same key. 2: the gate
// sites and the aux region's code, the line marks' section, the standard entries' fixed signature (ABI 3). 3: groups of functions, parts of
// split functions, a tier per function (phase 5b). 4: phase 5d's units: the NaN helpers declared (the tables' object defines them), the
// registers forwarded within blocks and NZCV made lazily, rotates by a constant as shifts, the tier pipelines of 5d and tier C's code for
// FastISel (an object of format 3 under the same key would hold its own helpers, which the tables' object now defines too)
constexpr const char* kUnitKeyFormat = "nxaot-unit 4";

// The section each unit's !pcsections marks go to (the line table, "The line table" below): read back after code generation and kept out of
// the link (SHF_EXCLUDE)
constexpr const char* kLineMarks = ".nxaot_pcs";

std::string Sanitize(std::string s) {
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".nro") == 0) {
        s.resize(s.size() - 4);
    }
    for (char& c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
            c = '_';
        }
    }
    return s.empty() ? "module" : s;
}

llvm::CodeGenOptLevel CodegenLevel(Tier t) {
    switch (t) {
    case Tier::C:
        return llvm::CodeGenOptLevel::None;
    case Tier::W:
        return llvm::CodeGenOptLevel::Less;
    case Tier::H:
        return llvm::CodeGenOptLevel::Default;
    default:
        return llvm::CodeGenOptLevel::Aggressive;
    }
}

std::unique_ptr<llvm::TargetMachine> MakeTarget(const std::string& cpu_name, Tier tier, std::string* cpu_out, std::string* features_out,
                                                std::string* error) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
    const std::string triple = llvm::sys::getProcessTriple();
    std::string err;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, err);
    if (!target) {
        *error = err;
        return nullptr;
    }
    llvm::SubtargetFeatures features;
    std::string cpu = cpu_name;
    if (cpu.empty()) {
        // The pack machine's own CPU and its features (section 9.3: the -march=native of the brief)
        cpu = llvm::sys::getHostCPUName().str();
        llvm::StringMap<bool> host;
        if (llvm::sys::getHostCPUFeatures(host)) {
            for (const auto& f : host) {
                features.AddFeature(f.getKey(), f.getValue());
            }
        }
    }
    llvm::TargetOptions to;
    auto* tm = target->createTargetMachine(triple, cpu, features.getString(), to, llvm::Reloc::PIC_, std::nullopt, CodegenLevel(tier));
    if (!tm) {
        *error = "cannot create a target machine for " + triple + " " + cpu;
        return nullptr;
    }
    *cpu_out = cpu;
    *features_out = features.getString();
    return std::unique_ptr<llvm::TargetMachine>(tm);
}

lift::Target TargetOf(const llvm::TargetMachine& tm) {
    const llvm::MCSubtargetInfo* sti = tm.getMCSubtargetInfo();
    lift::Target t;
    t.fma = sti->checkFeatures("+fma");
    t.f16c = sti->checkFeatures("+f16c");
    t.avx2 = sti->checkFeatures("+avx2");
    t.aes = sti->checkFeatures("+aes");
    t.sha = sti->checkFeatures("+sha");
    t.pclmul = sti->checkFeatures("+pclmul");
    return t;
}

// ---- Tier C's code for FastISel (README.md, "Phase 5d") ----
// Tier C selects instructions with FastISel (CodeGenOptLevel::None), which hands to SelectionDAG what it cannot select: a call alone, but for
// any other instruction, and for a terminator, the whole block up to it, built, combined, legalised and scheduled as a DAG. Measured on the
// 4,000-function benchmark at tier C, SelectionDAG was 43 % of the optimisation and code generation (FastISel itself 1 %), and it compiled
// two thirds of the IR (--isel-misses counts the misses and what they handed over). The functions below rewrite, after the tier's passes,
// the shapes of IR that missed most into ones FastISel selects, each the same operation (the same machine code, or SelectionDAG's own
// expansion of it); run by Optimise in this order.
//
// SimplifyCFG's tail merging (tailMergeBlocksWithSimilarFunctionTerminators) joins a function's returns into one block, `common.ret`, that
// returns a phi of the returned values. The returned value is the convention's struct ({x0, x1, v0..v3}), a type FastISel has no register for,
// so the terminator of every block that branches there missed (handlePHINodesInSuccessorBlocks), and those blocks are every block that ends in
// a guest call (its pending check, design 3.3, leaves through a return) and every return: most of the code. The returns are split again here,
// each predecessor returning its own value (a swifttailcc return is SelectionDAG's either way, as one block)
void SplitCommonReturns(llvm::Function& f) {
    std::vector<llvm::BasicBlock*> merged;
    for (llvm::BasicBlock& bb : f) {
        auto* ret = llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator());
        auto* phi = ret ? llvm::dyn_cast_or_null<llvm::PHINode>(ret->getReturnValue()) : nullptr;
        if (phi && phi->getParent() == &bb && &bb.front() == phi && phi->getNextNode() == ret && phi->hasOneUse()) merged.push_back(&bb);
    }
    for (llvm::BasicBlock* bb : merged) {
        auto* phi = llvm::cast<llvm::PHINode>(&bb->front());
        const llvm::DebugLoc loc = bb->getTerminator()->getDebugLoc();
        std::vector<llvm::BasicBlock*> preds;
        for (llvm::BasicBlock* p : phi->blocks()) {
            if (std::find(preds.begin(), preds.end(), p) == preds.end()) preds.push_back(p);
        }
        for (llvm::BasicBlock* p : preds) {
            llvm::Value* v = phi->getIncomingValueForBlock(p);
            auto* br = llvm::dyn_cast<llvm::BranchInst>(p->getTerminator());
            if (br && br->isUnconditional()) {
                // The predecessor returns itself
                llvm::ReturnInst::Create(f.getContext(), v, br)->setDebugLoc(loc);
                br->eraseFromParent();
                continue;
            }
            auto* nb = llvm::BasicBlock::Create(f.getContext(), bb->getName() + ".split", &f, bb);
            llvm::ReturnInst::Create(f.getContext(), v, nb)->setDebugLoc(loc);
            p->getTerminator()->replaceSuccessorWith(bb, nb);
        }
        phi->replaceAllUsesWith(llvm::PoisonValue::get(phi->getType()));
        bb->eraseFromParent();
    }
}

// InstCombine folds the same field taken from two calls' results into one extractvalue of a phi of the results (foldPHIArgExtractValue...):
// the same struct-typed phi, whose predecessors' terminators then miss. Each field taken from it becomes a phi of that field again
void ScalarizeStructPhis(llvm::Function& f) {
    std::vector<llvm::PHINode*> phis;
    for (llvm::BasicBlock& bb : f) {
        for (llvm::PHINode& phi : bb.phis()) {
            if (!phi.getType()->isStructTy()) continue;
            const bool fields = std::all_of(phi.user_begin(), phi.user_end(), [](const llvm::User* u) {
                const auto* ev = llvm::dyn_cast<llvm::ExtractValueInst>(u);
                return ev && ev->getNumIndices() == 1;
            });
            if (fields) phis.push_back(&phi);
        }
    }
    for (llvm::PHINode* phi : phis) {
        std::map<unsigned, llvm::PHINode*> per_field;
        std::map<std::pair<llvm::BasicBlock*, unsigned>, llvm::Value*> taken;  // a predecessor's entries take one value
        const auto field = [&](unsigned k) {
            llvm::PHINode*& p = per_field[k];
            if (p) return p;
            llvm::Type* ty = llvm::cast<llvm::StructType>(phi->getType())->getElementType(k);
            p = llvm::PHINode::Create(ty, phi->getNumIncomingValues(), phi->getName() + "." + llvm::Twine(k), phi);
            p->setDebugLoc(phi->getDebugLoc());
            for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
                llvm::BasicBlock* b = phi->getIncomingBlock(i);
                llvm::Value*& v = taken[{b, k}];
                if (!v) {
                    llvm::Value* agg = phi->getIncomingValue(i);
                    if (auto* c = llvm::dyn_cast<llvm::Constant>(agg)) {
                        v = c->getAggregateElement(k);
                    } else {
                        auto* ev = llvm::ExtractValueInst::Create(agg, {k}, "", b->getTerminator());
                        ev->setDebugLoc(b->getTerminator()->getDebugLoc());
                        v = ev;
                    }
                }
                p->addIncoming(v, b);
            }
            return p;
        };
        for (llvm::User* u : llvm::make_early_inc_range(phi->users())) {
            auto* ev = llvm::cast<llvm::ExtractValueInst>(u);
            ev->replaceAllUsesWith(field(ev->getIndices()[0]));
            ev->eraseFromParent();
        }
        phi->eraseFromParent();
    }
}

// A return of the convention (swifttailcc, preserve_all) is SelectionDAG's (X86FastISel::X86SelectRet takes the C conventions), and so is a
// musttail call: the terminator misses, and the whole block before it with it, which is the whole body of a function without branches and
// the code since the last branch of any other. Each such block is split before what FastISel cannot select at its end: the return's
// insertvalue chain and the return, or the musttail call and the return
void SplitReturnBlocks(llvm::Function& f) {
    if (f.getCallingConv() == llvm::CallingConv::C) return;
    std::vector<llvm::Instruction*> at;
    for (llvm::BasicBlock& bb : f) {
        auto* ret = llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator());
        if (!ret) continue;
        llvm::Instruction* first = ret;
        if (llvm::CallInst* tail = bb.getTerminatingMustTailCall()) {
            first = tail;
        } else {
            while (first->getPrevNode() && llvm::isa<llvm::InsertValueInst>(first->getPrevNode())) first = first->getPrevNode();
        }
        if (first != bb.getFirstNonPHIOrDbg()) at.push_back(first);
    }
    for (llvm::Instruction* i : at) {
        i->getParent()->splitBasicBlock(i, i->getParent()->getName() + ".ret");
    }
}

// The NaN chains' checks (lower_fp.cpp, Escape) ask whether any lane of a vector is a NaN: vector.reduce.or of a vector compare, which the
// code generator's IR passes expand to a compare of the <N x i1> bits as an iN (ExpandReductions): FastISel selects no vector compare and no
// i4, so the terminator missed and the block, the whole computation the check guards, went to SelectionDAG. The check becomes the sign mask of
// the lanes instead (movmskps / movmskpd, as SelectionDAG makes it), a call FastISel hands to SelectionDAG alone, and a scalar compare:
//   fcmp uno a, b (the chains' quiet check)       cmpunordps / cmpunordpd, then the mask
//   icmp ugt (and x, ~sign), c (IsNaN of the bits)  add (SMAX - c): the lane's sign bit is set exactly when it is above c (the and leaves at
//                                                   most SMAX, so the sum stays below 2^width), then the mask
void LaneMasks(llvm::Function& f) {
    llvm::Module& m = *f.getParent();
    std::vector<llvm::IntrinsicInst*> reduces;
    for (llvm::BasicBlock& bb : f) {
        for (llvm::Instruction& i : bb) {
            if (auto* ii = llvm::dyn_cast<llvm::IntrinsicInst>(&i); ii && ii->getIntrinsicID() == llvm::Intrinsic::vector_reduce_or) reduces.push_back(ii);
        }
    }
    for (llvm::IntrinsicInst* red : reduces) {
        auto* vt = llvm::cast<llvm::FixedVectorType>(red->getArgOperand(0)->getType());
        const unsigned lanes = vt->getNumElements();
        if (lanes != 4 && lanes != 2) continue;
        const bool single = lanes == 4;
        llvm::IRBuilder<> b{red};
        b.SetCurrentDebugLocation(red->getDebugLoc());
        llvm::Type* fvec = llvm::FixedVectorType::get(single ? b.getFloatTy() : b.getDoubleTy(), lanes);
        llvm::Value* signs = nullptr;  // a vector whose lanes' sign bits are the answer
        if (auto* fc = llvm::dyn_cast<llvm::FCmpInst>(red->getArgOperand(0));
            fc && fc->getPredicate() == llvm::CmpInst::FCMP_UNO && fc->getOperand(0)->getType() == fvec) {
            const auto id = single ? llvm::Intrinsic::x86_sse_cmp_ps : llvm::Intrinsic::x86_sse2_cmp_pd;
            signs = b.CreateCall(llvm::Intrinsic::getDeclaration(&m, id), {fc->getOperand(0), fc->getOperand(1), b.getInt8(3)});  // UNORD_Q
        } else if (auto* ic = llvm::dyn_cast<llvm::ICmpInst>(red->getArgOperand(0)); ic && ic->getPredicate() == llvm::CmpInst::ICMP_UGT) {
            auto* masked = llvm::dyn_cast<llvm::BinaryOperator>(ic->getOperand(0));
            auto* bound = llvm::dyn_cast_or_null<llvm::ConstantInt>(llvm::isa<llvm::Constant>(ic->getOperand(1))
                                                                         ? llvm::cast<llvm::Constant>(ic->getOperand(1))->getSplatValue()
                                                                         : nullptr);
            auto* keep = masked && masked->getOpcode() == llvm::Instruction::And && llvm::isa<llvm::Constant>(masked->getOperand(1))
                             ? llvm::dyn_cast_or_null<llvm::ConstantInt>(llvm::cast<llvm::Constant>(masked->getOperand(1))->getSplatValue())
                             : nullptr;
            const unsigned width = masked ? masked->getType()->getScalarSizeInBits() : 0;
            if (!bound || !keep || width * lanes != 128 || !keep->getValue().isMaxSignedValue() || bound->getValue().isNegative()) continue;
            llvm::Value* sum = b.CreateAdd(masked, llvm::ConstantInt::get(masked->getType(), llvm::APInt::getSignedMaxValue(width) - bound->getValue()));
            signs = b.CreateBitCast(sum, fvec);
        }
        if (!signs) continue;
        const auto id = single ? llvm::Intrinsic::x86_sse_movmsk_ps : llvm::Intrinsic::x86_sse2_movmsk_pd;
        llvm::Value* mask = b.CreateCall(llvm::Intrinsic::getDeclaration(&m, id), {signs});
        llvm::Value* any = b.CreateICmpNE(mask, b.getInt32(0));
        llvm::Instruction* cmp = llvm::cast<llvm::Instruction>(red->getArgOperand(0));
        red->replaceAllUsesWith(any);
        red->eraseFromParent();
        if (cmp->use_empty()) cmp->eraseFromParent();
    }
}

// fptoui to i64 is no x86 instruction before AVX-512, nor one FastISel selects (to i32 neither): the scalar ones, which the FPToFixed
// lowering makes of a value already in range (lower_fp.cpp, ToFixed), become what SelectionDAG expands them to: below 2^63 a signed
// conversion; at or above it the signed conversion of x - 2^63 (exact: x is an integer there) with the top bit set. To i32 the signed
// conversion to i64, truncated
void ExpandFPToUI(llvm::Function& f) {
    std::vector<llvm::FPToUIInst*> convs;
    for (llvm::BasicBlock& bb : f) {
        for (llvm::Instruction& i : bb) {
            if (auto* c = llvm::dyn_cast<llvm::FPToUIInst>(&i); c && !c->getType()->isVectorTy()) convs.push_back(c);
        }
    }
    for (llvm::FPToUIInst* c : convs) {
        llvm::IRBuilder<> b{c};
        b.SetCurrentDebugLocation(c->getDebugLoc());
        llvm::Value* x = c->getOperand(0);
        llvm::Type* it = c->getType();
        llvm::Value* r = nullptr;
        if (it->isIntegerTy(64)) {
            llvm::Value* two63 = llvm::ConstantFP::get(x->getType(), 9223372036854775808.0);
            llvm::Value* high = b.CreateFCmpOGE(x, two63);
            llvm::Value* low = b.CreateFPToSI(x, it);
            llvm::Value* upper = b.CreateXor(b.CreateFPToSI(b.CreateFSub(x, two63), it), b.getInt64(1ull << 63));
            r = b.CreateSelect(high, upper, low);
        } else if (it->getIntegerBitWidth() <= 32) {
            r = b.CreateTrunc(b.CreateFPToSI(x, b.getInt64Ty()), it);
        } else {
            continue;
        }
        c->replaceAllUsesWith(r);
        c->eraseFromParent();
    }
}

// FastISel selects no atomic load (X86FastISel::X86SelectLoad): an inline cache's slot (unordered, lower_calls.cpp FnDispatch) and the
// safepoint flag (monotonic, FnPoll) missed, and the block before them went to SelectionDAG. A naturally aligned load of at most 8 bytes is
// one mov on x86-64 as an unordered, a monotonic or a volatile load, and none of them is removed, merged or moved across another volatile
// access: after the passes, at tier C, those two become volatile loads. Acquire loads (guest ldar) stay
void PlainAtomicLoads(llvm::Function& f) {
    for (llvm::BasicBlock& bb : f) {
        for (llvm::Instruction& i : bb) {
            auto* l = llvm::dyn_cast<llvm::LoadInst>(&i);
            if (!l || !l->isAtomic()) continue;
            const llvm::AtomicOrdering o = l->getOrdering();
            if (o != llvm::AtomicOrdering::Unordered && o != llvm::AtomicOrdering::Monotonic) continue;
            const uint64_t bytes = f.getParent()->getDataLayout().getTypeStoreSize(l->getType());
            if (bytes > 8 || l->getAlign().value() < bytes) continue;
            l->setAtomic(llvm::AtomicOrdering::NotAtomic);
            l->setVolatile(true);
        }
    }
}

// A vector constant (a splat of 0.5, a zero vector) is no value FastISel can materialise (X86FastISel::fastMaterializeConstant takes
// integers, floating-point scalars and addresses), so an instruction with one as an operand missed, and its block went to SelectionDAG,
// which loads it from the constant pool. Here it is loaded from a constant of the unit's own (the same load), in front of the instructions
// FastISel selects; a phi's incoming constant is loaded at the end of its predecessor
void LoadVectorConstants(llvm::Function& f, std::map<llvm::Constant*, llvm::GlobalVariable*>& pool) {
    llvm::Module& m = *f.getParent();
    const auto global = [&](llvm::Constant* c) {
        llvm::GlobalVariable*& g = pool[c];
        if (!g) {
            g = new llvm::GlobalVariable(m, c->getType(), true, llvm::GlobalValue::PrivateLinkage, c, "nxaot.vconst");
            g->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
            g->setAlignment(llvm::Align(16));
        }
        return g;
    };
    const auto is_vector_constant = [](llvm::Value* v) {
        auto* c = llvm::dyn_cast<llvm::Constant>(v);
        return c && v->getType()->isVectorTy() && !v->getType()->getScalarType()->isIntegerTy(1) && !llvm::isa<llvm::UndefValue>(c) &&
               !llvm::isa<llvm::ConstantExpr>(c);  // a mask of i1 lanes stays a constant: FastISel selects no vector of i1 either way
    };
    for (llvm::BasicBlock& bb : f) {
        for (llvm::Instruction& i : bb) {
            const bool selectable = llvm::isa<llvm::BinaryOperator>(i) || llvm::isa<llvm::UnaryOperator>(i) || llvm::isa<llvm::StoreInst>(i) ||
                                    llvm::isa<llvm::SelectInst>(i) || llvm::isa<llvm::CastInst>(i) || llvm::isa<llvm::PHINode>(i);
            if (!selectable) continue;
            std::map<llvm::BasicBlock*, llvm::LoadInst*> per_pred;  // a phi's entries for one predecessor take one value
            for (unsigned k = 0; k < i.getNumOperands(); ++k) {
                llvm::Value* v = i.getOperand(k);
                if (!is_vector_constant(v)) continue;
                auto* c = llvm::cast<llvm::Constant>(v);
                llvm::Instruction* at = &i;
                llvm::LoadInst** shared = nullptr;
                if (auto* phi = llvm::dyn_cast<llvm::PHINode>(&i)) {
                    shared = &per_pred[phi->getIncomingBlock(k)];
                    at = phi->getIncomingBlock(k)->getTerminator();
                }
                llvm::LoadInst* load = shared && *shared ? *shared : nullptr;
                if (!load) {
                    load = new llvm::LoadInst(c->getType(), global(c), "vconst", false, llvm::Align(16), at);
                    load->setDebugLoc(llvm::isa<llvm::PHINode>(i) ? at->getDebugLoc() : i.getDebugLoc());
                    if (shared) *shared = load;
                }
                i.setOperand(k, load);
            }
        }
    }
}

void Optimise(llvm::Module& m, llvm::TargetMachine* tm, Tier tier) {
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    // --time-passes (llvm::TimePassesIsEnabled): the standard instrumentations time every pass of the new pass manager
    llvm::PassInstrumentationCallbacks pic;
    std::optional<llvm::StandardInstrumentations> si;
    if (llvm::TimePassesIsEnabled) {
        si.emplace(m.getContext(), false);
        si->registerCallbacks(pic, &mam);
    }
    llvm::PassBuilder pb{tm, llvm::PipelineTuningOptions(), std::nullopt, &pic};
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    llvm::ModulePassManager mpm;
    switch (tier) {
    case Tier::C: {
        // Section 9.3, tier C: promotion of the registers' allocas, simplification, simplifycfg, dead-code elimination; FastISel and the fast
        // register allocator. Measured on the 4,000-function benchmark (README.md, "Phase 5d"): mem2reg in place of SROA (the lowering's
        // allocas are whole registers loaded and stored as their own type, which mem2reg promotes as SROA did: the same code, a fifth of
        // the passes' time), and InstSimplify in place of InstCombine (the builder folds as InstSimplify does already; InstCombine was a
        // fifth of tier C's compile for 9 % less code, which is code the profile never sampled)
        llvm::FunctionPassManager fpm;
        fpm.addPass(llvm::PromotePass());
        fpm.addPass(llvm::InstSimplifyPass());
        fpm.addPass(llvm::SimplifyCFGPass());
        fpm.addPass(llvm::DCEPass());
        mpm.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));
        break;
    }
    case Tier::W: {
        // Section 9.3, tier W: LLVM's O1 function pipeline, one function at a time, without the passes that do not pay for themselves on this
        // IR (README.md, "Phase 5d", measured pass by pass on the 4,000-function benchmark, its text and the run time of int_ops, the calls
        // programs and the benchmark's native run): mem2reg in place of SROA (whole-register allocas); EarlyCSE, SimplifyCFG, one InstCombine,
        // SimplifyCFG, SCCP, BDCE, SimplifyCFG. Left out: the second SROA, MemCpyOpt, LibCallsShrinkWrap, CoroElide and ADCE (nothing to do:
        // the pack the same byte for byte), the other three InstCombines (0.01 % of the text, four tenths of the passes' time), Reassociate
        // and the loop passes (LICM, LoopRotate, SimpleLoopUnswitch, LoopIdiomRecognize, IndVarSimplify, LoopDeletion, LoopFullUnroll: the
        // guest's memory accesses are volatile, so they move nothing that matters; without them the text is 0.6 % smaller and int_ops as
        // fast). Phase 5b ran the O1 function pipeline whole, phase 5a the O1 module pipeline
        const auto cfg = llvm::SimplifyCFGOptions().convertSwitchRangeToICmp(true);  // as O1's
        llvm::FunctionPassManager fpm;
        fpm.addPass(llvm::PromotePass());
        fpm.addPass(llvm::EarlyCSEPass(/*UseMemorySSA=*/true));
        fpm.addPass(llvm::SimplifyCFGPass(cfg));
        fpm.addPass(llvm::InstCombinePass());
        fpm.addPass(llvm::SimplifyCFGPass(cfg));
        fpm.addPass(llvm::SCCPPass());
        fpm.addPass(llvm::BDCEPass());
        fpm.addPass(llvm::SimplifyCFGPass(cfg));
        mpm.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));
        break;
    }
    case Tier::H:
        mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
        break;
    case Tier::HPlus:
        mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
        break;
    }
    mpm.run(m, mam);
    if (tier == Tier::C) {
        // What FastISel then selects itself (README.md, "Phase 5d")
        std::map<llvm::Constant*, llvm::GlobalVariable*> pool;
        for (llvm::Function& f : m) {
            if (f.isDeclaration()) continue;
            SplitCommonReturns(f);
            ScalarizeStructPhis(f);
            SplitReturnBlocks(f);
            LaneMasks(f);
            ExpandFPToUI(f);
            PlainAtomicLoads(f);
            LoadVectorConstants(f, pool);
        }
    }
}

// The module descriptor's layout (include/nxaot/aot_module.h), field by field
llvm::StructType* DescType(llvm::LLVMContext& c) {
    auto* i32 = llvm::Type::getInt32Ty(c);
    auto* i64 = llvm::Type::getInt64Ty(c);
    auto* ptr = llvm::PointerType::get(c, 0);
    auto* name = llvm::ArrayType::get(llvm::Type::getInt8Ty(c), 32);
    return llvm::StructType::get(c, {i32, i32, name, name, i64, i64, i64, i64, i32, i32, i32, i32, ptr, ptr, ptr, i64, ptr, ptr, ptr, ptr, ptr, i64, ptr,
                                     i32, i32, ptr, i64});
}

// The gates of design 1.1 the module's manifest asks for (its obs and spin rows), include/nxaot/aot_module.h
struct Gates {
    std::map<uint64_t, uint64_t> obs;   // observed instruction -> the number | parameter << 8
    std::map<uint64_t, uint64_t> spin;  // back-edge branch -> the head
    bool Empty() const { return obs.empty() && spin.empty(); }
};

Gates GatesOf(const module::Image& image) {
    Gates g;
    if (!image.manifest) {
        return g;
    }
    for (const auto& o : image.manifest->obs) {
        g.obs[o.offset] = o.number | o.parameter << 8;
    }
    for (const auto& s : image.manifest->spin) {
        for (const uint64_t e : s.back_edges) {
            g.spin[e] = s.head;
        }
    }
    return g;
}

// The gate sites in a function's code (the blocks discovery's walk reached, the aux region's among them), sorted by offset: the gates its compiled
// code calls. An observation gate whose instruction a block holds; a spin wait's back-edge that ends a block (a branch ends its block)
std::vector<nxaot_gate_site> GateSitesOf(const analysis::Function& f, const Gates& g) {
    std::vector<nxaot_gate_site> out;
    for (const auto& [start, b] : f.blocks) {
        for (auto it = g.obs.lower_bound(b.start); it != g.obs.end() && it->first < b.end; ++it) {
            out.push_back({static_cast<uint32_t>(it->first), NXAOT_GATE_OBS, it->second});
        }
        if (b.end >= 4) {
            if (const auto it = g.spin.find(b.end - 4); it != g.spin.end()) {
                out.push_back({static_cast<uint32_t>(it->first), NXAOT_GATE_SPIN, it->second});
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const nxaot_gate_site& a, const nxaot_gate_site& b) { return a.offset < b.offset; });
    return out;
}

// ---- Functions split for size (design 9.3) ----
// LLVM's compile time grows faster than linearly with a function's size, and one unit's time bounds the pack's wall time however many workers
// there are (switch-gcc's sw_word, 44,059 instructions, was 4.5 of the corpus pack's 4.7 s with 8 workers: README.md, "Pack throughput"). A
// function of more than kSplitAbove instructions becomes parts: its blocks in address order, cut where the fewest of its edges cross, each part
// of kPartMin to kPartMax instructions. Each part is an LLVM function of its own and a compile unit of its own; the function's body is a stub
// that stores what it was passed to the state block and tail-calls the part that holds its entry; a transfer between parts stores the dirty
// registers and tail-calls the other part (lower_calls.cpp, FnSplit): the design's "LLVM functions joined by tail calls through the state
// block". The cut points are the design's safe points: instruction boundaries outside exclusive windows (block starts, and points inside long
// straight runs), which a transfer between parts reaches with the whole register file in the block. kSplitAbove is BuildOptions::split_above
// (8,192 by default: parts of 2,048 to 4,096 instructions, some 0.2 to 0.5 s each at tier W); CTest split lowers it to split most of the corpus

struct Part {
    uint64_t lo = 0, hi = 0;         // its blocks start in [lo, hi)
    std::vector<uint64_t> entries;   // sorted: the blocks entered from outside it (lo, the function's entry, the targets of edges that cross into it,
                                     // landing pads and setjmp continuations, which a call in any part may resume at)
    uint64_t words = 0;              // instructions of its blocks
};

std::vector<Part> SplitParts(const analysis::Function& f, const FnFacts& ff, uint64_t split_above) {
    if (ff.words <= split_above || !ff.extra.empty()) {
        return {};  // small, or reaching the aux region (its gate sites are checked per function: kept whole)
    }
    const uint64_t kPartMin = std::max<uint64_t>(8, split_above / 4), kPartMax = std::max<uint64_t>(16, split_above / 2);
    // The cut candidates: block starts, and inside a block longer than kPiece instructions every kPiece-th instruction (a straight run: the
    // part before it runs into the next part, FnExit; sw_word is 19 blocks of up to thousands of instructions), never inside an exclusive
    // window (its load and store stay in one part, as the rewriter keeps them in one piece)
    const uint64_t kPiece = std::max<uint64_t>(2, std::min<uint64_t>(256, kPartMin / 4));
    struct Piece {
        uint64_t start = 0, end = 0;
        std::vector<uint64_t> succ;  // successor starts (a cut inside a block: the next piece)
    };
    const auto in_window = [&](uint64_t a) {
        return std::any_of(f.windows.begin(), f.windows.end(), [a](const analysis::Window& w) { return a > w.lo && a < w.hi; });
    };
    std::vector<Piece> bl;
    std::map<uint64_t, size_t> index;
    for (const auto& [start, b] : f.blocks) {
        if (start < f.entry || start >= ff.limit) continue;
        uint64_t at = b.start;
        while (true) {
            uint64_t next = at + 4 * kPiece;
            while (next < b.end && in_window(next)) next += 4;
            Piece p;
            p.start = at;
            p.end = std::min(next, b.end);
            if (p.end < b.end) {
                p.succ.push_back(p.end);
            } else {
                for (const auto& [t, kind] : b.succ) p.succ.push_back(t);
            }
            index[p.start] = bl.size();
            bl.push_back(std::move(p));
            if (next >= b.end) break;
            at = next;
        }
    }
    const size_t n = bl.size();
    // crossing[k]: the edges between pieces of the function that cross the cut before piece k (one end before it, the other at or after it)
    std::vector<int64_t> crossing(n + 1, 0);
    for (size_t i = 0; i < n; ++i) {
        for (const uint64_t t : bl[i].succ) {
            const auto it = index.find(t);
            if (it == index.end() || it->second == i) continue;
            const size_t lo = std::min(i, it->second), hi = std::max(i, it->second);
            ++crossing[lo + 1];
            --crossing[hi + 1];
        }
    }
    for (size_t k = 1; k <= n; ++k) crossing[k] += crossing[k - 1];
    std::vector<uint64_t> words(n);
    uint64_t total = 0;
    for (size_t i = 0; i < n; ++i) total += (words[i] = (bl[i].end - bl[i].start) / 4);
    // Cuts: from each part's start, the block at which it holds at least kPartMin instructions and at most kPartMax with the fewest crossing
    // edges (the first of equals); none that leaves less than kPartMin after it
    std::vector<size_t> cuts{0};
    uint64_t done = 0;
    for (size_t s = 0; s < n;) {
        uint64_t acc = 0;
        size_t best = n;
        int64_t best_cross = INT64_MAX;
        for (size_t k = s; k < n; ++k) {
            acc += words[k];
            if (acc > kPartMax && best != n) break;
            if (acc >= kPartMin && k + 1 < n && total - done - acc >= kPartMin && crossing[k + 1] < best_cross && !in_window(bl[k + 1].start)) {
                best = k + 1;
                best_cross = crossing[k + 1];
            }
        }
        if (best == n) break;
        for (size_t k = s; k < best; ++k) done += words[k];
        cuts.push_back(best);
        s = best;
    }
    if (cuts.size() < 2) {
        return {};
    }
    std::vector<Part> parts;
    for (size_t c = 0; c < cuts.size(); ++c) {
        Part p;
        p.lo = c == 0 ? f.entry : bl[cuts[c]].start;
        p.hi = c + 1 < cuts.size() ? bl[cuts[c + 1]].start : ff.limit;
        const size_t end = c + 1 < cuts.size() ? cuts[c + 1] : n;
        for (size_t k = cuts[c]; k < end; ++k) p.words += words[k];
        parts.push_back(p);
    }
    const auto part_of = [&](uint64_t a) -> Part* {
        for (Part& p : parts) {
            if (a >= p.lo && a < p.hi) return &p;
        }
        return nullptr;
    };
    for (Part& p : parts) p.entries.push_back(p.lo);
    part_of(f.entry)->entries.push_back(f.entry);
    for (size_t i = 0; i < n; ++i) {
        for (const uint64_t t : bl[i].succ) {
            Part* to = part_of(t);
            if (to && to != part_of(bl[i].start) && index.count(t)) to->entries.push_back(t);
        }
    }
    for (const uint64_t r : ff.roots) {
        if (Part* p = part_of(r)) p->entries.push_back(r);
    }
    for (const uint64_t r : ff.continuations) {
        if (Part* p = part_of(r)) p->entries.push_back(r);
    }
    for (const auto& [call, pad] : ff.pads) {
        if (Part* p = part_of(pad)) p->entries.push_back(pad);
    }
    for (Part& p : parts) {
        std::sort(p.entries.begin(), p.entries.end());
        p.entries.erase(std::unique(p.entries.begin(), p.entries.end()), p.entries.end());
    }
    return parts;
}

// ---- What every module of a pack shares ----

struct Common {
    const module::Image& image;
    const BuildOptions& o;
    const analysis::Discovery& d;
    const Plan& plan;
    lift::Convention conv;
    std::string prefix;  // the module's symbols are <prefix>+0x<offset>
    bool fpsr = false;
    std::string triple, cpu, features;
    lift::Target target;
    std::string tool;    // the toolchain's identity for the cache keys: nxaot.lock, LLVM, this executable's SHA-256
    Gates gates;                          // the manifest's gate rows
    std::vector<nxaot_gate_site> served;  // the gate sites compiled code calls (some function's code holds them), sorted: the descriptor's table
    std::map<uint64_t, std::vector<Part>> parts;  // the functions split for size (SplitParts), by entry
    std::map<uint64_t, Tier> tiers;               // each function's tier when a profile chose them (TiersFromProfile); else the mode's
    // What the compile reads of discovery's functions, taken before it starts (BuildObjects), after which they are freed (README.md, "Memory at
    // scale"): the gate sites of the functions that have some (GateSitesOf), and the return addresses of every call site, sorted, each once
    std::map<uint64_t, std::vector<nxaot_gate_site>> sites;
    std::vector<uint32_t> returns;

    std::vector<nxaot_gate_site> SitesOf(uint64_t entry) const {
        const auto it = sites.find(entry);
        return it == sites.end() ? std::vector<nxaot_gate_site>{} : it->second;
    }
    Tier TierOf(uint64_t entry) const {
        const auto it = tiers.find(entry);
        return it == tiers.end() ? o.tier : it->second;
    }
    // The part of the split function fn that has a block of its own at offset among its entries, or null
    const Part* PartAt(uint64_t fn, uint64_t offset) const {
        const auto it = parts.find(fn);
        if (it == parts.end()) return nullptr;
        for (const Part& p : it->second) {
            if (offset >= p.lo && offset < p.hi) {
                return std::binary_search(p.entries.begin(), p.entries.end(), offset) ? &p : nullptr;
            }
        }
        return nullptr;
    }
};

// The counts of one unit (or of the tables' object), which a pack sums and the cache keeps beside each unit's object
struct UnitStats {
    uint64_t blocks = 0, calls = 0, indirect_calls = 0, tail_calls = 0, indirect_tails = 0, returns = 0, svcs = 0, traps = 0, refused = 0;
    uint64_t lines_lost = 0, spills = 0, reloads = 0, text_bytes = 0, object_bytes = 0, gates = 0;
    uint64_t splits = 0;  // transfers between the parts of a split function
    double t_lift = 0, t_verify = 0, t_opt = 0, t_codegen = 0;
    std::vector<std::string> notes;
    // --isel-misses: FastISel's misses by kind (IselMissKind): how many, and the IR instructions they handed to SelectionDAG; not cached
    std::map<std::string, std::pair<uint64_t, uint64_t>> isel_misses;

    cache::Facts ToFacts() const {
        cache::Facts f;
        f.counts = {{"blocks", blocks}, {"calls", calls}, {"indirect_calls", indirect_calls}, {"tail_calls", tail_calls},
                    {"indirect_tails", indirect_tails}, {"returns", returns}, {"svcs", svcs}, {"traps", traps}, {"refused", refused},
                    {"lines_lost", lines_lost}, {"text_bytes", text_bytes}, {"object_bytes", object_bytes}, {"gates", gates},
                    {"splits", splits}};
        f.notes = notes;
        return f;
    }
    static UnitStats FromFacts(const cache::Facts& f) {
        UnitStats u;
        const auto get = [&](const char* k) {
            const auto it = f.counts.find(k);
            return it == f.counts.end() ? uint64_t{0} : it->second;
        };
        u.blocks = get("blocks");
        u.calls = get("calls");
        u.indirect_calls = get("indirect_calls");
        u.tail_calls = get("tail_calls");
        u.indirect_tails = get("indirect_tails");
        u.returns = get("returns");
        u.svcs = get("svcs");
        u.traps = get("traps");
        u.refused = get("refused");
        u.lines_lost = get("lines_lost");
        u.text_bytes = get("text_bytes");
        u.object_bytes = get("object_bytes");
        u.gates = get("gates");
        u.splits = get("splits");
        u.notes = f.notes;
        return u;
    }
};

// ---- The line table (design 3.5; include/nxaot/aot_module.h) ----
// Each unit marks its guest memory accesses with !pcsections, into the section kLineMarks of its object. For each metadata node LLVM writes the
// native address of every machine instruction that carries it (each a 32-bit pc-relative relocation to the code) and then the node's data once,
// here the guest offset (AsmPrinter::emitPCSections). Several machine instructions share a node whenever LLVM splits or duplicates an access (a
// guest ldp, dc zva's eight stores, a loop rotated or unrolled), so the section is no array of {native, offset} pairs, which the readers of ABI 2
// took it for: in the corpus 246 of fnptr-gcc's 415 entries and 8,812 of switch-gcc's 24,293 named an address outside the code or an offset
// outside the text. Its words are told apart by their relocations, which only the unit's object has: ReadLineMarks reads them after code
// generation (and from a cached object), attributes each address to the function whose symbol holds it, and MarkExcluded sets SHF_EXCLUDE on the
// section, which the link then drops. The pack's table is nxaot's own (lines.o: CompileLines), a run per function of {native - body, offset}
struct LineRow {
    uint64_t fn = 0;              // the module offset of the function whose body holds the native address
    uint32_t native = 0, offset = 0;  // bytes from the start of that body, and the guest instruction's module offset
    uint64_t part = kNoPart;      // a part of a split function (<body>.part<lo>): native counts from the part's start until the parts are joined
    static constexpr uint64_t kNoPart = UINT64_MAX;
};

// Every unit's line rows by the function whose body holds them, {native, offset}, in link order (BuildObjects takes each unit's rows into it and
// frees them: a vector of every LineRow of the pack beside them was 24 to 48 bytes per row, README.md "Memory at scale"); CompileLines sorts each
// function's run and consumes them
using LineRuns = std::map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>>;
void AddLineRows(LineRuns& runs, const std::vector<LineRow>& rows) {
    for (const LineRow& r : rows) runs[r.fn].emplace_back(r.native, r.offset);
}

// The function symbols of a unit's code that are bodies (<prefix>+0x<offset>), standard entries (<body>.std) or parts of split functions
// (<body>.part<lo>, *part = lo): the module offset of the body
std::optional<uint64_t> BodyOf(llvm::StringRef name, const std::string& prefix, bool* std_entry, uint64_t* part = nullptr) {
    const std::string head = prefix + "+0x";
    if (!name.starts_with(head)) {
        return std::nullopt;
    }
    name = name.drop_front(head.size());
    *std_entry = name.consume_back(".std");
    if (part) *part = LineRow::kNoPart;
    if (const size_t dot = name.find(".part"); dot != llvm::StringRef::npos) {
        uint64_t lo = 0;
        if (!part || name.drop_front(dot + 5).getAsInteger(16, lo)) {
            return std::nullopt;
        }
        *part = lo;
        name = name.take_front(dot);
    }
    uint64_t v = 0;
    if (name.getAsInteger(16, v)) {
        return std::nullopt;
    }
    return v;
}

bool ReadLineMarks(llvm::StringRef object, const std::string& prefix, std::vector<LineRow>* rows, uint64_t* lost, std::string* error) {
    auto file = llvm::object::ObjectFile::createObjectFile(llvm::MemoryBufferRef(object, "unit"));
    if (!file) {
        *error = "the line marks: " + llvm::toString(file.takeError());
        return false;
    }
    const auto* elf = llvm::dyn_cast<llvm::object::ELF64LEObjectFile>(file->get());
    if (!elf) {
        *error = "the line marks: the unit's object is not ELF64";
        return false;
    }
    std::optional<llvm::object::SectionRef> marks;
    for (const llvm::object::SectionRef& s : elf->sections()) {
        auto name = s.getName();
        if (!name) {
            llvm::consumeError(name.takeError());
            continue;
        }
        if (*name == kLineMarks) {
            marks = s;
            break;
        }
    }
    if (!marks) {
        return true;  // no guest memory access in the unit
    }
    auto data = marks->getContents();
    if (!data) {
        *error = "the line marks: " + llvm::toString(data.takeError());
        return false;
    }
    // The section's relocations: the words that are native addresses, each the code-section offset it names (S + A of R_X86_64_PC32: the
    // difference to the word itself, as emitLabelDifference writes it)
    std::map<uint64_t, uint64_t> address_at;
    std::optional<llvm::object::SectionRef> code;
    for (const llvm::object::SectionRef& rs : elf->sections()) {
        auto target = rs.getRelocatedSection();
        if (!target) {
            llvm::consumeError(target.takeError());
            continue;
        }
        if (*target == elf->section_end() || **target != *marks) {
            continue;
        }
        for (const llvm::object::RelocationRef& r : rs.relocations()) {
            const llvm::object::ELFRelocationRef er{r};
            auto addend = er.getAddend();
            const llvm::object::symbol_iterator sym = r.getSymbol();
            if (!addend || sym == elf->symbol_end() || r.getType() != llvm::ELF::R_X86_64_PC32) {
                if (!addend) llvm::consumeError(addend.takeError());
                *error = "the line marks: a relocation that is no R_X86_64_PC32 to the code";
                return false;
            }
            auto value = sym->getValue();
            auto sec = sym->getSection();
            if (!value || !sec || *sec == elf->section_end()) {
                if (!value) llvm::consumeError(value.takeError());
                if (!sec) llvm::consumeError(sec.takeError());
                *error = "the line marks: a relocation to no section";
                return false;
            }
            if (code && **sec != *code) {
                *error = "the line marks name more than one code section";
                return false;
            }
            code = **sec;
            address_at[r.getOffset()] = *value + static_cast<uint64_t>(*addend);
        }
    }
    if (!code) {
        return true;
    }
    // The functions of that section by address: bodies (their rows) and standard entries (no guest access of their own)
    struct Fn {
        uint64_t lo = 0, hi = 0, body = 0, part = LineRow::kNoPart;
        bool std_entry = false;
    };
    std::vector<Fn> fns;
    for (const llvm::object::ELFSymbolRef s : elf->symbols()) {
        if (s.getELFType() != llvm::ELF::STT_FUNC) continue;
        auto sec = s.getSection();
        auto name = s.getName();
        auto value = s.getValue();
        if (!sec || !name || !value) {
            if (!sec) llvm::consumeError(sec.takeError());
            if (!name) llvm::consumeError(name.takeError());
            if (!value) llvm::consumeError(value.takeError());
            continue;
        }
        if (*sec == elf->section_end() || **sec != *code) continue;
        Fn f;
        if (const auto body = BodyOf(*name, prefix, &f.std_entry, &f.part)) {
            f.lo = *value;
            f.hi = *value + s.getSize();
            f.body = *body;
            fns.push_back(f);
        }
    }
    std::sort(fns.begin(), fns.end(), [](const Fn& a, const Fn& b) { return a.lo < b.lo; });
    const llvm::StringRef bytes = *data;
    std::vector<uint64_t> group;
    for (uint64_t p = 0; p + 4 <= bytes.size(); p += 4) {
        if (const auto it = address_at.find(p); it != address_at.end()) {
            group.push_back(it->second);
            continue;
        }
        uint32_t offset = 0;
        std::memcpy(&offset, bytes.data() + p, 4);
        if (group.empty()) {
            *error = "the line marks: a guest offset with no native address before it at +" + std::to_string(p);
            return false;
        }
        for (const uint64_t native : group) {
            auto f = std::upper_bound(fns.begin(), fns.end(), native, [](uint64_t a, const Fn& g) { return a < g.lo; });
            if (f == fns.begin() || native >= std::prev(f)->hi || std::prev(f)->std_entry) {
                ++*lost;  // outside every body (none expected)
                continue;
            }
            --f;
            rows->push_back({f->body, static_cast<uint32_t>(native - f->lo), offset, f->part});
        }
        group.clear();
    }
    if (!group.empty() || bytes.size() % 4) {
        *error = "the line marks end in native addresses with no guest offset after them";
        return false;
    }
    return true;
}

// Sets SHF_EXCLUDE on the line marks' section of an ELF64 object in place, so that the link drops it (lld discards SHF_EXCLUDE input sections,
// and the relocations that apply to them); nothing else of the object changes
bool MarkExcluded(std::string* object, std::string* error) {
    const auto u16 = [&](size_t at) { uint16_t v; std::memcpy(&v, object->data() + at, 2); return v; };
    const auto u32 = [&](size_t at) { uint32_t v; std::memcpy(&v, object->data() + at, 4); return v; };
    const auto u64 = [&](size_t at) { uint64_t v; std::memcpy(&v, object->data() + at, 8); return v; };
    if (object->size() < 64 || std::memcmp(object->data(), "\x7f" "ELF\x02\x01", 6) != 0) {
        *error = "the unit's object is not ELF64 little-endian";
        return false;
    }
    const uint64_t shoff = u64(0x28);
    const uint16_t shentsize = u16(0x3A), shnum = u16(0x3C), shstrndx = u16(0x3E);
    if (shentsize < 64 || shstrndx >= shnum || shoff + static_cast<uint64_t>(shnum) * shentsize > object->size()) {
        *error = "the unit's object has no section header table";
        return false;
    }
    const uint64_t strtab = u64(shoff + static_cast<uint64_t>(shstrndx) * shentsize + 0x18);
    for (uint16_t i = 0; i < shnum; ++i) {
        const uint64_t h = shoff + static_cast<uint64_t>(i) * shentsize;
        const uint64_t name = strtab + u32(h);
        if (name < object->size() && std::strncmp(object->data() + name, kLineMarks, object->size() - name) == 0) {
            const uint64_t flags = u64(h + 8) | llvm::ELF::SHF_EXCLUDE;
            std::memcpy(object->data() + h + 8, &flags, 8);
        }
    }
    return true;
}

// What a module (the tables' object or a unit) holds of the run time and of the code every unit shares. The tables' object defines the
// module's base, the empty inline-cache descriptor, the slow-path wrappers and jit_entry; a unit declares them. Every name is hidden: lld
// resolves them inside the pack's shared object, which exports only the descriptor
struct Kit {
    llvm::Module* m = nullptr;
    llvm::GlobalVariable* base = nullptr;
    llvm::GlobalVariable* ic_empty = nullptr;
    llvm::Function *svc = nullptr, *trap = nullptr, *dispatch = nullptr, *counter = nullptr, *rt_jit = nullptr, *safepoint = nullptr;
    llvm::Function* gate = nullptr;  // nxaot_rt_gate's preserve_all wrapper, in a module with gate sites only (design 1.1)
    llvm::Function* jit_entry = nullptr;
    llvm::FunctionType* std_ty = nullptr;  // the standard entries' one signature (section 2.3)
    // Debug information: a synthetic file per module, line = guest offset (section 9.2)
    std::unique_ptr<llvm::DIBuilder> dib;
    llvm::DIFile* file = nullptr;
    llvm::DISubroutineType* sub_ty = nullptr;
    bool optimized = true;

    llvm::DISubprogram* Subprogram(llvm::Function* f, uint64_t line) const {
        if (!dib) {
            return nullptr;
        }
        auto* sp = dib->createFunction(file, f->getName(), f->getName(), file, static_cast<unsigned>(line), sub_ty, static_cast<unsigned>(line),
                                       llvm::DINode::FlagZero,
                                       llvm::DISubprogram::SPFlagDefinition | (optimized ? llvm::DISubprogram::SPFlagOptimized
                                                                                         : llvm::DISubprogram::SPFlagZero));
        f->setSubprogram(sp);
        return sp;
    }
};

void Hide(llvm::GlobalValue* g) {
    g->setVisibility(llvm::GlobalValue::HiddenVisibility);
    g->setDSOLocal(true);
}

// What a FastISel miss was (--isel-misses; README.md, "Phase 5d"): the remark's kind and the IR it names, without the operands. SelectionDAGISel
// reports "FastISel missed call: <call>" (the call alone goes to SelectionDAG), "FastISel missed terminator: <terminator>" and "FastISel missed:
// <instruction>" (the block up to and including it goes to SelectionDAG), and "FastISel didn't lower all arguments: <prototype>" (the
// arguments through SelectionDAG)
std::string IselMissKind(llvm::StringRef msg) {
    std::string kind;
    if (msg.consume_front("FastISel missed call")) {
        kind = "call";
    } else if (msg.consume_front("FastISel missed terminator")) {
        kind = "terminator";
    } else if (msg.consume_front("FastISel missed")) {
        kind = "instruction";
    } else if (msg.consume_front("FastISel didn't lower all arguments")) {
        return "arguments";
    } else {
        return "";
    }
    msg = msg.ltrim(": ");
    if (const size_t in = msg.find(" (in function:"); in != llvm::StringRef::npos) msg = msg.take_front(in);
    if (const size_t eq = msg.find(" = "); eq != llvm::StringRef::npos) msg = msg.drop_front(eq + 3);
    msg = msg.trim();
    // The opcode and its qualifiers, then for a call its convention and callee, for the others the type of the result or first operand
    llvm::SmallVector<llvm::StringRef, 16> words;
    msg.split(words, ' ', -1, false);
    std::string what;
    size_t i = 0;
    for (; i < words.size(); ++i) {
        const llvm::StringRef w = words[i];
        if (w == "tail" || w == "musttail" || w == "notail" || w == "atomic" || w == "volatile" || w == "call" || w == "ret" ||
            w == "fast" || w == "nnan" || w == "nuw" || w == "nsw" || w == "exact") {
            what += (what.empty() ? "" : " ") + w.str();
            continue;
        }
        break;
    }
    if (what.empty() && i < words.size()) what = words[i++].str();
    if (what.find("call") != std::string::npos) {
        for (; i < words.size(); ++i) {
            const llvm::StringRef w = words[i];
            if (w.ends_with("cc") || w.starts_with("cc")) what += " " + w.str();
            if (const size_t at = w.find('@'); at != llvm::StringRef::npos) {
                llvm::StringRef callee = w.drop_front(at).take_until([](char c) { return c == '('; });
                if (callee.starts_with("@llvm.")) {
                    // An intrinsic without its type suffixes (llvm.fshr.i64 -> llvm.fshr)
                    llvm::SmallVector<llvm::StringRef, 4> parts;
                    callee.drop_front(1).split(parts, '.');
                    std::string name;
                    for (const llvm::StringRef p : parts) {
                        if (!name.empty() && (p.starts_with("i") || p.starts_with("v") || p.starts_with("f") || p.starts_with("p")) &&
                            p.size() > 1 && std::isdigit(static_cast<unsigned char>(p[1]))) {
                            break;
                        }
                        name += (name.empty() ? "" : ".") + p.str();
                    }
                    what += " @" + name;
                } else if (callee.starts_with("@nxaot.")) {
                    what += " " + callee.str();
                } else {
                    what += " @<guest function>";
                }
                break;
            }
            if (w.starts_with("%")) {
                what += " <indirect>";
                break;
            }
        }
    } else if (i < words.size()) {
        // The type: a vector type spans several words (<2 x i64>)
        std::string type = words[i].str();
        if (type.starts_with("<")) {
            for (size_t j = i + 1; j < words.size() && type.back() != '>' && type.find(">,") == std::string::npos; ++j) type += " " + words[j].str();
        }
        while (!type.empty() && type.back() == ',') type.pop_back();
        what += " " + type;
    }
    return kind + ": " + what;
}

// The diagnostics of an LLVM context: a guest memory access folded into another instruction keeps no !pcsections entry ("loosing !pcsections
// metadata"); those are counted (the line table then maps that native address through its function), any other warning or error is kept as a
// note. Remarks are ignored (SelectionDAGISel emits one for every FastISel miss) unless --isel-misses counts them (IselMissKind)
struct UnitDiagnostics final : llvm::DiagnosticHandler {
    UnitStats* st;
    bool isel;
    UnitDiagnostics(UnitStats* s, bool isel_misses) : st{s}, isel{isel_misses} {}
    bool isMissedOptRemarkEnabled(llvm::StringRef pass) const override { return isel && pass == "sdagisel"; }
    bool handleDiagnostics(const llvm::DiagnosticInfo& di) override {
        const llvm::DiagnosticSeverity sev = di.getSeverity();
        if (const auto* r = llvm::dyn_cast<llvm::DiagnosticInfoOptimizationBase>(&di)) {
            if (isel && r->getPassName() == "sdagisel") {
                const std::string msg = r->getMsg();
                const std::string kind = IselMissKind(msg);
                if (!kind.empty()) {
                    // The IR instructions the miss hands to SelectionDAG: a call alone; a terminator its whole block; another instruction the
                    // block up to it (the remark names it by its text, the bottom-most one of that text, as FastISel works bottom-up); the
                    // arguments as one
                    const auto* ir = llvm::dyn_cast<llvm::DiagnosticInfoIROptimization>(r);
                    const auto* bb = ir ? llvm::dyn_cast_or_null<llvm::BasicBlock>(ir->getCodeRegion()) : nullptr;
                    uint64_t weight = 1;
                    if (bb && llvm::StringRef(kind).starts_with("terminator")) {
                        weight = bb->size();
                    } else if (bb && llvm::StringRef(kind).starts_with("instruction")) {
                        llvm::StringRef want = msg;
                        want.consume_front("FastISel missed: ");
                        if (const size_t in = want.find(" (in function:"); in != llvm::StringRef::npos) want = want.take_front(in);
                        want = want.trim();
                        uint64_t pos = 0, found = 0;
                        for (const llvm::Instruction& i : *bb) {
                            ++pos;
                            std::string text;
                            llvm::raw_string_ostream out{text};
                            i.print(out);
                            if (llvm::StringRef(out.str()).trim() == want) found = pos;
                        }
                        weight = found ? found : bb->size();
                    }
                    auto& [count, to_dag] = st->isel_misses[kind];
                    ++count;
                    to_dag += weight;
                }
            }
            return true;
        }
        std::string text;
        llvm::raw_string_ostream out{text};
        llvm::DiagnosticPrinterRawOStream printer{out};
        di.print(printer);
        if (out.str().find("pcsections") != std::string::npos) {
            ++st->lines_lost;
        } else if (sev == llvm::DS_Error || sev == llvm::DS_Warning) {
            st->notes.push_back(std::string(sev == llvm::DS_Error ? "[RED] " : "") + "nxaot build: llvm: " + out.str());
        }
        return true;
    }
};

void Diagnose(llvm::LLVMContext& ctx, UnitStats* st, bool isel_misses = false) {
    ctx.setDiagnosticHandler(std::make_unique<UnitDiagnostics>(st, isel_misses));
}

Kit MakeKit(llvm::Module& m, const Common& c, bool tables, Tier tier) {
    llvm::LLVMContext& ctx = m.getContext();
    llvm::IRBuilder<> b{ctx};
    auto* i32 = b.getInt32Ty();
    auto* i64 = b.getInt64Ty();
    auto* ptr = b.getPtrTy();
    auto* v128 = llvm::FixedVectorType::get(i64, 2);
    const BuildOptions& o = c.o;
    Kit k;
    k.m = &m;
    k.optimized = tier != Tier::C;

    // ---- Debug information ----
    if (o.debug_info) {
        k.dib = std::make_unique<llvm::DIBuilder>(m);
        k.file = k.dib->createFile(c.prefix + ".s", ".");
        k.dib->createCompileUnit(llvm::dwarf::DW_LANG_C, k.file, std::string("nxaot (LLVM ") + NXAOT_LOCK_LLVM + ")", k.optimized, "", 0);
        k.sub_ty = k.dib->createSubroutineType(k.dib->getOrCreateTypeArray({}));
        m.addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
        m.addModuleFlag(llvm::Module::Warning, "Dwarf Version", 5);
    }

    // ---- The module's base, the empty inline-cache descriptor ----
    k.base = new llvm::GlobalVariable(m, i64, false, llvm::GlobalValue::ExternalLinkage, tables ? b.getInt64(0) : nullptr, "nxaot.base");
    k.base->setAlignment(llvm::Align(8));
    Hide(k.base);
    auto* ic_desc_ty = llvm::StructType::get(ctx, {i64, ptr});
    k.ic_empty = new llvm::GlobalVariable(
        m, ic_desc_ty, true, llvm::GlobalValue::ExternalLinkage,
        tables ? llvm::ConstantStruct::get(ic_desc_ty, {b.getInt64(UINT64_MAX), llvm::ConstantPointerNull::get(ptr)}) : nullptr, "nxaot.ic_empty");
    k.ic_empty->setAlignment(llvm::Align(16));
    Hide(k.ic_empty);

    // ---- The run time's entry points, and the slow paths through preserve_all / preserve_most wrappers (section 2.3: the call sites keep
    // their registers, the wrapper saves them) ----
    const auto extern_fn = [&](const char* name, llvm::FunctionType* ty) {
        auto* f = llvm::Function::Create(ty, llvm::GlobalValue::ExternalLinkage, name, m);
        f->addFnAttr(llvm::Attribute::NoUnwind);
        return f;
    };
    k.svc = extern_fn("dm_aot_svc", llvm::FunctionType::get(b.getVoidTy(), {ptr, i32}, false));
    k.trap = extern_fn("nxaot_rt_trap", llvm::FunctionType::get(b.getVoidTy(), {ptr, i64, i32}, false));
    k.trap->setDoesNotReturn();
    k.trap->addFnAttr(llvm::Attribute::Cold);
    const auto wrapper = [&](const char* name, llvm::FunctionType* ty, llvm::Function* target, unsigned cc) {
        auto* w = llvm::Function::Create(ty, llvm::GlobalValue::ExternalLinkage, name, m);
        Hide(w);
        w->setCallingConv(cc);
        w->addFnAttr(llvm::Attribute::NoUnwind);
        w->addFnAttr(llvm::Attribute::NoInline);
        w->addFnAttr(llvm::Attribute::Cold);
        if (target) {
            llvm::IRBuilder<> wb{llvm::BasicBlock::Create(ctx, "entry", w)};
            std::vector<llvm::Value*> args;
            for (auto& a : w->args()) {
                args.push_back(&a);
            }
            auto* call = wb.CreateCall(target, args);
            if (call->getType()->isVoidTy()) {
                wb.CreateRetVoid();
            } else {
                wb.CreateRet(call);
            }
        }
        return w;
    };
    auto* dispatch_ty = llvm::FunctionType::get(ptr, {ptr, i64, ptr, i64}, false);
    auto* counter_ty = llvm::FunctionType::get(i64, {ptr}, false);
    auto* safepoint_ty = llvm::FunctionType::get(b.getVoidTy(), {ptr}, false);
    k.dispatch = wrapper("nxaot.dispatch", dispatch_ty, tables ? extern_fn("nxaot_rt_dispatch", dispatch_ty) : nullptr,
                         llvm::CallingConv::PreserveAll);
    k.counter = wrapper("nxaot.cntpct", counter_ty, tables ? extern_fn("dm_aot_cntpct", counter_ty) : nullptr, llvm::CallingConv::PreserveMost);
    k.counter->removeFnAttr(llvm::Attribute::Cold);
    // Section 11: the JIT fallback, a C call from the code that hands it a continuation (and from jit_entry, below); section 6.2: the
    // safepoint, through a preserve_all wrapper so that a poll's cold path saves nothing it does not store itself
    k.rt_jit = extern_fn("nxaot_rt_jit", llvm::FunctionType::get(b.getVoidTy(), {ptr, i64, i64}, false));
    k.rt_jit->addFnAttr(llvm::Attribute::Cold);
    k.safepoint = wrapper("nxaot.safepoint", safepoint_ty, tables ? extern_fn("nxaot_rt_safepoint", safepoint_ty) : nullptr,
                          llvm::CallingConv::PreserveAll);
    // Design 1.1: the gates (observation gates, spin waits' back-edges), through a preserve_all wrapper: the code stored every register the
    // block lacks before the call, and keeps the rest of its host registers across it. Only a module with gate sites imports nxaot_rt_gate.
    // Not cold: an observation gate runs at every pass of its instruction, a spin wait's at every pass of its loop
    if (!c.served.empty()) {
        auto* gate_ty = llvm::FunctionType::get(b.getVoidTy(), {ptr, i64}, false);
        k.gate = wrapper("nxaot.gate", gate_ty, tables ? extern_fn("nxaot_rt_gate", gate_ty) : nullptr, llvm::CallingConv::PreserveAll);
        k.gate->removeFnAttr(llvm::Attribute::Cold);
    }

    // ---- jit_entry (include/nxaot/aot_module.h): the JIT fallback with the standard entries' signature ----
    // What it is passed to the block (the caller stored the rest: the block then holds the whole register file), the JIT from st->pc until
    // the guest returns to lr, then x0, x1 and the standard entries' RV from the block (poison-equivalent when st->pending is set: the caller
    // propagates the transfer before it looks at them)
    const lift::Regs std_pass = c.plan.indirect_pass;
    const lift::Convention& conv = c.conv;
    k.std_ty = conv.Type(ctx, std_pass);
    k.jit_entry = llvm::Function::Create(k.std_ty, llvm::GlobalValue::ExternalLinkage, "nxaot.jit_entry", m);
    Hide(k.jit_entry);
    conv.Apply(k.jit_entry);
    k.jit_entry->addFnAttr(llvm::Attribute::NoUnwind);
    k.jit_entry->addFnAttr(llvm::Attribute::NoInline);
    k.jit_entry->addFnAttr(llvm::Attribute::Cold);
    k.jit_entry->addFnAttr("frame-pointer", "none");
    k.jit_entry->addFnAttr(llvm::Attribute::getWithUWTableKind(ctx, llvm::UWTableKind::Async));
    if (tables) {
        llvm::Function* je = k.jit_entry;
        llvm::DISubprogram* sp = k.Subprogram(je, 0);
        llvm::IRBuilder<> jb{llvm::BasicBlock::Create(ctx, "entry", je)};
        if (sp) {
            jb.SetCurrentDebugLocation(llvm::DILocation::get(ctx, 0, 0, sp));
        }
        llvm::Value* st = je->getArg(0);
        const auto field = [&](size_t off) { return jb.CreateConstInBoundsGEP1_64(b.getInt8Ty(), st, off); };
        for (unsigned i = 0; i < 31; ++i) {
            const int a = conv.ArgX(std_pass, i);
            if (a >= 0 && (std_pass.x >> i & 1)) {
                jb.CreateAlignedStore(je->getArg(static_cast<unsigned>(a)), field(offsetof(dm_guest_state, x) + 8 * i), llvm::Align(8));
            }
        }
        for (unsigned i = 0; i < 32; ++i) {
            const int a = conv.ArgV(std_pass, i);
            if (a >= 0 && (std_pass.v >> i & 1)) {
                jb.CreateAlignedStore(je->getArg(static_cast<unsigned>(a)), field(offsetof(dm_guest_state, v) + 16 * i), llvm::Align(16));
            }
        }
        jb.CreateAlignedStore(je->getArg(1), field(offsetof(dm_guest_state, sp)), llvm::Align(8));
        jb.CreateAlignedStore(je->getArg(2), field(offsetof(dm_guest_state, x) + 8 * 30), llvm::Align(8));
        jb.CreateCall(k.rt_jit, {st, je->getArg(2), je->getArg(1)});
        llvm::Value* r = llvm::PoisonValue::get(conv.RetType(ctx));
        r = jb.CreateInsertValue(r, jb.CreateAlignedLoad(i64, field(offsetof(dm_guest_state, x)), llvm::Align(8)), 0);
        r = jb.CreateInsertValue(r, jb.CreateAlignedLoad(i64, field(offsetof(dm_guest_state, x) + 8), llvm::Align(8)), 1);
        for (unsigned i = 0; i < 4; ++i) {
            if (c.plan.indirect_rv >> i & 1) {
                r = jb.CreateInsertValue(r, jb.CreateAlignedLoad(v128, field(offsetof(dm_guest_state, v) + 16 * i), llvm::Align(16)), 2 + i);
            }
        }
        jb.CreateRet(r);
    }
    return k;
}

std::string BodyName(const Common& c, uint64_t entry) {
    return c.prefix + "+0x" + llvm::utohexstr(entry);
}

// The body of a guest function in a module: declared (hidden) where another unit defines it, or the definition-to-be of the unit's own
llvm::Function* Body(llvm::Module& m, const Common& c, uint64_t entry, const FnFacts& ff) {
    const std::string name = BodyName(c, entry);
    if (llvm::Function* f = m.getFunction(name)) {
        return f;
    }
    auto* f = llvm::Function::Create(c.conv.Type(m.getContext(), ff.pass), llvm::GlobalValue::ExternalLinkage, name, m);
    Hide(f);
    c.conv.Apply(f);
    if (c.o.no_inline) {
        f->addFnAttr(llvm::Attribute::NoInline);  // nxaot_bench: calls stay calls, to measure the convention
    }
    return f;
}

// A part of a split function (SplitParts): <body>.part<lo>, entered with the block holding every register (lift::FunctionPlan::part_entries)
std::string PartName(const Common& c, uint64_t entry, uint64_t lo) {
    return BodyName(c, entry) + ".part" + llvm::utohexstr(lo);
}

llvm::Function* PartFn(llvm::Module& m, const Common& c, uint64_t entry, uint64_t lo) {
    const std::string name = PartName(c, entry, lo);
    if (llvm::Function* f = m.getFunction(name)) {
        return f;
    }
    auto* f = llvm::Function::Create(c.conv.Type(m.getContext(), lift::Regs{}), llvm::GlobalValue::ExternalLinkage, name, m);
    Hide(f);
    c.conv.Apply(f);
    f->addFnAttr(llvm::Attribute::NoInline);
    return f;
}

// Whether a function has a standard entry of its own (section 2.3); with ghccc a body that takes every passable register and returns the fixed
// signature's v registers is its own
bool HasStdEntry(const Common& c, const FnFacts& ff) {
    return ff.host_callable && !(c.conv.cc == lift::CallConv::Ghc && ff.pass == c.plan.indirect_pass && ff.rv == c.plan.indirect_rv);
}

// The standard entry of a host-callable function: declared, or defined (every passable register in, what the body does not take stored to the
// block, then a tail call of the body)
llvm::Function* StdEntry(const Kit& k, const Common& c, uint64_t entry, const FnFacts& ff, llvm::Function* body, bool define) {
    llvm::Module& m = *k.m;
    llvm::LLVMContext& ctx = m.getContext();
    if (!HasStdEntry(c, ff)) {
        return body;
    }
    auto* e = llvm::Function::Create(k.std_ty, llvm::GlobalValue::ExternalLinkage, body->getName() + ".std", m);
    Hide(e);
    c.conv.Apply(e);
    e->addFnAttr(llvm::Attribute::NoUnwind);
    e->addFnAttr("frame-pointer", "none");
    e->addFnAttr(llvm::Attribute::getWithUWTableKind(ctx, llvm::UWTableKind::Async));
    if (!define) {
        return e;
    }
    const lift::Regs std_pass = c.plan.indirect_pass;
    const lift::Convention& conv = c.conv;
    auto* i8 = llvm::Type::getInt8Ty(ctx);
    auto* i64 = llvm::Type::getInt64Ty(ctx);
    auto* v128 = llvm::FixedVectorType::get(i64, 2);
    llvm::DISubprogram* sp = k.Subprogram(e, entry);
    llvm::IRBuilder<> eb{llvm::BasicBlock::Create(ctx, "entry", e)};
    if (sp) {
        eb.SetCurrentDebugLocation(llvm::DILocation::get(ctx, static_cast<unsigned>(entry), 0, sp));
    }
    llvm::Value* st = e->getArg(0);
    for (unsigned i = 0; i < 31; ++i) {
        const int a = conv.ArgX(std_pass, i);
        if (a >= 0 && (std_pass.x >> i & 1) && !(ff.pass.x >> i & 1)) {
            eb.CreateAlignedStore(e->getArg(static_cast<unsigned>(a)), eb.CreateConstInBoundsGEP1_64(i8, st, offsetof(dm_guest_state, x) + 8 * i),
                                  llvm::Align(8));
        }
    }
    for (unsigned i = 0; i < 32; ++i) {
        const int a = conv.ArgV(std_pass, i);
        if (a >= 0 && (std_pass.v >> i & 1) && !(ff.pass.v >> i & 1)) {
            eb.CreateAlignedStore(e->getArg(static_cast<unsigned>(a)), eb.CreateConstInBoundsGEP1_64(i8, st, offsetof(dm_guest_state, v) + 16 * i),
                                  llvm::Align(16));
        }
    }
    std::vector<llvm::Value*> args{st, e->getArg(1), e->getArg(2)};
    llvm::FunctionType* bt = body->getFunctionType();
    args.resize(bt->getNumParams());
    for (unsigned i = 0; i < 31; ++i) {
        const int a = conv.ArgX(ff.pass, i);
        if (a >= 0) {
            args[static_cast<size_t>(a)] = (ff.pass.x >> i & 1) ? static_cast<llvm::Value*>(e->getArg(static_cast<unsigned>(conv.ArgX(std_pass, i))))
                                                                : llvm::PoisonValue::get(i64);
        }
    }
    for (unsigned i = 0; i < 32; ++i) {
        const int a = conv.ArgV(ff.pass, i);
        if (a >= 0) {
            args[static_cast<size_t>(a)] = (ff.pass.v >> i & 1) ? static_cast<llvm::Value*>(e->getArg(static_cast<unsigned>(conv.ArgV(std_pass, i))))
                                                                : llvm::PoisonValue::get(v128);
        }
    }
    auto* call = eb.CreateCall(bt, body, args);
    conv.Apply(call);
    call->setTailCallKind(llvm::CallInst::TCK_MustTail);
    call->setIsNoInline();
    eb.CreateRet(call);
    return e;
}

// ---- Windows packs (--target windows; README.md, "Windows packs"; docs/design/aot-recompiler.md 9.4) ----
// A Windows pack's code is a Linux pack's: its units are compiled for the same Linux triple, as ELF objects, with the same conventions, state
// block, inline caches, line marks, gate sites, split parts and groups. The Windows triple is not an option with LLVM 18: its swifttailcc
// assigns arguments as Win64's C convention does (four in registers, 128-bit vectors by reference) and a musttail call to a prototype with
// more stack arguments stops code generation ("Can't handle guaranteed tail call under win64 yet"), and !pcsections, the line marks, has no
// COFF section (AsmPrinter::emitPCSections switches to a null section). What differs is only where the code meets the host, which is a
// Windows program (Win64 C convention, exceptions dispatched on the faulting thread's stack):
//   - every call of a run-time entry the host exports (IsHostImport) and nxaot.enter, which the host calls, take the Win64 convention
//     (win64cc: rcx, rdx, r8, r9 and the 32-byte home area; rsi, rdi and xmm6..xmm15 preserved), the C convention of the host's compiler;
//   - no function uses the red zone: Windows writes the exception record and context of a fault below the faulting rsp, where SysV code may
//     keep values (the dirty tracker's write faults resume the faulting instruction, 5.4);
// both only in a Windows pack, whose units' cache keys say so. The objects are then joined (ld.lld -r), converted to COFF (coff.h) and linked
// by lld-link (LinkWindows). The pack's imports are not the PE loader's: a pack is loaded by nxhost-run.exe and by every test program that
// links the engine (t_aot_host.exe, t_aot_module.exe, t_aot_hostfn.exe), and a PE import names one module. So, as a Linux pack's imports
// resolve against whatever executable loads it, the pack reaches each run-time entry through a thunk (imports.o: `jmp *slot`, the PLT's
// instruction) and a slot that the host's loader binds against the executable that loads it (dm/host/aot_arena.c, the exported table
// nxaot_imports of include/nxaot/aot_module.h); a call of anything else fails the pack (CompileImports)

// The run-time entries a pack may import (include/nxaot/aot_module.h; src/lift/runtime.h for the floating-point helpers)
constexpr const char* kHostImports[] = {"dm_aot_svc",         "nxaot_rt_dispatch",  "nxaot_rt_trap",         "nxaot_rt_jit",
                                        "nxaot_rt_safepoint", "nxaot_rt_gate",      "dm_aot_cntpct",         "nxaot_rt_fp_unary",
                                        "nxaot_rt_fp_binary", "nxaot_rt_fp_muladd", "nxaot_rt_fp_round_int", "nxaot_rt_fp_to_fixed",
                                        "nxaot_rt_fp_convert", "nxaot_rt_aes"};

}  // namespace

bool IsHostImport(const std::string& name) {
    return std::find_if(std::begin(kHostImports), std::end(kHostImports), [&](const char* k) { return name == k; }) != std::end(kHostImports);
}

namespace {

// The host's boundary of a module of a Windows pack (above): the Win64 convention at the run-time entries' calls and at nxaot.enter, no red zone
void AdaptForWindows(llvm::Module& m) {
    for (llvm::Function& f : m) {
        if (f.isIntrinsic()) {
            continue;
        }
        if (!f.isDeclaration()) {
            f.addFnAttr(llvm::Attribute::NoRedZone);
            if (f.getName() == "nxaot.enter") {
                f.setCallingConv(llvm::CallingConv::Win64);  // the host's call: desc->enter(st, entry)
            }
            continue;
        }
        if (!IsHostImport(f.getName().str())) {
            continue;  // the pack's own (bodies, entries, wrappers): hidden, defined by another unit; anything else fails CompileImports
        }
        f.setCallingConv(llvm::CallingConv::Win64);
        for (llvm::User* u : f.users()) {
            if (auto* call = llvm::dyn_cast<llvm::CallBase>(u); call && call->getCalledOperand() == &f) {
                call->setCallingConv(llvm::CallingConv::Win64);
            }
        }
    }
}

// The module's last steps: the verifier, the tier's passes, native code (and assembly for --spills / --asm) into *object
bool Finish(llvm::Module& m, const Common& c, llvm::TargetMachine& tm, Tier tier, std::string* object, UnitStats* st, std::string* asm_text,
            std::string* error, std::string* ir_text = nullptr) {
    if (c.o.windows) {
        AdaptForWindows(m);
    }
    auto t = Clock::now();
    std::string verify;
    llvm::raw_string_ostream verify_out{verify};
    if (llvm::verifyModule(m, &verify_out)) {
        *error = "invalid module " + m.getName().str() + ": " + verify_out.str().substr(0, 4000);
        return false;
    }
    st->t_verify += Since(t);
    t = Clock::now();
    Optimise(m, &tm, tier);
    st->t_opt += Since(t);
    if (ir_text && c.o.ir_after_passes) {
        llvm::raw_string_ostream out{*ir_text};
        m.print(out, nullptr);
    }
    if (c.o.count_spills || !c.o.asm_path.empty()) {
        // The register allocator's spills and reloads, from the comments of verbose assembly of the same code
        std::unique_ptr<llvm::Module> copy = llvm::CloneModule(m);
        llvm::SmallString<0> text_asm;
        llvm::raw_svector_ostream out{text_asm};
        llvm::legacy::PassManager pm;
        tm.Options.MCOptions.AsmVerbose = true;
        if (!tm.addPassesToEmitFile(pm, out, nullptr, llvm::CodeGenFileType::AssemblyFile)) {
            pm.run(*copy);
            llvm::StringRef rest = text_asm.str();
            while (!rest.empty()) {
                auto [line, tail] = rest.split('\n');
                rest = tail;
                if (line.contains("Spill")) {
                    ++st->spills;
                } else if (line.contains("Reload")) {
                    ++st->reloads;
                }
            }
            if (asm_text) {
                *asm_text = text_asm.str().str();
            }
        }
    }
    t = Clock::now();
    llvm::SmallVector<char, 0> buf;
    {
        llvm::raw_svector_ostream out{buf};
        llvm::legacy::PassManager pm;
        if (tm.addPassesToEmitFile(pm, out, nullptr, llvm::CodeGenFileType::ObjectFile)) {
            *error = "the target machine cannot emit an object file";
            return false;
        }
        pm.run(m);
    }
    st->t_codegen += Since(t);
    object->assign(buf.data(), buf.size());
    st->object_bytes = object->size();
    if (auto obj = llvm::object::ObjectFile::createObjectFile(llvm::MemoryBufferRef(llvm::StringRef(*object), m.getName()))) {
        for (const auto& sec : (*obj)->sections()) {
            auto n = sec.getName();
            if (n && *n == ".text") {
                st->text_bytes += sec.getSize();
            }
        }
    } else {
        llvm::consumeError(obj.takeError());
    }
    return true;
}

// ---- Compile units (design 9.3, 10) ----
// At tiers C and W a unit is a group of neighbouring functions of one tier (Units: about kGroupMin to kGroupMax instructions, each function
// optimised on its own: their pipelines do not inline), at H and H+ a shard of neighbouring functions that inline within it, and a function
// split for size is one unit per part
struct Unit {
    Tier tier = Tier::W;
    std::vector<uint64_t> entries;  // the functions, in address order; a part's: its function
    int part = -1;                  // >= 0: this part of the split function entries[0] (Common::parts)
    uint64_t words = 0;
};

// The body of a split function (SplitParts): what it was passed to the state block (with sp and lr), the entry's safepoint poll, then a tail
// call of the part that holds its entry, with the function's entry SP and lr and the entry's offset (lift::FunctionPlan::part_entries)
void DefineSplitStub(const Kit& k, const Common& c, uint64_t entry, const FnFacts& ff, llvm::Function* body, llvm::Function* part) {
    llvm::LLVMContext& ctx = k.m->getContext();
    auto* i8 = llvm::Type::getInt8Ty(ctx);
    llvm::IRBuilder<> b{llvm::BasicBlock::Create(ctx, "entry", body)};
    if (llvm::DISubprogram* sp = k.Subprogram(body, entry)) {
        b.SetCurrentDebugLocation(llvm::DILocation::get(ctx, static_cast<unsigned>(entry), 0, sp));
    }
    body->addFnAttr(llvm::Attribute::NoUnwind);
    body->addFnAttr(llvm::Attribute::getWithUWTableKind(ctx, llvm::UWTableKind::Async));
    body->addFnAttr("frame-pointer", "none");
    llvm::Value* st = body->getArg(0);
    const auto field = [&](size_t off) { return b.CreateConstInBoundsGEP1_64(i8, st, off); };
    for (unsigned i = 0; i < 31; ++i) {
        const int a = c.conv.ArgX(ff.pass, i);
        if (a >= 0 && (ff.pass.x >> i & 1)) b.CreateAlignedStore(body->getArg(static_cast<unsigned>(a)), field(offsetof(dm_guest_state, x) + 8 * i), llvm::Align(8));
    }
    for (unsigned i = 0; i < 32; ++i) {
        const int a = c.conv.ArgV(ff.pass, i);
        if (a >= 0 && (ff.pass.v >> i & 1)) b.CreateAlignedStore(body->getArg(static_cast<unsigned>(a)), field(offsetof(dm_guest_state, v) + 16 * i), llvm::Align(16));
    }
    b.CreateAlignedStore(body->getArg(1), field(offsetof(dm_guest_state, sp)), llvm::Align(8));
    b.CreateAlignedStore(body->getArg(2), field(offsetof(dm_guest_state, x) + 8 * 30), llvm::Align(8));
    if (c.o.safepoints) {
        // Section 6.2's poll at the entry: the block already holds the whole register file
        auto* flag = b.CreateAlignedLoad(b.getInt8Ty(), field(offsetof(dm_guest_state, poll)), llvm::Align(1), "poll");
        flag->setAtomic(llvm::AtomicOrdering::Monotonic);
        auto* cold = llvm::BasicBlock::Create(ctx, "safepoint", body);
        auto* cont = llvm::BasicBlock::Create(ctx, "polled", body);
        llvm::MDBuilder md{ctx};
        b.CreateCondBr(b.CreateICmpNE(flag, b.getInt8(0)), cold, cont, md.createBranchWeights(1, 1u << 20));
        b.SetInsertPoint(cold);
        llvm::Value* at = b.CreateAdd(b.CreateAlignedLoad(b.getInt64Ty(), k.base, llvm::Align(8)), b.getInt64(entry));
        b.CreateAlignedStore(at, field(offsetof(dm_guest_state, pc)), llvm::Align(8));
        b.CreateCall(k.safepoint, {st})->setCallingConv(k.safepoint->getCallingConv());
        b.CreateBr(cont);
        b.SetInsertPoint(cont);
    }
    llvm::FunctionType* type = part->getFunctionType();
    std::vector<llvm::Value*> args(type->getNumParams());
    for (unsigned i = 0; i < type->getNumParams(); ++i) args[i] = llvm::PoisonValue::get(type->getParamType(i));
    args[0] = st;
    args[1] = body->getArg(1);
    args[2] = body->getArg(2);
    args[static_cast<size_t>(c.conv.ArgX(lift::Regs{}, 0))] = b.getInt64(entry);
    auto* call = b.CreateCall(type, part, args);
    c.conv.Apply(call);
    call->setTailCallKind(llvm::CallInst::TCK_MustTail);
    b.CreateRet(call);
}

bool CompileUnit(const Common& c, llvm::TargetMachine& tm, const Unit& u, std::string* object, UnitStats* st, std::string* ir_text,
                 std::string* asm_text, std::string* error) {
    const BuildOptions& o = c.o;
    const std::vector<uint64_t>& entries = u.entries;
    const auto t0 = Clock::now();
    llvm::LLVMContext ctx;
    Diagnose(ctx, st, o.isel_misses);
    const Part* part = u.part >= 0 ? &c.parts.at(entries.front())[static_cast<size_t>(u.part)] : nullptr;
    llvm::Module m{part ? PartName(c, entries.front(), part->lo) : BodyName(c, entries.front()), ctx};
    m.setDataLayout(tm.createDataLayout());
    m.setTargetTriple(tm.getTargetTriple().str());
    Kit k = MakeKit(m, c, false, u.tier);
    auto* i8 = llvm::Type::getInt8Ty(ctx);
    auto* i64 = llvm::Type::getInt64Ty(ctx);
    auto* v128 = llvm::FixedVectorType::get(i64, 2);

    if (!part || u.part == 0) {
        for (const uint64_t entry : entries) {
            const FnFacts& ff = c.plan.fns.at(entry);
            llvm::Function* body = Body(m, c, entry, ff);
            StdEntry(k, c, entry, ff, body, true);
            if (part) {
                // The split function's body: the stub (its first part's unit defines it)
                DefineSplitStub(k, c, entry, ff, body, PartFn(m, c, entry, c.PartAt(entry, entry)->lo));
            }
        }
    }

    // The callees as this unit sees them: declared when the lifter first asks for one
    std::map<uint64_t, lift::CalleeView> views;
    const auto view_of = [&](uint64_t off) -> const lift::CalleeView* {
        if (const auto it = views.find(off); it != views.end()) {
            return &it->second;
        }
        const auto pf = c.plan.fns.find(off);
        if (pf == c.plan.fns.end()) {
            return nullptr;
        }
        lift::CalleeView v;
        v.fn = Body(m, c, off, pf->second);
        v.pass = pf->second.pass;
        v.writes = pf->second.writes;
        v.rv = pf->second.rv;
        v.returns = pf->second.returns;
        return &views.emplace(off, v).first->second;
    };
    lift::CalleeView indirect;
    indirect.pass = c.plan.indirect_pass;
    indirect.writes = c.plan.indirect_writes;
    indirect.rv = c.plan.indirect_rv;
    indirect.returns = true;
    indirect.exact = false;  // a union: the function that runs stores only its own writes
    lift::LiftOptions lo;
    lo.fpcr = 0;
    lo.fpsr_flags = c.fpsr;
    lo.target = c.target;
    for (const uint64_t entry : entries) {
        const FnFacts& ff = c.plan.fns.at(entry);
        // What is lifted: the function's body, or one part of it into the part's own function
        llvm::Function* body = part ? PartFn(m, c, entry, part->lo) : Body(m, c, entry, ff);
        lift::FunctionPlan fp;
        fp.conv = c.conv;
        fp.fn = body;
        fp.self = *view_of(entry);
        fp.live_after = ff.live_after;
        fp.entry = entry;
        fp.limit = ff.limit;
        fp.correctness = o.correctness;
        fp.base = k.base;
        fp.callee = view_of;
        fp.indirect = indirect;
        const FnFacts* facts = &ff;
        fp.resume = [facts](uint64_t call_pc) {
            std::vector<uint64_t> r = facts->continuations;
            if (const auto p = facts->pads.find(call_pc); p != facts->pads.end()) {
                r.push_back(p->second);
            }
            return r;
        };
        fp.tables = [facts](uint64_t br) -> const std::vector<uint64_t>* {
            const auto it = facts->tables.find(br);
            return it == facts->tables.end() ? nullptr : &it->second;
        };
        fp.roots = ff.roots;
        fp.rt_dispatch = k.dispatch;
        fp.rt_svc = k.svc;
        fp.rt_trap = k.trap;
        fp.rt_counter = k.counter;
        fp.rt_jit = k.rt_jit;
        fp.rt_safepoint = k.safepoint;
        fp.jit_entry = k.jit_entry;
        fp.safepoints = o.safepoints;
        fp.ic_empty = k.ic_empty;
        fp.ic_prefix = "nxaot.ic." + llvm::utohexstr(part ? part->lo : entry) + ".";
        fp.debug = k.Subprogram(body, part ? part->lo : entry);
        fp.line_section = o.line_table ? kLineMarks : "";
        // The gates its code holds (design 1.1), which its compiled code must call: checked against what the lifter emitted below (a part:
        // those in its range; SplitParts keeps a function whose code reaches the aux region whole)
        std::vector<nxaot_gate_site> sites = c.SitesOf(entry);
        if (part) {
            std::erase_if(sites, [&](const nxaot_gate_site& g) { return g.offset < part->lo || g.offset >= part->hi; });
        }
        for (const auto& g : sites) {
            if (g.kind == NXAOT_GATE_OBS) fp.obs_gates.insert(g.offset);
            else fp.spin_gates[g.offset] = g.arg;
        }
        fp.rt_gate = k.gate;
        fp.shared_nan_helpers = true;  // the tables' object defines them (CompileTables)
        lift::Region region;
        region.entry = entry;
        region.lo = entry;
        region.hi = ff.limit;
        if (part) {
            // A part (SplitParts): its range, entered at its entries; the other parts' entries are reached through part_of
            region.entry = part->entries.front();
            region.lo = part->lo;
            region.hi = part->hi;
            fp.part_entries = part->entries;
            fp.roots.assign(part->entries.begin() + 1, part->entries.end());
            for (const uint64_t r : ff.roots) {
                if (r >= part->lo && r < part->hi && !std::binary_search(part->entries.begin(), part->entries.end(), r)) fp.roots.push_back(r);
            }
            fp.split = true;
            // Every x and v register outside WRITES: what a return stores again (FnDataflow's restore_block over the whole function, which
            // no part sees; storing a restored register's value again is what the convention allows, design 2.3)
            fp.restore = lift::Regs{0x7FFFFFFFu & ~ff.writes.x, ~ff.writes.v, false, false};
            fp.part_of = [&m, &c, entry, self = part](uint64_t at) -> llvm::Function* {
                const Part* p = c.PartAt(entry, at);
                return p && p != self ? PartFn(m, c, entry, p->lo) : nullptr;
            };
        }
        if (c.image.aux_size) {
            region.extra.emplace_back(c.image.aux_off, c.image.aux_off + c.image.aux_size);  // the frame step's stubs (design 1.1)
        }
        region.max_blocks = size_t{1} << 20;
        lo.plan = &fp;
        const module::Image& image = c.image;
        const uint64_t code_lo = region.lo, code_hi = region.hi;
        const decode::CodeReader read = [&image, code_lo, code_hi](uint64_t a) -> std::optional<uint32_t> {
            if ((a < code_lo || a >= code_hi) && !image.InAux(a)) {
                return std::nullopt;  // a block runs to the function's limit: the next function's entry is reached by a tail call (or the next
                                      // part's, FnSplit)
            }
            if (!image.InCode(a)) {
                return std::nullopt;
            }
            return image.Word(a);
        };
        const lift::Lifted l = lift::LiftRegion(m, body->getName().str(), region, read, lo);
        if (l.function) {
            std::vector<uint64_t> want;
            for (const auto& g : sites) want.push_back(g.offset);
            if (want != l.gates) {
                std::string list;
                for (const uint64_t g : want) list += " " + llvm::utohexstr(g);
                *error = "[RED] nxaot build: " + body->getName().str() + " holds the gate sites" + list + " but its code calls " +
                         std::to_string(l.gates.size()) + " of them (design 1.1: every obs and spin row a function's code holds must be served)";
                return false;
            }
            st->gates += l.gates.size();
        } else if (!sites.empty()) {
            *error = "[RED] nxaot build: " + body->getName().str() + " holds " + std::to_string(sites.size()) +
                     " gate sites and the lifter refused it: the JIT fallback that would run it calls no gate (design 1.1)";
            return false;
        }
        st->blocks += l.blocks;
        st->calls += l.calls;
        st->indirect_calls += l.indirect_calls;
        st->tail_calls += l.tail_calls;
        st->indirect_tails += l.indirect_tails;
        st->returns += l.returns;
        st->svcs += l.svcs;
        st->traps += l.traps;
        st->splits += l.splits;
        if (!l.function) {
            // Refused (an opcode the lowering does not cover, an internal error): a body that hands the function to the JIT fallback (section 11,
            // case 4), so that the rest of the module still runs: what it is passed to the block, the JIT from its entry until it returns to lr,
            // then the native return of what the JIT left in the block
            ++st->refused;
            std::string why = l.error;
            for (const auto& op : l.unsupported) {
                why += (why.empty() ? "" : ", ") + op;
            }
            st->notes.push_back("[RED] nxaot build: " + body->getName().str() + " refused (" + why + "): it runs in the JIT fallback");
            llvm::Function* f = body;
            llvm::IRBuilder<> fb{llvm::BasicBlock::Create(ctx, "entry", f)};
            if (fp.debug) {
                fb.SetCurrentDebugLocation(llvm::DILocation::get(ctx, static_cast<unsigned>(entry), 0, fp.debug));
            }
            llvm::Value* stp = f->getArg(0);
            const auto field = [&](size_t off) { return fb.CreateConstInBoundsGEP1_64(i8, stp, off); };
            llvm::Value* at = nullptr;
            if (part) {
                // A part: the block holds every register already; the JIT from the block the part was entered at
                at = fb.CreateAdd(fb.CreateAlignedLoad(i64, k.base, llvm::Align(8)), f->getArg(static_cast<unsigned>(c.conv.ArgX(lift::Regs{}, 0))));
            } else {
                for (unsigned i = 0; i < 31; ++i) {
                    const int a = c.conv.ArgX(ff.pass, i);
                    if (a >= 0 && (ff.pass.x >> i & 1)) {
                        fb.CreateAlignedStore(f->getArg(static_cast<unsigned>(a)), field(offsetof(dm_guest_state, x) + 8 * i), llvm::Align(8));
                    }
                }
                for (unsigned i = 0; i < 32; ++i) {
                    const int a = c.conv.ArgV(ff.pass, i);
                    if (a >= 0 && (ff.pass.v >> i & 1)) {
                        fb.CreateAlignedStore(f->getArg(static_cast<unsigned>(a)), field(offsetof(dm_guest_state, v) + 16 * i), llvm::Align(16));
                    }
                }
                fb.CreateAlignedStore(f->getArg(1), field(offsetof(dm_guest_state, sp)), llvm::Align(8));
                fb.CreateAlignedStore(f->getArg(2), field(offsetof(dm_guest_state, x) + 8 * 30), llvm::Align(8));
                at = fb.CreateAdd(fb.CreateAlignedLoad(i64, k.base, llvm::Align(8)), fb.getInt64(entry));
            }
            fb.CreateAlignedStore(at, field(offsetof(dm_guest_state, pc)), llvm::Align(8));
            fb.CreateAlignedStore(fb.getInt32(NXAOT_TRAP_JIT_REFUSED), field(offsetof(dm_guest_state, exit_reason)), llvm::Align(4));
            fb.CreateCall(k.rt_jit, {stp, f->getArg(2), f->getArg(1)});
            llvm::Value* r = llvm::PoisonValue::get(c.conv.RetType(ctx));
            r = fb.CreateInsertValue(r, fb.CreateAlignedLoad(i64, field(offsetof(dm_guest_state, x)), llvm::Align(8)), 0);
            r = fb.CreateInsertValue(r, fb.CreateAlignedLoad(i64, field(offsetof(dm_guest_state, x) + 8), llvm::Align(8)), 1);
            for (unsigned i = 0; i < 4; ++i) {
                if (ff.rv >> i & 1) {
                    r = fb.CreateInsertValue(r, fb.CreateAlignedLoad(v128, field(offsetof(dm_guest_state, v) + 16 * i), llvm::Align(16)), 2 + i);
                }
            }
            fb.CreateRet(r);
        }
    }
    if (k.dib) {
        k.dib->finalize();
    }
    st->t_lift += Since(t0);
    if (ir_text && !o.ir_after_passes) {
        llvm::raw_string_ostream out{*ir_text};
        m.print(out, nullptr);
    }
    return Finish(m, c, tm, u.tier, object, st, asm_text, error, ir_text);
}

// ---- The tables' object: the lookup hash, the function map, the returns, the trampoline, the descriptor, and the shared code ----

bool CompileTables(const Common& c, llvm::TargetMachine& tm, std::string* object, UnitStats* st, BuildStats* s, std::string* ir_text,
                   std::string* asm_text, std::string* error) {
    const BuildOptions& o = c.o;
    const Plan& plan = c.plan;
    const auto t0 = Clock::now();
    llvm::LLVMContext ctx;
    Diagnose(ctx, st);
    llvm::Module mod{c.prefix, ctx};
    llvm::Module* m = &mod;
    m->setDataLayout(tm.createDataLayout());
    m->setTargetTriple(tm.getTargetTriple().str());
    Kit k = MakeKit(*m, c, true, c.o.tier);
    llvm::IRBuilder<> b{ctx};
    auto* i8 = b.getInt8Ty();
    auto* i32 = b.getInt32Ty();
    auto* i64 = b.getInt64Ty();
    auto* ptr = b.getPtrTy();
    auto* v128 = llvm::FixedVectorType::get(i64, 2);
    const lift::Convention& conv = c.conv;
    const lift::Regs std_pass = plan.indirect_pass;

    // The NaN chains' helpers the units call (lift::FunctionPlan::shared_nan_helpers), once for the whole pack
    {
        lift::LiftOptions lo;
        lo.fpcr = 0;
        lo.fpsr_flags = c.fpsr;
        lo.target = c.target;
        lift::DefineNaNHelpers(*m, lo);
    }

    // Every body and standard entry, declared: the units define them
    std::map<uint64_t, llvm::Function*> bodies, std_entries;
    for (const auto& [entry, ff] : plan.fns) {
        bodies[entry] = Body(*m, c, entry, ff);
        s->words += ff.words;
        ++s->tier_functions[static_cast<size_t>(c.TierOf(entry))];
    }
    s->functions = plan.fns.size();
    for (const auto& [entry, ff] : plan.fns) {
        if (ff.host_callable) {
            std_entries[entry] = StdEntry(k, c, entry, ff, bodies[entry], false);
        }
    }
    s->standard_entries = std_entries.size();
    memory::Rss("the tables' object: every body and standard entry declared");
    for (const auto& [entry, ff] : plan.fns) {
        s->tables += ff.tables.size();
        s->resume_sites += ff.continuations.size() + ff.pads.size();
    }

    const auto rel = [&](llvm::Constant* target, llvm::Constant* field) {
        llvm::Constant* diff = llvm::ConstantExpr::getSub(llvm::ConstantExpr::getPtrToInt(target, i64), llvm::ConstantExpr::getPtrToInt(field, i64));
        return llvm::ConstantExpr::getTrunc(diff, i32);
    };
    // The address of field 1 (the self-relative native address) of row i of a table
    const auto field_of = [&](llvm::ArrayType* ty, llvm::GlobalVariable* table, uint64_t i) {
        return llvm::ConstantExpr::getInBoundsGetElementPtr(ty, table, llvm::ArrayRef<llvm::Constant*>{b.getInt64(0), b.getInt64(i), b.getInt32(1)});
    };
    uint32_t slots = 2;
    while (slots < 2 * std_entries.size()) {
        slots *= 2;
    }
    s->lookup_slots = slots;
    auto* entry_ty = llvm::StructType::get(ctx, {i64, i32, i32});
    auto* lookup_ty = llvm::ArrayType::get(entry_ty, slots);
    auto* lookup = new llvm::GlobalVariable(*m, lookup_ty, true, llvm::GlobalValue::InternalLinkage, nullptr, "nxaot.lookup");
    lookup->setSection(".nxaot.lookup");
    lookup->setAlignment(llvm::Align(16));
    {
        std::vector<uint64_t> keys(slots, NXAOT_LOOKUP_EMPTY);
        std::vector<llvm::Function*> targets(slots, nullptr);
        for (const auto& [entry, fn] : std_entries) {
            uint32_t h = nxaot_lookup_hash(entry, slots - 1);
            while (keys[h] != NXAOT_LOOKUP_EMPTY) {
                h = (h + 1) & (slots - 1);
            }
            keys[h] = entry;
            targets[h] = fn;
        }
        std::vector<llvm::Constant*> rows;
        for (uint32_t i = 0; i < slots; ++i) {
            llvm::Constant* field = field_of(lookup_ty, lookup, i);
            const auto ff = keys[i] == NXAOT_LOOKUP_EMPTY ? plan.fns.end() : plan.fns.find(keys[i]);
            const uint32_t flags = ff != plan.fns.end() && !ff->second.writes.sp ? NXAOT_LOOKUP_SP_KEPT : 0u;
            rows.push_back(llvm::ConstantStruct::get(entry_ty, {b.getInt64(keys[i]), targets[i] ? rel(targets[i], field) : b.getInt32(0), b.getInt32(flags)}));
        }
        lookup->setInitializer(llvm::ConstantArray::get(lookup_ty, rows));
    }
    auto* fns_ty = llvm::ArrayType::get(entry_ty, plan.fns.size());
    auto* fnmap = new llvm::GlobalVariable(*m, fns_ty, true, llvm::GlobalValue::InternalLinkage, nullptr, "nxaot.fns");
    fnmap->setSection(".nxaot.fnmap");
    fnmap->setAlignment(llvm::Align(16));
    {
        std::vector<llvm::Constant*> rows;
        uint64_t i = 0;
        for (const auto& [entry, ff] : plan.fns) {
            llvm::Constant* field = field_of(fns_ty, fnmap, i);
            const uint32_t size = static_cast<uint32_t>(ff.size);
            rows.push_back(llvm::ConstantStruct::get(entry_ty, {b.getInt64(entry), rel(bodies[entry], field), b.getInt32(size)}));
            ++i;
        }
        fnmap->setInitializer(llvm::ConstantArray::get(fns_ty, rows));
    }
    // The return addresses of every call site (bl, blr) of compiled code, sorted: where a native frame may wait for its callee, which the JIT
    // fallback stops at besides the compiled entries (src/rt/jit.cpp); Common::returns, made by BuildObjects from discovery's calls
    const std::vector<uint32_t>& returns = c.returns;
    auto* returns_ty = llvm::ArrayType::get(i32, returns.size());
    auto* returns_table = new llvm::GlobalVariable(*m, returns_ty, true, llvm::GlobalValue::InternalLinkage,
                                                   llvm::ConstantDataArray::get(ctx, llvm::ArrayRef<uint32_t>(returns)), "nxaot.returns");
    returns_table->setSection(".nxaot.returns");
    returns_table->setAlignment(llvm::Align(16));
    // The line table: lines.o defines it once every unit is compiled (CompileLines, "The line table" above); none with --no-line-table
    llvm::Constant* fn_lines = llvm::ConstantPointerNull::get(ptr);
    llvm::Constant* lines = llvm::ConstantPointerNull::get(ptr);
    if (o.line_table) {
        auto* fl = new llvm::GlobalVariable(*m, i8, true, llvm::GlobalValue::ExternalLinkage, nullptr, "nxaot.fn_lines");
        auto* ln = new llvm::GlobalVariable(*m, i8, true, llvm::GlobalValue::ExternalLinkage, nullptr, "nxaot.lines");
        Hide(fl);
        Hide(ln);
        fn_lines = fl;
        lines = ln;
    }
    // The gate sites compiled code calls (design 1.1), sorted by offset: what the host checks the manifest's obs and spin rows against
    llvm::Constant* gates = llvm::ConstantPointerNull::get(ptr);
    if (!c.served.empty()) {
        auto* site_ty = llvm::StructType::get(ctx, {i32, i32, i64});
        std::vector<llvm::Constant*> rows;
        for (const auto& g : c.served) {
            rows.push_back(llvm::ConstantStruct::get(site_ty, {b.getInt32(g.offset), b.getInt32(g.kind), b.getInt64(g.arg)}));
        }
        auto* gates_ty = llvm::ArrayType::get(site_ty, rows.size());
        auto* table = new llvm::GlobalVariable(*m, gates_ty, true, llvm::GlobalValue::InternalLinkage, llvm::ConstantArray::get(gates_ty, rows),
                                               "nxaot.gates");
        table->setSection(".nxaot.gates");
        table->setAlignment(llvm::Align(16));
        gates = table;
    }

    // enter(st, standard entry): the C convention into the pack's convention, every passable register from st; x0, x1 and the standard entries'
    // RV back to st unless the guest left through a non-local transfer (st->pending: the block then holds the whole register file)
    auto* enter = llvm::Function::Create(llvm::FunctionType::get(b.getVoidTy(), {ptr, ptr}, false), llvm::GlobalValue::InternalLinkage, "nxaot.enter", *m);
    enter->addFnAttr(llvm::Attribute::NoUnwind);
    enter->addFnAttr(llvm::Attribute::NoInline);
    enter->addFnAttr(llvm::Attribute::getWithUWTableKind(ctx, llvm::UWTableKind::Async));
    {
        llvm::DISubprogram* sp = k.Subprogram(enter, 0);
        llvm::IRBuilder<> eb{llvm::BasicBlock::Create(ctx, "entry", enter)};
        if (sp) {
            eb.SetCurrentDebugLocation(llvm::DILocation::get(ctx, 0, 0, sp));
        }
        llvm::Value* st0 = enter->getArg(0);
        const auto field = [&](size_t off) { return eb.CreateConstInBoundsGEP1_64(i8, st0, off); };
        std::vector<llvm::Value*> args(k.std_ty->getNumParams());
        args[0] = st0;
        args[1] = eb.CreateAlignedLoad(i64, field(offsetof(dm_guest_state, sp)), llvm::Align(8));
        args[2] = eb.CreateAlignedLoad(i64, field(offsetof(dm_guest_state, x) + 8 * 30), llvm::Align(8));
        for (unsigned i = 0; i < 31; ++i) {
            if (const int a = conv.ArgX(std_pass, i); a >= 0) {
                args[static_cast<size_t>(a)] = eb.CreateAlignedLoad(i64, field(offsetof(dm_guest_state, x) + 8 * i), llvm::Align(8));
            }
        }
        for (unsigned i = 0; i < 32; ++i) {
            if (const int a = conv.ArgV(std_pass, i); a >= 0) {
                args[static_cast<size_t>(a)] = eb.CreateAlignedLoad(v128, field(offsetof(dm_guest_state, v) + 16 * i), llvm::Align(16));
            }
        }
        auto* call = eb.CreateCall(k.std_ty, enter->getArg(1), args);
        conv.Apply(call);
        llvm::Value* pending = eb.CreateAlignedLoad(i64, field(offsetof(dm_guest_state, pending)), llvm::Align(8));
        auto* store_bb = llvm::BasicBlock::Create(ctx, "results", enter);
        auto* done_bb = llvm::BasicBlock::Create(ctx, "done", enter);
        eb.CreateCondBr(eb.CreateICmpEQ(pending, eb.getInt64(0)), store_bb, done_bb);
        eb.SetInsertPoint(store_bb);
        eb.CreateAlignedStore(eb.CreateExtractValue(call, 0), field(offsetof(dm_guest_state, x)), llvm::Align(8));
        eb.CreateAlignedStore(eb.CreateExtractValue(call, 1), field(offsetof(dm_guest_state, x) + 8), llvm::Align(8));
        for (unsigned i = 0; i < 4; ++i) {
            if (plan.indirect_rv >> i & 1) {
                eb.CreateAlignedStore(eb.CreateExtractValue(call, 2 + i), field(offsetof(dm_guest_state, v) + 16 * i), llvm::Align(16));
            }
        }
        eb.CreateBr(done_bb);
        eb.SetInsertPoint(done_bb);
        eb.CreateRetVoid();
    }

    auto* desc_ty = DescType(ctx);
    const llvm::StructLayout* layout = m->getDataLayout().getStructLayout(desc_ty);
    if (layout->getSizeInBytes() != sizeof(nxaot_module_desc) || layout->getElementOffset(12) != offsetof(nxaot_module_desc, base) ||
        layout->getElementOffset(16) != offsetof(nxaot_module_desc, fn_lines) || layout->getElementOffset(17) != offsetof(nxaot_module_desc, lines) ||
        layout->getElementOffset(19) != offsetof(nxaot_module_desc, tool) || layout->getElementOffset(20) != offsetof(nxaot_module_desc, returns) ||
        layout->getElementOffset(22) != offsetof(nxaot_module_desc, jit_entry) || layout->getElementOffset(23) != offsetof(nxaot_module_desc, std_x) ||
        layout->getElementOffset(25) != offsetof(nxaot_module_desc, gates) || layout->getElementOffset(26) != offsetof(nxaot_module_desc, gate_count)) {
        *error = "the descriptor's LLVM layout differs from include/nxaot/aot_module.h";
        return false;
    }
    const module::Image& image = c.image;
    std::string name_bytes = image.name.substr(0, 31);
    name_bytes.resize(32, '\0');
    const std::string tool = std::string("nxaot llvm ") + NXAOT_LOCK_LLVM + " dynarmic " + NXAOT_LOCK_DYNARMIC + " cc " + lift::CallConvName(o.cc) +
                             " tier " + TierName(o.tier) + " mode " + o.mode + (o.correctness ? " correctness" : "") +
                             (o.all_entries ? " all-entries" : "") + (o.safepoints ? "" : " no-safepoints") + " cpu " + c.cpu;
    auto* tool_str = b.CreateGlobalString(tool, "nxaot.tool", 0, m);
    const uint32_t flags = (o.cc == lift::CallConv::Ghc ? NXAOT_FLAG_GHC : 0) | (o.correctness ? NXAOT_FLAG_CORRECTNESS : 0) |
                           (o.all_entries ? NXAOT_FLAG_ALL_ENTRIES : 0) | (c.fpsr ? NXAOT_FLAG_FPSR : 0) |
                           (o.safepoints ? NXAOT_FLAG_SAFEPOINTS : 0);
    auto* desc = new llvm::GlobalVariable(
        *m, desc_ty, true, llvm::GlobalValue::ExternalLinkage,
        llvm::ConstantStruct::get(
            desc_ty, {b.getInt32(NXAOT_MODULE_ABI), b.getInt32(flags), llvm::ConstantDataArray::getString(ctx, name_bytes, false),
                      llvm::ConstantDataArray::get(ctx, llvm::ArrayRef<uint8_t>(image.sha256.data(), image.sha256.size())),
                      b.getInt64(image.bytes.size()), b.getInt64(image.TextEnd()), b.getInt64(image.bss_size), b.getInt64(image.text_off),
                      b.getInt32(0), b.getInt32(slots - 1), b.getInt32(plan.indirect_rv), b.getInt32(0), k.base, lookup, fnmap,
                      b.getInt64(plan.fns.size()), fn_lines, lines, enter, tool_str, returns_table, b.getInt64(returns.size()),
                      k.jit_entry, b.getInt32(plan.indirect_pass.x), b.getInt32(plan.indirect_pass.v), gates, b.getInt64(c.served.size())}),
        "nxaot_module");
    desc->setAlignment(llvm::Align(8));
    if (k.dib) {
        k.dib->finalize();
    }
    st->t_lift += Since(t0);
    if (ir_text && !o.ir_after_passes) {
        llvm::raw_string_ostream out{*ir_text};
        m->print(out, nullptr);
    }
    memory::Rss("the tables' object: its tables made");
    const bool ok = Finish(*m, c, tm, c.o.tier, object, st, asm_text, error, ir_text);
    memory::Rss("the tables' object: code generated (" + std::to_string(object->size() >> 20) + " MiB)");
    return ok;
}

// ---- lines.o: the line table, from every unit's line marks (ReadLineMarks), once all are compiled ----
// nxaot.fn_lines: a {first, count} run per function in the function map's order; nxaot.lines: {native - body, offset} per guest memory access,
// each run sorted by native address (include/nxaot/aot_module.h)
bool CompileLines(const Common& c, llvm::TargetMachine& tm, LineRuns& by_fn, std::string* object, UnitStats* st, uint64_t* count,
                  std::string* error) {
    llvm::LLVMContext ctx;
    Diagnose(ctx, st);
    llvm::Module m{c.prefix + ".lines", ctx};
    m.setDataLayout(tm.createDataLayout());
    m.setTargetTriple(tm.getTargetTriple().str());
    std::vector<uint32_t> index, rows;
    for (const auto& [entry, ff] : c.plan.fns) {
        auto it = by_fn.find(entry);
        std::vector<std::pair<uint32_t, uint32_t>> run;
        if (it != by_fn.end()) {
            run = std::move(it->second);
            std::sort(run.begin(), run.end());
            run.erase(std::unique(run.begin(), run.end()), run.end());
        }
        index.push_back(static_cast<uint32_t>(rows.size() / 2));
        index.push_back(static_cast<uint32_t>(run.size()));
        for (const auto& [native, offset] : run) {
            rows.push_back(native);
            rows.push_back(offset);
        }
    }
    *count = rows.size() / 2;
    if (memory::On()) {
        uint64_t b = memory::Of(by_fn) + memory::Of(index) + memory::Of(rows);
        for (const auto& [entry, run] : by_fn) b += memory::Of(run);
        memory::Note("pack: the line table's words", b, "the line table");
    }
    const auto table = [&](const char* name, const char* section, const std::vector<uint32_t>& words) {
        auto* init = llvm::ConstantDataArray::get(ctx, llvm::ArrayRef<uint32_t>(words));
        auto* g = new llvm::GlobalVariable(m, init->getType(), true, llvm::GlobalValue::ExternalLinkage, init, name);
        Hide(g);
        g->setSection(section);
        g->setAlignment(llvm::Align(8));
    };
    table("nxaot.fn_lines", ".nxaot.fnlines", index);
    table("nxaot.lines", ".nxaot.lines", rows);
    return Finish(m, c, tm, c.o.tier, object, st, nullptr, error);
}

// ---- imports.o of a Windows pack ("Windows packs" above) ----
// Every symbol the other objects call and none of them defines must be a run-time entry of the host (IsHostImport), else the pack fails, naming
// it. Per entry: a hidden thunk of its name, `jmp *nxaot.imp.<name>(%rip)` (the code calls it as a Linux pack's code calls its PLT entry), and
// that slot (zero until the host binds it); then the exported table nxaot_imports of include/nxaot/aot_module.h, {name, slot} per entry in
// the order of the names, ended by {0, 0}, which the host's loader binds against the executable that loads the pack (dm/host/aot_arena.c)
bool CompileImports(const Common& c, llvm::TargetMachine& tm, const std::vector<std::string>& objects, std::string* object, UnitStats* st,
                    std::vector<std::string>* imports, std::string* error) {
    std::set<std::string> defined, undefined;
    for (const std::string& path : objects) {
        auto buf = llvm::MemoryBuffer::getFile(path);
        if (!buf) {
            *error = "cannot read " + path + ": " + buf.getError().message();
            return false;
        }
        auto file = llvm::object::ObjectFile::createObjectFile((*buf)->getMemBufferRef());
        if (!file) {
            *error = path + ": " + llvm::toString(file.takeError());
            return false;
        }
        for (const llvm::object::SymbolRef& s : (*file)->symbols()) {
            auto flags = s.getFlags();
            auto name = s.getName();
            if (!flags || !name) {
                if (!flags) llvm::consumeError(flags.takeError());
                if (!name) llvm::consumeError(name.takeError());
                continue;
            }
            if (*flags & llvm::object::SymbolRef::SF_Undefined) {
                undefined.insert(name->str());
            } else if (*flags & llvm::object::SymbolRef::SF_Global) {
                defined.insert(name->str());
            }
        }
    }
    std::string unknown;
    imports->clear();
    for (const std::string& u : undefined) {
        if (defined.count(u)) continue;
        if (IsHostImport(u)) {
            imports->push_back(u);
        } else {
            unknown += (unknown.empty() ? "" : ", ") + u;
        }
    }
    if (!unknown.empty()) {
        *error = "the pack calls " + unknown + ", which the host does not export to packs (include/nxaot/aot_module.h, src/lift/runtime.h): a "
                 "Windows pack imports the run-time entries only";
        return false;
    }
    std::string s = "\t.section\t.text.nxaot.imports,\"ax\",@progbits\n";
    for (const std::string& name : *imports) {
        s += "\t.p2align\t4, 0xcc\n\t.globl\t" + name + "\n\t.hidden\t" + name + "\n\t.type\t" + name + ",@function\n" + name + ":\n\tjmpq\t*nxaot.imp." +
             name + "(%rip)\n\t.size\t" + name + ", .-" + name + "\n";
    }
    s += "\t.section\t.bss.nxaot.imports,\"aw\",@nobits\n\t.p2align\t3\n";
    for (const std::string& name : *imports) {
        const std::string slot = "nxaot.imp." + name;
        s += "\t.globl\t" + slot + "\n\t.hidden\t" + slot + "\n\t.type\t" + slot + ",@object\n\t.size\t" + slot + ", 8\n" + slot + ":\n\t.zero\t8\n";
    }
    s += "\t.section\t.rodata.nxaot.imports,\"a\",@progbits\n";
    for (size_t i = 0; i < imports->size(); ++i) {
        s += ".Lnxaot.import." + std::to_string(i) + ":\n\t.asciz\t\"" + (*imports)[i] + "\"\n";
    }
    s += "\t.section\t.data.rel.ro.nxaot.imports,\"aw\",@progbits\n\t.p2align\t3\n\t.globl\tnxaot_imports\n\t.type\tnxaot_imports,@object\n\t.size\t"
         "nxaot_imports, " + std::to_string(16 * (imports->size() + 1)) + "\nnxaot_imports:\n";
    for (size_t i = 0; i < imports->size(); ++i) {
        s += "\t.quad\t.Lnxaot.import." + std::to_string(i) + "\n\t.quad\tnxaot.imp." + (*imports)[i] + "\n";
    }
    s += "\t.quad\t0\n\t.quad\t0\n";
    llvm::LLVMContext ctx;
    Diagnose(ctx, st);
    llvm::Module m{c.prefix + ".imports", ctx};
    m.setDataLayout(tm.createDataLayout());
    m.setTargetTriple(tm.getTargetTriple().str());
    m.setModuleInlineAsm(s);
    return Finish(m, c, tm, c.o.tier, object, st, nullptr, error);
}

// ---- The cache key of a unit (design section 13) ----

void AddRegs(cache::KeyBuilder& k, std::string_view name, const lift::Regs& r) {
    k.Add(name, std::to_string(r.x) + "," + std::to_string(r.v) + "," + std::to_string(r.sp) + "," + std::to_string(r.nzcv));
}

// The SHA-256 of everything a unit's object depends on: the toolchain (nxaot.lock, LLVM, this executable), the target machine (triple, CPU and
// its features), the tier and every option that changes code (the convention, the correctness mode, the debug information and line table,
// the safepoints, FPCR and the FPSR modelling), the module's symbol prefix, the standard entries' signature and the indirect view; then the
// function: its bounds and convention facts (PASS, WRITES, RV, returns, LIVE_AFTER, a standard entry, resume points, landing pads, jump tables,
// roots), its instruction words from its entry to its limit with the pc-relative fields (b, bl, b.cond, cbz, tbz, adr, adrp, literal loads)
// replaced by reference numbers as nso_match.py normalises relocations, the referenced addresses in order, and the convention facts of every
// function it references (its callees' classes). The lowering writes module offsets into the code (the symbol of the body, return addresses,
// pcs, the line table), so the referenced addresses are module offsets and the entry is part of the key: a function that moved in a game
// update misses until the lowering reaches other code through symbols only (phase 5b)
std::string UnitKey(const Common& c, uint64_t entry) {
    const BuildOptions& o = c.o;
    const FnFacts& ff = c.plan.fns.at(entry);
    cache::KeyBuilder k;
    k.Add("format", kUnitKeyFormat).Add("tool", c.tool).Add("target", c.triple + " " + c.cpu + " " + c.features);
    k.Add("tier", TierName(c.TierOf(entry))).Add("cc", lift::CallConvName(o.cc)).Add("correctness", o.correctness).Add("debug_info", o.debug_info);
    k.Add("line_table", o.line_table).Add("safepoints", o.safepoints).Add("no_inline", o.no_inline).Add("fpsr", c.fpsr).Add("fpcr", uint64_t{0});
    k.Add("prefix", c.prefix);
    if (o.windows) {
        k.Add("host", "windows: win64cc run-time calls, no red zone");  // a Windows pack's units (AdaptForWindows); a Linux pack's keys are as before
    }
    AddRegs(k, "indirect_pass", c.plan.indirect_pass);
    AddRegs(k, "indirect_writes", c.plan.indirect_writes);
    k.Add("indirect_rv", c.plan.indirect_rv);
    k.Add("entry", entry).Add("limit", ff.limit);
    AddRegs(k, "pass", ff.pass);
    AddRegs(k, "writes", ff.writes);
    AddRegs(k, "live_after", ff.live_after);
    k.Add("rv", ff.rv).Add("returns", ff.returns).Add("std_entry", HasStdEntry(c, ff));
    std::string list;
    for (const uint64_t r : ff.continuations) list += std::to_string(r) + ",";
    k.Add("continuations", list);
    list.clear();
    for (const auto& [call, pad] : ff.pads) list += std::to_string(call) + ":" + std::to_string(pad) + ",";
    k.Add("pads", list);
    list.clear();
    for (const auto& [br, targets] : ff.tables) {
        list += std::to_string(br) + ":";
        for (const uint64_t t : targets) list += std::to_string(t) + ",";
        list += ";";
    }
    k.Add("tables", list);
    list.clear();
    for (const uint64_t r : ff.roots) list += std::to_string(r) + ",";
    k.Add("roots", list);
    // The code: per word of [entry, limit) whether the lifter may read it, and the word with its pc-relative field cleared and a reference
    // number in its place
    const module::Image& img = c.image;
    std::string code;
    std::vector<uint64_t> refs;
    std::map<uint64_t, uint32_t> ref_of;
    const auto ref = [&](uint64_t target) {
        const auto [it, added] = ref_of.try_emplace(target, static_cast<uint32_t>(refs.size()));
        if (added) refs.push_back(target);
        return it->second;
    };
    // The words of [entry, limit), then of its blocks in the aux region (the frame step's stubs it reaches, design 1.1)
    std::vector<uint64_t> pcs;
    for (uint64_t pc = entry; pc < ff.limit; pc += 4) pcs.push_back(pc);
    for (const auto& [lo, hi] : ff.extra) {
        for (uint64_t pc = lo; pc < hi; pc += 4) pcs.push_back(pc);
    }
    for (const uint64_t pc : pcs) {
        const auto w = img.InCode(pc) ? img.Word(pc) : std::nullopt;
        uint32_t word = w.value_or(0), number = 0;
        bool relative = false;
        if (w) {
            const analysis::Insn i = analysis::ClassifyWord(word, pc);
            if (i.kind == analysis::Kind::B || i.kind == analysis::Kind::BL) {
                word &= 0xFC000000u;
                relative = true;
            } else if (i.kind == analysis::Kind::BCond) {
                word &= (word & 0x7E000000u) == 0x36000000u ? 0xFFF8001Fu : 0xFF00001Fu;  // tbz / tbnz: imm14; b.cond, cbz / cbnz: imm19
                relative = true;
            } else if (i.adr || i.adrp) {
                word &= 0x9F00001Fu;
                relative = true;
            } else if ((word & 0x3B000000u) == 0x18000000u) {  // ldr / ldrsw / prfm (literal), scalar and vector
                const uint64_t target = pc + static_cast<uint64_t>(analysis::SignExtend((word >> 5) & 0x7FFFFu, 19) * 4);
                word &= 0xFF00001Fu;
                number = ref(target);
                code += 'L';
            }
            if (relative) {
                number = ref(i.target);
                code += 'R';
            }
        }
        code += w ? 'W' : '-';
        code.append(reinterpret_cast<const char*>(&word), 4);
        code.append(reinterpret_cast<const char*>(&number), 4);
    }
    k.Add("code", code);
    list.clear();
    for (const uint64_t r : refs) list += std::to_string(r) + ",";
    k.Add("refs", list);
    list.clear();
    for (const auto& [lo, hi] : ff.extra) list += std::to_string(lo) + "-" + std::to_string(hi) + ",";
    k.Add("extra", list);
    // The gates its code calls (design 1.1): their sites, kinds and arguments
    list.clear();
    for (const auto& g : c.SitesOf(entry)) list += std::to_string(g.offset) + ":" + std::to_string(g.kind) + ":" + std::to_string(g.arg) + ",";
    k.Add("gates", list);
    // The convention facts of every function the code references: its direct branch targets, the next function (a block that runs to the
    // limit tail-calls it), its tables' targets and resume points
    std::set<uint64_t> referenced(refs.begin(), refs.end());
    referenced.insert(ff.limit);
    for (const auto& [br, targets] : ff.tables) referenced.insert(targets.begin(), targets.end());
    referenced.insert(ff.continuations.begin(), ff.continuations.end());
    for (const auto& [call, pad] : ff.pads) referenced.insert(pad);
    list.clear();
    for (const uint64_t t : referenced) {
        const auto it = c.plan.fns.find(t);
        if (it == c.plan.fns.end()) continue;
        const FnFacts& cf = it->second;
        list += std::to_string(t) + ":" + std::to_string(cf.pass.x) + "," + std::to_string(cf.pass.v) + "," + std::to_string(cf.pass.sp) + "/" +
                std::to_string(cf.writes.x) + "," + std::to_string(cf.writes.v) + "," + std::to_string(cf.writes.sp) + "," +
                std::to_string(cf.writes.nzcv) + "/" + std::to_string(cf.rv) + "/" + std::to_string(cf.returns) + ";";
    }
    k.Add("callees", list);
    return k.Hex();
}

// The key of a unit (each function's key is UnitKey above, the cache's keys stay per function): a unit of one function, its function's key; a
// group or a shard, the keys of its functions in order; a part of a split function, its function's key with the part's range and entries
std::string UnitKey(const Common& c, const Unit& u) {
    if (u.part >= 0) {
        const Part& p = c.parts.at(u.entries.front())[static_cast<size_t>(u.part)];
        cache::KeyBuilder k;
        k.Add("format", kUnitKeyFormat).Add("fn", UnitKey(c, u.entries.front())).Add("part", static_cast<uint64_t>(u.part));
        k.Add("parts", c.parts.at(u.entries.front()).size()).Add("lo", p.lo).Add("hi", p.hi);
        std::string list;
        for (const uint64_t e : p.entries) list += std::to_string(e) + ",";
        k.Add("entries", list);
        return k.Hex();
    }
    if (u.entries.size() == 1) {
        return UnitKey(c, u.entries.front());
    }
    cache::KeyBuilder k;
    k.Add("format", kUnitKeyFormat).Add("shard", u.entries.size());
    for (const uint64_t e : u.entries) {
        k.Add("fn", UnitKey(c, e));
    }
    return k.Hex();
}

// Whether a unit ends after this function, by its code alone: the first byte of the SHA-256 of its words a multiple of cut
bool CutAfter(const module::Image& image, uint64_t entry, const FnFacts& ff, unsigned cut) {
    const uint64_t end = std::min<uint64_t>(ff.limit, image.bytes.size());
    const auto code = llvm::ArrayRef<uint8_t>(image.bytes.data() + std::min<uint64_t>(entry, end), image.bytes.data() + end);
    return llvm::SHA256::hash(code)[0] % cut == 0;
}

// The compile units of a module (design 9.3), in address order. At tiers C and W, whose pipelines optimise one function at a time, groups of
// neighbouring functions of one tier: a group ends after a function whose code's SHA-256 starts with a multiple of kGroupCut once it holds
// kGroupMin instructions, and at kGroupMax, so that a change in one function moves the groups' ends near it only. One function per unit cost a
// fixed 0.5 ms of code generation set-up and some 2.7 KiB of object, link and cache entry per unit (README.md, "Pack throughput", phase 5b):
// over Monster Hunter Rise's estimated 600,000 functions of 52 instructions on average, minutes. At H and H+, whose pipelines inline, shards
// of neighbouring functions (the callees a function calls most are mostly near it), cut the same way between kShardMin and kShardMax. A
// function split for size (SplitParts) is a unit per part, and ends the group before it
std::vector<Unit> Units(const Common& c) {
    // A group holds up to 1,024 instructions, fewer in a module of less than 32 groups' worth (a thirty-second of it, at least 64): a small
    // NRO's pack keeps enough units for the workers (fnptr-gcc, 1,603 instructions, was 4 units of up to 1,024 and took 0.35 s with 8 workers
    // against 0.20 s as 61 units). Above 32,768 instructions the limits are fixed, so a game module's groups do not move with its size
    uint64_t module_words = 0;
    for (const auto& [entry, ff] : c.plan.fns) module_words += ff.words;
    const uint64_t kGroupMax = std::clamp<uint64_t>(module_words / 32, 64, 1'024), kGroupMin = kGroupMax / 4;
    constexpr uint64_t kShardMin = 16'000, kShardMax = 64'000;
    constexpr unsigned kGroupCut = 4, kShardCut = 8;
    std::vector<Unit> units;
    Unit cur;
    const auto flush = [&] {
        if (!cur.entries.empty()) units.push_back(std::move(cur));
        cur = Unit{};
    };
    for (const auto& [entry, ff] : c.plan.fns) {
        const Tier tier = c.TierOf(entry);
        if (const auto sp = c.parts.find(entry); sp != c.parts.end()) {
            flush();
            for (size_t i = 0; i < sp->second.size(); ++i) {
                units.push_back(Unit{tier, {entry}, static_cast<int>(i), sp->second[i].words});
            }
            continue;
        }
        if (!cur.entries.empty() && cur.tier != tier) flush();
        cur.tier = tier;
        cur.entries.push_back(entry);
        cur.words += ff.words;
        const bool small = tier == Tier::C || tier == Tier::W;
        const uint64_t min = small ? kGroupMin : kShardMin, max = small ? kGroupMax : kShardMax;
        if ((small && !c.o.groups) || cur.words >= max || (cur.words >= min && CutAfter(c.image, entry, ff, small ? kGroupCut : kShardCut))) flush();
    }
    flush();
    return units;
}

std::string ToolIdentity() {
    std::string self;
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = '\0';
        self = cache::FileSha256(buf);
    }
    return std::string("nxaot.lock llvm ") + NXAOT_LOCK_LLVM + " dynarmic " + NXAOT_LOCK_DYNARMIC + "; LLVM " + LLVM_VERSION_STRING +
           "; executable " + (self.empty() ? "unknown" : self);
}

bool WriteFile(const std::string& path, const std::string& data, std::string* error) {
    std::ofstream out{path, std::ios::binary};
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out) {
        *error = "cannot write " + path;
        return false;
    }
    return true;
}

// The free pages of the heap returned to the system (glibc's malloc_trim: the analysis's freed tables otherwise stay resident while the workers
// allocate their own: README.md, "Memory at scale"); nothing elsewhere
void ReturnFreeMemory() {
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
}

unsigned CpusAvailable() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        const int n = CPU_COUNT(&set);
        if (n > 0) return static_cast<unsigned>(n);
    }
    const unsigned h = std::thread::hardware_concurrency();
    return h ? h : 1;
}

// The memory the system can give this process now without swapping, in bytes: Linux's MemAvailable, Windows' available physical memory; 0 when
// unknown. NXAOT_MEM_AVAILABLE_MIB (a test switch: tests/test_workers.py) stands for it
uint64_t MemoryAvailable() {
    if (const char* v = std::getenv("NXAOT_MEM_AVAILABLE_MIB"); v && *v) {
        return std::strtoull(v, nullptr, 10) << 20;
    }
#if defined(_WIN32)
    MEMORYSTATUSEX st{};
    st.dwLength = sizeof st;
    return GlobalMemoryStatusEx(&st) ? static_cast<uint64_t>(st.ullAvailPhys) : 0;
#else
    std::ifstream in{"/proc/meminfo"};
    std::string key;
    uint64_t kib = 0;
    std::string unit;
    while (in >> key >> kib >> unit) {
        if (key == "MemAvailable:") return kib << 10;
    }
    return 0;  // unknown
#endif
}

}  // namespace

const char* TierName(Tier t) {
    switch (t) {
    case Tier::C:
        return "C";
    case Tier::W:
        return "W";
    case Tier::H:
        return "H";
    default:
        return "H+";
    }
}

unsigned AnalysisThreads() {
    return CpusAvailable();
}

unsigned CompileWorkers(const BuildOptions& o, size_t units, const std::set<Tier>& tiers, size_t functions, std::string* line) {
    const unsigned cpus = CpusAvailable();
    unsigned n = o.workers;
    char buf[160];
    if (n) {
        std::snprintf(buf, sizeof buf, "aot: workers %u (--workers)", std::max<unsigned>(1, std::min<uint64_t>(n, units + 1)));
    } else {
        uint64_t each = 0;
        for (const Tier t : tiers) each = std::max(each, kWorkerPeak[static_cast<size_t>(t)]);
        const uint64_t tables = kTablesFixed + kTablesPerFunction * functions;
        const uint64_t available = MemoryAvailable();
        n = !available ? cpus : available > tables ? static_cast<unsigned>(std::min<uint64_t>(cpus, (available - tables) / each)) : 0;
        n = std::max<unsigned>(1, std::min<uint64_t>(n, units + 1));
        if (available) {
            std::snprintf(buf, sizeof buf, "aot: workers %u (%llu MiB available, about %llu MiB each, %llu MiB for the tables)", n,
                          static_cast<unsigned long long>(available >> 20), static_cast<unsigned long long>(each >> 20),
                          static_cast<unsigned long long>(tables >> 20));
        } else {
            std::snprintf(buf, sizeof buf, "aot: workers %u (the available memory unknown: one per CPU)", n);
        }
    }
    *line = buf;
    return std::max<unsigned>(1, std::min<uint64_t>(n, units + 1));
}

uint64_t AnalysisPeak(const module::Image& image) {
    return kAnalysisPerWord * (image.text_size / 4) + image.bytes.size() + kAnalysisFixed;
}

namespace {

// The parts of a split function joined into one relocatable object (ld.lld -r, the part holding the body's stub first): lld concatenates their
// code sections into one, which the pack's link places whole, so every part lies after the body at an offset known now. The line rows of the
// parts (ReadLineMarks: native from the part's start) become rows from the body's start, as the line table's runs are (ABI 3)
bool JoinParts(const Common& c, uint64_t entry, const std::vector<std::string>& parts, const std::string& joined, std::vector<LineRow>* rows,
               std::string* error) {
    if (!LinkRelocatable(parts, joined, error)) {
        return false;
    }
    auto buf = llvm::MemoryBuffer::getFile(joined);
    if (!buf) {
        *error = "cannot read " + joined + ": " + buf.getError().message();
        return false;
    }
    auto file = llvm::object::ObjectFile::createObjectFile((*buf)->getMemBufferRef());
    if (!file) {
        *error = joined + ": " + llvm::toString(file.takeError());
        return false;
    }
    const std::string body = BodyName(c, entry);
    std::optional<llvm::object::SectionRef> code;
    std::map<uint64_t, uint64_t> part_at;  // part lo -> its offset in the code section
    std::optional<uint64_t> body_at;
    for (const llvm::object::SymbolRef& s : (*file)->symbols()) {
        auto name = s.getName();
        auto value = s.getValue();
        auto sec = s.getSection();
        if (!name || !value || !sec) {
            if (!name) llvm::consumeError(name.takeError());
            if (!value) llvm::consumeError(value.takeError());
            if (!sec) llvm::consumeError(sec.takeError());
            continue;
        }
        bool std_entry = false;
        uint64_t part = LineRow::kNoPart;
        const auto fn = BodyOf(*name, c.prefix, &std_entry, &part);
        if (!fn || *fn != entry || std_entry || *sec == (*file)->section_end()) continue;
        if (code && **sec != *code) {
            *error = joined + ": the parts of " + body + " lie in more than one section";
            return false;
        }
        code = **sec;
        if (part == LineRow::kNoPart) body_at = *value;
        else part_at[part] = *value;
    }
    if (!body_at) {
        *error = joined + ": no " + body;
        return false;
    }
    for (LineRow& r : *rows) {
        if (r.part == LineRow::kNoPart) continue;
        const auto it = part_at.find(r.part);
        if (it == part_at.end() || it->second + r.native < *body_at || it->second + r.native - *body_at > UINT32_MAX) {
            *error = joined + ": a line mark of " + body + "'s part at " + llvm::utohexstr(r.part) + " outside the joined code";
            return false;
        }
        r.native = static_cast<uint32_t>(it->second + r.native - *body_at);
        r.part = LineRow::kNoPart;
    }
    return true;
}

// The tiers of section 9.3 by profile (--profile; README.md, "Tiers by profile"): in the balanced mode a function holding a sampled offset at
// tier H and the others at tier C ("tier H only with a profile"; C for "functions the profile never sampled"), in the max mode H+ and H.
// Without a profile every function has the mode's tier (no entries in *tiers)
bool TiersFromProfile(const BuildOptions& o, const Plan& plan, std::map<uint64_t, Tier>* tiers, BuildStats* s, std::string* error) {
    if (o.profile_path.empty()) {
        return true;
    }
    const Tier hot = o.mode == "max" ? Tier::HPlus : Tier::H, cold = o.mode == "max" ? Tier::H : Tier::C;
    std::ifstream in{o.profile_path};
    if (!in) {
        *error = "cannot read the profile " + o.profile_path;
        return false;
    }
    std::map<uint64_t, uint64_t> samples;  // offset -> count
    std::string line;
    unsigned number = 0;
    while (std::getline(in, line)) {
        ++number;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        llvm::StringRef rest = llvm::StringRef(line).trim();
        if (rest.empty()) continue;
        const size_t gap = rest.find_first_of(" \t");
        llvm::StringRef off_text = rest.take_front(gap).trim(), count_text = gap == llvm::StringRef::npos ? "" : rest.drop_front(gap).trim();
        off_text.consume_front_insensitive("0x");
        uint64_t offset = 0, count = 0;
        if (off_text.getAsInteger(16, offset)) {
            *error = o.profile_path + ":" + std::to_string(number) + ": the offset is no hexadecimal number";
            return false;
        }
        if (count_text.getAsInteger(10, count)) {
            *error = o.profile_path + ":" + std::to_string(number) + ": the count is no decimal number";
            return false;
        }
        samples[offset] += count;
        s->profile_samples += count;
    }
    s->profile_offsets = samples.size();
    for (const auto& [entry, ff] : plan.fns) {
        (*tiers)[entry] = cold;
    }
    for (const auto& [offset, count] : samples) {
        if (count == 0) continue;
        auto it = plan.fns.upper_bound(offset);
        if (it == plan.fns.begin() || offset >= std::prev(it)->second.limit) {
            ++s->profile_outside;
            continue;
        }
        (*tiers)[std::prev(it)->first] = hot;
    }
    return true;
}

// The analysis cache's key with the misses (--misses; README.md, "JIT misses"): the module's key (AnalysisKey) and what discovery reads of them, each address with
// its reasons (the entries' counts are not read: a later session's file with the same addresses keeps the analysis), and a test's --no-root addresses. Without
// either it is the module's key as it is, so the inputs of packs without misses keep their entries
std::string MissesKey(const std::string& akey, const BuildOptions& o) {
    if (o.misses.empty() && o.no_roots.empty()) return akey;
    std::set<std::pair<uint64_t, std::string>> rows;
    for (const auto& m : o.misses) rows.emplace(m.offset, m.reason);
    std::string list;
    for (const auto& [off, reason] : rows) list += std::to_string(off) + ":" + reason + ",";
    std::string held;
    for (const uint64_t a : o.no_roots) held += std::to_string(a) + ",";
    cache::KeyBuilder k;
    k.Add("analysis", akey).Add("misses", list).Add("no_roots", held);
    return k.Hex();
}

// The misses' resume points (design 3.3: analysis::Discovery::miss_resumes) as continuations of their function's call sites, as a setjmp's continuation is
// (FnFacts::continuations: the pending check after each call resumes there with every register from the block, when the SP is the frame's)
void AddMissResumes(const analysis::Discovery& d, Plan* plan) {
    for (const auto& [entry, points] : d.miss_resumes) {
        const auto it = plan->fns.find(entry);
        if (it == plan->fns.end()) continue;
        std::vector<uint64_t>& c = it->second.continuations;
        for (const uint64_t r : points) {
            if (std::find(c.begin(), c.end(), r) == c.end()) c.push_back(r);
        }
        std::sort(c.begin(), c.end());
    }
}

}  // namespace

bool ReadMisses(const std::string& path, std::vector<analysis::Miss>* out, std::string* sha256, std::string* error) {
    std::ifstream in{path};
    if (!in) {
        *error = "cannot read the misses " + path;
        return false;
    }
    out->clear();
    sha256->clear();
    std::string line;
    unsigned number = 0;
    while (std::getline(in, line)) {
        ++number;
        llvm::StringRef rest = llvm::StringRef(line).trim();
        if (rest.consume_front("#")) {
            // The header the host writes (as the profile's): # module <name>, build ID <id>, image SHA-256 <hex>
            const size_t at = rest.find("image SHA-256 ");
            if (rest.trim().starts_with("module ") && at != llvm::StringRef::npos) *sha256 = rest.drop_front(at + 14).trim().lower();
            continue;
        }
        if (rest.empty()) continue;
        llvm::SmallVector<llvm::StringRef, 4> f;
        rest.split(f, ' ', -1, false);
        if (f.size() == 1) rest.split(f, '\t', -1, false);
        llvm::StringRef off_text = f.empty() ? "" : f[0];
        off_text.consume_front_insensitive("0x");
        analysis::Miss m;
        if (f.size() != 3 || off_text.getAsInteger(16, m.offset) || f[2].getAsInteger(10, m.count) || f[1].empty()) {
            *error = path + ":" + std::to_string(number) + ": not `<module offset in hex> <reason> <entries>`";
            return false;
        }
        m.reason = f[1].str();
        out->push_back(std::move(m));
    }
    return true;
}

bool BuildObjects(const module::Image& image, const BuildOptions& o, const std::string& work_dir, std::vector<std::string>* objects,
                  BuildStats* stats, std::string* error) {
    BuildStats& s = *stats;
    s.module = image.name;

    // The analysis's threads (its result does not depend on how many there are): --workers, else one per CPU (its memory hardly depends on
    // them); the compile's workers are counted once the analysis is done and its units known (CompileWorkers)
    unsigned workers = o.workers ? o.workers : AnalysisThreads();
    if (llvm::TimePassesIsEnabled) {
        workers = 1;  // LLVM's pass timers are not thread-safe
    }
    if (!o.workers) {
        // The analysis cannot be split among fewer threads to need less: say so when the machine has less than its measured peak
        const uint64_t available = MemoryAvailable(), need = AnalysisPeak(image);
        if (available && available < need) {
            s.notes.push_back("[RED] nxaot build: the analysis of " + std::to_string(image.text_size / 4) + " instructions needs about " +
                              std::to_string(need >> 20) + " MiB, " + std::to_string(available >> 20) +
                              " MiB are available: the system may swap (README.md, \"Memory at scale\")");
        }
    }

    // ---- Analysis: discovery, the census's FPSR answer (decision 5), the convention facts; from the analysis cache when it holds this module's
    // (analysis_cache.h: design 13, a warm repack runs neither) ----
    lift::Convention conv;
    conv.cc = o.cc;
    std::optional<cache::Cache> cache;
    // --asm, --spills and --ir want every unit compiled (the cache keeps objects, not their assembly or IR)
    const bool use_cache = !o.cache_dir.empty() && !o.count_spills && o.asm_path.empty() && o.ir_path.empty() && !o.isel_misses;
    std::string tool;
    if (use_cache) {
        cache.emplace(o.cache_dir);
        if (!cache->Open(error)) {
            return false;
        }
        tool = ToolIdentity();
    }
    auto t = Clock::now();
    CachedAnalysis cached;
    analysis::Discovery fresh_d;
    Plan fresh_plan;
    bool reads_fpsr = false, warm = false;
    std::string akey, apath;
    if (use_cache) {
        akey = MissesKey(AnalysisKey(image, conv, o.all_entries, o.split ? o.split_above : UINT64_MAX, tool), o);
        apath = AnalysisPath(o.cache_dir, akey);
        std::string why;
        warm = LoadAnalysis(apath, akey, &cached, &why);
        s.analysis_cache = warm ? "hit" : "miss (" + why + ")";
        s.t_analysis_cache = Since(t);
        reads_fpsr = cached.reads_fpsr;
    }
    if (!warm) {
        analysis::DiscoverOptions dopts;
        dopts.threads = workers;
        dopts.misses = o.misses;      // the host's JIT feedback: roots of discovery (README.md, "JIT misses")
        dopts.no_roots = o.no_roots;
        fresh_d = analysis::Discover(image, dopts);
        s.discover_times = fresh_d.times;
        const auto tc = Clock::now();
        reads_fpsr = analysis::ReadsFpsr(image.bytes.data() + image.text_off, image.text_size);  // the census's answer.fpsr_read
        s.t_census = Since(tc);
    }
    const analysis::Discovery& d = warm ? cached.d : fresh_d;
    const bool fpsr = o.fpsr_flags == 1 || (o.fpsr_flags < 0 && reads_fpsr);
    s.fpsr_flags = fpsr;
    s.t_discover = Since(t);
    for (const auto& n : d.notes) {
        s.notes.push_back(n);
    }
    t = Clock::now();
    if (!warm) {
        // The analysis runs on the workers too, the same plan with any number of them; its threads and CPU seconds (discovery's from its
        // own times, the plan's from the process's) beside the stage line's wall seconds (README.md, "Phase 5c")
        const auto cpu = [] {
            struct rusage ru {};
            getrusage(RUSAGE_SELF, &ru);
            return static_cast<double>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) +
                   1e-6 * static_cast<double>(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec);
        };
        const double c0 = cpu();
        fresh_plan = MakePlan(image, d, conv, o.all_entries, workers);
        fresh_d.insns.reset();  // the instructions' effects: the plan's, not the compile's (README.md, "Memory at scale")
        AddMissResumes(d, &fresh_plan);   // kept with the plan in the analysis cache
        const auto& dt = d.times;
        char line[256];
        std::snprintf(line, sizeof line,
                      "aot: analysis on %u threads: discover CPU %.3f s, plan %.3f s wall, CPU %.3f s; summaries %llu rounds in %llu sweeps",
                      dt.threads, dt.cpu, Since(t), cpu() - c0, static_cast<unsigned long long>(dt.summary_rounds),
                      static_cast<unsigned long long>(dt.summary_sweeps));
        s.notes.push_back(line);
    }
    const Plan& plan = warm ? cached.plan : fresh_plan;
    s.t_plan = Since(t);
    if (use_cache && !warm) {
        const auto ts = Clock::now();
        std::string store_error;
        if (!StoreAnalysis(apath, akey, d, plan, reads_fpsr, o.split ? o.split_above : UINT64_MAX, &store_error)) {
            s.notes.push_back("[RED] nxaot build: the analysis cache: " + store_error);
        }
        s.t_analysis_cache += Since(ts);
    }

    // ---- The units and the workers; target machines per worker and tier, made here (LLVM's target registry is set up once, on this thread) ----
    std::unique_ptr<llvm::TargetMachine> tm0 = MakeTarget(o.cpu, o.tier, &s.cpu, &s.features, error);
    if (!tm0) {
        return false;
    }
    // The gates (design 1.1): the manifest's obs and spin rows the functions' code holds are served (the units call them, CompileUnit checks
    // it); a row no discovered code holds is not, and the host, which checks every row against the descriptor, says so as well
    Common c{image, o, d, plan, conv, Sanitize(image.name), fpsr, tm0->getTargetTriple().str(), s.cpu, s.features, TargetOf(*tm0), "",
             GatesOf(image), {}, {}, {}};
    if (o.split) {
        for (const auto& [entry, ff] : plan.fns) {
            std::vector<Part> parts = SplitParts(d.functions.at(entry), ff, o.split_above);
            if (!parts.empty()) {
                s.split_functions += 1;
                s.split_parts += parts.size();
                c.parts.emplace(entry, std::move(parts));
            }
        }
    }
    if (!TiersFromProfile(o, plan, &c.tiers, &s, error)) {
        return false;
    }
    const std::vector<Unit> units = Units(c);
    s.units = units.size();
    if (memory::On()) {
        uint64_t ub = memory::Of(units);
        for (const Unit& u : units) ub += memory::Of(u.entries);
        memory::Note("pack: units", ub, "the compile stage");
    }
    std::set<Tier> used{o.tier};
    for (const Unit& u : units) used.insert(u.tier);
    {
        std::map<uint64_t, nxaot_gate_site> served;
        for (const auto& [entry, f] : d.functions) {
            std::vector<nxaot_gate_site> sites = GateSitesOf(f, c.gates);
            for (const auto& g : sites) served[g.offset] = g;
            if (!sites.empty()) c.sites.emplace(entry, std::move(sites));
        }
        for (const auto& [off, g] : served) c.served.push_back(g);
        for (const auto& [off, arg] : c.gates.obs) {
            if (!served.count(off)) {
                s.notes.push_back("[RED] nxaot build: the observation gate at " + c.prefix + "+0x" + llvm::utohexstr(off) +
                                  " is in no discovered function's code: the pack does not call it (its observer will not run)");
            }
        }
        for (const auto& [off, head] : c.gates.spin) {
            if (!served.count(off)) {
                s.notes.push_back("[RED] nxaot build: the spin wait back-edge at " + c.prefix + "+0x" + llvm::utohexstr(off) +
                                  " is in no discovered function's code: the pack does not call its gate");
            }
        }
        s.gate_sites = c.served.size();
    }
    for (const auto& [entry, f] : d.functions) {
        for (const auto& [pc, target] : f.calls) {
            c.returns.push_back(static_cast<uint32_t>(pc + 4));
        }
    }
    std::sort(c.returns.begin(), c.returns.end());
    c.returns.erase(std::unique(c.returns.begin(), c.returns.end()), c.returns.end());
    // The compile reads nothing more of discovery's functions (their blocks were the analysis's largest tables: README.md, "Memory at scale"):
    // freed, and the pages the analysis's freed tables leave in the heap returned to the system before the workers start
    if (memory::On()) {
        const analysis::FunctionsBytes fb = analysis::BytesOf(d.functions);
        memory::Note("pack: discovery's functions until the compile", fb.other + fb.blocks, "the compile stage's start");
    }
    std::map<uint64_t, analysis::Function>().swap((warm ? cached.d : fresh_d).functions);
    ReturnFreeMemory();
    memory::Rss("discovery's functions freed");

    // The compile's workers: by the memory available now that the analysis is done (CompileWorkers), or --workers; their target machines
    std::string workers_line = "aot: workers 1 (--time-passes)";
    workers = llvm::TimePassesIsEnabled ? 1 : CompileWorkers(o, units.size(), used, plan.fns.size(), &workers_line);
    s.workers = workers;
    s.notes.push_back(workers_line);
    std::vector<std::array<std::unique_ptr<llvm::TargetMachine>, 4>> tms(workers);
    for (unsigned i = 0; i < workers; ++i) {
        for (const Tier t : used) {
            std::string cpu, features;
            if (i == 0 && t == o.tier) {
                tms[0][static_cast<size_t>(t)] = std::move(tm0);
                continue;
            }
            auto tm = MakeTarget(o.cpu, t, &cpu, &features, error);
            if (!tm) {
                return false;
            }
            tms[i][static_cast<size_t>(t)] = std::move(tm);
        }
    }

    c.tool = tool;
    if (memory::On()) {
        // What the compile keeps of the analysis: the plan, the split functions' parts, the gate sites and the returns
        uint64_t parts = memory::Of(c.parts) + memory::Of(c.sites) + memory::Of(c.returns) + memory::Of(c.tiers);
        for (const auto& [entry, ps] : c.parts) {
            parts += memory::Of(ps);
            for (const Part& part : ps) parts += memory::Of(part.entries);
        }
        for (const auto& [entry, ss] : c.sites) parts += memory::Of(ss);
        memory::Note("pack: the analysis kept for the compile (plan, parts, sites, returns)", BytesOf(plan) + parts, "the compile stage");
        memory::Rss("before the compile (the analysis kept)");
    }
    if (const std::error_code ec = llvm::sys::fs::create_directories(work_dir)) {
        *error = "cannot create " + work_dir + ": " + ec.message();
        return false;
    }

    // Jobs: the tables' object first (job 0), then the units, the largest first so that the last to finish are small ones; each unit's object
    // depends only on the unit, so the order and the number of workers change nothing in the pack
    std::vector<size_t> order(units.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return units[a].words > units[b].words; });
    struct Result {
        std::string path, error, ir, asm_text;
        UnitStats st;
        bool hit = false;
        uint64_t read = 0, written = 0;
        double t_key = 0;
        std::vector<LineRow> lines;  // the unit's line marks (ReadLineMarks)
        uint64_t lines_outside = 0;  // marks in no body
    };
    // A unit's object: its line marks read back and its marks' section kept out of the link (in the cache too: a hit is read the same way)
    const auto line_marks = [&](std::string* object, Result& res, bool fresh) {
        if (!o.line_table) return true;
        if (fresh && !MarkExcluded(object, &res.error)) return false;
        return ReadLineMarks(*object, c.prefix, &res.lines, &res.lines_outside, &res.error);
    };
    std::vector<Result> results(units.size() + 1);
    std::atomic<size_t> next{0};
    static const bool trace = std::getenv("NXAOT_TRACE") != nullptr;  // each unit's entry on stderr before it is compiled
    const auto work = [&](unsigned w) {
        for (size_t j; (j = next.fetch_add(1)) < results.size();) {
            const size_t r = j == 0 ? 0 : order[j - 1] + 1;
            Result& res = results[r];
            std::string object;
            if (r == 0) {
                if (CompileTables(c, *tms[w][static_cast<size_t>(o.tier)], &object, &res.st, &s, o.ir_path.empty() ? nullptr : &res.ir,
                                  o.asm_path.empty() ? nullptr : &res.asm_text, &res.error)) {
                    res.path = work_dir + "/tables.o";
                    WriteFile(res.path, object, &res.error);
                }
                memory::Rss("the tables' object compiled (the first job: alone with one worker)");
                continue;
            }
            const Unit& unit = units[r - 1];
            llvm::TargetMachine* tm = tms[w][static_cast<size_t>(unit.tier)].get();
            const uint64_t entry = unit.entries.front();
            std::string key;
            if (cache) {
                const auto tk = Clock::now();
                key = UnitKey(c, unit);
                res.t_key = Since(tk);
                cache::Facts facts;
                uint64_t bytes = 0;
                if (cache->Lookup(key, &facts, &bytes)) {
                    res.hit = true;
                    res.read = bytes;
                    res.st = UnitStats::FromFacts(facts);
                    res.path = cache->ObjectPath(key);
                    if (o.line_table) {
                        auto buf = llvm::MemoryBuffer::getFile(res.path);
                        if (!buf) {
                            res.error = "cannot read " + res.path + ": " + buf.getError().message();
                            continue;
                        }
                        std::string cached = (*buf)->getBuffer().str();
                        line_marks(&cached, res, false);
                    }
                    continue;
                }
            }
            if (trace) {
                std::fprintf(stderr, "nxaot: unit %#llx\n", static_cast<unsigned long long>(entry));
            }
            if (!CompileUnit(c, *tm, unit, &object, &res.st, o.ir_path.empty() ? nullptr : &res.ir, o.asm_path.empty() ? nullptr : &res.asm_text,
                             &res.error)) {
                continue;
            }
            if (!line_marks(&object, res, true)) {
                continue;
            }
            if (cache) {
                if (cache->Store(key, object, res.st.ToFacts(), &res.error)) {
                    res.path = cache->ObjectPath(key);
                    res.written = object.size();
                }
            } else {
                res.path = work_dir + "/f" + llvm::utohexstr(entry) + (unit.part >= 0 ? ".p" + std::to_string(unit.part) : "") + ".o";
                WriteFile(res.path, object, &res.error);
            }
        }
    };
    t = Clock::now();
    if (workers == 1) {
        work(0);
    } else {
        std::vector<std::thread> pool;
        for (unsigned i = 0; i < workers; ++i) {
            pool.emplace_back(work, i);
        }
        for (auto& th : pool) {
            th.join();
        }
    }
    s.t_compile_wall = Since(t);
    ReturnFreeMemory();  // the workers' freed memory (each thread's malloc arena keeps its own): README.md, "Memory at scale"
    if (memory::On()) {
        uint64_t rb = memory::Of(results);
        for (const Result& res : results) {
            rb += memory::Of(res.lines) + memory::Of(res.path) + memory::Of(res.error) + memory::Of(res.ir) + memory::Of(res.asm_text);
        }
        memory::Note("pack: the units' results (line rows, paths)", rb, "the end of the compile");
        memory::Rss("after the compile on " + std::to_string(workers) + " workers");
    }

    // ---- The results, in link order: the tables' object, then the units by entry (a split function's parts joined into one object), then
    // the line table's ----
    std::string ir, asm_text;
    LineRuns lines;
    std::vector<std::string> part_objects;
    std::vector<LineRow> part_rows;
    for (size_t r = 0; r < results.size(); ++r) {
        Result& res = results[r];
        if (!res.error.empty()) {
            *error = res.error;
            return false;
        }
        const Unit* unit = r ? &units[r - 1] : nullptr;
        if (unit && unit->part >= 0) {
            // A part of a split function: its parts' objects are joined (ld.lld -r, in part order: the body's stub first) into one object whose
            // code is one section, so that the line table's offsets from the body hold for every part (SplitObject)
            part_objects.push_back(res.path);
            part_rows.insert(part_rows.end(), res.lines.begin(), res.lines.end());
            if (static_cast<size_t>(unit->part) + 1 == c.parts.at(unit->entries.front()).size()) {
                const std::string joined = work_dir + "/split" + llvm::utohexstr(unit->entries.front()) + ".o";
                const auto tj = Clock::now();
                if (!JoinParts(c, unit->entries.front(), part_objects, joined, &part_rows, error)) {
                    return false;
                }
                s.t_join += Since(tj);
                objects->push_back(joined);
                AddLineRows(lines, part_rows);
                part_objects.clear();
                part_rows.clear();
            }
        } else {
            objects->push_back(res.path);
            AddLineRows(lines, res.lines);
        }
        res.lines.clear();
        res.lines.shrink_to_fit();
        s.lines_lost += res.lines_outside;
        const UnitStats& u = res.st;
        s.blocks += u.blocks;
        s.calls += u.calls;
        s.indirect_calls += u.indirect_calls;
        s.tail_calls += u.tail_calls;
        s.indirect_tails += u.indirect_tails;
        s.returns += u.returns;
        s.svcs += u.svcs;
        s.traps += u.traps;
        s.splits += u.splits;
        s.refused += u.refused;
        s.lines_lost += u.lines_lost;
        s.spills += u.spills;
        s.reloads += u.reloads;
        s.text_bytes += u.text_bytes;
        s.object_bytes += u.object_bytes;
        s.t_lift += u.t_lift;
        s.t_verify += u.t_verify;
        s.t_opt += u.t_opt;
        s.t_codegen += u.t_codegen;
        s.t_key += res.t_key;
        for (const auto& n : u.notes) {
            s.notes.push_back(n);
        }
        for (const auto& [kind, n] : u.isel_misses) {
            s.isel_misses[kind].first += n.first;
            s.isel_misses[kind].second += n.second;
        }
        if (r) {
            s.cache_hits += res.hit;
            (res.hit ? s.cache_hit_functions : s.cache_miss_functions) += cache && unit->part <= 0 ? unit->entries.size() : 0;
            s.cache_misses += cache && !res.hit;
            s.cache_bytes_read += res.read;
            s.cache_bytes_written += res.written;
        }
        ir += res.ir;
        asm_text += res.asm_text;
    }
    if (memory::On()) {
        uint64_t b = memory::Of(lines) + memory::Of(part_rows);
        for (const auto& [entry, run] : lines) b += memory::Of(run);
        memory::Note("pack: the line rows of every unit, by function", b, "the results in link order");
    }
    if (o.line_table) {
        const auto tl = Clock::now();
        UnitStats ls;
        std::string object;
        if (!CompileLines(c, *tms[0][static_cast<size_t>(o.tier)], lines, &object, &ls, &s.line_entries, error)) {
            return false;
        }
        const std::string path = work_dir + "/lines.o";
        if (!WriteFile(path, object, error)) {
            return false;
        }
        objects->push_back(path);
        s.object_bytes += ls.object_bytes;
        s.t_compile_wall += Since(tl);
        memory::Rss("after the line table");
    }
    if (o.windows) {
        // imports.o: the run-time entries the objects call, their thunks and slots, and the table the host binds ("Windows packs")
        UnitStats is;
        std::string object;
        if (!CompileImports(c, *tms[0][static_cast<size_t>(o.tier)], *objects, &object, &is, &s.imports, error)) {
            return false;
        }
        for (const std::string& n : is.notes) {
            if (n.rfind("[RED] ", 0) == 0) {
                *error = "imports.o: " + n;  // the thunks' assembly did not assemble
                return false;
            }
            s.notes.push_back(n);
        }
        const std::string path = work_dir + "/imports.o";
        if (!WriteFile(path, object, error)) {
            return false;
        }
        objects->push_back(path);
        s.object_bytes += is.object_bytes;
    }
    if (!o.ir_path.empty() && !WriteFile(o.ir_path, ir, error)) {
        return false;
    }
    if (!o.asm_path.empty() && !WriteFile(o.asm_path, asm_text, error)) {
        return false;
    }
    struct rusage ru {};
    getrusage(RUSAGE_SELF, &ru);
    s.peak_rss_kib = static_cast<uint64_t>(ru.ru_maxrss);
    if (llvm::TimePassesIsEnabled) {
        llvm::reportAndResetTimings(&llvm::errs());
        // The timers inside passes (SelectionDAG's phases, the AsmPrinter's handlers: DWARF, the line marks' metadata), which
        // reportAndResetTimings leaves to the end of the process
        llvm::TimerGroup::printAll(llvm::errs());
    }
    return true;
}

bool BuildObject(const module::Image& image, const BuildOptions& o, const std::string& object_path, BuildStats* stats, std::string* error) {
    const std::string work = object_path + ".parts";
    std::vector<std::string> objects;
    bool ok = BuildObjects(image, o, work, &objects, stats, error);
    if (ok) {
        const auto t = Clock::now();
        ok = LinkRelocatable(objects, object_path, error);
        stats->t_link += Since(t);
    }
    llvm::sys::fs::remove_directories(work);
    return ok;
}

bool LinkShared(const std::vector<std::string>& objects, const std::string& output, const std::string& soname, std::string* error) {
    // The compile's freed memory (the plan, the units' results, the line rows) returned to the system first: lld's inputs and output take
    // their place (README.md, "Memory at scale")
    ReturnFreeMemory();
    memory::Rss("before the link");
    std::vector<const char*> args{"ld.lld", "-shared", "--hash-style=gnu", "-z", "now", "-z", "relro", "-z", "noexecstack", "--eh-frame-hdr",
                                  "-soname", soname.c_str(), "-o", output.c_str()};
    for (const auto& o : objects) {
        args.push_back(o.c_str());
    }
    std::string out, err;
    llvm::raw_string_ostream out_s{out}, err_s{err};
    const lld::Result r = lld::lldMain(args, out_s, err_s, {{lld::Gnu, &lld::elf::link}});
    if (r.retCode != 0) {
        *error = "lld: " + err_s.str();
        return false;
    }
    return true;
}

bool LinkRelocatable(const std::vector<std::string>& objects, const std::string& output, std::string* error) {
    std::vector<const char*> args{"ld.lld", "-r", "-o", output.c_str()};
    for (const auto& o : objects) {
        args.push_back(o.c_str());
    }
    std::string out, err;
    llvm::raw_string_ostream out_s{out}, err_s{err};
    const lld::Result r = lld::lldMain(args, out_s, err_s, {{lld::Gnu, &lld::elf::link}});
    if (r.retCode != 0) {
        *error = "lld -r: " + err_s.str();
        return false;
    }
    return true;
}

bool LinkWindows(const std::vector<std::string>& objects, const std::string& work_dir, const std::string& dll, const std::string& coff_object,
                 bool debug_info, BuildStats* stats, std::string* error) {
    // One relocatable ELF object of the whole module (ld.lld -r keeps one copy of each comdat group: the shared helpers of the units), in COFF
    if (const std::error_code ec = llvm::sys::fs::create_directories(work_dir)) {
        *error = "cannot create " + work_dir + ": " + ec.message();
        return false;
    }
    const std::string whole = work_dir + "/whole.o";
    if (!LinkRelocatable(objects, whole, error)) {
        return false;
    }
    auto buf = llvm::MemoryBuffer::getFile(whole);
    if (!buf) {
        *error = "cannot read " + whole + ": " + buf.getError().message();
        return false;
    }
    std::string coff;
    CoffStats cs;
    if (!ElfToCoff((*buf)->getBuffer(), debug_info, &coff, &cs, error)) {
        return false;
    }
    stats->coff_sections = cs.sections;
    stats->coff_symbols = cs.symbols;
    stats->coff_relocations = cs.relocations;
    {  // The unwind information translated from the CFI (coff.cpp, "Unwind information"): what Windows debuggers, profilers and RtlVirtualUnwind see
        const CoffStats::UnwindCounts& u = cs.unwind;
        std::string note = "aot: windows: unwind info for " + std::to_string(u.described) + " of " + std::to_string(u.functions) + " functions, " +
                           std::to_string(u.not_described) + " not describable: " + (u.not_described ? u.why_not : "none");
        if (u.without_cfi) {
            note += "; " + std::to_string(u.without_cfi) + " without CFI, which Windows unwinds as leaf functions: " + u.without_cfi_names;
        }
        note += "; " + std::to_string(u.entries) + " RUNTIME_FUNCTION entries";
        if (u.split) note += ", " + std::to_string(u.split) + " functions in several (a stretch with a frame of its own): " + u.split_why;
        stats->notes.push_back(note);
    }
    const std::string obj = coff_object.empty() ? work_dir + "/whole.obj" : coff_object;
    if (!WriteFile(obj, coff, error)) {
        return false;
    }
    if (dll.empty()) {
        return true;
    }
    // The DLL: lld's COFF driver as a library. No entry point and no C library: the DLL runs nothing when it is loaded and imports from no
    // module (its run-time entries are bound by the host: "Windows packs"); every section kept (/opt:noref: as the ELF link keeps them, and
    // the descriptor reaches what it needs anyway); the time stamp from the contents (/Brepro), so a pack is the same from build to build;
    // the export table holds the descriptor and the import table; DWARF kept as a MinGW program keeps it (/debug:dwarf), no PDB
    const std::string out = "/out:" + dll;
    std::vector<const char*> args{"lld-link",     "/nologo",     "/dll",     "/noentry",       "/nodefaultlib",
                                  "/machine:x64", "/noimplib",   "/Brepro",  "/incremental:no", "/opt:noref",
                                  "/opt:noicf",   "/export:nxaot_module,DATA", "/export:nxaot_imports,DATA", out.c_str()};
    if (debug_info) {
        args.push_back("/debug:dwarf");
    }
    args.push_back(obj.c_str());
    std::string so, se;
    llvm::raw_string_ostream out_s{so}, err_s{se};
    const lld::Result r = lld::lldMain(args, out_s, err_s, {{lld::WinLink, &lld::coff::link}});
    if (r.retCode != 0) {
        *error = "lld-link: " + err_s.str() + out_s.str();
        return false;
    }
    if (!err_s.str().empty()) {
        stats->notes.push_back("nxaot build: lld-link: " + err_s.str());  // warnings
    }
    return true;
}

}  // namespace nxaot::build
