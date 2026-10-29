// What discovery needs to know of one A64 instruction word (src/analysis/discover.cpp, summary.cpp): its control flow class and target, the
// pc-relative address forms, exclusive accesses, and whether it decodes. Validity comes from dynarmic's decoder table (as reach.cpp and the
// census classify words), plus the encodings dynarmic leaves to an interpreter (LSE atomics, ldapr, msr immediate, hvc / smc: valid ARMv8.1+
// instructions, census.cpp masks); the fields come from the Arm ARM's encodings.
#pragma once

#include <cstdint>

#include "dynarmic/frontend/A64/decoder/a64.h"
#include "dynarmic/frontend/A64/translate/impl/impl.h"

namespace nxaot::analysis {

enum class Kind : uint8_t {
    Normal,
    B,      // b
    BL,     // bl
    BCond,  // b.cond, cbz, cbnz, tbz, tbnz
    BR,     // br
    BLR,    // blr
    RET,    // ret
    SVC,    // svc
    Trap,   // brk, hlt, udf
    Undefined,
};

struct Insn {
    uint32_t word = 0;
    Kind kind = Kind::Undefined;
    uint64_t target = 0;       // branch target; for adr / adrp the address or page
    uint8_t rn = 0, rd = 0;    // br / blr / ret: rn; every word: bits 4..0 as the written register for the adrp tracking
    bool adr = false, adrp = false, add_imm = false;  // add_imm: add xd, xn, #imm (64-bit, no flags), imm in target
    bool excl_load = false, excl_store = false;
    bool Ends() const { return kind != Kind::Normal; }  // ends a block of discovery's CFG
};

inline int64_t SignExtend(uint64_t v, unsigned bits) {
    return static_cast<int64_t>(v << (64 - bits)) >> (64 - bits);
}

inline Insn ClassifyWord(uint32_t w, uint64_t pc) {
    Insn i;
    i.word = w;
    i.rd = w & 31;
    if ((w & 0x7C000000u) == 0x14000000u) {  // b, bl
        i.kind = (w >> 31) ? Kind::BL : Kind::B;
        i.target = pc + static_cast<uint64_t>(SignExtend(w & 0x3FFFFFFu, 26) * 4);
        return i;
    }
    if ((w & 0xFF000010u) == 0x54000000u) {  // b.cond
        i.kind = Kind::BCond;
        i.target = pc + static_cast<uint64_t>(SignExtend((w >> 5) & 0x7FFFFu, 19) * 4);
        return i;
    }
    if ((w & 0x7E000000u) == 0x34000000u) {  // cbz, cbnz
        i.kind = Kind::BCond;
        i.target = pc + static_cast<uint64_t>(SignExtend((w >> 5) & 0x7FFFFu, 19) * 4);
        return i;
    }
    if ((w & 0x7E000000u) == 0x36000000u) {  // tbz, tbnz
        i.kind = Kind::BCond;
        i.target = pc + static_cast<uint64_t>(SignExtend((w >> 5) & 0x3FFFu, 14) * 4);
        return i;
    }
    if ((w & 0xFFFFFC1Fu) == 0xD61F0000u || (w & 0xFFFFFC1Fu) == 0xD63F0000u || (w & 0xFFFFFC1Fu) == 0xD65F0000u) {
        i.rn = (w >> 5) & 31;
        i.kind = (w & 0x00600000u) == 0 ? Kind::BR : (w & 0x00600000u) == 0x00200000u ? Kind::BLR : Kind::RET;
        return i;
    }
    if ((w & 0xFFE0001Fu) == 0xD4000001u) {
        i.kind = Kind::SVC;
        return i;
    }
    if ((w & 0xFFE0001Fu) == 0xD4200000u || (w & 0xFFE0001Fu) == 0xD4400000u || (w >> 16) == 0) {  // brk, hlt, udf
        i.kind = Kind::Trap;
        return i;
    }
    // Valid words dynarmic does not decode (census.cpp's masks): LSE atomics, ldapr, msr immediate, hvc / smc
    const bool lse = (w & 0x3FA07C00u) == 0x08A07C00u || (w & 0xBFA07C00u) == 0x08207C00u ||
                     ((w & 0x3F200C00u) == 0x38200000u && ((w >> 12) & 0xF) <= 0b1100 && ((w >> 12) & 0xF) != 0b1001 &&
                      ((w >> 12) & 0xF) != 0b1010 && ((w >> 12) & 0xF) != 0b1011);
    const bool other = (w & 0xFFF8F01Fu) == 0xD500401Fu || (w & 0xFFE0001Fu) == 0xD4000002u || (w & 0xFFE0001Fu) == 0xD4000003u;
    const auto matcher = Dynarmic::A64::Decode<Dynarmic::A64::TranslatorVisitor>(w);
    if (!lse && !other && (!matcher || std::string_view(matcher->get().GetName()).empty())) {
        i.kind = Kind::Undefined;
        return i;
    }
    i.kind = Kind::Normal;
    if ((w & 0x9F000000u) == 0x10000000u) {  // adr
        i.adr = true;
        i.target = pc + static_cast<uint64_t>(SignExtend(((w >> 5) & 0x7FFFFu) << 2 | ((w >> 29) & 3), 21));
    } else if ((w & 0x9F000000u) == 0x90000000u) {  // adrp
        i.adrp = true;
        i.target = (pc & ~0xFFFull) + static_cast<uint64_t>(SignExtend(((w >> 5) & 0x7FFFFu) << 2 | ((w >> 29) & 3), 21) << 12);
    } else if ((w & 0xFF800000u) == 0x91000000u) {  // add xd, xn, #imm{, lsl #12}
        i.add_imm = true;
        i.rn = (w >> 5) & 31;
        i.target = ((w >> 10) & 0xFFFu) << ((w >> 22) & 1 ? 12 : 0);
    } else if ((w & 0x3F000000u) == 0x08000000u && !((w >> 23) & 1)) {  // load / store exclusive (o2 = 0)
        i.excl_load = (w >> 22) & 1;
        i.excl_store = !i.excl_load;
    }
    return i;
}

}  // namespace nxaot::analysis
