// The cryptographic extension (aot-recompiler.md section 7.2, row 17): AES rounds as AES-NI (AESE is AESENCLAST with a zero key after the
// frontend's XOR, AESMC is AESDECLAST then AESENC, as dynarmic's x86-64 backend composes them), SHA-256 with SHA-NI in dynarmic's register
// arrangement, and on targets without them dynarmic's own definitions: its software AES (lift/runtime.h) and its SHA-256 polyfill, which the
// lifter runs on the IR block before lowering (FunctionLifter::Discover). SM4's substitution box is a constant table.
#include <llvm/IR/Constants.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsX86.h>

#include "lift/function_lifter.h"
#include "lift/runtime.h"

namespace nxaot::lift::detail {

using IR::Opcode;

bool FunctionLifter::LowerCrypto(IR::Inst& inst) {
    const Opcode op = inst.GetOpcode();
    switch (op) {
    case Opcode::AESDecryptSingleRound:
    case Opcode::AESEncryptSingleRound:
    case Opcode::AESInverseMixColumns:
    case Opcode::AESMixColumns: {
        Value* a = ArgVec(inst, 0, 64);
        if (opts.target.aes) {
            Value* zero = llvm::Constant::getNullValue(V128());
            switch (op) {
            case Opcode::AESDecryptSingleRound:
                Def(inst, Call(llvm::Intrinsic::x86_aesni_aesdeclast, {}, {a, zero}));
                break;
            case Opcode::AESEncryptSingleRound:
                Def(inst, Call(llvm::Intrinsic::x86_aesni_aesenclast, {}, {a, zero}));
                break;
            case Opcode::AESInverseMixColumns:
                Def(inst, Call(llvm::Intrinsic::x86_aesni_aesimc, {}, {a}));
                break;
            default:
                Def(inst, Call(llvm::Intrinsic::x86_aesni_aesenc, {}, {Call(llvm::Intrinsic::x86_aesni_aesdeclast, {}, {a, zero}), zero}));
                break;
            }
            return true;
        }
        const uint32_t which = op == Opcode::AESDecryptSingleRound ? 0 : op == Opcode::AESEncryptSingleRound ? 1 : op == Opcode::AESInverseMixColumns ? 2 : 3;
        llvm::AllocaInst* in = nullptr;
        llvm::AllocaInst* out = nullptr;
        {
            llvm::IRBuilderBase::InsertPointGuard guard{B};
            B.SetInsertPoint(&F->getEntryBlock(), F->getEntryBlock().begin());
            in = B.CreateAlloca(V128(), nullptr, "aes_in");
            out = B.CreateAlloca(V128(), nullptr, "aes_out");
        }
        B.CreateStore(a, in);
        HelperCall("nxaot_rt_aes", B.getVoidTy(), {B.getInt32(which), out, in});
        Def(inst, B.CreateLoad(V128(), out));
        return true;
    }
    case Opcode::SHA256Hash: {
        // sha256h / sha256h2: x = {a, b, c, d} (lane 0 first), y = {e, f, g, h}, w = the four wk values. SHA256RNDS2 does two rounds on
        // the state split as {c, d, g, h} and {a, b, e, f}; dynarmic's arrangement (emit_x64_sha.cpp)
        if (!opts.target.sha) {
            error = "SHA256Hash on a target without SHA (the polyfill should have replaced it)";
            return true;
        }
        const bool part1 = inst.GetArg(3).GetU1();
        Value* x = ArgVec(inst, 0, 32);
        Value* y = ArgVec(inst, 1, 32);
        Value* w = ArgVec(inst, 2, 32);
        Value* src1 = Shuffle(y, x, {3, 2, 7, 6});
        Value* src2 = Shuffle(y, x, {1, 0, 5, 4});
        Value* t = Call(llvm::Intrinsic::x86_sha256rnds2, {}, {src1, src2, w});
        Value* w_hi = Shuffle(w, w, {2, 3, 2, 3});
        Value* u = Call(llvm::Intrinsic::x86_sha256rnds2, {}, {src2, t, w_hi});
        Def(inst, part1 ? Shuffle(u, t, {3, 2, 7, 6}) : Shuffle(u, t, {1, 0, 5, 4}));
        return true;
    }
    case Opcode::SHA256MessageSchedule0:
        if (!opts.target.sha) {
            error = "SHA256MessageSchedule0 on a target without SHA";
            return true;
        }
        Def(inst, Call(llvm::Intrinsic::x86_sha256msg1, {}, {ArgVec(inst, 0, 32), ArgVec(inst, 1, 32)}));
        return true;
    case Opcode::SHA256MessageSchedule1: {
        if (!opts.target.sha) {
            error = "SHA256MessageSchedule1 on a target without SHA";
            return true;
        }
        Value* x = ArgVec(inst, 0, 32);
        Value* y = ArgVec(inst, 1, 32);
        Value* z = ArgVec(inst, 2, 32);
        Value* t = Shuffle(y, z, {1, 2, 3, 4});  // palignr(z, y, 4): y1 y2 y3 z0
        Def(inst, Call(llvm::Intrinsic::x86_sha256msg2, {}, {B.CreateAdd(x, t), z}));
        return true;
    }
    case Opcode::SM4AccessSubstitutionBox: {
        llvm::GlobalVariable* table = M.getGlobalVariable("nxaot.sm4.sbox", true);
        if (!table) {
            std::vector<uint8_t> entries(256);
            for (unsigned i = 0; i < 256; ++i) {
                entries[i] = Sm4Sbox(static_cast<uint8_t>(i));
            }
            auto* init = llvm::ConstantDataArray::get(C, entries);
            table = new llvm::GlobalVariable(M, init->getType(), true, llvm::GlobalValue::PrivateLinkage, init, "nxaot.sm4.sbox");
        }
        Value* index = B.CreateZExt(Arg(inst, 0), B.getInt64Ty());
        Def(inst, B.CreateAlignedLoad(B.getInt8Ty(), B.CreateInBoundsGEP(table->getValueType(), table, {B.getInt64(0), index}), llvm::Align(1)));
        return true;
    }
    default:
        return false;
    }
}

}  // namespace nxaot::lift::detail
