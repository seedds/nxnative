#include "lift/opcodes.h"

#include <string>

namespace nxaot::lift {

using Dynarmic::IR::Opcode;

namespace {

bool Lowered(Opcode op) {
    switch (op) {
    // Context: registers, flags, system registers (lifter.cpp, LowerContext)
    case Opcode::Void:
    case Opcode::Identity:
    case Opcode::PushRSB:
    case Opcode::A64SetCheckBit:
    case Opcode::A64GetCFlag:
    case Opcode::A64GetNZCVRaw:
    case Opcode::A64SetNZCVRaw:
    case Opcode::A64SetNZCV:
    case Opcode::A64GetW:
    case Opcode::A64GetX:
    case Opcode::A64GetSP:
    case Opcode::A64GetFPCR:
    case Opcode::A64GetFPSR:
    case Opcode::A64SetW:
    case Opcode::A64SetX:
    case Opcode::A64SetSP:
    case Opcode::A64SetFPCR:
    case Opcode::A64SetFPSR:
    case Opcode::A64SetPC:
    case Opcode::A64CallSupervisor:
    case Opcode::A64ExceptionRaised:
    case Opcode::A64DataCacheOperationRaised:
    case Opcode::A64InstructionCacheOperationRaised:
    case Opcode::A64DataSynchronizationBarrier:
    case Opcode::A64DataMemoryBarrier:
    case Opcode::A64InstructionSynchronizationBarrier:
    case Opcode::A64GetCNTFRQ:
    case Opcode::A64GetCNTPCT:
    case Opcode::A64GetCTR:
    case Opcode::A64GetDCZID:
    case Opcode::A64GetTPIDR:
    case Opcode::A64GetTPIDRRO:
    case Opcode::A64SetTPIDR:
    // Flags: pseudo-operations and conversions (LowerFlags)
    case Opcode::GetNZCVFromOp:
    case Opcode::NZCVFromPackedFlags:
    // Integer calculations (LowerInteger)
    case Opcode::Pack2x32To1x64:
    case Opcode::Pack2x64To1x128:
    case Opcode::LeastSignificantWord:
    case Opcode::LeastSignificantHalf:
    case Opcode::LeastSignificantByte:
    case Opcode::MostSignificantWord:
    case Opcode::IsZero32:
    case Opcode::IsZero64:
    case Opcode::TestBit:
    case Opcode::ConditionalSelect32:
    case Opcode::ConditionalSelect64:
    case Opcode::ConditionalSelectNZCV:
    case Opcode::LogicalShiftLeft32:
    case Opcode::LogicalShiftLeft64:
    case Opcode::LogicalShiftRight32:
    case Opcode::LogicalShiftRight64:
    case Opcode::ArithmeticShiftRight32:
    case Opcode::ArithmeticShiftRight64:
    case Opcode::RotateRight32:
    case Opcode::RotateRight64:
    case Opcode::LogicalShiftLeftMasked32:
    case Opcode::LogicalShiftLeftMasked64:
    case Opcode::LogicalShiftRightMasked32:
    case Opcode::LogicalShiftRightMasked64:
    case Opcode::ArithmeticShiftRightMasked32:
    case Opcode::ArithmeticShiftRightMasked64:
    case Opcode::RotateRightMasked32:
    case Opcode::RotateRightMasked64:
    case Opcode::Add32:
    case Opcode::Add64:
    case Opcode::Sub32:
    case Opcode::Sub64:
    case Opcode::Mul32:
    case Opcode::Mul64:
    case Opcode::SignedMultiplyHigh64:
    case Opcode::UnsignedMultiplyHigh64:
    case Opcode::UnsignedDiv32:
    case Opcode::UnsignedDiv64:
    case Opcode::SignedDiv32:
    case Opcode::SignedDiv64:
    case Opcode::And32:
    case Opcode::And64:
    case Opcode::AndNot32:
    case Opcode::AndNot64:
    case Opcode::Eor32:
    case Opcode::Eor64:
    case Opcode::Or32:
    case Opcode::Or64:
    case Opcode::Not32:
    case Opcode::Not64:
    case Opcode::SignExtendByteToWord:
    case Opcode::SignExtendHalfToWord:
    case Opcode::SignExtendByteToLong:
    case Opcode::SignExtendHalfToLong:
    case Opcode::SignExtendWordToLong:
    case Opcode::ZeroExtendByteToWord:
    case Opcode::ZeroExtendHalfToWord:
    case Opcode::ZeroExtendByteToLong:
    case Opcode::ZeroExtendHalfToLong:
    case Opcode::ZeroExtendWordToLong:
    case Opcode::ByteReverseWord:
    case Opcode::ByteReverseDual:
    case Opcode::CountLeadingZeros32:
    case Opcode::CountLeadingZeros64:
    case Opcode::ExtractRegister32:
    case Opcode::ExtractRegister64:
    case Opcode::ReplicateBit32:
    case Opcode::ReplicateBit64:
    case Opcode::CRC32Castagnoli8:
    case Opcode::CRC32Castagnoli16:
    case Opcode::CRC32Castagnoli32:
    case Opcode::CRC32Castagnoli64:
    case Opcode::CRC32ISO8:
    case Opcode::CRC32ISO16:
    case Opcode::CRC32ISO32:
    case Opcode::CRC32ISO64:
    // The one vector opcode integer code needs: ldxp / ldaxp of two X registers split their 128-bit load with it (load_store_exclusive.cpp)
    case Opcode::VectorGetElement64:
    // Memory (LowerMemory)
    case Opcode::A64ClearExclusive:
    case Opcode::A64ReadMemory8:
    case Opcode::A64ReadMemory16:
    case Opcode::A64ReadMemory32:
    case Opcode::A64ReadMemory64:
    case Opcode::A64ExclusiveReadMemory8:
    case Opcode::A64ExclusiveReadMemory16:
    case Opcode::A64ExclusiveReadMemory32:
    case Opcode::A64ExclusiveReadMemory64:
    case Opcode::A64ExclusiveReadMemory128:
    case Opcode::A64WriteMemory8:
    case Opcode::A64WriteMemory16:
    case Opcode::A64WriteMemory32:
    case Opcode::A64WriteMemory64:
    case Opcode::A64ExclusiveWriteMemory8:
    case Opcode::A64ExclusiveWriteMemory16:
    case Opcode::A64ExclusiveWriteMemory32:
    case Opcode::A64ExclusiveWriteMemory64:
    case Opcode::A64ExclusiveWriteMemory128:
        return true;
    default:
        return false;
    }
}

const char* const kNames[] = {
#define OPCODE(name, type, ...) #name,
#define A32OPC(name, type, ...) "A32" #name,
#define A64OPC(name, type, ...) "A64" #name,
#include "dynarmic/ir/opcodes.inc"
#undef OPCODE
#undef A32OPC
#undef A64OPC
};
static_assert(sizeof(kNames) / sizeof(kNames[0]) == Dynarmic::IR::OpcodeCount);

bool StartsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

}  // namespace

OpcodeGroup GroupOf(Opcode op) {
    if (Lowered(op)) {
        return OpcodeGroup::Lowered;
    }
    const std::string name = OpcodeName(op);
    // The AArch32 frontend's own opcodes, and the shared ones only it emits: no file of frontend/A64 emits the packed SIMD-within-a-register
    // arithmetic, ssat / usat, the GE flags, the carry and overflow pseudo-operations, nor these four (A64 blocks hold a GetCarryFromOp only unused,
    // after MostSignificantWord, and dead-code elimination removes it before the lifter looks)
    static const char* const a32[] = {"SignedSaturation", "UnsignedSaturation", "GetGEFromOp", "GetCarryFromOp", "GetOverflowFromOp", "GetCFlagFromNZCV",
                                      "MostSignificantBit", "RotateRightExtended", "ByteReverseHalf"};
    if (StartsWith(name, "A32") || StartsWith(name, "Packed")) {
        return OpcodeGroup::A32;
    }
    for (const char* n : a32) {
        if (name == n) {
            return OpcodeGroup::A32;
        }
    }
    // Vector, FP and crypto opcodes, the 128-bit register accessors and memory accesses (ldr / str q), the saturating arithmetic of the SIMD scalar
    // instructions, the pseudo-operations of the widening vector multiplies, and the integer max / min only the SIMD translators emit
    static const char* const fp_simd[] = {"Vector", "FP", "ZeroVector", "A64GetS", "A64GetD", "A64GetQ", "A64SetS", "A64SetD", "A64SetQ", "AES", "SHA",
                                          "SM4", "SignedSaturated", "UnsignedSaturated", "GetUpperFromOp", "GetLowerFromOp", "MaxSigned", "MaxUnsigned",
                                          "MinSigned", "MinUnsigned", "ZeroExtendLongToQuad", "A64ReadMemory128", "A64WriteMemory128"};
    for (const char* prefix : fp_simd) {
        if (StartsWith(name, prefix)) {
            return OpcodeGroup::FpSimd;
        }
    }
    return OpcodeGroup::Other;
}

const char* OpcodeName(Opcode op) {
    const auto i = static_cast<size_t>(op);
    return i < Dynarmic::IR::OpcodeCount ? kNames[i] : "?";
}

const char* GroupName(OpcodeGroup group) {
    switch (group) {
    case OpcodeGroup::Lowered:
        return "lowered";
    case OpcodeGroup::FpSimd:
        return "fp_simd";
    case OpcodeGroup::A32:
        return "a32";
    case OpcodeGroup::Other:
        return "other";
    }
    return "?";
}

}  // namespace nxaot::lift
