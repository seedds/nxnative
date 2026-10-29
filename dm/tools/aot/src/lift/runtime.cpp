#include "lift/runtime.h"

#include <cstring>

#include "dynarmic/common/crypto/aes.h"
#include "dynarmic/common/crypto/sm4.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/fp/fpsr.h"
#include "dynarmic/common/fp/op.h"
#include "dynarmic/common/fp/rounding_mode.h"
#include "dynarmic/common/math_util.h"

using namespace Dynarmic;

namespace {

// dynarmic's FPSR keeps NZCV, QC and the cumulative bits; the helpers only ever add cumulative bits, which are ORed into the guest's FPSR
struct Fpsr {
    explicit Fpsr(uint32_t* p) : p(p), f(0) {}
    ~Fpsr() { *p |= f.Value() & 0x9Fu; }
    uint32_t* p;
    FP::FPSR f;
};

template<typename F>
uint64_t BySize(uint32_t fsize, F&& f) {
    switch (fsize) {
    case 16:
        return f(uint16_t{});
    case 32:
        return f(uint32_t{});
    default:
        return f(uint64_t{});
    }
}

}  // namespace

extern "C" {

uint64_t nxaot_rt_fp_unary(uint32_t which, uint32_t fsize, uint64_t a, uint32_t fpcr, uint32_t* fpsr) {
    Fpsr s{fpsr};
    const FP::FPCR c{fpcr};
    return BySize(fsize, [&](auto t) -> uint64_t {
        using T = decltype(t);
        const T op = static_cast<T>(a);
        switch (which) {
        case NXAOT_RT_RECIP_ESTIMATE:
            return FP::FPRecipEstimate<T>(op, c, s.f);
        case NXAOT_RT_RSQRT_ESTIMATE:
            return FP::FPRSqrtEstimate<T>(op, c, s.f);
        default:
            return FP::FPRecipExponent<T>(op, c, s.f);
        }
    });
}

uint64_t nxaot_rt_fp_binary(uint32_t which, uint32_t fsize, uint64_t a, uint64_t b, uint32_t fpcr, uint32_t* fpsr) {
    Fpsr s{fpsr};
    const FP::FPCR c{fpcr};
    return BySize(fsize, [&](auto t) -> uint64_t {
        using T = decltype(t);
        const T x = static_cast<T>(a), y = static_cast<T>(b);
        switch (which) {
        case NXAOT_RT_RECIP_STEP:
            return FP::FPRecipStepFused<T>(x, y, c, s.f);
        case NXAOT_RT_RSQRT_STEP:
            return FP::FPRSqrtStepFused<T>(x, y, c, s.f);
        default:
            return FP::FPCompareEQ<T>(x, y, c, s.f) ? 1 : 0;
        }
    });
}

uint64_t nxaot_rt_fp_muladd(uint32_t fsize, uint32_t negate, uint64_t addend, uint64_t op1, uint64_t op2, uint32_t fpcr, uint32_t* fpsr) {
    Fpsr s{fpsr};
    const FP::FPCR c{fpcr};
    return BySize(fsize, [&](auto t) -> uint64_t {
        using T = decltype(t);
        return negate ? FP::FPMulSub<T>(static_cast<T>(addend), static_cast<T>(op1), static_cast<T>(op2), c, s.f)
                      : FP::FPMulAdd<T>(static_cast<T>(addend), static_cast<T>(op1), static_cast<T>(op2), c, s.f);
    });
}

uint64_t nxaot_rt_fp_round_int(uint32_t fsize, uint64_t op, uint32_t rounding, uint32_t exact, uint32_t fpcr, uint32_t* fpsr) {
    Fpsr s{fpsr};
    const FP::FPCR c{fpcr};
    return BySize(fsize, [&](auto t) -> uint64_t {
        using T = decltype(t);
        return FP::FPRoundInt<T>(static_cast<T>(op), c, static_cast<FP::RoundingMode>(rounding), exact != 0, s.f);
    });
}

uint64_t nxaot_rt_fp_to_fixed(uint32_t fsize, uint32_t isize, uint32_t is_unsigned, uint32_t fbits, uint32_t rounding, uint64_t op, uint32_t fpcr,
                              uint32_t* fpsr) {
    Fpsr s{fpsr};
    const FP::FPCR c{fpcr};
    return BySize(fsize, [&](auto t) -> uint64_t {
        using T = decltype(t);
        return FP::FPToFixed<T>(isize, static_cast<T>(op), fbits, is_unsigned != 0, c, static_cast<FP::RoundingMode>(rounding), s.f);
    });
}

uint64_t nxaot_rt_fp_convert(uint32_t to_size, uint32_t from_size, uint64_t op, uint32_t rounding, uint32_t fpcr, uint32_t* fpsr) {
    Fpsr s{fpsr};
    const FP::FPCR c{fpcr};
    const auto mode = static_cast<FP::RoundingMode>(rounding);
    switch (to_size * 100 + from_size) {
    case 1632:
        return FP::FPConvert<u16, u32>(static_cast<u32>(op), c, mode, s.f);
    case 1664:
        return FP::FPConvert<u16, u64>(op, c, mode, s.f);
    case 3216:
        return FP::FPConvert<u32, u16>(static_cast<u16>(op), c, mode, s.f);
    case 3264:
        return FP::FPConvert<u32, u64>(op, c, mode, s.f);
    case 6416:
        return FP::FPConvert<u64, u16>(static_cast<u16>(op), c, mode, s.f);
    default:
        return FP::FPConvert<u64, u32>(static_cast<u32>(op), c, mode, s.f);
    }
}

void nxaot_rt_aes(uint32_t which, uint8_t* out, const uint8_t* in) {
    Common::Crypto::AES::State a, r;
    std::memcpy(a.data(), in, 16);
    switch (which) {
    case 0:
        Common::Crypto::AES::DecryptSingleRound(r, a);
        break;
    case 1:
        Common::Crypto::AES::EncryptSingleRound(r, a);
        break;
    case 2:
        Common::Crypto::AES::InverseMixColumns(r, a);
        break;
    default:
        Common::Crypto::AES::MixColumns(r, a);
        break;
    }
    std::memcpy(out, r.data(), 16);
}

}  // extern "C"

namespace nxaot::lift {

const std::vector<std::pair<const char*, void*>>& RuntimeSymbols() {
    static const std::vector<std::pair<const char*, void*>> symbols = {
        {"nxaot_rt_fp_unary", reinterpret_cast<void*>(&nxaot_rt_fp_unary)},
        {"nxaot_rt_fp_binary", reinterpret_cast<void*>(&nxaot_rt_fp_binary)},
        {"nxaot_rt_fp_muladd", reinterpret_cast<void*>(&nxaot_rt_fp_muladd)},
        {"nxaot_rt_fp_round_int", reinterpret_cast<void*>(&nxaot_rt_fp_round_int)},
        {"nxaot_rt_fp_to_fixed", reinterpret_cast<void*>(&nxaot_rt_fp_to_fixed)},
        {"nxaot_rt_fp_convert", reinterpret_cast<void*>(&nxaot_rt_fp_convert)},
        {"nxaot_rt_aes", reinterpret_cast<void*>(&nxaot_rt_aes)},
    };
    return symbols;
}

uint8_t UnsignedRecipEstimate(uint32_t nine_bits) {
    return Common::RecipEstimate(nine_bits);
}

uint8_t UnsignedRecipSqrtEstimate(uint32_t nine_bits) {
    return Common::RecipSqrtEstimate(nine_bits);
}

uint8_t Sm4Sbox(uint8_t index) {
    return Common::Crypto::SM4::AccessSubstitutionBox(index);
}

}  // namespace nxaot::lift
