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
    // Vector registers, 128-bit memory and the 128-bit helpers (lifter.cpp, LowerVectorRegisters, LowerMemory; lower_vector.cpp)
    case Opcode::A64GetS:
    case Opcode::A64GetD:
    case Opcode::A64GetQ:
    case Opcode::A64SetS:
    case Opcode::A64SetD:
    case Opcode::A64SetQ:
    case Opcode::ZeroExtendLongToQuad:
    case Opcode::ZeroVector:
    case Opcode::A64ReadMemory128:
    case Opcode::A64WriteMemory128:
    // Saturating arithmetic, scalar and vector, with FPSR.QC (lower_vector.cpp, LowerSaturated), and the scalar max / min of the across-lanes forms
    case Opcode::MaxSigned32:
    case Opcode::MaxUnsigned32:
    case Opcode::MinSigned32:
    case Opcode::MinUnsigned32:
    case Opcode::SignedSaturatedAdd8:
    case Opcode::SignedSaturatedAdd16:
    case Opcode::SignedSaturatedAdd32:
    case Opcode::SignedSaturatedAdd64:
    case Opcode::SignedSaturatedDoublingMultiplyReturnHigh16:
    case Opcode::SignedSaturatedDoublingMultiplyReturnHigh32:
    case Opcode::SignedSaturatedSub8:
    case Opcode::SignedSaturatedSub16:
    case Opcode::SignedSaturatedSub32:
    case Opcode::SignedSaturatedSub64:
    case Opcode::UnsignedSaturatedAdd8:
    case Opcode::UnsignedSaturatedAdd16:
    case Opcode::UnsignedSaturatedAdd32:
    case Opcode::UnsignedSaturatedAdd64:
    case Opcode::UnsignedSaturatedSub8:
    case Opcode::UnsignedSaturatedSub16:
    case Opcode::UnsignedSaturatedSub32:
    case Opcode::UnsignedSaturatedSub64:
    case Opcode::VectorSignedSaturatedAdd8:
    case Opcode::VectorSignedSaturatedAdd16:
    case Opcode::VectorSignedSaturatedAdd32:
    case Opcode::VectorSignedSaturatedAdd64:
    case Opcode::VectorSignedSaturatedSub8:
    case Opcode::VectorSignedSaturatedSub16:
    case Opcode::VectorSignedSaturatedSub32:
    case Opcode::VectorSignedSaturatedSub64:
    case Opcode::VectorUnsignedSaturatedAdd8:
    case Opcode::VectorUnsignedSaturatedAdd16:
    case Opcode::VectorUnsignedSaturatedAdd32:
    case Opcode::VectorUnsignedSaturatedAdd64:
    case Opcode::VectorUnsignedSaturatedSub8:
    case Opcode::VectorUnsignedSaturatedSub16:
    case Opcode::VectorUnsignedSaturatedSub32:
    case Opcode::VectorUnsignedSaturatedSub64:
    case Opcode::VectorSignedSaturatedAbs8:
    case Opcode::VectorSignedSaturatedAbs16:
    case Opcode::VectorSignedSaturatedAbs32:
    case Opcode::VectorSignedSaturatedAbs64:
    case Opcode::VectorSignedSaturatedAccumulateUnsigned8:
    case Opcode::VectorSignedSaturatedAccumulateUnsigned16:
    case Opcode::VectorSignedSaturatedAccumulateUnsigned32:
    case Opcode::VectorSignedSaturatedAccumulateUnsigned64:
    case Opcode::VectorSignedSaturatedDoublingMultiplyHigh16:
    case Opcode::VectorSignedSaturatedDoublingMultiplyHigh32:
    case Opcode::VectorSignedSaturatedDoublingMultiplyHighRounding16:
    case Opcode::VectorSignedSaturatedDoublingMultiplyHighRounding32:
    case Opcode::VectorSignedSaturatedDoublingMultiplyLong16:
    case Opcode::VectorSignedSaturatedDoublingMultiplyLong32:
    case Opcode::VectorSignedSaturatedNarrowToSigned16:
    case Opcode::VectorSignedSaturatedNarrowToSigned32:
    case Opcode::VectorSignedSaturatedNarrowToSigned64:
    case Opcode::VectorSignedSaturatedNarrowToUnsigned16:
    case Opcode::VectorSignedSaturatedNarrowToUnsigned32:
    case Opcode::VectorSignedSaturatedNarrowToUnsigned64:
    case Opcode::VectorSignedSaturatedNeg8:
    case Opcode::VectorSignedSaturatedNeg16:
    case Opcode::VectorSignedSaturatedNeg32:
    case Opcode::VectorSignedSaturatedNeg64:
    case Opcode::VectorSignedSaturatedShiftLeft8:
    case Opcode::VectorSignedSaturatedShiftLeft16:
    case Opcode::VectorSignedSaturatedShiftLeft32:
    case Opcode::VectorSignedSaturatedShiftLeft64:
    case Opcode::VectorSignedSaturatedShiftLeftUnsigned8:
    case Opcode::VectorSignedSaturatedShiftLeftUnsigned16:
    case Opcode::VectorSignedSaturatedShiftLeftUnsigned32:
    case Opcode::VectorSignedSaturatedShiftLeftUnsigned64:
    case Opcode::VectorUnsignedSaturatedAccumulateSigned8:
    case Opcode::VectorUnsignedSaturatedAccumulateSigned16:
    case Opcode::VectorUnsignedSaturatedAccumulateSigned32:
    case Opcode::VectorUnsignedSaturatedAccumulateSigned64:
    case Opcode::VectorUnsignedSaturatedNarrow16:
    case Opcode::VectorUnsignedSaturatedNarrow32:
    case Opcode::VectorUnsignedSaturatedNarrow64:
    case Opcode::VectorUnsignedSaturatedShiftLeft8:
    case Opcode::VectorUnsignedSaturatedShiftLeft16:
    case Opcode::VectorUnsignedSaturatedShiftLeft32:
    case Opcode::VectorUnsignedSaturatedShiftLeft64:
    // Cryptography (lower_crypto.cpp)
    case Opcode::AESDecryptSingleRound:
    case Opcode::AESEncryptSingleRound:
    case Opcode::AESInverseMixColumns:
    case Opcode::AESMixColumns:
    case Opcode::SM4AccessSubstitutionBox:
    case Opcode::SHA256Hash:
    case Opcode::SHA256MessageSchedule0:
    case Opcode::SHA256MessageSchedule1:
    // Scalar floating point (lower_fp.cpp, LowerFP)
    case Opcode::FPAbs16:
    case Opcode::FPAbs32:
    case Opcode::FPAbs64:
    case Opcode::FPAdd32:
    case Opcode::FPAdd64:
    case Opcode::FPCompare32:
    case Opcode::FPCompare64:
    case Opcode::FPDiv32:
    case Opcode::FPDiv64:
    case Opcode::FPMax32:
    case Opcode::FPMax64:
    case Opcode::FPMaxNumeric32:
    case Opcode::FPMaxNumeric64:
    case Opcode::FPMin32:
    case Opcode::FPMin64:
    case Opcode::FPMinNumeric32:
    case Opcode::FPMinNumeric64:
    case Opcode::FPMul32:
    case Opcode::FPMul64:
    case Opcode::FPMulAdd16:
    case Opcode::FPMulAdd32:
    case Opcode::FPMulAdd64:
    case Opcode::FPMulSub16:
    case Opcode::FPMulSub32:
    case Opcode::FPMulSub64:
    case Opcode::FPMulX32:
    case Opcode::FPMulX64:
    case Opcode::FPNeg16:
    case Opcode::FPNeg32:
    case Opcode::FPNeg64:
    case Opcode::FPRecipEstimate16:
    case Opcode::FPRecipEstimate32:
    case Opcode::FPRecipEstimate64:
    case Opcode::FPRecipExponent16:
    case Opcode::FPRecipExponent32:
    case Opcode::FPRecipExponent64:
    case Opcode::FPRecipStepFused16:
    case Opcode::FPRecipStepFused32:
    case Opcode::FPRecipStepFused64:
    case Opcode::FPRoundInt16:
    case Opcode::FPRoundInt32:
    case Opcode::FPRoundInt64:
    case Opcode::FPRSqrtEstimate16:
    case Opcode::FPRSqrtEstimate32:
    case Opcode::FPRSqrtEstimate64:
    case Opcode::FPRSqrtStepFused16:
    case Opcode::FPRSqrtStepFused32:
    case Opcode::FPRSqrtStepFused64:
    case Opcode::FPSqrt32:
    case Opcode::FPSqrt64:
    case Opcode::FPSub32:
    case Opcode::FPSub64:
    case Opcode::FPHalfToDouble:
    case Opcode::FPHalfToSingle:
    case Opcode::FPSingleToDouble:
    case Opcode::FPSingleToHalf:
    case Opcode::FPDoubleToHalf:
    case Opcode::FPDoubleToSingle:
    case Opcode::FPDoubleToFixedS32:
    case Opcode::FPDoubleToFixedS64:
    case Opcode::FPDoubleToFixedU32:
    case Opcode::FPDoubleToFixedU64:
    case Opcode::FPHalfToFixedS32:
    case Opcode::FPHalfToFixedS64:
    case Opcode::FPHalfToFixedU32:
    case Opcode::FPHalfToFixedU64:
    case Opcode::FPSingleToFixedS32:
    case Opcode::FPSingleToFixedS64:
    case Opcode::FPSingleToFixedU32:
    case Opcode::FPSingleToFixedU64:
    case Opcode::FPFixedU32ToSingle:
    case Opcode::FPFixedS32ToSingle:
    case Opcode::FPFixedU32ToDouble:
    case Opcode::FPFixedS32ToDouble:
    case Opcode::FPFixedU64ToDouble:
    case Opcode::FPFixedU64ToSingle:
    case Opcode::FPFixedS64ToDouble:
    case Opcode::FPFixedS64ToSingle:
    // Vector floating point (lower_fp.cpp, LowerFPVector)
    case Opcode::FPVectorAbs16:
    case Opcode::FPVectorAbs32:
    case Opcode::FPVectorAbs64:
    case Opcode::FPVectorAdd32:
    case Opcode::FPVectorAdd64:
    case Opcode::FPVectorDiv32:
    case Opcode::FPVectorDiv64:
    case Opcode::FPVectorEqual16:
    case Opcode::FPVectorEqual32:
    case Opcode::FPVectorEqual64:
    case Opcode::FPVectorFromSignedFixed32:
    case Opcode::FPVectorFromSignedFixed64:
    case Opcode::FPVectorFromUnsignedFixed32:
    case Opcode::FPVectorFromUnsignedFixed64:
    case Opcode::FPVectorGreater32:
    case Opcode::FPVectorGreater64:
    case Opcode::FPVectorGreaterEqual32:
    case Opcode::FPVectorGreaterEqual64:
    case Opcode::FPVectorMax32:
    case Opcode::FPVectorMax64:
    case Opcode::FPVectorMaxNumeric32:
    case Opcode::FPVectorMaxNumeric64:
    case Opcode::FPVectorMin32:
    case Opcode::FPVectorMin64:
    case Opcode::FPVectorMinNumeric32:
    case Opcode::FPVectorMinNumeric64:
    case Opcode::FPVectorMul32:
    case Opcode::FPVectorMul64:
    case Opcode::FPVectorMulAdd16:
    case Opcode::FPVectorMulAdd32:
    case Opcode::FPVectorMulAdd64:
    case Opcode::FPVectorMulX32:
    case Opcode::FPVectorMulX64:
    case Opcode::FPVectorNeg16:
    case Opcode::FPVectorNeg32:
    case Opcode::FPVectorNeg64:
    case Opcode::FPVectorPairedAdd32:
    case Opcode::FPVectorPairedAdd64:
    case Opcode::FPVectorPairedAddLower32:
    case Opcode::FPVectorRecipEstimate16:
    case Opcode::FPVectorRecipEstimate32:
    case Opcode::FPVectorRecipEstimate64:
    case Opcode::FPVectorRecipStepFused16:
    case Opcode::FPVectorRecipStepFused32:
    case Opcode::FPVectorRecipStepFused64:
    case Opcode::FPVectorRoundInt16:
    case Opcode::FPVectorRoundInt32:
    case Opcode::FPVectorRoundInt64:
    case Opcode::FPVectorRSqrtEstimate16:
    case Opcode::FPVectorRSqrtEstimate32:
    case Opcode::FPVectorRSqrtEstimate64:
    case Opcode::FPVectorRSqrtStepFused16:
    case Opcode::FPVectorRSqrtStepFused32:
    case Opcode::FPVectorRSqrtStepFused64:
    case Opcode::FPVectorSqrt32:
    case Opcode::FPVectorSqrt64:
    case Opcode::FPVectorSub32:
    case Opcode::FPVectorSub64:
    case Opcode::FPVectorToSignedFixed32:
    case Opcode::FPVectorToSignedFixed64:
    case Opcode::FPVectorToUnsignedFixed32:
    case Opcode::FPVectorToUnsignedFixed64:
    // Integer vector operations (lower_vector.cpp, LowerVector)
    case Opcode::VectorGetElement8:
    case Opcode::VectorGetElement16:
    case Opcode::VectorGetElement32:
    case Opcode::VectorSetElement8:
    case Opcode::VectorSetElement16:
    case Opcode::VectorSetElement32:
    case Opcode::VectorSetElement64:
    case Opcode::VectorAbs8:
    case Opcode::VectorAbs16:
    case Opcode::VectorAbs32:
    case Opcode::VectorAbs64:
    case Opcode::VectorAdd8:
    case Opcode::VectorAdd16:
    case Opcode::VectorAdd32:
    case Opcode::VectorAdd64:
    case Opcode::VectorAnd:
    case Opcode::VectorAndNot:
    case Opcode::VectorArithmeticShiftRight8:
    case Opcode::VectorArithmeticShiftRight16:
    case Opcode::VectorArithmeticShiftRight32:
    case Opcode::VectorArithmeticShiftRight64:
    case Opcode::VectorArithmeticVShift8:
    case Opcode::VectorArithmeticVShift16:
    case Opcode::VectorArithmeticVShift32:
    case Opcode::VectorArithmeticVShift64:
    case Opcode::VectorBroadcastLower8:
    case Opcode::VectorBroadcastLower16:
    case Opcode::VectorBroadcastLower32:
    case Opcode::VectorBroadcast8:
    case Opcode::VectorBroadcast16:
    case Opcode::VectorBroadcast32:
    case Opcode::VectorBroadcast64:
    case Opcode::VectorBroadcastElementLower8:
    case Opcode::VectorBroadcastElementLower16:
    case Opcode::VectorBroadcastElementLower32:
    case Opcode::VectorBroadcastElement8:
    case Opcode::VectorBroadcastElement16:
    case Opcode::VectorBroadcastElement32:
    case Opcode::VectorBroadcastElement64:
    case Opcode::VectorCountLeadingZeros8:
    case Opcode::VectorCountLeadingZeros16:
    case Opcode::VectorCountLeadingZeros32:
    case Opcode::VectorDeinterleaveEven8:
    case Opcode::VectorDeinterleaveEven16:
    case Opcode::VectorDeinterleaveEven32:
    case Opcode::VectorDeinterleaveEven64:
    case Opcode::VectorDeinterleaveEvenLower8:
    case Opcode::VectorDeinterleaveEvenLower16:
    case Opcode::VectorDeinterleaveEvenLower32:
    case Opcode::VectorDeinterleaveOdd8:
    case Opcode::VectorDeinterleaveOdd16:
    case Opcode::VectorDeinterleaveOdd32:
    case Opcode::VectorDeinterleaveOdd64:
    case Opcode::VectorDeinterleaveOddLower8:
    case Opcode::VectorDeinterleaveOddLower16:
    case Opcode::VectorDeinterleaveOddLower32:
    case Opcode::VectorEor:
    case Opcode::VectorEqual8:
    case Opcode::VectorEqual16:
    case Opcode::VectorEqual32:
    case Opcode::VectorEqual64:
    case Opcode::VectorExtract:
    case Opcode::VectorExtractLower:
    case Opcode::VectorGreaterS8:
    case Opcode::VectorGreaterS16:
    case Opcode::VectorGreaterS32:
    case Opcode::VectorGreaterS64:
    case Opcode::VectorHalvingAddS8:
    case Opcode::VectorHalvingAddS16:
    case Opcode::VectorHalvingAddS32:
    case Opcode::VectorHalvingAddU8:
    case Opcode::VectorHalvingAddU16:
    case Opcode::VectorHalvingAddU32:
    case Opcode::VectorHalvingSubS8:
    case Opcode::VectorHalvingSubS16:
    case Opcode::VectorHalvingSubS32:
    case Opcode::VectorHalvingSubU8:
    case Opcode::VectorHalvingSubU16:
    case Opcode::VectorHalvingSubU32:
    case Opcode::VectorInterleaveLower8:
    case Opcode::VectorInterleaveLower16:
    case Opcode::VectorInterleaveLower32:
    case Opcode::VectorInterleaveLower64:
    case Opcode::VectorInterleaveUpper8:
    case Opcode::VectorInterleaveUpper16:
    case Opcode::VectorInterleaveUpper32:
    case Opcode::VectorInterleaveUpper64:
    case Opcode::VectorLogicalShiftLeft8:
    case Opcode::VectorLogicalShiftLeft16:
    case Opcode::VectorLogicalShiftLeft32:
    case Opcode::VectorLogicalShiftLeft64:
    case Opcode::VectorLogicalShiftRight8:
    case Opcode::VectorLogicalShiftRight16:
    case Opcode::VectorLogicalShiftRight32:
    case Opcode::VectorLogicalShiftRight64:
    case Opcode::VectorLogicalVShift8:
    case Opcode::VectorLogicalVShift16:
    case Opcode::VectorLogicalVShift32:
    case Opcode::VectorLogicalVShift64:
    case Opcode::VectorMaxS8:
    case Opcode::VectorMaxS16:
    case Opcode::VectorMaxS32:
    case Opcode::VectorMaxU8:
    case Opcode::VectorMaxU16:
    case Opcode::VectorMaxU32:
    case Opcode::VectorMaxU64:
    case Opcode::VectorMinS8:
    case Opcode::VectorMinS16:
    case Opcode::VectorMinS32:
    case Opcode::VectorMinU8:
    case Opcode::VectorMinU16:
    case Opcode::VectorMinU32:
    case Opcode::VectorMinU64:
    case Opcode::VectorMultiply8:
    case Opcode::VectorMultiply16:
    case Opcode::VectorMultiply32:
    case Opcode::VectorMultiplySignedWiden8:
    case Opcode::VectorMultiplySignedWiden16:
    case Opcode::VectorMultiplySignedWiden32:
    case Opcode::VectorMultiplyUnsignedWiden8:
    case Opcode::VectorMultiplyUnsignedWiden16:
    case Opcode::VectorMultiplyUnsignedWiden32:
    case Opcode::VectorNarrow16:
    case Opcode::VectorNarrow32:
    case Opcode::VectorNarrow64:
    case Opcode::VectorNot:
    case Opcode::VectorOr:
    case Opcode::VectorPairedAddLower8:
    case Opcode::VectorPairedAddLower16:
    case Opcode::VectorPairedAddLower32:
    case Opcode::VectorPairedAddSignedWiden8:
    case Opcode::VectorPairedAddSignedWiden16:
    case Opcode::VectorPairedAddSignedWiden32:
    case Opcode::VectorPairedAddUnsignedWiden8:
    case Opcode::VectorPairedAddUnsignedWiden16:
    case Opcode::VectorPairedAddUnsignedWiden32:
    case Opcode::VectorPairedAdd8:
    case Opcode::VectorPairedAdd16:
    case Opcode::VectorPairedAdd32:
    case Opcode::VectorPairedAdd64:
    case Opcode::VectorPairedMaxS8:
    case Opcode::VectorPairedMaxS16:
    case Opcode::VectorPairedMaxS32:
    case Opcode::VectorPairedMaxU8:
    case Opcode::VectorPairedMaxU16:
    case Opcode::VectorPairedMaxU32:
    case Opcode::VectorPairedMinS8:
    case Opcode::VectorPairedMinS16:
    case Opcode::VectorPairedMinS32:
    case Opcode::VectorPairedMinU8:
    case Opcode::VectorPairedMinU16:
    case Opcode::VectorPairedMinU32:
    case Opcode::VectorPairedMaxLowerS8:
    case Opcode::VectorPairedMaxLowerS16:
    case Opcode::VectorPairedMaxLowerS32:
    case Opcode::VectorPairedMaxLowerU8:
    case Opcode::VectorPairedMaxLowerU16:
    case Opcode::VectorPairedMaxLowerU32:
    case Opcode::VectorPairedMinLowerS8:
    case Opcode::VectorPairedMinLowerS16:
    case Opcode::VectorPairedMinLowerS32:
    case Opcode::VectorPairedMinLowerU8:
    case Opcode::VectorPairedMinLowerU16:
    case Opcode::VectorPairedMinLowerU32:
    case Opcode::VectorPolynomialMultiply8:
    case Opcode::VectorPolynomialMultiplyLong8:
    case Opcode::VectorPolynomialMultiplyLong64:
    case Opcode::VectorPopulationCount:
    case Opcode::VectorReverseBits:
    case Opcode::VectorReverseElementsInHalfGroups8:
    case Opcode::VectorReverseElementsInWordGroups8:
    case Opcode::VectorReverseElementsInWordGroups16:
    case Opcode::VectorReverseElementsInLongGroups8:
    case Opcode::VectorReverseElementsInLongGroups16:
    case Opcode::VectorReverseElementsInLongGroups32:
    case Opcode::VectorReduceAdd8:
    case Opcode::VectorReduceAdd16:
    case Opcode::VectorReduceAdd32:
    case Opcode::VectorRotateWholeVectorRight:
    case Opcode::VectorRoundingHalvingAddS8:
    case Opcode::VectorRoundingHalvingAddS16:
    case Opcode::VectorRoundingHalvingAddS32:
    case Opcode::VectorRoundingHalvingAddU8:
    case Opcode::VectorRoundingHalvingAddU16:
    case Opcode::VectorRoundingHalvingAddU32:
    case Opcode::VectorRoundingShiftLeftS8:
    case Opcode::VectorRoundingShiftLeftS16:
    case Opcode::VectorRoundingShiftLeftS32:
    case Opcode::VectorRoundingShiftLeftS64:
    case Opcode::VectorRoundingShiftLeftU8:
    case Opcode::VectorRoundingShiftLeftU16:
    case Opcode::VectorRoundingShiftLeftU32:
    case Opcode::VectorRoundingShiftLeftU64:
    case Opcode::VectorSignExtend8:
    case Opcode::VectorSignExtend16:
    case Opcode::VectorSignExtend32:
    case Opcode::VectorSignedAbsoluteDifference8:
    case Opcode::VectorSignedAbsoluteDifference16:
    case Opcode::VectorSignedAbsoluteDifference32:
    case Opcode::VectorSub8:
    case Opcode::VectorSub16:
    case Opcode::VectorSub32:
    case Opcode::VectorSub64:
    case Opcode::VectorTable:
    case Opcode::VectorTableLookup128:
    case Opcode::VectorTranspose8:
    case Opcode::VectorTranspose16:
    case Opcode::VectorTranspose32:
    case Opcode::VectorTranspose64:
    case Opcode::VectorUnsignedAbsoluteDifference8:
    case Opcode::VectorUnsignedAbsoluteDifference16:
    case Opcode::VectorUnsignedAbsoluteDifference32:
    case Opcode::VectorUnsignedRecipEstimate:
    case Opcode::VectorUnsignedRecipSqrtEstimate:
    case Opcode::VectorZeroExtend8:
    case Opcode::VectorZeroExtend16:
    case Opcode::VectorZeroExtend32:
    case Opcode::VectorZeroExtend64:
    case Opcode::VectorZeroUpper:
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
    // after MostSignificantWord, and dead-code elimination removes it before the lifter looks), nor the floating-point and SIMD ones listed
    // last, which `nxaot opcodes --reach --check` proves for every encoding of dynarmic's A64 decoder
    static const char* const a32[] = {"SignedSaturation", "UnsignedSaturation", "GetGEFromOp", "GetCarryFromOp", "GetOverflowFromOp", "GetCFlagFromNZCV",
                                      "MostSignificantBit", "RotateRightExtended", "ByteReverseHalf",
                                      // Floating-point and SIMD opcodes no A64 translator emits (`nxaot opcodes --reach`, CTest opcodes_reach): the
                                      // widening multiply pseudo-operations, the 64-bit integer lane max / min / multiply, VTBL's 64-bit lookup,
                                      // the 16-bit fixed-point and the vector half-precision conversions of VCVT
                                      "GetUpperFromOp", "GetLowerFromOp", "MaxSigned64", "MaxUnsigned64", "MinSigned64", "MinUnsigned64", "SignedSaturatedAddWithFlag32", "SignedSaturatedSubWithFlag32", "VectorEqual128", "VectorMaxS64", "VectorMinS64", "VectorMultiply64", "VectorReduceAdd64", "VectorSignExtend64", "VectorSignedMultiply16", "VectorSignedMultiply32", "VectorTableLookup64", "VectorUnsignedMultiply16", "VectorUnsignedMultiply32", "FPDoubleToFixedS16", "FPDoubleToFixedU16", "FPHalfToFixedS16", "FPHalfToFixedU16", "FPSingleToFixedS16", "FPSingleToFixedU16", "FPFixedU16ToSingle", "FPFixedS16ToSingle", "FPFixedU16ToDouble", "FPFixedS16ToDouble", "FPVectorFromHalf32", "FPVectorPairedAddLower64", "FPVectorToHalf32", "FPVectorToSignedFixed16", "FPVectorToUnsignedFixed16"};
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
