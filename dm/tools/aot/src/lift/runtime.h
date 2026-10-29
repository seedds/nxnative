// The run-time helpers lifted code calls for the floating-point and cryptographic operations whose exact Arm semantics are not one native x86
// sequence (aot-recompiler.md section 7.2, rows 8 to 10, the half-precision operations of 7.1 and the slow paths of 7.3): the reciprocal and
// reciprocal square root estimates and steps of half precision and their fix-ups, fused multiply-add where FPCR.FZ needs Arm's flush-before-
// rounding (row 13), rounding to integral and conversions to fixed point in the modes x86 has no instruction for, the conversions between half
// and the other precisions, and AES rounds on a target without AES-NI. Each wraps the function of dynarmic's common/fp or common/crypto that
// dynarmic's own x86-64 backend calls for the same case, so the lifted code and the JIT fallback (design section 11) share one definition.
//
// They take the operands as raw bits, the FPCR the code is specialised for, and a pointer to the guest's FPSR (dm_guest_state.fpsr), into which
// they OR the cumulative exception bits the Arm pseudocode raises (FPProcessException). The lifter refers to them by these names; nxaot_vec's
// JIT defines them as absolute symbols (lift/jit.cpp), and the host runtime of a pack will export them (later: dm/host/aot_*.c).
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

extern "C" {

// Unary FP helpers (which: NXAOT_RT_*): the result's bits
enum {
    NXAOT_RT_RECIP_ESTIMATE = 0,
    NXAOT_RT_RSQRT_ESTIMATE = 1,
    NXAOT_RT_RECIP_EXPONENT = 2,
    NXAOT_RT_RECIP_STEP = 3,     // binary
    NXAOT_RT_RSQRT_STEP = 4,     // binary
    NXAOT_RT_COMPARE_EQ = 5,     // binary: 1 or 0 (fcmeq of half precision)
};

uint64_t nxaot_rt_fp_unary(uint32_t which, uint32_t fsize, uint64_t a, uint32_t fpcr, uint32_t* fpsr);
uint64_t nxaot_rt_fp_binary(uint32_t which, uint32_t fsize, uint64_t a, uint64_t b, uint32_t fpcr, uint32_t* fpsr);
// FPMulAdd (negate = 0) or FPMulSub (negate = 1): addend + op1 * op2, or minuend - op1 * op2, rounded once
uint64_t nxaot_rt_fp_muladd(uint32_t fsize, uint32_t negate, uint64_t addend, uint64_t op1, uint64_t op2, uint32_t fpcr, uint32_t* fpsr);
// FPRoundInt: rounding is dynarmic's FP::RoundingMode; exact raises Inexact when the result differs from the operand
uint64_t nxaot_rt_fp_round_int(uint32_t fsize, uint64_t op, uint32_t rounding, uint32_t exact, uint32_t fpcr, uint32_t* fpsr);
// FPToFixed: op * 2^fbits rounded with rounding, saturated to isize bits (signed or unsigned)
uint64_t nxaot_rt_fp_to_fixed(uint32_t fsize, uint32_t isize, uint32_t is_unsigned, uint32_t fbits, uint32_t rounding, uint64_t op, uint32_t fpcr,
                              uint32_t* fpsr);
// FPConvert between precisions (16, 32, 64 bits) with an explicit rounding mode (ToOdd included)
uint64_t nxaot_rt_fp_convert(uint32_t to_size, uint32_t from_size, uint64_t op, uint32_t rounding, uint32_t fpcr, uint32_t* fpsr);
// AES (which: 0 AESD single round, 1 AESE single round, 2 AESIMC, 3 AESMC) on a 16-byte state, for targets without AES-NI
void nxaot_rt_aes(uint32_t which, uint8_t* out, const uint8_t* in);

}  // extern "C"

namespace nxaot::lift {

// Every helper above with its address, for a JIT that runs lifted code in this process
const std::vector<std::pair<const char*, void*>>& RuntimeSymbols();

// The 8-bit results of the Arm ARM's RecipEstimate and RecipSqrtEstimate for 9-bit inputs (the tables of URECPE and URSQRTE), from dynarmic's
// common/math_util.cpp; the lifter embeds them as constant tables
uint8_t UnsignedRecipEstimate(uint32_t nine_bits);
uint8_t UnsignedRecipSqrtEstimate(uint32_t nine_bits);
// SM4's substitution box (dynarmic's common/crypto/sm4.cpp), embedded the same way
uint8_t Sm4Sbox(uint8_t index);

}  // namespace nxaot::lift
