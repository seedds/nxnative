#pragma GCC diagnostic ignored "-Wshadow"
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.
#pragma once

#include <cstdlib>
#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <optional>
#include <string_view>
#include <vector>
#include "common/common_types.h"

namespace Shader::Ampere {

struct Instruction {
    std::array<u32, 4> words;

    u32 Bits(u32 offset, u32 count) const {
        u64 bits = words[offset / 32];
        if (offset / 32 + 1 < words.size()) {
            bits |= u64{words[offset / 32 + 1]} << 32;
        }
        return static_cast<u32>((bits >> (offset % 32)) & ((u64{1} << count) - 1));
    }
};
static_assert(sizeof(Instruction) == 16);

struct LoweredProgram {
    std::vector<u64> instructions;
    std::vector<u32> source_instruction_offsets;
    u32 source_size{};
    u32 required_texture_buffer = ~0U;
    u32 error_offset{};
    std::string_view error;

    explicit operator bool() const { return error.empty(); }
};

// Lower only operations whose semantics are representable by the existing
// Maxwell frontend. These are new instructions, not reinterpretations of the
// native bytes. Scheduling/control words are added by the environment adapter.
inline LoweredProgram LowerToMaxwell(std::span<const Instruction> code) {
    LoweredProgram result;
    struct Relocation { u32 output_index; s64 target; };
    std::vector<Relocation> relocations;
    std::vector<u32> barrier_tokens;
    std::vector<u32> call_targets;
    std::vector<u32> call_links;
    bool main_exited = false;
    bool computed_branch = false; // a jump table: the program ends at its self-branch, not an EXIT
    bool uses_uniform_bank = false;
    for (const auto& instruction : code) {
        const u32 op = instruction.Bits(0, 12);
        uses_uniform_bank |= op == 0x3c4 || op == 0x2bd || op == 0xc0c || op == 0x986 || op == 0x9a8 || op == 0xc12 || op == 0xc17 || op == 0xe24 || op == 0xc24 || op == 0xd00 || op == 0xd09 || op == 0x9c3 || op == 0xc45 || op == 0x2a4 || op == 0x891 || op == 0x890 || op == 0x290 || op == 0x28c || op == 0x88c || op == 0x287 || op == 0x887 || op == 0x958 || op == 0xab9 || op == 0xabb || op == 0x899 || op == 0xc20 || op == 0xe21 || op == 0xe23 || op == 0xc02 ||
                             op == 0x886 || op == 0x3c2 || op == 0xc82 || op == 0x882 || op == 0x292 || op == 0x892 || op == 0x291 || op == 0x981 || op == 0xc13 || op == 0xc10 || op == 0xc23 || op == 0x526 || op == 0x2be || op == 0x8a4 || op == 0xd05 || (op == 0x984 && instruction.Bits(91, 1)) || op == 0x89c || op == 0x883 || op == 0x2a5 || op == 0xd06 || op == 0x299 || op == 0xc09 || (op == 0x81c && instruction.Bits(67, 1));
    }
    const auto address_of = [](u32 index) { return 0x50U + index * 8 + ((index + 1) / 3) * 8; };
    const auto fail = [&](u32 index, std::string_view reason) {
        result.instructions.clear();
        result.source_instruction_offsets.clear();
        result.error_offset = index * sizeof(Instruction);
        result.error = reason;
        return result;
    };
    const auto finish = [&](u32 index) {
        for (const auto& relocation : relocations) {
            if (relocation.target > index) return fail(index, "native branch or call leaves captured program");
            const u32 target = result.source_instruction_offsets[static_cast<size_t>(relocation.target)];
            const s64 relative = s64{address_of(target)} - address_of(relocation.output_index) - 8;
            if (relative < -0x800000 || relative > 0x7fffff) return fail(index, "lowered branch distance");
            result.instructions[relocation.output_index] |= (static_cast<u64>(relative) & 0xffffff) << 20;
        }
        result.source_size = (index + 1) * sizeof(Instruction);
        return result;
    };
    if (code.empty() || code.size() > 65536) return fail(0, "invalid native program size");
    for (u32 index = 0; index < code.size(); ++index) {
        auto insn = code[index];
        result.source_instruction_offsets.push_back(static_cast<u32>(result.instructions.size()));
        u32 opcode = insn.Bits(0, 12);
        if (insn.Bits(12, 4) == 15) continue; // @!PT: never executes (compiler padding)
        std::optional<u32> active_link;
        u32 active_entry = 0;
        if (main_exited) {
            for (size_t call = 0; call < call_targets.size(); ++call) {
                if (call_targets[call] <= index && call_targets[call] >= active_entry) {
                    active_entry = call_targets[call];
                    active_link = call_links[call];
                }
            }
            if (active_link && insn.Bits(16, 8) == *active_link)
                return fail(index, "native callee overwrites link register");
        }
        // Scratch values and emulated uniform state occupy reserved high GPRs.
        // Reject native accesses that could alias that state.
        for (u32 bit : {16U, 24U, 32U, 64U}) {
            if (bit == 32 && insn.Bits(9, 3) != 1 && opcode != 0xb60 && opcode != 0xb66) continue;
            const u32 reg = insn.Bits(bit, 8);
            if (reg >= (uses_uniform_bank ? 216U : 246U) && reg < 255) return fail(index, "native program uses lowering scratch registers");
        }
        // UR0-UR21 live in R216-R237 and UR22-UR27 in R240-R245 (R239 holds the uniform
        // predicates), so even/odd pairs stay adjacent. Sites that still check reg < 16 rely on
        // UR0-UR15 being R216-R231.
        const auto uniform_register = [&](u32 reg) -> u32 {
            if (reg < 22) return 216 + reg;
            if (reg < 28) return 218 + reg;
            if (reg == 63) return 255;
            return ~0U;
        };
        bool rewritten_uniform = false;
        bool uniform_guard = false;
        u32 uniform_output = 7; // ULOP3's UPn output: computed in P(6-n), then merged into R239
        if (opcode == 0x2a4 || opcode == 0x8a4 || opcode == 0x891 || opcode == 0x890 || opcode == 0x290 ||
            opcode == 0x292 || opcode == 0x892 || opcode == 0x291) {
            if (insn.Bits(12, 4) != 7 && opcode != 0x292 && opcode != 0x892)
                return fail(index, "native uniform arithmetic predication");
            uniform_guard = insn.Bits(12, 4) != 7;
            rewritten_uniform = true;
            const u32 dst = uniform_register(insn.Bits(16, 8));
            const u32 a = uniform_register(insn.Bits(24, 8));
            const u32 c = uniform_register(insn.Bits(64, 8));
            if (dst == ~0U || a == ~0U || c == ~0U) return fail(index, "native uniform arithmetic register");
            insn.words[0] = (insn.words[0] & 0xffff) | (dst << 16) | (a << 24);
            insn.words[2] = (insn.words[2] & ~0x080000ffU) | c;
            if (opcode == 0x2a4 || opcode == 0x290 || opcode == 0x292 || opcode == 0x291) {
                const u32 b = uniform_register(insn.Bits(32, 8));
                if (b == ~0U) return fail(index, "native uniform arithmetic second register");
                insn.words[1] = (insn.words[1] & ~0xffU) | b;
            }
            if ((opcode == 0x292 || opcode == 0x892) && insn.Bits(80, 4) != 0xe) {
                if (insn.Bits(80, 1)) return fail(index, "native uniform logic predicate output");
                uniform_output = insn.Bits(81, 3);
                insn.words[2] = (insn.words[2] & ~(7U << 17)) | ((6 - uniform_output) << 17);
            }
            opcode = opcode == 0x2a4 ? 0x224 : opcode == 0x8a4 ? 0x824 : opcode == 0x891 ? 0x811 : opcode == 0x890 ? 0x810
                   : opcode == 0x292 ? 0x212 : opcode == 0x892 ? 0x812 : opcode == 0x291 ? 0x211 : 0x210;
        }
        const bool uniform_predication = opcode == 0xabb || opcode == 0xab9 || opcode == 0xc82 || opcode == 0x882;
        const u32 native_predicate = insn.Bits(12, 4);
        const bool guarded_uniform = (uniform_predication || uniform_guard) && native_predicate != 7;
        // A uniform guard UPn lives in R239 bit 6-n and is swapped into P(6-n) for the op.
        const u32 guard_slot = 6 - (native_predicate & 7);
        u64 predicate = u64{guarded_uniform ? ((native_predicate & 8) | guard_slot) : native_predicate} << 16;
        const u64 dest = insn.Bits(16, 8);
        const auto base = [&](u64 opcode_bits) { return opcode_bits | predicate | dest; };
        // NVN2 fullscreen vertex shaders read bank 24, which the game never binds (the console reads
        // zeros from an unbound bank); Maxwell has 18, so read it from bank 17, also never bound.
        const auto bank = [&]() -> u32 {
            const u32 value = insn.Bits(54, 5);
            return value == 24 ? 17U : value;
        };
        const auto cbuf = [&]() -> u64 {
            return (u64{insn.Bits(38, 16) / 4} << 20) | (u64{bank()} << 34);
        };
        const auto valid_cbuf = [&] {
            return !insn.Bits(91, 1) && bank() < 18 &&
                   (insn.Bits(38, 16) % 4) == 0;
        };
        std::string_view operand_error;
        const auto emit = [&](u64 word) { result.instructions.push_back(word); };
        // P(guard slot) while a uniform guard is swapped in; P6 for lowerings that use it as
        // scratch (R2UR, MATCH, HFMA2.RELU), which save and restore it with the defaults below.
        const u32 guard_mask = guard_slot < 7 ? 1U << guard_slot : 0U;
        const auto save_predicate = [&](u32 mask = 1U << 6) {
            emit(0x38e8000000070000ULL | 253 | (u64{255} << 8) | (u64{mask} << 20));
        };
        const auto restore_predicate = [&](u32 mask = 1U << 6) {
            emit(0x38f0000000070000ULL | (u64{253} << 8) | (u64{mask} << 20));
        };
        if (guarded_uniform) {
            save_predicate(guard_mask);
            emit(0x38f0000000070000ULL | (u64{239} << 8) | (u64{guard_mask} << 20));
        }
        const u32 output_mask = uniform_output == 7 ? 0U : 1U << (6 - uniform_output);
        if (output_mask) {
            // Keep the guest P(6-n) in R254 and start from the current UPn (a false guard keeps it).
            emit(0x38e8000000070000ULL | 254 | (u64{255} << 8) | (u64{output_mask} << 20));
            emit(0x38f0000000070000ULL | (u64{239} << 8) | (u64{output_mask} << 20));
        }
        // Pd = LUT(a, b, c) over predicate operands (index | negate << 3): each input becomes
        // 0 or ~0 (SEL), and LOP3 sets Pd when the result is nonzero.
        const auto predicate_lut = [&](u32 lut, u32 a, u32 b, u32 c, u32 target, bool uniform_c = false) {
            const u32 inputs[3]{a, b, c};
            for (u32 i = 0; i < 3; ++i) {
                if (i == 2 && uniform_c) {
                    // UPn from R239 bit 6-n as 0 or ~0 (UPT is ~0); a negation flips the table.
                    if ((c & 7) == 7) {
                        emit(0x0100000000000000ULL | predicate | 252 | (u64{15} << 12) | (u64{~0U} << 20));
                    } else {
                        emit(0x3800000000000000ULL | predicate | 252 | (u64{239} << 8) |
                             (u64{(6 - (c & 7)) | (1U << 8)} << 20));
                        emit(0x5c10000000000000ULL | predicate | 252 | (u64{255} << 8) |
                             (u64{252} << 20) | (u64{1} << 48));
                    }
                    if (c & 8) lut = ((lut & 0x55) << 1) | ((lut & 0xaa) >> 1);
                    continue;
                }
                emit(0x38a0000000000000ULL | predicate | (250 + i) | (u64{255} << 8) |
                     (u64{0x7ffff} << 20) | (u64{1} << 56) | (u64{inputs[i] ^ 8U} << 39));
            }
            emit(0x5be0000000000000ULL | predicate | 255 | (u64{250} << 8) | (u64{251} << 20) |
                 (u64{lut} << 28) | (u64{3} << 36) | (u64{252} << 39) | (u64{target} << 48));
        };
        const auto mov_imm = [&](u32 reg, u32 value) {
            emit(0x0100000000000000ULL | predicate | reg | (u64{15} << 12) | (u64{value} << 20));
        };
        const auto materialize = [&](u32 kind, u32 reg, u32 scratch, bool absolute, bool negate) -> u32 {
            if (kind == 3) {
                const u32 mapped = uniform_register(reg);
                if (mapped == ~0U) { operand_error = "native uniform source register"; return 255; }
                reg = mapped;
            } else if (kind == 1) {
                mov_imm(scratch, insn.words[1]);
                reg = scratch;
            } else if (kind == 2) {
                if (!valid_cbuf()) { operand_error = "native constant buffer address"; return 255; }
                emit(0x4c98000000000000ULL | predicate | scratch | (u64{15} << 39) | cbuf());
                reg = scratch;
            }
            if (absolute || negate) {
                if (reg != scratch) emit(0x5c98000000000000ULL | predicate | scratch | (u64{reg} << 20) | (u64{15} << 39));
                if (absolute) emit(0x0400000000000000ULL | predicate | scratch | (u64{scratch} << 8) | (u64{0x7fffffff} << 20));
                if (negate) emit(0x0400000000000000ULL | predicate | scratch | (u64{scratch} << 8) | (u64{0x80000000} << 20) | (u64{2} << 53));
                reg = scratch;
            }
            return reg;
        };
        const auto operand = [&](u32 which, bool floating = true) -> u32 {
            const u32 form = insn.Bits(9, 3);
            u32 reg{}, kind{}, abs{}, neg{};
            if (which == 0) {
                reg = insn.Bits(24, 8); abs = insn.Bits(73, 1); neg = insn.Bits(72, 1);
            } else {
                if (form != 1 && form != 2 && form != 3 && form != 4 && form != 5 && form != 6 && form != 7) {
                    operand_error = "native ALU source form"; return 255;
                }
                const bool wide = which == 1 ? (form == 4 || form == 5 || form == 6) : (form == 2 || form == 3 || form == 7);
                if (wide) {
                    kind = form == 6 || form == 7 ? 3 : (form == 4 || form == 2) ? 1 : 2;
                    if (kind == 3) reg = insn.Bits(32, 8);
                    if (kind == 2 || kind == 3) { abs = insn.Bits(62, 1); neg = insn.Bits(63, 1); }
                } else {
                    const bool upper = which == 1 ? (form == 2 || form == 3 || form == 7) : true;
                    reg = insn.Bits(upper ? 64 : 32, 8);
                    abs = insn.Bits(upper ? 74 : 62, 1); neg = insn.Bits(upper ? 75 : 63, 1);
                }
            }
            return materialize(kind, reg, 250 + which, floating && abs, floating && neg);
        };
        struct HalfSource { u32 reg, swizzle; bool absolute, negate; };
        const auto half_source = [&](u32 which) -> HalfSource {
            if (which == 0) return {insn.Bits(24, 8), insn.Bits(74, 2),
                                    insn.Bits(73, 1) != 0, insn.Bits(72, 1) != 0};
            const u32 form = insn.Bits(9, 3);
            const bool wide = which == 1 ? (form == 4 || form == 5) : (form == 2 || form == 3);
            if (wide) {
                const bool immediate = form == 2 || form == 4;
                const u32 reg = materialize(immediate ? 1 : 2, 255, 250 + which, false, false);
                return {reg, immediate ? 0U : insn.Bits(60, 2),
                        !immediate && insn.Bits(62, 1), !immediate && insn.Bits(63, 1)};
            }
            const bool upper = which == 2 || form == 2 || form == 3;
            return {insn.Bits(upper ? 64 : 32, 8), insn.Bits(upper ? 81 : 60, 2),
                    insn.Bits(upper ? 83 : 62, 1) != 0, insn.Bits(upper ? 84 : 63, 1) != 0};
        };
        const auto extract_half = [&](HalfSource source, u32 lane, u32 target) {
            const bool high = source.swizzle == 3 || (source.swizzle == 0 && lane == 1);
            emit(0x5ca8000000000000ULL | predicate | target | (u64{2} << 8) |
                 (u64{source.swizzle == 1 ? 2U : 1U} << 10) | (u64{source.reg} << 20) |
                 (u64{high} << 41) | (u64{source.negate} << 45) | (u64{source.absolute} << 49));
        };
        const auto xmad = [&](u32 target, u32 a, u32 b, u32 c, bool a_high,
                              bool b_high, u32 mode, bool immediate) {
            emit((immediate ? 0x3600000000000000ULL : 0x5b00000000000000ULL) |
                 predicate | target | (u64{a} << 8) |
                 (u64{immediate ? ((b >> (b_high ? 16 : 0)) & 0xffff) : b} << 20) |
                 (u64{!immediate && b_high} << 35) | (u64{c} << 39) |
                 (u64{mode} << 50) | (u64{a_high} << 53));
        };
        const auto multiply_low = [&](u32 a, u32 b, u32 c, bool immediate) {
            xmad(250, a, b, c, false, false, 0, immediate);
            xmad(251, a, b, 255, true, false, 0, immediate);
            xmad(251, a, b, 251, false, true, 0, immediate);
            emit(base(0x5c18000000000000ULL) | (u64{251} << 8) |
                 (u64{250} << 20) | (u64{16} << 39));
        };
        switch (opcode) {
        case 0x304: { // F2F.F16.F32, scalar round-to-nearest conversion.
            if (insn.words[2] != 0x00200800 || insn.Bits(24, 8) ||
                (insn.words[1] & ~0xc00000ffU))
                return fail(index, "native scalar float conversion mode");
            emit(base(0x5ca8000000000000ULL) | (u64{1} << 8) | (u64{2} << 10) |
                 (u64{insn.Bits(32, 8)} << 20) | (u64{insn.Bits(62, 1)} << 49) |
                 (u64{insn.Bits(63, 1)} << 45));
            break;
        }
        case 0x23e: case 0xa3e: { // F2FP.PACK_AB / MERGE_C, round-to-nearest
            if (insn.Bits(62, 2) || insn.Bits(73, 5) || insn.Bits(79, 2) ||
                insn.Bits(81, 10) || insn.Bits(91, 1)) {
                return fail(index, "native float packing modifiers");
            }
            const bool merge = insn.Bits(78, 1);
            if (insn.Bits(72, 1)) return fail(index, "native float packing selector");
            const u32 a = merge ? operand(2, false) : operand(0, false);
            const u32 b = operand(1, false);
            // Canonical F2F writes the converted half in its low 16 bits.
            if (merge) {
                emit(0x5c98000000000000ULL | predicate | 250 | (u64{a} << 20) | (u64{15} << 39));
            } else {
                emit(0x5ca8000000000000ULL | predicate | 250 | (u64{1} << 8) |
                     (u64{2} << 10) | (u64{a} << 20));
            }
            emit(0x5ca8000000000000ULL | predicate | 251 | (u64{1} << 8) |
                 (u64{2} << 10) | (u64{b} << 20));
            emit(base(0x5c18000000000000ULL) | (u64{250} << 8) | (u64{251} << 20) | (u64{16} << 39));
            break;
        }
        case 0x232: case 0x832: case 0xa32: { // HMUL2
            if (insn.Bits(78, 2) || insn.Bits(81, 10) || insn.Bits(91, 1))
                return fail(index, "native half multiply mode");
            const auto a = half_source(0), b = half_source(1);
            const u32 precision = insn.Bits(76, 1) ? 2 : insn.Bits(80, 1);
            emit(base(0x5d08000000000000ULL) | (u64{a.reg} << 8) | (u64{b.reg} << 20) |
                 (u64{b.swizzle} << 28) | (u64{b.absolute} << 30) | (u64{a.negate != b.negate} << 31) |
                 (u64{insn.Bits(77, 1)} << 32) | (u64{precision} << 39) |
                 (u64{a.absolute} << 44) | (u64{a.swizzle} << 47));
            break;
        }
        case 0x230: case 0x430: case 0x630: { // HADD2
            if (insn.Bits(76, 1) || insn.Bits(79, 1) || insn.Bits(81, 10) || insn.Bits(91, 1))
                return fail(index, "native half add mode");
            const auto a = half_source(0);
            const auto b = half_source(opcode == 0x230 ? 1 : 2);
            if (insn.Bits(78, 1)) {
                if (a.reg != 255 || !a.negate || a.absolute || a.swizzle != 0 ||
                    b.swizzle < 2 || insn.Bits(77, 1) || insn.Bits(80, 1))
                    return fail(index, "native float-output half add mode");
                extract_half(b, 0, static_cast<u32>(dest));
            } else {
                emit(base(0x5d10000000000000ULL) | (u64{a.reg} << 8) | (u64{b.reg} << 20) |
                     (u64{b.swizzle} << 28) | (u64{b.absolute} << 30) | (u64{b.negate} << 31) |
                     (u64{insn.Bits(77, 1)} << 32) | (u64{insn.Bits(80, 1)} << 39) |
                     (u64{a.negate} << 43) | (u64{a.absolute} << 44) | (u64{a.swizzle} << 47));
            }
            break;
        }
        case 0x231: case 0x431: case 0x631: case 0x831: case 0xa31: { // HFMA2
            const bool relu = insn.Bits(79, 1) != 0;
            if (insn.Bits(78, 1) || insn.Bits(85, 1) || insn.Bits(91, 1) ||
                (relu ? (insn.Bits(87, 4) != 7 ||
                         insn.Bits(77, 1)) : insn.Bits(87, 4) != 0))
                return fail(index, "native half fused multiply-add mode");
            if (relu && native_predicate != 7) {
                // Guard the whole expansion before its temporary P6 updates.
                relocations.push_back({static_cast<u32>(result.instructions.size()), index + 1});
                emit(0xe24000000000000fULL | (u64{native_predicate ^ 8U} << 16));
                predicate = u64{7} << 16;
            }
            auto a = half_source(0);
            auto b = half_source(1);
            const auto c = half_source(2);
            if (insn.Bits(86, 1)) { // H0_NH1: negate only the upper packed component.
                if (b.swizzle != 0 || b.absolute || b.reg >= 250)
                    return fail(index, "native half mixed-sign source mode");
                emit(0x0400000000000000ULL | predicate | 251 | (u64{b.reg} << 8) |
                     (u64{0x80000000} << 20) | (u64{2} << 53));
                b.reg = 251;
            }
            if (b.absolute || c.absolute || (a.absolute && a.reg >= 248))
                return fail(index, "native half fused multiply-add absolute source");
            if (a.absolute) {
                emit(0x0400000000000000ULL | predicate | 250 | (u64{a.reg} << 8) |
                     (u64{a.swizzle == 1 ? 0x7fffffffU : 0x7fff7fffU} << 20));
                a.reg = 250;
                a.absolute = false;
            }
            const u32 precision = insn.Bits(76, 1) ? 2 : insn.Bits(80, 1);
            emit(0x5d00000000000000ULL | predicate | (relu ? 254U : dest) | (u64{a.reg} << 8) | (u64{b.reg} << 20) |
                 (u64{b.swizzle} << 28) | (u64{c.negate} << 30) | (u64{a.negate != b.negate} << 31) |
                 (u64{insn.Bits(77, 1)} << 32) | (u64{c.swizzle} << 35) |
                 (u64{precision} << 37) | (u64{c.reg} << 39) | (u64{a.swizzle} << 47));
            if (relu) {
                // Clamp the rounded binary16 lanes, preserving positive infinity
                // and canonicalizing either sign of NaN without saturating to one.
                save_predicate();
                for (u32 lane = 0; lane < 2; ++lane) {
                    emit(0x3828000000070000ULL | 251 | (u64{254} << 8) | (u64{lane * 16} << 20));
                    emit(0x0400000000070000ULL | 251 | (u64{251} << 8) | (u64{0xffff} << 20));
                    emit(0x0400000000070000ULL | 252 | (u64{251} << 8) | (u64{0x7fff} << 20));
                    mov_imm(250, 0x7c00);
                    emit(0x5b60000000070000ULL | 7 | (u64{6} << 3) | (u64{250} << 8) |
                         (u64{252} << 20) | (u64{7} << 39) | (u64{1} << 49));
                    emit(0x3828000000070000ULL | 252 | (u64{251} << 8) | (u64{15} << 20));
                    mov_imm(250, 1);
                    emit(0x5c10000000070000ULL | 252 | (u64{252} << 8) | (u64{250} << 20) | (u64{1} << 48));
                    emit(0x5c40000000070000ULL | 251 | (u64{251} << 8) | (u64{252} << 20) | (u64{7} << 48));
                    mov_imm(250, 0x7fff);
                    emit(0x5ca0000000070000ULL | 251 | (u64{250} << 8) | (u64{251} << 20) | (u64{6} << 39));
                    if (lane == 0) {
                        emit(base(0x5c98000000000000ULL) | (u64{251} << 20) | (u64{15} << 39));
                    } else {
                        emit(0x3848000000070000ULL | 251 | (u64{251} << 8) | (u64{16} << 20));
                        emit(base(0x5c40000000000000ULL) | (dest << 8) | (u64{251} << 20) |
                             (u64{1} << 41) | (u64{7} << 48));
                    }
                }
                restore_predicate();
            }
            break;
        }
        case 0x240: case 0x840: { // HMNMX2, exact half promotion for min/max
            if (insn.Bits(76, 11) || insn.Bits(91, 1))
                return fail(index, "native half min/max mode");
            const auto a = half_source(0);
            const auto b = opcode == 0x840 ? HalfSource{255, 0, false, false} : half_source(1);
            if (a.swizzle == 1 || b.swizzle == 1 || (b.reg >= 250 && b.reg != 255))
                return fail(index, "native half min/max source mode");
            for (u32 lane = 0; lane < 2; ++lane) {
                extract_half(a, lane, 250);
                if (opcode == 0x840) {
                    const u32 half = (insn.words[1] >> (lane * 16)) & 0xffff;
                    mov_imm(251, half);
                    extract_half(HalfSource{251, 2, false, false}, 0, 251);
                } else {
                    extract_half(b, lane, 251);
                }
                const u32 target = lane == 0 ? 252 : 250;
                emit(0x5c60000000000000ULL | predicate | target | (u64{250} << 8) | (u64{251} << 20) |
                     (u64{insn.Bits(87, 3)} << 39) | (u64{insn.Bits(90, 1)} << 42));
                emit(0x5ca8000000000000ULL | predicate | target | (u64{1} << 8) |
                     (u64{2} << 10) | (u64{target} << 20));
            }
            emit(base(0x5c18000000000000ULL) | (u64{250} << 8) | (u64{252} << 20) | (u64{16} << 39));
            break;
        }
        case 0xc24: case 0xe24: { // IMAD low word with a uniform multiplier or addend.
            // 0xe24 keeps the uniform addend in bits 32-37; bit 63 negates it ("RZ, RZ, -UR4").
            const bool negate_c = opcode == 0xe24 && insn.Bits(63, 1) != 0;
            if ((insn.words[2] & ~0x080002ffU) != 0x078e0000 ||
                (insn.words[1] & ~(0xffU | (negate_c ? 0x80000000U : 0U))))
                return fail(index, "native uniform-addend multiply mode");
            const u32 a = operand(0, false), b = operand(1, false);
            u32 c = operand(2, false);
            if (negate_c) {
                emit(0x5c10000000000000ULL | predicate | 252 | (u64{255} << 8) | (u64{c} << 20) |
                     (u64{1} << 48));
                c = 252;
            }
            multiply_low(a, b, c, false);
            break;
        }
        case 0x224: { // IMAD register / zero-product move
            if (insn.Bits(24, 8) == 255 && insn.words[1] == 255 && insn.Bits(75, 1) &&
                (insn.words[2] & ~0xaffU) == 0x078e0000) {
                emit(base(0x5c10000000000000ULL) | (u64{255} << 8) |
                    (u64{insn.Bits(64, 8)} << 20) | (u64{1} << 48));
                break;
            }
            if (insn.Bits(24, 8) == 255 && insn.words[1] == 255 &&
                (insn.words[2] & ~0xffU) == 0x078e0000) {
                emit(base(0x5c98000000000000ULL) | (u64{15} << 39) | (u64{insn.Bits(64, 8)} << 20));
                break;
            }
            if ((insn.words[2] & ~0x2ffU) != 0x078e0000 || (insn.words[1] & ~0xffU))
                return fail(index, "native integer register multiply mode");
            multiply_low(insn.Bits(24, 8), insn.Bits(32, 8), insn.Bits(64, 8), false);
            break;
        }
        case 0x2a5: { // UIMAD.WIDE.U32 UR(d):UR(d+1) = URa * URb + URc:UR(c+1)
            const u32 a = uniform_register(insn.Bits(24, 8)), b = uniform_register(insn.Bits(32, 8));
            const u32 c = insn.Bits(64, 8);
            const u32 c_low = uniform_register(c), c_high = c == 63 ? 255U : uniform_register(c + 1);
            const u32 d_low = uniform_register(insn.Bits(16, 8)), d_high = uniform_register(insn.Bits(16, 8) + 1);
            if (native_predicate != 7 || (insn.words[1] & ~0xffU) || (insn.words[2] & ~0xffU) != 0x0f8e0000 ||
                (insn.Bits(16, 8) & 1) || (c != 63 && (c & 1)) || a == ~0U || b == ~0U || c_low == ~0U ||
                c_high == ~0U || d_low == ~0U || d_high == ~0U || d_low == 255)
                return fail(index, "native uniform wide multiply mode");
            // High word in R253 (base-2^16 long multiplication, as IMAD.HI), low word in R250.
            xmad(250, a, b, 255, false, false, 0, false);
            xmad(251, a, b, 250, true, false, 2, false);
            xmad(250, a, b, 251, false, true, 1, false);
            xmad(252, a, b, 251, true, true, 2, false);
            emit(0x3828000000070000ULL | 250 | (u64{250} << 8) | (u64{16} << 20));
            emit(0x5c10000000070000ULL | 253 | (u64{250} << 8) | (u64{252} << 20));
            xmad(250, a, b, 255, false, false, 0, false);
            xmad(251, a, b, 255, true, false, 0, false);
            xmad(251, a, b, 251, false, true, 0, false);
            emit(0x5c18000000070000ULL | 250 | (u64{251} << 8) | (u64{250} << 20) | (u64{16} << 39));
            emit(0x5c10000000070000ULL | 250 | (u64{250} << 8) | (u64{c_low} << 20) | (u64{1} << 47));
            emit(0x5c10000000070000ULL | d_high | (u64{253} << 8) | (u64{c_high} << 20) | (u64{1} << 43));
            emit(0x5c10000000070000ULL | d_low | (u64{250} << 8) | (u64{255} << 20));
            break;
        }
        case 0x827: { // IMAD.HI.U32, captured zero addend
            if (insn.words[2] != 0x078e00ff) return fail(index, "native high multiply mode");
            const u32 a = insn.Bits(24, 8), b = insn.words[1];
            // Base-2^16 long multiplication. Each intermediate sum fits U32.
            xmad(250, a, b, 255, false, false, 0, true);
            xmad(251, a, b, 250, true, false, 2, true);
            xmad(250, a, b, 251, false, true, 1, true);
            xmad(252, a, b, 251, true, true, 2, true);
            emit(0x3828000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{16} << 20));
            emit(base(0x5c10000000000000ULL) | (u64{250} << 8) | (u64{252} << 20));
            break;
        }
        case 0xa24: { // IMAD, constant-buffer multiplier
            if ((insn.words[2] & ~0x2ffU) != 0x078e0000 || !valid_cbuf() || insn.Bits(62, 2))
                return fail(index, "native integer constant-buffer multiply mode");
            emit(0x4c98000000000000ULL | predicate | 252 | (u64{15} << 39) | cbuf());
            multiply_low(insn.Bits(24, 8), 252, insn.Bits(64, 8), false);
            break;
        }
        case 0x424: case 0x624: { // IMAD.MOV.U32: zero product plus immediate/cbuf
            if (opcode == 0x624 && insn.Bits(24, 8) == 255 &&
                (insn.words[2] & ~0x07800000U) == 0x000e06ffU) {
                const u32 source = operand(2, false);
                mov_imm(250, 1);
                emit(0x5ca0000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{255} << 20) |
                    (u64{insn.Bits(87, 3)} << 39) | (u64{insn.Bits(90, 1)} << 42));
                emit(base(0x5c10000000000000ULL) | (u64{source} << 8) | (u64{250} << 20));
                break;
            }
            if (insn.Bits(24, 8) != 255 || insn.Bits(64, 8) != 255 || insn.words[2] != 0x078e00ff) {
                if ((insn.words[2] & ~0x2ffU) != 0x078e0000 || (opcode == 0x624 && insn.Bits(62, 2)))
                    return fail(index, "native multiply wide addend mode");
                const u32 a = operand(0, false), b = operand(1, false), c = operand(2, false);
                multiply_low(a, b, c, false);
                break;
            }
            if (insn.Bits(24, 8) != 255 || insn.Bits(64, 8) != 255 ||
                insn.words[2] != 0x078e00ff)
                return fail(index, "native integer move mode");
            if (opcode == 0x624) {
                if (!valid_cbuf() || insn.Bits(62, 2))
                    return fail(index, "native integer move constant buffer");
                emit(base(0x4c98000000000000ULL) | (u64{15} << 39) | cbuf());
            } else {
                emit(base(0x0100000000000000ULL) | (u64{15} << 12) |
                     (u64{insn.words[1]} << 20));
            }
            break;
        }
        case 0x824: { // Captured low-word IMAD forms; bit 75 negates the addend.
            const bool negate_c = insn.Bits(75, 1) != 0;
            if (insn.words[1] == 1 && (insn.words[2] & 0xfffff700U) == 0x078e0200) {
                emit(base(0x5c10000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) |
                     (u64{insn.Bits(64, 8)} << 20) | (u64{negate_c} << 48));
                break;
            }
            const u32 multiplier = insn.words[1];
            if (insn.Bits(64, 8) == 255 && !negate_c && multiplier > 1 && !(multiplier & (multiplier - 1)) &&
                (insn.words[2] & ~0x2ffU) == 0x078e0000) {
                // IMAD.SHL by a power of two: one shift (also the form Eden's jump-table tracking
                // expects between the clamp and the table load).
                u32 shift = 0;
                while ((1U << shift) != multiplier) ++shift;
                emit(base(0x3848000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) | (u64{shift} << 20));
                break;
            }
            if (insn.words[1] != 48 || insn.words[2] != 0x078e02ff) {
                if ((insn.words[2] & ~0xaffU) != 0x078e0000)
                    return fail(index, "native integer multiply mode");
                u32 c = insn.Bits(64, 8);
                if (negate_c && c != 255) {
                    emit(0x5c10000000000000ULL | predicate | 252 | (u64{255} << 8) | (u64{c} << 20) |
                         (u64{1} << 48));
                    c = 252;
                }
                multiply_low(insn.Bits(24, 8), insn.words[1], c, true);
                break;
            }
            const u64 source = insn.Bits(24, 8);
            emit(0x5c18000000000000ULL | predicate | 250 | (source << 8) | (source << 20) | (u64{1} << 39));
            emit(base(0x3848000000000000ULL) | (u64{250} << 8) | (u64{4} << 20));
            break;
        }
        case 0x806: { // VOTE: register ballot, ordinary predicate input and output.
            if (insn.words[1] || insn.Bits(72, 2) > 2 || (insn.words[2] & ~0x078e0300U))
                return fail(index, "native vote mode");
            emit(base(0x50d8000000000000ULL) | (u64{insn.Bits(87, 4)} << 39) |
                 (u64{insn.Bits(81, 3)} << 45) | (u64{insn.Bits(72, 2)} << 48));
            break;
        }
        case 0x886: { // VOTEU ballot and independent uniform predicate output.
            const u32 output = insn.Bits(81, 3), target = insn.Bits(16, 8);
            if ((target >= 16 && target != 63) || insn.words[1] ||
                insn.Bits(72, 2) > 2 || native_predicate != 7)
                return fail(index, "native uniform vote mode");
            const u32 temporary = output == 7 ? 7 : 6 - output;
            const u32 mask = 1U << temporary;
            if (output != 7) emit(0x38e8000000070000ULL | 253 | (u64{255} << 8) | (u64{mask} << 20));
            emit(0x50d8000000070000ULL | uniform_register(target) |
                (u64{insn.Bits(87, 4)} << 39) | (u64{temporary} << 45) | (u64{insn.Bits(72, 2)} << 48));
            if (output != 7) {
                emit(0x38e8000000070000ULL | 239 | (u64{239} << 8) | (u64{mask} << 20));
                emit(0x38f0000000070000ULL | (u64{253} << 8) | (u64{mask} << 20));
            }
            break;
        }
        case 0x3c2: { // R2UR: broadcast the first lane satisfying the execution predicate.
            const u32 target = uniform_register(static_cast<u32>(dest));
            if (target == ~0U || target == 255 || insn.words[1] || insn.words[2] != 0x000e0000)
                return fail(index, "native register-to-uniform mode");
            save_predicate();
            emit(0x50d8000000070000ULL | 250 | (u64{native_predicate} << 39) |
                 (u64{6} << 45) | (u64{1} << 48));
            emit(0x5c10000000070000ULL | 251 | (u64{250} << 8) | (u64{255} << 20) | (u64{1} << 49));
            emit(0x5c40000000070000ULL | 250 | (u64{250} << 8) | (u64{251} << 20) | (u64{7} << 48));
            emit(0x5c30000000070000ULL | 250 | (u64{250} << 20));
            emit(0xef10000000070000ULL | 251 | (u64{insn.Bits(24, 8)} << 8) |
                 (u64{250} << 20) | (u64{1} << 29) | (u64{31} << 34) | (u64{7} << 48));
            emit(0x5ca0000000070000ULL | target | (u64{251} << 8) |
                 (u64{target} << 20) | (u64{6} << 39));
            restore_predicate();
            break;
        }
        case 0x81c: { // PLOP3.LUT; bit 67 makes the third source a uniform predicate.
            const u32 target = insn.Bits(81, 3);
            const u32 lut = insn.Bits(64, 3) | (insn.Bits(72, 5) << 3);
            if (target == 7 || insn.Bits(84, 3) != 7 ||
                insn.Bits(16, 8) || insn.words[1])
                return fail(index, "native predicate LUT mode");
            if (!insn.Bits(67, 1) && (lut == 0xfe || lut == 0xa8)) {
                emit(0x5090000000000000ULL | predicate | (u64{target} << 3) | 7 |
                     (u64{insn.Bits(87, 4)} << 12) | (u64{1} << 24) |
                     (u64{insn.Bits(77, 4)} << 29) | (u64{insn.Bits(68, 4)} << 39) |
                     (u64{lut == 0xfe} << 45));
                break;
            }
            if (!insn.Bits(67, 1) && (lut == 0x80 ||
                (lut == 8 && insn.Bits(68, 4) == 7 && insn.Bits(77, 4) == 7))) {
                emit(0x5090000000000000ULL | predicate | (u64{target} << 3) | 7 |
                     (u64{insn.Bits(87, 4) ^ (lut == 8 ? 8U : 0U)} << 12) | (u64{insn.Bits(77, 4)} << 29) |
                     (u64{insn.Bits(68, 4)} << 39));
                break;
            }
            predicate_lut(lut, insn.Bits(87, 4), insn.Bits(77, 4), insn.Bits(68, 4), target,
                          insn.Bits(67, 1) != 0);
            break;
        }
        case 0x89c: { // UPLOP3.LUT: load the uniform predicates into P0-P6, combine, merge back.
            const u32 target = insn.Bits(81, 3);
            const u32 lut = insn.Bits(64, 3) | (insn.Bits(72, 5) << 3);
            if (insn.Bits(84, 3) != 7 || insn.Bits(67, 1) || insn.Bits(16, 8) || insn.words[1])
                return fail(index, "native uniform predicate LUT mode");
            if (target == 7) break;
            const auto slot = [](u32 operand) { return (operand & 7) == 7 ? operand : (operand & 8) | (6 - (operand & 7)); };
            predicate = u64{slot(native_predicate)} << 16; // a UPn guard is loaded as well
            emit(0x38e8000000070000ULL | 254 | (u64{255} << 8) | (u64{0x7f} << 20));
            emit(0x38f0000000070000ULL | (u64{239} << 8) | (u64{0x7f} << 20));
            predicate_lut(lut, slot(insn.Bits(87, 4)), slot(insn.Bits(77, 4)), slot(insn.Bits(68, 4)), 6 - target);
            emit(0x38e8000000070000ULL | 239 | (u64{239} << 8) | (u64{1U << (6 - target)} << 20));
            emit(0x38f0000000070000ULL | (u64{254} << 8) | (u64{0x7f} << 20));
            break;
        }
        case 0x883: { // UP2UR UR, UPR, URZ, mask: bit k of the result is UPk (R239 bit 6-k).
            const u32 target = uniform_register(insn.Bits(16, 8)), mask = insn.words[1];
            if (target == ~0U || insn.Bits(24, 8) != 63 || native_predicate != 7 ||
                insn.words[2] != 0x08000000 || mask > 0x7f)
                return fail(index, "native uniform predicate move mode");
            if (target == 255) break;
            mov_imm(target, 0);
            for (u32 k = 0; k < 7; ++k) {
                if (!((mask >> k) & 1)) continue;
                emit(0x3800000000070000ULL | 250 | (u64{239} << 8) | (u64{(6 - k) | (1U << 8)} << 20));
                emit(0x36f0000000070000ULL | target | (u64{250} << 8) | (u64{k | (1U << 8)} << 20) |
                     (u64{target} << 39));
            }
            break;
        }
        case 0x882: { // UMOV immediate.
            if (dest >= 16 || insn.words[2]) return fail(index, "native uniform immediate move mode");
            mov_imm(static_cast<u32>(dest) + 216, insn.words[1]);
            break;
        }
        case 0xc82: { // UMOV UR, UR (or URZ), including uniform execution predicates.
            const u32 source = insn.words[1] < 64 ? uniform_register(insn.words[1]) : ~0U;
            if (dest >= 16 || source == ~0U || insn.words[2] != 0x08000000)
                return fail(index, "native uniform move mode");
            emit(0x5c98000000000000ULL | predicate | (dest + 216) |
                 (u64{source} << 20) | (u64{15} << 39));
            break;
        }
        case 0xab9: case 0xabb: { // ULDC U32/U64, direct or indexed.
            const u32 uniform_dest = insn.Bits(16, 8), index_reg = insn.Bits(24, 8);
            const u32 width = insn.Bits(73, 3);
            const u32 target = uniform_register(uniform_dest);
            const u32 index_target = opcode == 0xabb ? uniform_register(index_reg) : 255U;
            if (target == ~0U || target == 255 || (width != 4 && width != 5) ||
                (width == 5 && ((uniform_dest & 1) || uniform_register(uniform_dest + 1) != target + 1)) ||
                bank() >= 18 || insn.Bits(38, 16) % 4 || insn.Bits(62, 2) ||
                (insn.words[2] & ~0xe00U) != (opcode == 0xab9 ? 0U : 0x08000000U) ||
                index_target == ~0U)
                return fail(index, "native uniform constant load mode");
            if (opcode == 0xab9 && width == 4) {
                emit(0x4c98000000000000ULL | predicate | target | (u64{15} << 39) | cbuf());
            } else {
                emit(0xef90000000000000ULL | predicate | target |
                     (u64{index_target} << 8) | (u64{insn.Bits(38, 16)} << 20) |
                     (u64{bank()} << 36) | (u64{width} << 48));
            }
            break;
        }
        case 0x899: { // USHF.L.U32; accept a zero word outside the selected half.
            const u32 dst = insn.Bits(16, 8), low = insn.Bits(24, 8), high = insn.Bits(64, 8);
            if (insn.words[2] == (0x08011400U | high) && low == 63 && high < 16 &&
                dst < 16 && native_predicate == 7 && insn.words[1] <= 32) {
                emit(0x3828000000000000ULL | predicate | (dst + 216) |
                     (u64{high + 216} << 8) | (u64{insn.words[1]} << 20) | (u64{1} << 48));
                break;
            }
            // Logical right shifts of one register (bit 76 right, bit 80 high word, bits 73-74
            // U32): USHF.R.U32 URd, URa, n, URZ and USHF.R.U32.HI URd, URZ, n, URb.
            const bool right_low = insn.words[2] == 0x0800163f && low < 16;
            const bool right_high = insn.words[2] == (0x08011600U | high) && low == 63 && high < 16;
            if ((right_low || right_high) && dst < 16 && native_predicate == 7 && insn.words[1] <= 32) {
                emit(0x3828000000000000ULL | predicate | (dst + 216) |
                     (u64{(right_high ? high : low) + 216} << 8) | (u64{insn.words[1]} << 20));
                break;
            }
            const bool high_result = insn.words[2] == (0x08010600U | high) && low == 63 && high < 16;
            const bool low_result = insn.words[2] == 0x0800063f && low < 16;
            if (dst >= 16 || native_predicate != 7 || insn.words[1] > 32 || (!high_result && !low_result))
                return fail(index, "native uniform shift mode");
            emit(0x3848000000000000ULL | predicate | (dst + 216) |
                 (u64{(high_result ? high : low) + 216} << 8) | (u64{insn.words[1]} << 20));
            break;
        }
        case 0xb82: { // LDC.64 indexed byte offset
            const u32 width = insn.Bits(73, 3);
            if ((width != 4 && width != 5) || insn.Bits(78, 2) || insn.Bits(91, 1) ||
                bank() >= 18 ||
                (width == 5 && ((dest & 1) || dest >= 254))) return fail(index, "native indexed constant load mode");
            emit(base(0xef90000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) |
                 (u64{insn.Bits(38, 16)} << 20) | (u64{bank()} << 36) | (u64{width} << 48));
            break;
        }
        case 0xf89: { // SHFL with immediate lane and segmentation/clamp mask, any mode.
            const u32 lane = insn.Bits(53, 5), mask = insn.Bits(40, 13), mode = insn.Bits(58, 2);
            if (insn.words[2] != 0x000e0000 || insn.Bits(32, 8) || insn.Bits(60, 4))
                return fail(index, "native immediate shuffle mode");
            emit(base(0xef10000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) |
                 (u64{lane} << 20) | (u64{1} << 28) | (u64{1} << 29) |
                 (u64{mode} << 30) | (u64{mask} << 34) | (u64{7} << 48));
            break;
        }
        case 0x822: { // FSWZADD: per lane 0 = a+b, 1 = a-b, 2 = b-a, 3 = b (nvdisasm PP/PN/NP/ZP).
            const u32 swizzle = insn.words[1];
            if (swizzle > 0xff || insn.words[2] > 255)
                return fail(index, "native swizzled add mode");
            // Maxwell's lane codes 1 and 2 are the other way round (1 = b-a, 2 = a-b).
            const u32 maxwell = ((swizzle & 0x55) << 1) | ((swizzle & 0xaa) >> 1);
            emit(base(0x50f8000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) |
                 (u64{insn.Bits(64, 8)} << 20) | (u64{maxwell} << 28));
            break;
        }
        case 0xb6c: { // TXD 2D or ARRAY_2D, four channels; the two destination pairs anywhere.
            const u32 binding = insn.Bits(54, 5), offset = insn.Bits(40, 14);
            const u32 coordinates = insn.Bits(24, 8), derivatives = insn.Bits(32, 8);
            // Ampere dimensions: 1 = 2D, 5 = ARRAY_2D; Maxwell TXD's are 2 and 3. The coordinates
            // (x, y[, layer]) and the four gradients sit in the same registers for both.
            const u32 dimension = insn.Bits(61, 3);
            const u32 high = insn.Bits(64, 8); // destination of the third and fourth channels
            if ((insn.words[2] & ~0xffU) != 0x001e0f00 || insn.Bits(59, 2) ||
                (dimension != 1 && dimension != 5) || binding >= 18 || offset > 0x1fff ||
                (dest & 1) || dest >= 248 || (high & 1) || (high >= 248 && high != 255) ||
                coordinates >= 248 || derivatives >= 247)
                return fail(index, "native gradient texture mode");
            if (result.required_texture_buffer != ~0U && result.required_texture_buffer != binding)
                return fail(index, "native texture descriptor buffer conflict");
            result.required_texture_buffer = binding;
            // Maxwell writes four consecutive registers: when the second pair is elsewhere (Splatoon
            // Raiders: R12/R18, R18/R44), write them to scratch and move each pair into place.
            const bool consecutive = high == dest + 2;
            const u64 target = consecutive ? dest : 248;
            emit(0xde00000000000000ULL | predicate | target | (u64{coordinates} << 8) |
                 (u64{derivatives} << 20) | (u64{dimension == 5 ? 3U : 2U} << 28) | (u64{15} << 31) |
                 (u64{offset} << 36) | (u64{insn.Bits(90, 1)} << 49) | (u64{7} << 51));
            if (!consecutive) {
                for (u32 i = 0; i < 2; ++i) {
                    emit(0x5c98000000000000ULL | predicate | (dest + i) | (u64{15} << 39) | (u64{248 + i} << 20));
                    if (high != 255)
                        emit(0x5c98000000000000ULL | predicate | (high + i) | (u64{15} << 39) | (u64{250 + i} << 20));
                }
            }
            break;
        }
        case 0x989: { // SHFL, immediate lane and register clamp/segmentation mask, any mode.
            if ((insn.words[1] & 0xf01fffffU) || (insn.words[2] & ~0x000e00ffU))
                return fail(index, "native register-mask shuffle mode");
            emit(base(0xef10000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) |
                 (u64{insn.Bits(53, 5)} << 20) | (u64{1} << 28) | (u64{insn.Bits(58, 2)} << 30) |
                 (u64{insn.Bits(64, 8)} << 39) | (u64{insn.Bits(81, 3)} << 48));
            break;
        }
        case 0x589: { // SHFL.IDX, register lane and immediate segmentation mask.
            if ((insn.words[2] & ~0x000e0000U) || insn.Bits(53, 11))
                return fail(index, "native shuffle mode");
            emit(0xef10000000000000ULL | predicate | dest |
                (u64{insn.Bits(24, 8)} << 8) | (u64{insn.Bits(32, 8)} << 20) |
                (u64{1} << 29) | (u64{insn.Bits(40, 13)} << 34) | (u64{insn.Bits(81, 3)} << 48));
            break;
        }
        case 0x98e: { // RED.E.{ADD..XOR}.STRONG.GPU U32 (op in bits 87-89), register address.
            if ((insn.words[2] & ~0x0380003fU) == 0x0c10e100 && insn.Bits(24, 8) == 255) {
                // [URa + offset]: a uniform 64-bit address with a signed byte offset.
                const u32 address = uniform_register(insn.Bits(64, 6));
                const s32 offset = static_cast<s32>(insn.Bits(40, 24) << 8) >> 8;
                if (address == ~0U || (address & 1) || offset < -0x80000 || offset > 0x7ffff)
                    return fail(index, "native global reduction uniform address");
                emit(0xebf8000000000000ULL | predicate | insn.Bits(32, 8) | (u64{address} << 8) |
                     (u64{insn.Bits(87, 3)} << 23) | (u64{static_cast<u32>(offset) & 0xfffff} << 28) |
                     (u64{1} << 48));
                break;
            }
            if ((insn.words[2] & ~0x03800000U) != 0x0010e100 || insn.Bits(40, 24) || (insn.Bits(24, 8) & 1))
                return fail(index, "native global reduction mode");
            emit(0xebf8000000000000ULL | predicate | insn.Bits(32, 8) |
                (u64{insn.Bits(24, 8)} << 8) | (u64{insn.Bits(87, 3)} << 23) | (u64{1} << 48));
            break;
        }
        case 0x2bd: // UFLO.U32, the highest set-bit index (.SH, bit 74: its shift amount).
            if (dest >= 16 || insn.words[1] >= 16 || (insn.words[2] & ~0x400U) != 0x080e0000 || native_predicate != 7)
                return fail(index, "native uniform leading bit mode");
            emit(0x5c30000000000000ULL | predicate | (dest + 216) | (u64{insn.words[1] + 216} << 20) |
                 (u64{insn.Bits(74, 1)} << 41));
            break;
        case 0x2be: // UBREV, as a bit-reversing zero shift like BREV.
            if (dest >= 16 || insn.words[1] >= 16 || insn.words[2] != 0x08000000 || native_predicate != 7)
                return fail(index, "native uniform bit reverse mode");
            emit(0x3828000000000000ULL | predicate | (dest + 216) | (u64{insn.words[1] + 216} << 8) |
                 (u64{1} << 39) | (u64{1} << 40));
            break;
        case 0x3a1: { // MATCH.ALL.U64 with only the all-equal predicate retained.
            const u32 source = insn.Bits(24, 8), target = insn.Bits(81, 3);
            if (dest != 255 || source >= 254 || (source & 1) || target >= 6 ||
                native_predicate != 7 || insn.words[1] || (insn.words[2] & ~0xe0000U) != 0x200)
                return fail(index, "native warp match mode");
            save_predicate();
            emit(0x50d8000000070000ULL | 250 | (u64{7} << 39) | (u64{7} << 45) | (u64{1} << 48));
            emit(0x5c10000000070000ULL | 251 | (u64{250} << 8) | (u64{255} << 20) | (u64{1} << 49));
            emit(0x5c40000000070000ULL | 250 | (u64{250} << 8) | (u64{251} << 20) | (u64{7} << 48));
            emit(0x5c30000000070000ULL | 250 | (u64{250} << 20));
            for (u32 word = 0; word < 2; ++word)
                emit(0xef10000000070000ULL | (251 + word) | (u64{source + word} << 8) |
                    (u64{250} << 20) | (u64{1} << 29) | (u64{31} << 34) | (u64{7} << 48));
            emit(0x5b60000000070000ULL | 7 | (u64{6} << 3) | (u64{source} << 8) |
                (u64{251} << 20) | (u64{7} << 39) | (u64{2} << 49));
            emit(0x5b60000000070000ULL | 7 | (u64{6} << 3) | (u64{source + 1} << 8) |
                (u64{252} << 20) | (u64{6} << 39) | (u64{2} << 49));
            emit(0x50d8000000070000ULL | 255 | (u64{6} << 39) | (u64{target} << 45));
            restore_predicate();
            break;
        }
        case 0x3c4: { // REDUX.SUM (modulo 2^32) or .MAX (unsigned) over the actual active lanes.
            const u32 source = insn.Bits(24, 8), target = uniform_register(static_cast<u32>(dest));
            const bool maximum = insn.words[2] == 0x14000;
            if (target == ~0U || target == 255 || insn.words[1] ||
                (insn.words[2] != 0xc000 && !maximum) || native_predicate != 7)
                return fail(index, "native warp sum mode");
            emit(0x50d8000000070000ULL | 250 | (u64{7} << 39) | (u64{7} << 45) | (u64{1} << 48));
            mov_imm(target, 0);
            for (u32 lane = 0; lane < 32; ++lane) {
                emit(0xef10000000070000ULL | 251 | (u64{source} << 8) | (u64{lane} << 20) |
                    (u64{1} << 28) | (u64{1} << 29) | (u64{31} << 34) | (u64{7} << 48));
                emit(0x3828000000070000ULL | 252 | (u64{250} << 8) | (u64{lane} << 20));
                emit(0x0400000000070000ULL | 252 | (u64{252} << 8) | (u64{1} << 20));
                emit(0x5c10000000070000ULL | 252 | (u64{255} << 8) | (u64{252} << 20) | (u64{1} << 48));
                emit(0x5c40000000070000ULL | 251 | (u64{251} << 8) | (u64{252} << 20) | (u64{7} << 48));
                if (maximum) {
                    emit(0x5c20000000070000ULL | target | (u64{target} << 8) | (u64{251} << 20) | (u64{15} << 39));
                } else {
                    emit(0x5c10000000070000ULL | target | (u64{target} << 8) | (u64{251} << 20));
                }
            }
            break;
        }
        case 0xf8c: { // ATOMS.POPC.INC.32 with no returned value: one increment per active lane.
            const u32 stride = insn.Bits(78, 2);
            if (dest != 255 || insn.words[1] || (insn.words[2] & ~0xc000U) != 0x0d00003f)
                return fail(index, "native shared increment mode");
            u32 address = insn.Bits(24, 8);
            if (stride && address != 255) {
                emit(0x3848000000000000ULL | predicate | 251 | (u64{address} << 8) |
                    (u64{stride + 1} << 20));
                address = 251;
            }
            mov_imm(250, 1);
            emit(0xec00000000000000ULL | predicate | 255 | (u64{address} << 8) | (u64{250} << 20));
            break;
        }
        case 0x38c: { // ATOMS.ADD.U32, naturally aligned shared byte offset.
            const u32 offset = insn.Bits(40, 24);
            if (insn.words[2] || (offset & 3)) return fail(index, "native shared atomic mode");
            emit(0xec00000000000000ULL | predicate | dest |
                (u64{insn.Bits(24, 8)} << 8) | (u64{insn.Bits(32, 8)} << 20) |
                (u64{offset >> 2} << 30));
            break;
        }
        case 0x3a8: { // ATOMG.E.ADD.STRONG.GPU U32 at a 64-bit register address plus an offset.
            const u32 address = insn.Bits(24, 8);
            const s32 offset = static_cast<s32>(insn.Bits(40, 24) << 8) >> 8;
            if (insn.words[2] != 0x001ee100 || (address & 1) || address >= 246 ||
                offset < -0x80000 || offset > 0x7ffff)
                return fail(index, "native global atomic register mode");
            emit(0xed00000000000000ULL | predicate | dest | (u64{address} << 8) |
                (u64{insn.Bits(32, 8)} << 20) | (u64{static_cast<u32>(offset) & 0xfffff} << 28) |
                (u64{1} << 48));
            break;
        }
        case 0x9a8: { // ATOMG.E.ADD.STRONG.GPU U32 through a uniform pointer plus an offset.
            const u32 address = uniform_register(insn.Bits(64, 6));
            const s32 offset = static_cast<s32>(insn.Bits(40, 24) << 8) >> 8;
            if (insn.Bits(24, 8) != 255 || offset < -0x80000 || offset > 0x7ffff ||
                (insn.words[2] & ~0x3fU) != 0x081ee140 || address == ~0U || (address & 1))
                return fail(index, "native global atomic mode");
            emit(0xed00000000000000ULL | predicate | dest | (u64{address} << 8) |
                (u64{insn.Bits(32, 8)} << 20) | (u64{static_cast<u32>(offset) & 0xfffff} << 28) |
                (u64{1} << 48));
            break;
        }
        case 0xb97: { // SULD.P.2D/2D_ARRAY.IGN, direct descriptor, channel mask at 72-75.
            const u32 binding = insn.Bits(54, 5), offset = insn.Bits(40, 14);
            const u32 coordinate = insn.Bits(24, 8);
            const u32 channels = insn.Bits(72, 4);
            const u32 dimension = insn.Bits(61, 3); // Maxwell's codes: 3 = 2D, 4 = 2D_ARRAY
            if ((insn.words[2] & ~0xf00U) != 0x001e0000 || channels == 0 || (dimension != 3 && dimension != 4) ||
                binding >= 18 || offset > 0x1fff || insn.Bits(59, 2) || dest == 255 ||
                coordinate > (dimension == 4 ? 252U : 253U))
                return fail(index, "native formatted surface load mode");
            if (result.required_texture_buffer != ~0U && result.required_texture_buffer != binding)
                return fail(index, "native surface descriptor buffer conflict");
            result.required_texture_buffer = binding;
            emit(base(0xeb00000000000000ULL) | (u64{coordinate} << 8) |
                (u64{channels} << 20) | (u64{dimension} << 33) | (u64{offset} << 36) | (u64{1} << 51));
            break;
        }
        case 0xb9b: { // SUST.P.2D/3D.IGN, direct descriptor and all four channels.
            const u32 binding = insn.Bits(54, 5), offset = insn.Bits(40, 14);
            const u32 data = insn.Bits(32, 8), coordinate = insn.Bits(24, 8);
            const u32 dimension = insn.Bits(61, 3);
            if (insn.words[2] != 0x00100f00 || dimension < 3 || dimension > 5 ||
                binding >= 18 || offset > 0x1fff || insn.Bits(59, 2) || data > 252 || coordinate > (dimension >= 4 ? 252U : 253U))
                return fail(index, "native formatted surface store mode");
            if (result.required_texture_buffer != ~0U && result.required_texture_buffer != binding)
                return fail(index, "native surface descriptor buffer conflict");
            result.required_texture_buffer = binding;
            emit(0xeb20000000000000ULL | predicate | data | (u64{coordinate} << 8) |
                (u64{15} << 20) | (u64{dimension} << 33) | (u64{offset} << 36) | (u64{1} << 51));
            break;
        }
        case 0x301: // BREV register.
            if (insn.words[2] || insn.words[1] > 255) return fail(index, "native bit reverse mode");
            emit(base(0x3828000000000000ULL) | (u64{insn.words[1]} << 8) |
                (u64{1} << 39) | (u64{1} << 40));
            break;
        case 0x219: case 0xc19: { // U32 register (or uniform, 0xc19) shifts with a zero word outside the selected result.
            const u32 low = insn.Bits(24, 8), high = insn.Bits(64, 8);
            const u32 shift = opcode == 0xc19 ? uniform_register(insn.Bits(32, 8)) : insn.Bits(32, 8);
            const bool right = insn.Bits(76, 1) != 0, high_result = insn.Bits(80, 1) != 0;
            // SHF.R.U32 of the low word with a zero high word: a plain logical right shift.
            if (right && !high_result && high == 255 && insn.Bits(73, 2) == 3 && shift != ~0U &&
                insn.words[1] <= 255 &&
                (insn.words[2] & ~(0x000118ffU | (opcode == 0xc19 ? 0x08000000U : 0U))) == 0x00000600U) {
                emit(base(0x5c28000000000000ULL) | (u64{low} << 8) | (u64{shift} << 20));
                break;
            }
            if (insn.words[1] > 255 || insn.Bits(73, 2) != 3 || shift == ~0U ||
                (insn.words[2] & ~(0x000118ffU | (opcode == 0xc19 ? 0x08000000U : 0U))) != 0x00000600U ||
                (right ? (!high_result || low != 255) : (high_result || high != 255)))
                return fail(index, "native register shift mode");
            emit(base(right ? 0x5cf8000000000000ULL : 0x5bf8000000000000ULL) |
                (u64{right ? high : 255U} << 8) | (u64{shift} << 20) |
                (u64{right ? 255U : low} << 39) | (u64{insn.Bits(75, 1)} << 50));
            break;
        }
        case 0x300: case 0xd00: { // FLO, including a uniform source.
            if ((insn.words[2] & ~(0x600U | (opcode == 0xd00 ? 0x08000000U : 0U))) != 0x000e0000U)
                return fail(index, "native leading bit mode");
            const u32 source = operand(1, false);
            emit(base(0x5c30000000000000ULL) | (u64{source} << 20) |
                (u64{insn.Bits(63, 1)} << 40) | (u64{insn.Bits(74, 1)} << 41) |
                (u64{insn.Bits(73, 1)} << 48));
            break;
        }
        case 0x309: case 0xd09: { // POPC, including a uniform source.
            if (insn.words[2] != (opcode == 0xd09 ? 0x08000000U : 0U))
                return fail(index, "native population count mode");
            const u32 source = operand(1, false);
            emit(base(0x5c08000000000000ULL) | (u64{source} << 20) | (u64{insn.Bits(63, 1)} << 40));
            break;
        }
        case 0x918:
            if (insn.words[1] || insn.words[2]) return fail(index, "native NOP mode");
            emit(0x50b0000000000f00ULL | predicate);
            break;
        case 0x992: { // MEMBAR.SC: preserve workgroup/device visibility.
            const u32 scope = insn.Bits(76, 3);
            if (insn.words[1] || insn.words[2] != (scope << 12) ||
                (scope != 0 && scope != 2 && scope != 3 && scope != 5))
                return fail(index, "native memory barrier scope");
            const u32 legacy_scope = scope == 0 ? 0 : scope == 2 ? 1 : scope == 3 ? 2 : 3;
            emit(0xef98000000000000ULL | predicate | (u64{legacy_scope} << 8));
            break;
        }
        case 0x9ab: // ERRBAR: wait for prior memory operations on the host backend.
            if (insn.words[1] || insn.words[2]) return fail(index, "native error barrier mode");
            emit(0xef98000000000000ULL | predicate | (u64{1} << 8));
            break;
        case 0xb1d: // BAR.SYNC 0, full workgroup.
            if (insn.words[1] || insn.words[2] || native_predicate != 7)
                return fail(index, "native workgroup barrier mode");
            emit(0xf0a8000000000000ULL | predicate | (u64{1} << 43) |
                (u64{1} << 44) | (u64{1} << 39) | (u64{7} << 39));
            break;
        case 0x388: case 0x984: { // Shared memory load/store with scaled register offsets.
            const u32 width = insn.Bits(73, 3), stride = insn.Bits(78, 2);
            // LDS [Ra.Xn + URb + offset]: bit 91 adds a uniform register (bits 32-39).
            const bool uniform_base = opcode == 0x984 && insn.Bits(91, 1);
            const u32 base_register = uniform_base ? uniform_register(insn.Bits(32, 8)) : 255U;
            if (width > 6 || (insn.words[2] & ~(uniform_base ? 0x08000000U : 0U)) != ((width << 9) | (stride << 14)) ||
                (opcode == 0x984 && !uniform_base && insn.Bits(32, 8)) || base_register == ~0U)
                return fail(index, "native shared memory addressing mode");
            u32 address = insn.Bits(24, 8);
            if (stride && address != 255) {
                emit(0x3848000000000000ULL | predicate | 250 |
                    (u64{address} << 8) | (u64{stride + 1} << 20));
                address = 250;
            }
            if (base_register != 255) {
                emit(0x5c10000000000000ULL | predicate | 250 | (u64{address} << 8) | (u64{base_register} << 20));
                address = 250;
            }
            const u32 data = opcode == 0x388 ? insn.Bits(32, 8) : static_cast<u32>(dest);
            emit((opcode == 0x388 ? 0xef58000000000000ULL : 0xef48000000000000ULL) |
                predicate | data | (u64{address} << 8) | (u64{insn.Bits(40, 24)} << 20) |
                (u64{width} << 48));
            break;
        }
        case 0x23a: { // MOVM.16.MT88: transpose an 8x8 16-bit matrix held one row pair per lane.
            // Lane t holds row t/4, columns 2(t%4)..+1; the result is the transposed matrix in the
            // same layout, so each half comes from lane 8(t%4) + t/8 (+4) at half (t/4)%2.
            const u32 source = insn.Bits(24, 8);
            if (insn.words[1] || insn.words[2] || source >= 246 || dest >= 246)
                return fail(index, "native matrix move mode");
            emit(0xf0c8000000000000ULL | predicate | 246);                                        // laneid
            emit(0x0400000000000000ULL | predicate | 249 | (u64{246} << 8) | (u64{3} << 20));
            emit(0x3848000000000000ULL | predicate | 249 | (u64{249} << 8) | (u64{3} << 20));     // 8(t%4)
            emit(0x3828000000000000ULL | predicate | 247 | (u64{246} << 8) | (u64{3} << 20));     // t/8
            emit(0x5c10000000000000ULL | predicate | 249 | (u64{249} << 8) | (u64{247} << 20));
            emit(0xef10000000000000ULL | predicate | 247 | (u64{source} << 8) | (u64{249} << 20) |
                 (u64{1} << 29) | (u64{31} << 34) | (u64{7} << 48));
            emit(0x1c00000000000000ULL | predicate | 249 | (u64{249} << 8) | (u64{4} << 20));
            emit(0xef10000000000000ULL | predicate | 248 | (u64{source} << 8) | (u64{249} << 20) |
                 (u64{1} << 29) | (u64{31} << 34) | (u64{7} << 48));
            emit(0x3828000000000000ULL | predicate | 249 | (u64{246} << 8) | (u64{2} << 20));
            emit(0x0400000000000000ULL | predicate | 249 | (u64{249} << 8) | (u64{1} << 20));
            emit(0x3848000000000000ULL | predicate | 249 | (u64{249} << 8) | (u64{4} << 20));     // 0 or 16
            emit(0x5c28000000000000ULL | predicate | 247 | (u64{247} << 8) | (u64{249} << 20));
            emit(0x0400000000000000ULL | predicate | 247 | (u64{247} << 8) | (u64{0xffff} << 20));
            emit(0x5c28000000000000ULL | predicate | 248 | (u64{248} << 8) | (u64{249} << 20));
            emit(0x5c18000000000000ULL | predicate | dest | (u64{248} << 8) | (u64{247} << 20) | (u64{16} << 39));
            break;
        }
        case 0x23c: { // HMMA.16816.F16: D(16x8) = A(16x16) * B(16x8) + C, f16 fragments.
            // Lane t (group g = t/4, i = t%4): A regs hold rows g / g+8 at k 2i..2i+1 (a0, a1) and
            // 8+2i.. (a2, a3); B regs hold k 2i.. (b0) / 8+2i.. (b1) of column g; D holds rows
            // g / g+8 at columns 2i, 2i+1. Rebuilt from shuffles with HFMA2, accumulating in R252/R253.
            // The expansion is correct but large: title 4's 372-HMMA network takes the host driver
            // minutes to compile, so it stays opt-in (the QMD still completes when rejected).
            // EDEN_NX2_TENSOR=stub keeps the rest of the program but makes every HMMA D = C.
            static const char* const mode = std::getenv("EDEN_NX2_TENSOR");
            if (!mode) return fail(index, "native matrix multiply (set EDEN_NX2_TENSOR to emulate)");
            const u32 a = insn.Bits(24, 8), b = insn.Bits(32, 8), c = insn.Bits(64, 8);
            if ((insn.words[1] & ~0xffU) || (insn.words[2] & ~0xffU) != 0x800 || dest + 2 > 246 ||
                a + 4 > 246 || b + 2 > 246 || (c != 255 && c + 2 > 246))
                return fail(index, "native matrix multiply mode");
            if (std::string_view{mode} == "stub") {
                for (u32 row = 0; row < 2; ++row) {
                    if (c == 255) mov_imm(static_cast<u32>(dest) + row, 0);
                    else if (c != dest) emit(0x5c98000000000000ULL | predicate | (dest + row) | (u64{15} << 39) | (u64{c + row} << 20));
                }
                break;
            }
            const auto shuffle = [&](u32 target, u32 source_reg) {
                emit(0xef10000000000000ULL | predicate | target | (u64{source_reg} << 8) | (u64{246} << 20) |
                     (u64{1} << 29) | (u64{31} << 34) | (u64{7} << 48));
            };
            const auto lane_a = [&](u32 i) { // 4g + i
                emit(0xf0c8000000000000ULL | predicate | 246);
                emit(0x0400000000000000ULL | predicate | 246 | (u64{246} << 8) | (u64{0x1c} << 20));
                if (i) emit(0x1c00000000000000ULL | predicate | 246 | (u64{246} << 8) | (u64{i} << 20));
            };
            const auto lane_b = [&](u32 add) { // 8(t%4) + add: column 2(t%4) (+1 when add >= 4)
                emit(0xf0c8000000000000ULL | predicate | 246);
                emit(0x0400000000000000ULL | predicate | 246 | (u64{246} << 8) | (u64{3} << 20));
                emit(0x3848000000000000ULL | predicate | 246 | (u64{246} << 8) | (u64{3} << 20));
                if (add) emit(0x1c00000000000000ULL | predicate | 246 | (u64{246} << 8) | (u64{add} << 20));
            };
            const auto hfma2 = [&](u32 accumulator, u32 swizzle_a, u32 pair) {
                emit(0x5d00000000000000ULL | predicate | accumulator | (u64{251} << 8) | (u64{pair} << 20) |
                     (u64{accumulator} << 39) | (u64{swizzle_a} << 47));
            };
            for (u32 row = 0; row < 2; ++row) {
                if (c == 255) mov_imm(252 + row, 0);
                else emit(0x5c98000000000000ULL | predicate | (252 + row) | (u64{15} << 39) | (u64{c + row} << 20));
            }
            for (u32 i = 0; i < 4; ++i) {
                for (u32 k_block = 0; k_block < 2; ++k_block) {
                    lane_b(i);
                    shuffle(247, b + k_block);
                    lane_b(4 + i);
                    shuffle(248, b + k_block);
                    // P0 = (B[2i][2t], B[2i][2t+1]), P1 = (B[2i+1][2t], B[2i+1][2t+1])
                    emit(0x0400000000000000ULL | predicate | 249 | (u64{247} << 8) | (u64{0xffff} << 20));
                    emit(0x5c18000000000000ULL | predicate | 249 | (u64{248} << 8) | (u64{249} << 20) | (u64{16} << 39));
                    emit(0x3828000000000000ULL | predicate | 250 | (u64{247} << 8) | (u64{16} << 20));
                    emit(0x0400000000000000ULL | predicate | 248 | (u64{248} << 8) | (u64{0xffff0000} << 20));
                    emit(0x5c10000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{248} << 20));
                    for (u32 row = 0; row < 2; ++row) {
                        lane_a(i);
                        shuffle(251, a + k_block * 2 + row);
                        hfma2(252 + row, 2, 249); // A.H0 broadcast * P0
                        hfma2(252 + row, 3, 250); // A.H1 broadcast * P1
                    }
                }
            }
            for (u32 row = 0; row < 2; ++row)
                emit(0x5c98000000000000ULL | predicate | (dest + row) | (u64{15} << 39) | (u64{252 + row} << 20));
            break;
        }
        case 0x83b: { // LDSM.16.M88.{1,2,4} (ldmatrix): 8x8 16-bit matrices from shared memory.
            // Lanes 8m..8m+7 give the row addresses of matrix m; lane t receives the 32 bits at
            // row t/4, byte (t%4)*4 of each matrix in Rd+m. Emulated with SHFL.IDX + LDS.32.
            const u32 count = 1U << insn.Bits(72, 2);
            const bool transpose = insn.Bits(78, 1) != 0;
            if (insn.Bits(72, 2) == 3 || (insn.words[2] & ~0x4300U) || insn.Bits(32, 8) ||
                dest + count > 246)
                return fail(index, "native matrix load mode");
            u32 address = insn.Bits(24, 8);
            const u64 offset = insn.Bits(40, 24);
            if (address != 255 && address >= dest && address < dest + count) {
                emit(0x5c10000000000000ULL | predicate | 253 | (u64{address} << 8) | (u64{255} << 20));
                address = 253;
            }
            if (transpose) {
                // .MT88: lane t receives stored rows 2(t%4) and 2(t%4)+1 at column t/4 (16 bits each).
                emit(0xf0c8000000000000ULL | predicate | 246);                                    // laneid
                emit(0x0400000000000000ULL | predicate | 247 | (u64{246} << 8) | (u64{3} << 20));
                emit(0x3848000000000000ULL | predicate | 247 | (u64{247} << 8) | (u64{1} << 20)); // row 2(t%4)
                emit(0x3828000000000000ULL | predicate | 248 | (u64{246} << 8) | (u64{2} << 20));
                emit(0x3848000000000000ULL | predicate | 248 | (u64{248} << 8) | (u64{1} << 20)); // byte 2(t/4)
                for (u32 matrix = 0; matrix < count; ++matrix) {
                    for (u32 half = 0; half < 2; ++half) {
                        emit(0x1c00000000000000ULL | predicate | 249 | (u64{247} << 8) |
                             (u64{matrix * 8 + half} << 20));
                        emit(0xef10000000000000ULL | predicate | 250 | (u64{address} << 8) | (u64{249} << 20) |
                             (u64{1} << 29) | (u64{31} << 34) | (u64{7} << 48));
                        emit(0x5c10000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{248} << 20));
                        emit(0xef48000000000000ULL | predicate | (251 + half) | (u64{250} << 8) |
                             (offset << 20) | (u64{2} << 48));                                  // LDS.U16
                    }
                    emit(0x5c18000000000000ULL | predicate | (dest + matrix) | (u64{252} << 8) |
                         (u64{251} << 20) | (u64{16} << 39));
                }
                break;
            }
            emit(0xf0c8000000000000ULL | predicate | 250);                                    // S2R laneid
            emit(0x3828000000000000ULL | predicate | 251 | (u64{250} << 8) | (u64{2} << 20)); // row
            emit(0x0400000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{3} << 20));
            emit(0x3848000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{2} << 20)); // column bytes
            for (u32 matrix = 0; matrix < count; ++matrix) {
                u32 lane = 251;
                if (matrix) {
                    emit(0x1c00000000000000ULL | predicate | 252 | (u64{251} << 8) | (u64{matrix * 8} << 20));
                    lane = 252;
                }
                emit(0xef10000000000000ULL | predicate | 253 | (u64{address} << 8) | (u64{lane} << 20) |
                     (u64{1} << 29) | (u64{31} << 34) | (u64{7} << 48));
                emit(0x5c10000000000000ULL | predicate | 253 | (u64{253} << 8) | (u64{250} << 20));
                emit(0xef48000000000000ULL | predicate | (dest + matrix) | (u64{253} << 8) |
                     (offset << 20) | (u64{4} << 48));
            }
            break;
        }
        case 0x919: case 0x9c3: { // S2R / S2UR thread and workgroup identities.
            const u32 special = insn.Bits(72, 8);
            if (insn.words[1] || insn.words[2] != (special << 8) ||
                (special != 0 && special != 33 && special != 34 && special != 35 &&
                 special != 37 && special != 38 && special != 39 &&
                 special != 56 && special != 57 && special != 58 && special != 59 && special != 60))
                return fail(index, "native special register");
            const u32 target = opcode == 0x9c3 ? uniform_register(static_cast<u32>(dest)) : static_cast<u32>(dest);
            if (target == ~0U) return fail(index, "native uniform special destination");
            emit(0xf0c8000000000000ULL | predicate | target | (u64{special} << 20));
            break;
        }
        case 0x28c: case 0x88c: { // UISETP, signed (bit 73) or unsigned, with UPT accumulation.
            const u32 output = insn.Bits(81, 3);
            const u32 a = uniform_register(insn.Bits(24, 8)), b = opcode == 0x88c ? 252U : uniform_register(insn.Bits(32, 8));
            if (output > 6 || insn.Bits(84, 3) != 7 || insn.Bits(87, 4) != 7 ||
                (insn.Bits(72, 4) & ~2U) || insn.Bits(12, 4) != 7 || a == ~0U || b == ~0U)
                return fail(index, "native uniform comparison mode");
            const u32 temporary = 6 - output, mask = 1U << temporary;
            if (opcode == 0x88c) mov_imm(252, insn.words[1]);
            emit(0x38e8000000070000ULL | 253 | (u64{255} << 8) | (u64{mask} << 20));
            emit(0x5b60000000070000ULL | 7 | (u64{temporary} << 3) |
                (u64{a} << 8) | (u64{b} << 20) | (u64{7} << 39) | (u64{insn.Bits(73, 1)} << 48) |
                (u64{insn.Bits(76, 3)} << 49));
            emit(0x38e8000000070000ULL | 239 | (u64{239} << 8) | (u64{mask} << 20));
            emit(0x38f0000000070000ULL | (u64{253} << 8) | (u64{mask} << 20));
            break;
        }
        case 0x287: case 0x887: { // USEL, UP0/UP1; preserve ordinary predicates.
            const u32 condition = insn.Bits(87, 3);
            const u32 target = uniform_register(static_cast<u32>(dest));
            const u32 a = uniform_register(insn.Bits(24, 8)), b = opcode == 0x887 ? 252U : uniform_register(insn.Bits(32, 8));
            if (condition > 6 || target == ~0U || a == ~0U || b == ~0U ||
                insn.Bits(12, 4) != 7 || (insn.words[2] & ~0x07800000U) != 0x08000000U)
                return fail(index, "native uniform select mode");
            if (opcode == 0x887 && index > 0 && condition == 0 && insn.words[1] <= 0x7ffff &&
                code[index - 1].Bits(0, 12) == 0x88c &&
                code[index - 1].Bits(24, 8) == insn.Bits(24, 8) &&
                code[index - 1].words[1] == insn.words[1] && code[index - 1].words[2] == 0x0bf01070 &&
                insn.words[2] == 0x08000000) {
                // Preserve the unsigned clamp idiom for indirect-table analysis.
                emit(0x3820000000070000ULL | target | (u64{a} << 8) |
                     (u64{insn.words[1]} << 20) | (u64{7} << 39));
                break;
            }
            if (opcode == 0x887) mov_imm(252, insn.words[1]);
            const u32 temporary = 6 - condition, mask = 1U << temporary;
            emit(0x38e8000000070000ULL | 253 | (u64{255} << 8) | (u64{mask} << 20));
            emit(0x38f0000000070000ULL | (u64{239} << 8) | (u64{mask} << 20));
            emit(0x5ca0000000070000ULL | target | (u64{a} << 8) | (u64{b} << 20) |
                (u64{temporary} << 39) | (u64{insn.Bits(90, 1)} << 42));
            emit(0x38f0000000070000ULL | (u64{253} << 8) | (u64{mask} << 20));
            break;
        }
        case 0x381: case 0x981: { // LDG.E of 8 to 128 bits, register or uniform 64-bit address.
            // Sizes as Maxwell's: U8, S8, U16, S16, 32, 64, 128.
            const u32 size = insn.Bits(73, 3);
            u32 address = insn.Bits(24, 8);
            if (size > 6 || (opcode == 0x381 && insn.Bits(32, 8)) ||
                (size == 5 && (dest & 1)) || (size == 6 && (dest & 3)))
                return fail(index, "native global load mode");
            if (opcode == 0x381) {
                // Bit 77 (strong/scoped ordering) needs nothing extra from a Maxwell LDG.
                if ((insn.words[2] & ~0x2e00U) != 0x001e8100 || (address & 1) || address >= 246)
                    return fail(index, "native global load mode");
            } else {
                // The uniform base sits where a register source would (bits 32-37).
                const u32 uniform_address =
                    insn.Bits(38, 2) ? ~0U : uniform_register(insn.Bits(32, 6));
                if ((insn.words[2] & ~(0x04000000U | 0x3fU | 0xe00U | 0x2000U)) != 0x081e8100 ||
                    uniform_address == ~0U || (uniform_address & 1))
                    return fail(index, "native global load uniform addressing mode");
                if (address == 255) {
                    address = uniform_address;
                } else {
                    if (insn.Bits(90, 1) && ((address & 1) || address >= 246))
                        return fail(index, "native global load register address pair");
                    emit(0x5c10000000000000ULL | predicate | 250 | (u64{uniform_address} << 8) |
                         (u64{address} << 20) | (u64{1} << 47));
                    emit(0x5c10000000000000ULL | predicate | 251 | (u64{uniform_address + 1} << 8) |
                         (u64{insn.Bits(90, 1) ? address + 1 : 255} << 20) | (u64{1} << 43));
                    address = 250;
                }
            }
            emit(base(0xeed0000000000000ULL) | (u64{address} << 8) |
                 (u64{insn.Bits(40, 24)} << 20) | (u64{1} << 45) | (u64{size} << 48));
            break;
        }
        case 0x386: case 0x986: { // Global store of 32/64/128 bits, 64-bit address.
            u32 data = insn.Bits(32, 8);
            const u32 size = insn.Bits(73, 3);
            if (size > 6 ||
                (data != 255 && ((size == 5 && (data & 1)) || (size == 6 && (data & 3)) ||
                                 data + (size < 4 ? 1U : 1U << (size - 4)) > 246)))
                return fail(index, "native global store size");
            if (data == 255 && size > 4) {
                // Zeros wider than one register: store from zeroed scratch registers (aligned to the
                // access: R248-R251 for 128 bits, R250-R251 for 64).
                data = size == 6 ? 248 : 250;
                for (u32 i = 0; i < (1U << (size - 4)); ++i) mov_imm(data + i, 0);
            }
            u32 address = insn.Bits(24, 8);
            bool extended = insn.Bits(72, 1) != 0;
            if (opcode == 0x986) {
                const u32 uniform_address = uniform_register(insn.Bits(64, 6));
                if ((insn.words[2] & ~(0x04000000U | 0x3fU | 0xe00U)) != 0x0810a100 ||
                    uniform_address == ~0U || (uniform_address & 1) || !extended)
                    return fail(index, "native global store uniform addressing mode");
                if (address == 255) address = uniform_address;
                else {
                    if (insn.Bits(90, 1) && ((address & 1) || address >= 254))
                        return fail(index, "native global store register address pair");
                    emit(0x5c10000000000000ULL | predicate | 250 | (u64{uniform_address} << 8) |
                        (u64{address} << 20) | (u64{1} << 47));
                    emit(0x5c10000000000000ULL | predicate | 251 | (u64{uniform_address + 1} << 8) |
                        (u64{insn.Bits(90, 1) ? address + 1 : 255} << 20) | (u64{1} << 43));
                    address = 250;
                }
            } else if ((insn.words[2] & ~0xe00U) != 0x0010a100) {
                return fail(index, "native global store register addressing mode");
            }
            emit(0xeed8000000000000ULL | predicate | data |
                (u64{address} << 8) | (u64{insn.Bits(40, 24)} << 20) |
                (u64{extended} << 45) | (u64{size} << 48));
            break;
        }
        case 0x95b: // Fragment discard; execution predicate is preserved.
            if (insn.words[1] || insn.words[2] != 0x03800000)
                return fail(index, "native fragment discard mode");
            emit(0xe33000000000000fULL | predicate);
            break;
        case 0x305: case 0xb05: case 0xd05: { // F2I NTZ, register/constant-buffer/uniform source
            const u32 mode = insn.words[2] & ~(opcode == 0xd05 ? 0x08000000U : 0U);
            const bool half = opcode == 0x305 && mode == 0x0010f000;
            const bool small = mode == 0x00212800;
            const bool floor = (mode & ~0x10100U) == 0x00207000;
            const bool nearest = (mode & ~0x10100U) == 0x00203000;
            if ((!half && !small && !floor && !nearest && (mode & ~0x100U) != 0x0021f000) ||
                (opcode != 0xb05 && (insn.words[1] & 0xffffff00U)) ||
                (opcode == 0xd05 && uniform_register(insn.Bits(32, 8)) == ~0U)) {
                return fail(index, "native float-to-integer conversion mode");
            }
            const bool is_signed = insn.Bits(72, 1);
            u32 source = opcode == 0xb05 ? materialize(2, 0, 252, false, false)
                       : opcode == 0xd05 ? uniform_register(insn.Bits(32, 8)) : insn.Bits(32, 8);
            if (half) {
                extract_half({source, 2, false, false}, 0, 252);
                source = 252;
            }
            if (!is_signed) {
                // Clear NaNs without consuming a guest predicate. Clamp the
                // conversion below 2^32, retaining an overflow mask for saturation.
                emit(0x5800000000000000ULL | predicate | 250 | (u64{source} << 8) |
                     (u64{source} << 20) | (u64{7} << 39) | (u64{7} << 48));
                emit(0x5c40000000000000ULL | predicate | 250 | (u64{source} << 8) |
                     (u64{250} << 20) | (u64{7} << 48));
                if (!small) {
                mov_imm(252, 0x4f800000);
                emit(0x5800000000000000ULL | predicate | 251 | (u64{250} << 8) |
                     (u64{252} << 20) | (u64{7} << 39) | (u64{6} << 48));
                mov_imm(252, 0x4f7fffff);
                emit(0x5c60000000000000ULL | predicate | 250 | (u64{250} << 8) |
                     (u64{252} << 20) | (u64{7} << 39));
                }
                source = 250;
            }
            emit(base(0x5cb0000000000000ULL) | (u64{small ? 1U : 2U} << 8) | (u64{2} << 10) |
                 (u64{is_signed} << 12) | (u64{source} << 20) |
                 (u64{small || nearest ? 0U : floor ? 1U : 3U} << 39) | (u64{insn.Bits(80, 1)} << 44));
            if (!is_signed && !small) {
                emit(base(0x5c40000000000000ULL) | (dest << 8) | (u64{251} << 20) |
                     (u64{1} << 41) | (u64{7} << 48));
            }
            break;
        }
        case 0xb6f: { // TXQ texture dimensions, two packed scalar outputs.
            if (insn.Bits(59, 5) || insn.Bits(64, 8) != 255 || insn.Bits(72, 4) == 0 || insn.Bits(72, 4) > 3 ||
                insn.Bits(76, 14) || insn.Bits(91, 1) || insn.Bits(54, 5) != 2 ||
                insn.Bits(40, 14) > 0x1fff || (insn.Bits(72, 4) == 3 && (dest & 1)) || dest >= 254)
                return fail(index, "native texture dimension query mode");
            result.required_texture_buffer = 2;
            emit(0xdf48000000000000ULL | predicate | dest | (u64{insn.Bits(24, 8)} << 8) |
                 (u64{1} << 22) | (u64{insn.Bits(72, 4)} << 31) | (u64{insn.Bits(40, 14)} << 36) |
                 (u64{insn.Bits(90, 1)} << 49));
            break;
        }
        case 0xb66: { // TLD.SCR.LZ, direct texel fetch
            const u32 mask = insn.Bits(72, 4);
            const u32 dimension = insn.Bits(61, 3);
            const bool array = dimension == 5;
            const bool volume = dimension == 2;
            u32 source_a = insn.Bits(24, 8), source_b = insn.Bits(32, 8);
            const u32 second_dest = insn.Bits(64, 8);
            const bool split = second_dest != 255;
            constexpr std::array<u32, 8> rg_masks{1, 2, 4, 8, 3, 9, 10, 12};
            constexpr std::array<u32, 5> rgba_masks{7, 11, 13, 14, 15};
            u32 swizzle = ~0U;
            if (split) {
                for (u32 i = 0; i < rgba_masks.size(); ++i) if (rgba_masks[i] == mask) swizzle = i;
            } else {
                for (u32 i = 0; i < rg_masks.size(); ++i) if (rg_masks[i] == mask) swizzle = i;
            }
            if (!insn.Bits(60, 1) || (dimension != 0 && dimension != 1 && !array && !volume) ||
                swizzle == ~0U || insn.Bits(76, 5) ||
                insn.Bits(81, 3) != 7 || insn.Bits(87, 3) != 1 ||
                insn.Bits(59, 1) ||
                insn.Bits(40, 14) > 0x1fff || insn.Bits(54, 5) >= 18 ||
                dest == 255 || (mask & (mask - 1) && ((dest & 1) || dest >= 254)) ||
                (split && mask == 15 && ((second_dest & 1) || second_dest >= 254)) ||
                (array && ((source_a & 1) || source_a >= 249)) ||
                (volume && ((source_a & 1) || source_a >= 254))) {
                return fail(index, "native texel fetch mode or descriptor binding");
            }
            const u32 binding = insn.Bits(54, 5);
            if (result.required_texture_buffer != ~0U && result.required_texture_buffer != binding)
                return fail(index, "native texture descriptor buffer conflict");
            result.required_texture_buffer = binding;
            if (array) {
                emit(0x5c98000000000000ULL | predicate | 250 | (u64{15} << 39) | (u64{source_a + 1} << 20));
                emit(0x5c98000000000000ULL | predicate | 251 | (u64{15} << 39) | (u64{source_b} << 20));
                source_b = 250;
            }
            emit(0xda00000000000000ULL | predicate | dest | (u64{source_a} << 8) |
                 (u64{source_b} << 20) | (u64{insn.Bits(64, 8)} << 28) |
                 (u64{insn.Bits(40, 14)} << 36) | (u64{insn.Bits(90, 1)} << 49) |
                 (u64{swizzle} << 50) | (u64{array ? 8U : volume ? 7U : dimension == 0 ? 0U : 2U} << 53));
            break;
        }
        case 0x91a: // DEPBAR.LE SB5, 1: scoreboard wait, represented by IR data dependencies.
            if (insn.words[0] != 0x791a || (insn.words[1] & ~0xfc0U) != 0xd000 || insn.words[2]) {
                return fail(index, "native dependency barrier mode");
            }
            break;
        case 0xb69: { // TMML (LOD query), 2D. Results are 8.8 fixed point, as on Maxwell.
            const u32 mask = insn.Bits(72, 4), second_dest = insn.Bits(64, 8);
            const u32 source_a = insn.Bits(24, 8), source_b = insn.Bits(32, 8);
            const u32 binding = insn.Bits(54, 5);
            if (insn.Bits(61, 3) != 1 || insn.Bits(76, 4) || insn.Bits(80, 10) || insn.Bits(59, 1) ||
                insn.Bits(40, 14) > 0x1fff || binding >= 18 || mask == 0 || source_a >= 246 ||
                (result.required_texture_buffer != ~0U && result.required_texture_buffer != binding))
                return fail(index, "native texture LOD query mode");
            result.required_texture_buffer = binding;
            // Coordinates: (Ra, Ra+1) when Rb is RZ, else (Ra, Rb).
            emit(0x5c98000000000000ULL | predicate | 250 | (u64{15} << 39) | (u64{source_a} << 20));
            emit(0x5c98000000000000ULL | predicate | 251 | (u64{15} << 39) |
                 (u64{source_b == 255 ? source_a + 1 : source_b} << 20));
            emit(0xdf58000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{255} << 20) |
                 (u64{2} << 28) | (u64{mask} << 31) | (u64{insn.Bits(40, 14)} << 36) |
                 (u64{insn.Bits(90, 1)} << 49));
            const u32 components = static_cast<u32>(std::popcount(mask));
            const bool split = second_dest != 255;
            for (u32 i = 0; i < components; ++i) {
                u32 target = 255;
                if (!split || i < 2) target = dest == 255 ? 255U : static_cast<u32>(dest) + i;
                else target = second_dest + (i - 2);
                if (target != 255)
                    emit(0x5c98000000000000ULL | predicate | target | (u64{15} << 39) | (u64{250 + i} << 20));
            }
            break;
        }
        case 0xb63: { // TLD4 (gather), 2D without offsets or depth compare; component at bits 87-88.
            const u32 mask = insn.Bits(72, 4), second_dest = insn.Bits(64, 8);
            const u32 source_a = insn.Bits(24, 8), source_b = insn.Bits(32, 8);
            const u32 binding = insn.Bits(54, 5);
            if (insn.Bits(61, 3) != 1 || insn.Bits(76, 4) || insn.Bits(81, 3) != 7 || insn.Bits(59, 1) ||
                insn.Bits(40, 14) > 0x1fff || binding >= 18 || insn.Bits(89, 1) || mask == 0 ||
                (result.required_texture_buffer != ~0U && result.required_texture_buffer != binding))
                return fail(index, "native texture gather mode");
            result.required_texture_buffer = binding;
            // Two coordinates: x in Ra, y in Rb (the coordinate list splits evenly across them).
            emit(0x5c98000000000000ULL | predicate | 250 | (u64{15} << 39) | (u64{source_a} << 20));
            emit(0x5c98000000000000ULL | predicate | 251 | (u64{15} << 39) |
                 (u64{source_b == 255 ? 255U : source_b} << 20));
            emit(0xc800000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{255} << 20) |
                 (u64{2} << 28) | (u64{mask} << 31) | (u64{insn.Bits(40, 14)} << 36) |
                 (u64{insn.Bits(90, 1)} << 49) | (u64{7} << 51) | (u64{insn.Bits(87, 2)} << 56));
            const u32 components = static_cast<u32>(std::popcount(mask));
            const bool split = second_dest != 255;
            for (u32 i = 0; i < components; ++i) {
                u32 target = 255;
                if (!split || i < 2) target = dest == 255 ? 255U : static_cast<u32>(dest) + i;
                else target = second_dest + (i - 2);
                if (target != 255)
                    emit(0x5c98000000000000ULL | predicate | target | (u64{15} << 39) | (u64{250 + i} << 20));
            }
            break;
        }
        case 0xb60: { // TEX, scalar 2D implicit-LOD sample
            const u32 mask = insn.Bits(72, 4);
            const u32 second_dest = insn.Bits(64, 8);
            constexpr std::array<u32, 8> rg_masks{1, 2, 4, 8, 3, 9, 10, 12};
            constexpr std::array<u32, 5> rgba_masks{7, 11, 13, 14, 15};
            u32 swizzle = ~0U;
            const bool split = second_dest != 255;
            const bool depth_compare = insn.Bits(78, 1);
            const u32 dimension = insn.Bits(61, 3), lod = insn.Bits(87, 3);
            const u32 source_a = insn.Bits(24, 8), source_b = insn.Bits(32, 8);
            if (dimension == 5 && depth_compare && lod == 0 && !split && mask == 1) {
                if (!insn.Bits(60, 1) || insn.Bits(76, 2) || insn.Bits(81, 3) != 7 ||
                    insn.Bits(59, 1) || insn.Bits(40, 14) > 0x1fff || insn.Bits(54, 5) != 2 ||
                    (source_a & 1) || (source_b & 1) || source_a >= 248 || source_b >= 248 || dest >= 248)
                    return fail(index, "native array depth comparison sample mode");
                result.required_texture_buffer = 2;
                for (u32 i = 0; i < 3; ++i) {
                    const u32 source = i < 2 ? source_a + i : source_b;
                    emit(0x5c98000000000000ULL | predicate | (250 + i) |
                         (u64{15} << 39) | (u64{source} << 20));
                }
                emit(0xc000000000000000ULL | predicate | dest | (u64{250} << 8) |
                     (u64{source_b + 1} << 20) | (u64{3} << 28) | (u64{1} << 31) |
                     (u64{insn.Bits(40, 14)} << 36) | (u64{insn.Bits(90, 1)} << 49) |
                     (u64{1} << 50) | (u64{7} << 51));
                break;
            }
            // General form: gather the native operand list (array index first, coordinates,
            // then LOD/bias and the depth reference) into Maxwell TEX's coordinate and meta
            // registers, sample into scratch and copy the packed components out. Native packs
            // up to four values as halves of Ra and Rb (1+1, 2+1, 2+2), five or more as 4+n.
            const auto general_tex = [&]() -> bool {
                constexpr std::array<u32, 8> coordinate_count{1, 2, 3, 3, 2, 3, 0, 4};
                constexpr std::array<u32, 8> maxwell_type{0, 2, 4, 6, 1, 3, 0, 7};
                const bool offsets = insn.Bits(76, 1) != 0;
                if (dimension == 6 || insn.Bits(77, 1) || insn.Bits(81, 3) != 7 || insn.Bits(59, 1) ||
                    insn.Bits(40, 14) > 0x1fff || insn.Bits(54, 5) >= 18 || (lod > 3 && lod != 6 && lod != 7) ||
                    (depth_compare && mask != 1) || mask == 0)
                    return false;
                const u32 coordinates = coordinate_count[dimension];
                const bool has_lod = lod == 2 || lod == 3 || lod == 6 || lod == 7;
                const u32 count = coordinates + (has_lod ? 1 : 0) + (offsets ? 1 : 0) + (depth_compare ? 1 : 0);
                const u32 in_a = count <= 4 ? (count + 1) / 2 : 4;
                const auto value = [&](u32 i) -> u32 {
                    if (i < in_a) return source_a == 255 ? 255U : source_a + i;
                    return source_b == 255 ? 255U : source_b + (i - in_a);
                };
                if ((source_a != 255 && source_a + in_a > 246) ||
                    (count > in_a && source_b != 255 && source_b + (count - in_a) > 246))
                    return false;
                const u32 binding = insn.Bits(54, 5);
                if (result.required_texture_buffer != ~0U && result.required_texture_buffer != binding)
                    return false;
                result.required_texture_buffer = binding;
                const auto copy = [&](u32 target, u32 source) {
                    emit(0x5c98000000000000ULL | predicate | target | (u64{15} << 39) | (u64{source} << 20));
                };
                // Coordinates to R250.., then LOD/bias, offsets and reference to R246...
                for (u32 i = 0; i < coordinates; ++i) copy(250 + i, value(i));
                u32 meta = 246;
                u32 next = coordinates;
                if (has_lod) copy(meta++, value(next++));
                if (offsets) copy(meta++, value(next++));
                if (depth_compare) copy(meta++, value(next++));
                emit(0xc000000000000000ULL | predicate | 250 | (u64{250} << 8) |
                     (u64{meta == 246 ? 255U : 246U} << 20) | (u64{maxwell_type[dimension]} << 28) |
                     (u64{mask} << 31) | (u64{insn.Bits(40, 14)} << 36) |
                     (u64{insn.Bits(90, 1)} << 49) | (u64{depth_compare} << 50) | (u64{7} << 51) |
                     (u64{offsets} << 54) | (u64{lod} << 55));
                const u32 components = static_cast<u32>(std::popcount(mask));
                if (insn.Bits(79, 1)) {
                    // FP16 results: (c0, c1) packed into the first destination, (c2, c3) into
                    // the second (F2F writes the half in the low 16 bits; ISCADD merges).
                    const auto pack = [&](u32 target, u32 low, u32 high) {
                        if (target == 255) return;
                        emit(0x5ca8000000000000ULL | predicate | 246 | (u64{1} << 8) | (u64{2} << 10) | (u64{low} << 20));
                        emit(0x5ca8000000000000ULL | predicate | 247 | (u64{1} << 8) | (u64{2} << 10) | (u64{high} << 20));
                        emit(0x5c18000000000000ULL | predicate | target | (u64{246} << 8) | (u64{247} << 20) | (u64{16} << 39));
                    };
                    pack(static_cast<u32>(dest), 250, components > 1 ? 251U : 255U);
                    if (components > 2) {
                        pack(split ? second_dest : (dest == 255 ? 255U : static_cast<u32>(dest) + 1),
                             252, components > 3 ? 253U : 255U);
                    }
                    return true;
                }
                for (u32 i = 0; i < components; ++i) {
                    u32 target = 255;
                    if (!split) target = dest == 255 ? 255U : static_cast<u32>(dest) + i;
                    else if (i < 2) target = dest == 255 ? 255U : static_cast<u32>(dest) + i;
                    else target = second_dest + (i - 2);
                    if (target != 255) copy(target, 250 + i);
                }
                return true;
            };
            if (split) {
                for (u32 i = 0; i < rgba_masks.size(); ++i) if (rgba_masks[i] == mask) swizzle = i;
            } else {
                for (u32 i = 0; i < rg_masks.size(); ++i) if (rg_masks[i] == mask) swizzle = i;
            }
            // SM70 scalar coordinates and split RG/BA destinations match TEXS.
            // Descriptor offsets are words in both encodings, but only native
            // explicitly encodes the constant-buffer binding.
            if (!insn.Bits(60, 1) || (dimension != 1 && dimension != 5) ||
                swizzle == ~0U || insn.Bits(76, 2) ||
                insn.Bits(81, 3) != 7 || (lod > 1 && (lod != 3 || dimension != 1 || depth_compare)) ||
                (lod == 3 && ((source_a & 1) || source_a >= 249)) ||
                insn.Bits(59, 1) ||
                insn.Bits(40, 14) > 0x1fff || insn.Bits(54, 5) != 2 ||
                dest == 255 || (mask & (mask - 1) && ((dest & 1) || dest >= 254)) ||
                (split && mask == 15 && ((second_dest & 1) || second_dest >= 254)) ||
                (depth_compare && (dimension != 1 || split || mask != 1 || (source_a & 1) || source_a >= 249)) ||
                (dimension == 5 && ((source_a & 1) || source_a >= 249))) {
                if (!general_tex()) return fail(index, "native texture mode or descriptor binding");
                break;
            }
            result.required_texture_buffer = 2;
            emit((0xd800000000000000ULL & ~(u64{insn.Bits(79, 1)} << 59)) | predicate | dest | (u64{insn.Bits(24, 8)} << 8) |
                 (u64{insn.Bits(32, 8)} << 20) | (u64{insn.Bits(64, 8)} << 28) |
                 (u64{insn.Bits(40, 14)} << 36) | (u64{insn.Bits(90, 1)} << 49) |
                 (u64{swizzle} << 50) | (u64{dimension == 5 ? (lod ? 8U : 7U) : depth_compare ? (lod ? 6U : 4U) : (lod == 3 ? 3U : lod ? 2U : 1U)} << 53));
            break;
        }
        case 0x816: { // Captured PRMT half-word concatenation
            if (insn.words[1] > 0xffff || (insn.words[2] & ~0xffU)) return fail(index, "native permutation mode");
            if (insn.words[1] != 0x5410 && insn.words[1] != 0x5432 && insn.words[1] != 0x7610 && insn.words[1] != 0x7632) {
                // Byte i of the result is byte (s & 7) of c:a, or its sign bit repeated when s & 8.
                const u32 a = insn.Bits(24, 8), c = insn.Bits(64, 8);
                if (a >= 250 && a != 255) return fail(index, "native permutation source");
                mov_imm(250, 0);
                for (u32 i = 0; i < 4; ++i) {
                    const u32 select = (insn.words[1] >> (i * 4)) & 0xf;
                    const u32 source = select & 4 ? c : a, position = (select & 3) * 8;
                    emit(0x3800000000000000ULL | predicate | 251 | (u64{source} << 8) |
                         (u64{select & 8 ? (position + 7) | (1U << 8) : position | (8U << 8)} << 20) |
                         (u64{(select >> 3) & 1} << 48));
                    emit(0x36f0000000000000ULL | predicate | 250 | (u64{251} << 8) |
                         (u64{(i * 8) | (8U << 8)} << 20) | (u64{250} << 39));
                }
                emit(base(0x5c10000000000000ULL) | (u64{250} << 8) | (u64{255} << 20));
                break;
            }
            const u32 a = insn.Bits(24, 8), b = insn.Bits(64, 8);
            if (insn.words[1] != 0x5432 && insn.words[1] != 0x7632) {
                emit(0x0400000000000000ULL | predicate | 250 | (u64{a} << 8) | (u64{0xffff} << 20));
            } else {
                emit(0x3828000000000000ULL | predicate | 250 | (u64{a} << 8) | (u64{16} << 20));
            }
            if (insn.words[1] == 0x7610 || insn.words[1] == 0x7632) {
                emit(0x0400000000000000ULL | predicate | 251 | (u64{b} << 8) | (u64{0xffff0000} << 20));
                emit(base(0x5c10000000000000ULL) | (u64{250} << 8) | (u64{251} << 20));
            } else {
                emit(base(0x5c18000000000000ULL) | (u64{b} << 8) | (u64{250} << 20) | (u64{16} << 39));
            }
            break;
        }
        case 0x307: { // FRND.F32, round to an integral floating-point value
            if (insn.Bits(75, 2) != 2 || insn.Bits(84, 2) != 2 || insn.Bits(81, 3) ||
                insn.Bits(86, 6)) return fail(index, "native floating round format");
            emit(base(0x5ca8000000000000ULL) | (u64{2} << 8) | (u64{2} << 10) |
                 (u64{insn.Bits(32, 8)} << 20) | (u64{8 + insn.Bits(78, 2)} << 39) |
                 (u64{insn.Bits(80, 1)} << 44) | (u64{insn.Bits(63, 1)} << 45) |
                 (u64{insn.Bits(62, 1)} << 49));
            break;
        }
        case 0x321: // ALD
        case 0x322: { // AST
            if (insn.Bits(76, 2) || insn.Bits(79, 1)) return fail(index, "native indexed/patch attribute access");
            const u32 address = insn.Bits(40, 10);
            const u32 components = insn.Bits(74, 2) + 1;
            if (address % 4 || address + components * 4 > 0x400 || insn.Bits(24, 8) != 255) {
                return fail(index, "native attribute address or indexing");
            }
            // a[0x5c] has no Maxwell meaning (Ampere uses it for a per-primitive hint such as the
            // shading rate). Nothing reads it back; dropping the store keeps full-rate shading.
            if (opcode == 0x322 && address == 0x5c && components == 1) break;
            u32 value_reg = opcode == 0x321 ? static_cast<u32>(dest) : insn.Bits(32, 8);
            if (value_reg == 255) {
                if (opcode == 0x321) break; // load into RZ: nothing to do
                // AST of RZ writes zeros: store them from zeroed scratch registers.
                for (u32 i = 0; i < components; ++i) mov_imm(250 + i, 0);
                value_reg = 250;
            }
            if (value_reg + components > 255) return fail(index, "native attribute register range");
            const u32 vertex = opcode == 0x321 ? insn.Bits(32, 8) : insn.Bits(64, 8);
            const u64 translated = (opcode == 0x321 ? 0xefd8000000000000ULL : 0xeff0000000000000ULL) |
                predicate | value_reg | (u64{255} << 8) | (u64{address} << 20) |
                (u64{vertex} << 39) | (u64{components - 1} << 47);
            result.instructions.push_back(translated);
            break;
        }
        case 0x387: case 0x983: { // Captured scalar local spill at an immediate byte offset.
            const bool store = opcode == 0x387;
            const u32 data = insn.Bits(32, 8), offset = insn.Bits(40, 24);
            if (insn.Bits(24, 8) != 255 || (store && (insn.Bits(16, 8) || data > 247)) ||
                (!store && (data || dest >= 248)) || (offset & 3) || offset > 0x7fffff ||
                insn.words[2] != (store ? 0x00100800U : 0x00300800U))
                return fail(index, "native local scalar spill mode");
            emit((store ? 0xef50000000000000ULL : 0xef40000000000000ULL) | predicate |
                 (store ? data : dest) | (u64{255} << 8) | (u64{offset} << 20) | (u64{4} << 48));
            break;
        }
        case 0x803: { // P2R PR, register merge source, immediate mask, byte select (76-77)
            if (insn.words[1] > 0x7f || (insn.words[2] & ~0x3000U))
                return fail(index, "native predicate-to-register mode");
            emit(base(0x38e8000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) |
                 (u64{insn.words[1]} << 20) | (u64{insn.Bits(76, 2)} << 41));
            break;
        }
        case 0x804: { // R2P PR, register, immediate mask, byte select (76-77)
            if (insn.Bits(16, 8) || insn.words[1] > 0x7f || (insn.words[2] & ~0x3000U))
                return fail(index, "native register-to-predicate mode");
            emit(0x38f0000000000000ULL | predicate | (u64{insn.Bits(24, 8)} << 8) |
                 (u64{insn.words[1]} << 20) | (u64{insn.Bits(76, 2)} << 41));
            break;
        }
        case 0xc02: // MOV uniform register
        case 0x202: // MOV register
        case 0x802: // MOV immediate
        case 0xa02: { // MOV constant buffer
            if (insn.Bits(72, 4) != 15 || (insn.Bits(76, 16) & ~(opcode == 0xc02 ? 0x8000U : 0U))) return fail(index, "native MOV modifiers");
            if (opcode == 0x802) {
                result.instructions.push_back(base(0x0100000000000000ULL) | (u64{15} << 12) |
                    (u64{insn.words[1]} << 20));
            } else if (opcode == 0xc02) {
                if (insn.words[1] >= 16 || insn.words[2] != 0x08000f00) return fail(index, "native uniform move source");
                emit(base(0x5c98000000000000ULL) | (u64{15} << 39) | (u64{insn.words[1] + 216} << 20));
            } else if (opcode == 0x202) {
                if (insn.Bits(40, 24)) return fail(index, "native MOV register modifiers");
                result.instructions.push_back(base(0x5c98000000000000ULL) | (u64{15} << 39) |
                    (u64{insn.Bits(32, 8)} << 20));
            } else {
                if (!valid_cbuf() || insn.Bits(62, 2)) return fail(index, "native MOV constant buffer");
                result.instructions.push_back(base(0x4c98000000000000ULL) | (u64{15} << 39) | cbuf());
            }
            break;
        }
        case 0x223: case 0x423: case 0x623: case 0x823: case 0xa23: case 0xc23: case 0xe23: { // FFMA
            if (insn.Bits(76, 1) || (insn.Bits(81, 11) & ~(opcode == 0xe23 || opcode == 0xc23 ? 0x400U : 0U))) return fail(index, "native FFMA denormal/source mode");
            const u32 a = operand(0), b = operand(1), c = operand(2);
            emit(base(0x5980000000000000ULL) | (u64{a} << 8) | (u64{b} << 20) | (u64{c} << 39) |
                (u64{insn.Bits(77, 1)} << 50) | (u64{insn.Bits(78, 2)} << 51) | (u64{insn.Bits(80, 1)} << 53));
            break;
        }
        case 0x220: case 0x820: case 0xa20: case 0xc20: { // FMUL
            const u32 scale = insn.Bits(84, 3);
            if (insn.Bits(76, 1) || scale == 0 || insn.Bits(81, 3) ||
                insn.Bits(87, 4) || insn.Bits(91, 1) != static_cast<u32>(opcode == 0xc20) ||
                (scale != 4 && (!insn.Bits(80, 1) || insn.Bits(78, 2))))
                return fail(index, "native FMUL scale/mode");
            const u32 a = operand(0), b = operand(1);
            emit(base(0x5c68000000000000ULL) | (u64{a} << 8) | (u64{b} << 20) |
                (u64{insn.Bits(78, 2)} << 39) | (u64{std::array<u32, 8>{0, 3, 2, 1, 0, 6, 5, 4}[scale]} << 41) | (u64{insn.Bits(80, 1)} << 44) | (u64{insn.Bits(77, 1)} << 50));
            break;
        }
        case 0x221: case 0x421: case 0x621: case 0x821: case 0xa21: case 0xe21: { // FADD
            if (insn.Bits(81, 10) || insn.Bits(91, 1) != static_cast<u32>(opcode == 0xe21))
                return fail(index, "native FADD mode");
            const u32 a = operand(0), b = operand(insn.Bits(9, 3) == 2 || insn.Bits(9, 3) == 3 || insn.Bits(9, 3) == 7 ? 2 : 1);
            emit(base(0x5c58000000000000ULL) | (u64{a} << 8) | (u64{b} << 20) |
                (u64{insn.Bits(78, 2)} << 39) | (u64{insn.Bits(80, 1)} << 44) | (u64{insn.Bits(77, 1)} << 50));
            break;
        }
        case 0x209: case 0x809: case 0xa09: case 0xc09: { // FMNMX, including a uniform source
            if (insn.Bits(74, 6) || insn.Bits(81, 6) || (opcode != 0xc09 && insn.Bits(91, 1)))
                return fail(index, "native floating min/max mode");
            const u32 a = operand(0), b = operand(1);
            emit(base(0x5c60000000000000ULL) | (u64{a} << 8) | (u64{b} << 20) |
                 (u64{insn.Bits(87, 3)} << 39) | (u64{insn.Bits(90, 1)} << 42) |
                 (u64{insn.Bits(80, 1)} << 44));
            break;
        }
        case 0x308: case 0xb08: { // MUFU
            const u32 operation = insn.Bits(74, 6);
            if (operation > 8 || operation == 6 || operation == 7 || insn.Bits(80, 12)) return fail(index, "native MUFU mode");
            // SM70+ MUFU.COS/SIN take the angle in turns (compilers multiply by 1/2pi first);
            // Eden's Maxwell MUFU takes radians (its RRO is a plain move), so scale by 2pi.
            const bool turns = operation == 0 || operation == 1;
            const auto to_radians = [&](u32 source) {
                emit(0x1e00000000000000ULL | predicate | 250 | (u64{source} << 8) |
                     (u64{0x40c90fdbU} << 20));
                return 250U;
            };
            if (insn.Bits(73, 1)) {
                if (opcode != 0x308 || insn.Bits(60, 2) > 1)
                    return fail(index, "native half special-function source");
                emit(0x5ca8000000000000ULL | predicate | 250 | (u64{2} << 8) | (u64{1} << 10) |
                     (u64{insn.Bits(32, 8)} << 20) | (u64{insn.Bits(60, 1)} << 41) |
                     (u64{insn.Bits(63, 1)} << 45) | (u64{insn.Bits(62, 1)} << 49));
                if (turns) to_radians(250);
                emit(0x5080000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{operation} << 20));
                emit(base(0x5ca8000000000000ULL) | (u64{1} << 8) | (u64{2} << 10) | (u64{250} << 20));
            } else {
                u32 src = operand(1);
                if (turns) src = to_radians(src);
                emit(base(0x5080000000000000ULL) | (u64{src} << 8) | (u64{operation} << 20));
            }
            break;
        }
        case 0x526: { // IPA.PASS / CONSTANT with the attribute indexed by a uniform register
            const u32 index_register = uniform_register(insn.Bits(24, 8));
            if (index_register == ~0U || insn.Bits(32, 8) != 255 ||
                (insn.Bits(76, 4) != 0 && insn.Bits(76, 4) != 4) || insn.Bits(81, 3) != 7)
                return fail(index, "native indexed IPA mode");
            if (index_register == 255) {
                emit(base(0xe000000000000000ULL) | (u64{255} << 8) | (u64{255} << 20) |
                     (u64{insn.Bits(64, 8)} << 30) | (u64{insn.Bits(78, 1)} << 55));
                break;
            }
            // Maxwell IPA.IDX reads the whole byte address from the index register.
            emit(0x1c00000000000000ULL | predicate | 250 | (u64{index_register} << 8) |
                 (u64{insn.Bits(64, 8) * 4} << 20));
            emit(base(0xe000000000000000ULL) | (u64{250} << 8) | (u64{255} << 20) |
                 (u64{insn.Bits(64, 8)} << 30) | (u64{1} << 38) | (u64{insn.Bits(78, 1)} << 55));
            break;
        }
        case 0x326: { // IPA.PASS / CONSTANT, default or centroid sample location
            // Bits 76-77 are the sample mode (title 1 interpolates most varyings at the centroid),
            // bit 78 the constant mode; Maxwell keeps them at bits 52-53 and 55.
            if (insn.Bits(32, 8) != 255 || insn.Bits(76, 2) > 1 || insn.Bits(79, 1) ||
                insn.Bits(81, 3) != 7)
                return fail(index, "native IPA interpolation mode");
            emit(base(0xe000000000000000ULL) | (u64{255} << 8) | (u64{255} << 20) | (u64{insn.Bits(64, 8)} << 30) |
                 (u64{insn.Bits(76, 2)} << 52) | (u64{insn.Bits(78, 1)} << 55));
            break;
        }
        case 0x81a: { // SGXT.U32 Rd, Ra, n: keep the low n bits (BFE.U32 at position 0)
            if (insn.words[2] || insn.words[1] > 32 || dest >= 255)
                return fail(index, "native sign extend mode");
            emit(base(0x3800000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) |
                 (u64{insn.words[1] << 8} << 20));
            break;
        }
        case 0x299: { // USHF by a uniform amount, one register shifted (SHL/SHR, bit 75 wraps).
            const u32 dst = uniform_register(insn.Bits(16, 8)), low = uniform_register(insn.Bits(24, 8));
            const u32 shift = uniform_register(insn.Bits(32, 8)), high = uniform_register(insn.Bits(64, 8));
            const bool right = insn.Bits(76, 1) != 0, high_result = insn.Bits(80, 1) != 0;
            if (dst == ~0U || dst == 255 || low == ~0U || shift == ~0U || high == ~0U ||
                native_predicate != 7 || insn.Bits(73, 2) != 3 ||
                (insn.words[2] & ~0x000118ffU) != 0x08000600U)
                return fail(index, "native uniform register shift mode");
            u32 source;
            if (high_result && low == 255) source = high;        // (high:0) >> or << s, high word
            else if (!high_result && high == 255) source = low;  // (0:low) >> or << s, low word
            else return fail(index, "native uniform funnel shift");
            emit((right ? 0x5c28000000000000ULL : 0x5c48000000000000ULL) | predicate | dst |
                 (u64{source} << 8) | (u64{shift} << 20) | (u64{insn.Bits(75, 1)} << 39));
            break;
        }
        case 0x419: { // SHF.L.W.U32.HI Rd, RZ, Rb, imm (high word of imm:0 << Rb) = imm << (Rb & 31)
            if (insn.Bits(24, 8) != 255 || (insn.words[2] & ~0xffU) != 0x00010e00)
                return fail(index, "native funnel shift immediate mode");
            mov_imm(250, insn.words[1]);
            emit(base(0x5c48000000000000ULL) | (u64{250} << 8) | (u64{insn.Bits(64, 8)} << 20) |
                 (u64{1} << 39));
            break;
        }
        case 0x805: { // CS2R SRZ
            if (insn.Bits(72, 8) != 255 || !insn.Bits(80, 1) || dest >= 249) return fail(index, "native special-register read");
            mov_imm(static_cast<u32>(dest), 0);
            mov_imm(static_cast<u32>(dest) + 1, 0);
            break;
        }
        case 0x306: case 0xd06: { // I2F.F32.U8, low byte, round-to-nearest (0xd06: uniform source)
            const u32 source = opcode == 0xd06 ? uniform_register(insn.Bits(32, 8)) : insn.Bits(32, 8);
            if (opcode == 0x306 && insn.words[2] == 0x00101000 && !(insn.words[1] & 0xcfffff00U)) {
                // I2F.U16 Rd, Rs(.H1): Maxwell's 16-bit source format, byte selector 0 or 2.
                emit(base(0x5cb8000000000000ULL) | (u64{2} << 8) | (u64{1} << 10) | (u64{source} << 20) |
                     (u64{insn.Bits(60, 2) * 2} << 41));
                break;
            }
            if (insn.words[2] != (opcode == 0xd06 ? 0x08001000U : 0x1000U) ||
                (insn.words[1] & 0xffffff00U) || source == ~0U)
                return fail(index, "native byte-to-float conversion mode");
            emit(base(0x5cb8000000000000ULL) | (u64{2} << 8) | (u64{source} << 20));
            break;
        }
        case 0x245: case 0xa45: case 0xc45: { // I2FP.F32.U32
            // Bits 78-79: rounding (RN/RM/RP/RZ), as Maxwell I2F's bits 39-40.
            if ((insn.words[2] & ~(0x400U | 0xc000U | (opcode == 0xc45 ? 0x08000000U : 0U))) != 0x00201000) return fail(index, "native packed integer-to-float format");
            const u32 src = operand(1, false);
            emit(base(0x5cb8000000000000ULL) | (u64{2} << 8) | (u64{2} << 10) | (u64{insn.Bits(74, 1)} << 13) | (u64{src} << 20) |
                 (u64{insn.Bits(78, 2)} << 39));
            break;
        }
        case 0x207: case 0x807: case 0xa07:
        case 0x208: case 0x808: case 0xa08: { // Bitwise SEL / FSEL
            const u32 a = operand(0, false), b = operand(1, false);
            emit(base(0x5ca0000000000000ULL) | (u64{a} << 8) | (u64{b} << 20) |
                (u64{insn.Bits(87, 3)} << 39) | (u64{insn.Bits(90, 1)} << 42));
            break;
        }
        case 0x20a: case 0x80a: case 0xa0a: { // FSET.BF, combined with Pc (bits 87-90) by bits 74-75
            if (insn.Bits(64, 8) || insn.Bits(74, 2) == 3 || insn.Bits(81, 6) || insn.Bits(91, 1))
                return fail(index, "native floating comparison output mode");
            const u32 a = operand(0), b = operand(1);
            emit(base(0x5800000000000000ULL) | (u64{a} << 8) | (u64{b} << 20) |
                 (u64{insn.Bits(87, 4)} << 39) | (u64{insn.Bits(74, 2)} << 45) |
                 (u64{insn.Bits(76, 4)} << 48) | (u64{1} << 52) | (u64{insn.Bits(80, 1)} << 55));
            break;
        }
        case 0x434: { // HSETP2 with an immediate half pair: A swizzle 74-75 (neg 72, abs 73),
                      // combine 69-70, H_AND 71, compare 76-79, FTZ 80, P/Q 81-86, input 87-90.
            mov_imm(250, insn.words[1]);
            emit(0x5d20000000000000ULL | predicate | insn.Bits(84, 3) | (u64{insn.Bits(81, 3)} << 3) |
                 (u64{insn.Bits(80, 1)} << 6) | (u64{insn.Bits(24, 8)} << 8) | (u64{250} << 20) |
                 (u64{insn.Bits(76, 4)} << 35) | (u64{insn.Bits(87, 3)} << 39) |
                 (u64{insn.Bits(90, 1)} << 42) | (u64{insn.Bits(72, 1)} << 43) |
                 (u64{insn.Bits(73, 1)} << 44) | (u64{insn.Bits(69, 2)} << 45) |
                 (u64{insn.Bits(74, 2)} << 47) | (u64{insn.Bits(71, 1)} << 49));
            break;
        }
        case 0x20b: case 0x80b: case 0xa0b: // FSETP
        case 0x20c: case 0x80c: case 0xa0c: case 0xc0c: { // ISETP
            const bool floating = (opcode & 0x1ff) == 0xb;
            if (!floating && insn.Bits(72, 1)) return fail(index, "native extended integer comparison");
            const u32 a = operand(0, floating), b = operand(1, floating);
            const u32 combine = insn.Bits(74, 2);
            if (combine > 2) return fail(index, "native predicate combination");
            u64 out = (floating ? 0x5bb0000000000000ULL : 0x5b60000000000000ULL) | predicate |
                insn.Bits(84, 3) | (u64{insn.Bits(81, 3)} << 3) | (u64{a} << 8) | (u64{b} << 20) |
                (u64{insn.Bits(87, 3)} << 39) | (u64{insn.Bits(90, 1)} << 42) | (u64{combine} << 45);
            if (floating) out |= (u64{insn.Bits(80, 1)} << 47) | (u64{insn.Bits(76, 4)} << 48);
            else out |= (u64{insn.Bits(73, 1)} << 48) | (u64{insn.Bits(76, 3)} << 49);
            emit(out);
            break;
        }
        case 0x212: case 0x812: case 0xa12: case 0xc12: { // LOP3.LUT, any table, optional P = (result != 0)
            const u32 lut = insn.Bits(72, 8);
            const u32 pred_target = insn.Bits(81, 3);
            if (insn.Bits(80, 1) || insn.Bits(87, 4) != 15 ||
                (opcode == 0x212 && insn.Bits(40, 24)) || (opcode == 0xa12 && insn.Bits(62, 2)))
                return fail(index, "native LUT/predicate mode");
            u32 b = 255;
            if (opcode == 0x212) {
                b = insn.Bits(32, 8);
            } else if (opcode == 0x812) {
                mov_imm(251, insn.words[1]);
                b = 251;
            } else if (opcode == 0xc12) {
                b = uniform_register(insn.Bits(32, 8));
                if (b == ~0U) return fail(index, "native uniform logic source");
            } else {
                b = operand(1, false);
            }
            const u32 a = insn.Bits(24, 8), c = insn.Bits(64, 8);
            emit(0x5be0000000000000ULL | predicate | dest | (u64{a} << 8) | (u64{b} << 20) |
                 (u64{lut} << 28) | (u64{pred_target == 7 ? 0U : 3U} << 36) | (u64{c} << 39) |
                 (u64{pred_target} << 48));
            break;
        }
        case 0x213: case 0xa13: case 0xc13: { // IABS: max(x, 0 - x), signed.
            if (insn.words[2] & ~(opcode == 0xc13 ? 0x08000000U : 0U))
                return fail(index, "native integer absolute mode");
            const u32 source = operand(1, false);
            emit(0x5c10000000000000ULL | predicate | 250 | (u64{255} << 8) | (u64{source} << 20) |
                 (u64{1} << 48));
            emit(base(0x5c20000000000000ULL) | (u64{source} << 8) | (u64{250} << 20) |
                 (u64{15} << 39) | (u64{1} << 48));
            break;
        }
        case 0x217: case 0x817: case 0xa17: case 0xc17: { // IMNMX register, immediate, cbuf or uniform operand.
            if ((insn.words[2] & ~(0x04000200U | (opcode == 0xc17 ? 0x08000000U : 0U))) != 0x03800000 ||
                (opcode != 0xa17 && insn.words[1] > (opcode == 0x817 ? 0x7ffffU : 255U)) ||
                (opcode == 0xa17 && (!valid_cbuf() || insn.Bits(62, 2))))
                return fail(index, "native integer min/max mode");
            const u32 source = opcode == 0xc17   ? uniform_register(insn.words[1])
                               : opcode == 0xa17 ? operand(1, false)
                                                 : insn.words[1];
            if (source == ~0U) return fail(index, "native uniform min/max operand");
            emit(base(opcode == 0x817 ? 0x3820000000000000ULL : 0x5c20000000000000ULL) |
                 (u64{insn.Bits(24, 8)} << 8) | (u64{source} << 20) |
                 (u64{insn.Bits(87, 4)} << 39) | (u64{insn.Bits(73, 1)} << 48));
            break;
        }
        case 0x819: { // SHF.HI, immediate shift
            const bool is_signed = insn.Bits(73, 2) == 2;
            if (!insn.Bits(80, 1) && !insn.Bits(76, 1) && insn.Bits(64, 8) == 255 &&
                insn.Bits(73, 2) == 3 && !insn.Bits(75, 1) && insn.words[1] <= 32) {
                emit(base(0x3848000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) | (u64{insn.words[1]} << 20));
                break;
            }
            if (!insn.Bits(80, 1) && insn.Bits(76, 1) && insn.Bits(64, 8) == 255 &&
                (insn.Bits(73, 2) == 3 || is_signed) && !insn.Bits(75, 1) && insn.words[1] <= 32) {
                // SHF.R low word with a zero high word: a logical right shift.
                emit(base(0x3828000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) | (u64{insn.words[1]} << 20));
                break;
            }
            if ((insn.Bits(73, 2) != 3 && !is_signed) || insn.Bits(75, 1) || !insn.Bits(80, 1) || insn.words[1] > 32) return fail(index, "native funnel-shift mode");
            const u32 low = insn.Bits(24, 8), high = insn.Bits(64, 8), shift = insn.words[1];
            if (insn.Bits(76, 1)) {
                if (low != 255) return fail(index, "native nonzero low right-shift word");
                emit(base(0x3828000000000000ULL) | (u64{high} << 8) | (u64{shift} << 20) |
                     (u64{is_signed} << 48));
            } else {
                if (is_signed) return fail(index, "native signed left funnel shift");
                emit(base(0x36f8000000000000ULL) | (u64{low} << 8) | (u64{high} << 39) | (u64{shift} << 20));
            }
            break;
        }
        case 0xa11: { // LEA with constant base and carry predicate.
            if (insn.Bits(72, 1) || insn.Bits(74, 1) || insn.Bits(80, 1) ||
                insn.Bits(81, 3) == 7 || insn.Bits(62, 2))
                return fail(index, "native constant LEA carry mode");
            const u32 b = operand(1, false);
            emit(base(0x5c18000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) |
                (u64{b} << 20) | (u64{insn.Bits(75, 5)} << 39));
            emit(0x5b60000000000000ULL | predicate | 7 | (u64{insn.Bits(81, 3)} << 3) |
                (dest << 8) | (u64{b} << 20) | (u64{7} << 39) | (u64{1} << 49));
            break;
        }
        case 0x211: case 0x811: { // LEA low word, no carry result
            if (insn.Bits(80, 1)) {
                // LEA.HI Rd, Ra, Rb (or an immediate, 0x811), RZ, s: Rb + high word of (Ra << s);
                // bit 73 sign-extends Ra.
                if (insn.Bits(72, 1) || insn.Bits(74, 1) || insn.Bits(81, 3) != 7 ||
                    insn.Bits(64, 8) != 255 || (opcode == 0x211 && insn.Bits(62, 2)))
                    return fail(index, "native high effective address mode");
                u32 b = insn.Bits(32, 8);
                if (opcode == 0x811) {
                    b = insn.words[1] ? 251U : 255U;
                    if (b == 251) mov_imm(251, insn.words[1]);
                }
                const u64 shift = (u64{32 - insn.Bits(75, 5)} << 20) | (u64{insn.Bits(73, 1)} << 48);
                if (b == 255) {
                    emit(base(0x3828000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) | shift);
                } else {
                    emit(0x3828000000000000ULL | predicate | 250 | (u64{insn.Bits(24, 8)} << 8) | shift);
                    emit(base(0x5c10000000000000ULL) | (u64{250} << 8) | (u64{b} << 20));
                }
                break;
            }
            if (insn.Bits(72, 1) || insn.Bits(74, 1) || insn.Bits(80, 1) || insn.Bits(81, 3) != 7 ||
                (opcode == 0x211 && insn.Bits(62, 1))) return fail(index, "native LEA carry/high mode");
            // 0x211 bit 63 negates Rb (LEA Rd, Ra, -Rb, s), as Maxwell ISCADD's bit 48.
            u32 b = insn.Bits(32, 8);
            if (opcode == 0x811) { mov_imm(251, insn.words[1]); b = 251; }
            emit(base(0x5c18000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) |
                 (u64{b} << 20) | (u64{insn.Bits(75, 5)} << 39) |
                 (u64{opcode == 0x211 ? insn.Bits(63, 1) : 0U} << 48));
            break;
        }
        case 0x210: case 0x810: case 0xa10: case 0xc10: { // IADD3 with zero third source and no carry outputs
            // 64-bit adds: IADD3 with a carry-out predicate (bits 81-83), then IADD3.X (bit 74) with
            // a carry-in predicate (bits 87-89, negate 90). Uniform forms keep UPn in R239 bit 6-n.
            if ((opcode == 0x210 || opcode == 0x810) && insn.Bits(64, 8) == 255 && !insn.Bits(73, 1) &&
                !insn.Bits(75, 2) && insn.Bits(84, 3) == 7 && insn.Bits(77, 4) == 15 && !insn.Bits(72, 1) &&
                (insn.Bits(81, 3) != 7 || insn.Bits(74, 1))) {
                const bool carry_in = insn.Bits(74, 1) != 0;
                const u32 carry_out = insn.Bits(81, 3);
                const u32 a = insn.Bits(24, 8);
                if ((carry_in && carry_out != 7) || (opcode == 0x210 && (insn.words[1] & ~0xffU)) ||
                    (rewritten_uniform && carry_out != 7 && carry_out > 1) ||
                    (rewritten_uniform && carry_in && insn.Bits(87, 3) > 1 && insn.Bits(87, 3) != 7))
                    return fail(index, "native integer carry mode");
                if (opcode == 0x810) {
                    emit(0x1c00000000000000ULL | predicate | 250 | (u64{a} << 8) | (u64{insn.words[1]} << 20));
                } else {
                    emit(0x5c10000000000000ULL | predicate | 250 | (u64{a} << 8) | (u64{insn.Bits(32, 8)} << 20));
                }
                const auto save = [&](u32 mask) { emit(0x38e8000000070000ULL | 253 | (u64{255} << 8) | (u64{mask} << 20)); };
                const auto restore = [&](u32 mask) { emit(0x38f0000000070000ULL | (u64{253} << 8) | (u64{mask} << 20)); };
                if (!carry_in) {
                    // carry = (a + b) < a, unsigned
                    const u32 target = rewritten_uniform ? 6 - carry_out : carry_out;
                    const u32 mask = 1U << target;
                    if (rewritten_uniform) save(mask);
                    emit(0x5b60000000000000ULL | predicate | 7 | (u64{target} << 3) | (u64{250} << 8) |
                         (u64{a} << 20) | (u64{7} << 39) | (u64{1} << 49));
                    if (rewritten_uniform) {
                        emit(0x38e8000000070000ULL | 239 | (u64{239} << 8) | (u64{mask} << 20));
                        restore(mask);
                    }
                    emit(base(0x5c98000000000000ULL) | (u64{15} << 39) | (u64{250} << 20));
                    break;
                }
                const u32 condition = insn.Bits(87, 3);
                const u32 source = rewritten_uniform && condition != 7 ? 6 - condition : condition;
                const u32 mask = 1U << source;
                mov_imm(252, 1);
                if (rewritten_uniform && condition != 7) {
                    save(mask);
                    emit(0x38f0000000070000ULL | (u64{239} << 8) | (u64{mask} << 20));
                }
                emit(0x5ca0000000000000ULL | predicate | 251 | (u64{252} << 8) | (u64{255} << 20) |
                     (u64{source} << 39) | (u64{insn.Bits(90, 1)} << 42));
                if (rewritten_uniform && condition != 7) restore(mask);
                emit(base(0x5c10000000000000ULL) | (u64{250} << 8) | (u64{251} << 20));
                break;
            }
            if (opcode == 0xa10 && insn.Bits(81, 3) != 7) {
                if (insn.Bits(64, 8) != 255 || insn.Bits(73, 4) || insn.Bits(84, 7) != 0x7f ||
                    insn.Bits(77, 4) != 15 || insn.Bits(63, 1) || insn.Bits(72, 1))
                    return fail(index, "native constant add carry mode");
                const u32 b = operand(1, false);
                emit(base(0x5c10000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) | (u64{b} << 20));
                emit(0x5b60000000000000ULL | predicate | 7 | (u64{insn.Bits(81, 3)} << 3) |
                    (dest << 8) | (u64{b} << 20) | (u64{7} << 39) | (u64{1} << 49));
                break;
            }
            if (opcode == 0xa10 && insn.Bits(74, 1)) {
                if (insn.Bits(24, 8) != 255 || (insn.words[2] & ~0x07800000U) != 0x007fe4ffU)
                    return fail(index, "native extended constant add mode");
                const u32 b = operand(1, false);
                mov_imm(250, 1);
                emit(0x5ca0000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{255} << 20) |
                    (u64{insn.Bits(87, 3)} << 39) | (u64{insn.Bits(90, 1)} << 42));
                emit(base(0x5c10000000000000ULL) | (u64{b} << 8) | (u64{250} << 20));
                break;
            }
            // Third source, optionally negated (bit 75; 63 and 72 negate b and a).
            if (insn.Bits(64, 8) != 255 && (insn.Bits(73, 4) & ~4U) == 0 && insn.Bits(77, 7) == 0x7f && insn.Bits(84, 7) == 0x7f) {
                const u32 b = operand(1, false);
                emit(0x5c10000000000000ULL | predicate | 250 | (u64{insn.Bits(24, 8)} << 8) |
                    (u64{b} << 20) | (u64{insn.Bits(63, 1)} << 48) | (u64{insn.Bits(72, 1)} << 49));
                emit(base(0x5c10000000000000ULL) | (u64{250} << 8) | (u64{insn.Bits(64, 8)} << 20) |
                     (u64{insn.Bits(75, 1)} << 48));
                break;
            }
            if (insn.Bits(64, 8) != 255 || insn.Bits(73, 4) || insn.Bits(77, 7) != 0x7f || insn.Bits(84, 7) != 0x7f) return fail(index, "native integer carry/third source");
            if (opcode == 0xa10 || opcode == 0xc10) {
                if (opcode == 0xc10 && (insn.words[1] & ~0x8000003fU)) return fail(index, "native uniform add operand");
                const u32 b = operand(1, false);
                emit(base(0x5c10000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) | (u64{b} << 20) |
                    (u64{insn.Bits(63, 1)} << 48) | (u64{insn.Bits(72, 1)} << 49));
            } else if (opcode == 0x210) {
                if (insn.words[1] & ~0x800000ffU) return fail(index, "native integer add register modifiers");
                emit(base(0x5c10000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) |
                     (u64{insn.Bits(32, 8)} << 20) | (u64{insn.Bits(63, 1)} << 48) | (u64{insn.Bits(72, 1)} << 49));
            } else {
                emit(base(0x1c00000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) | (u64{insn.words[1]} << 20) | (u64{insn.Bits(72, 1)} << 56));
            }
            break;
        }
        case 0x949: case 0x958: { // Captured BRX / BRXU with absolute native-PC table values
            if (insn.Bits(12, 4) != 7 || (insn.words[2] & ~0x3ffffU) != (opcode == 0x958 ? 0x0b800000U : 0x03800000U))
                return fail(index, "native computed branch mode");
            const u64 raw = insn.words[1] | (u64{insn.words[2] & 0x3ffff} << 32);
            const s64 relative = static_cast<s64>(raw << 14) >> 14;
            if (s64{index} * 16 + 16 + relative != 0)
                return fail(index, "native computed branch relative base");
            const s64 offset = -s64{address_of(static_cast<u32>(result.instructions.size()))} - 8;
            if (offset < -0x800000) return fail(index, "native computed branch range");
            const u32 source = opcode == 0x958 ? uniform_register(insn.Bits(24, 8)) : insn.Bits(24, 8);
            if (source == ~0U) return fail(index, "native uniform computed branch source");
            emit(0xe25000000000000fULL | predicate | (u64{source} << 8) |
                 ((static_cast<u64>(offset) & 0xffffff) << 20));
            computed_branch = true;
            break;
        }
        case 0x944: { // Verified CALL.REL.NOINC with R55 holding the return PC
            // A predicated NOINC forward transfer can terminate a loop at its
            // reconvergence point without a return-link setup or matching RET.
            if (native_predicate != 7 && insn.words[1] == 16 && insn.words[2] == 0x03c00000 &&
                index + 2 < code.size() && code[index + 1].Bits(0, 12) == 0x947) {
                relocations.push_back({static_cast<u32>(result.instructions.size()), index + 2});
                emit(0xe24000000000000fULL | predicate);
                break;
            }
            if (main_exited || index == 0 || insn.words[0] != 0x7944 ||
                (insn.words[2] & ~0x3ffffU) != 0x03c00000 ||
                (code[index - 1].words[0] & ~0x00ff0000U) != 0x00007802 ||
                code[index - 1].Bits(16, 8) >= 250 ||
                code[index - 1].words[1] != (index + 1) * 16 ||
                code[index - 1].words[2] != 0xf00)
                return fail(index, "native call linkage mode");
            const u64 raw = insn.words[1] | (u64{insn.words[2] & 0x3ffff} << 32);
            const s64 relative = static_cast<s64>(raw << 14) >> 14;
            const s64 target = s64{index} * 16 + 16 + relative;
            if (target < 0 || target % 16) return fail(index, "native call destination alignment");
            const u32 link = code[index - 1].Bits(16, 8);
            for (size_t call = 0; call < call_targets.size(); ++call) {
                if (call_targets[call] == target / 16 && call_links[call] != link)
                    return fail(index, "native inconsistent call linkage");
            }
            call_targets.push_back(static_cast<u32>(target / 16));
            call_links.push_back(link);
            relocations.push_back({static_cast<u32>(result.instructions.size()), target / 16});
            emit(0xe26000000007000fULL);
            break;
        }
        case 0x950: { // Verified RET.REL.NODEC epilogue restoring the call link
            const u32 return_reg = insn.Bits(24, 8);
            // The return register is the link itself, or the last write to it in this function
            // is a copy of the link (MOV or IMAD.MOV.U32 Rd, RZ, RZ, Rlink).
            const auto copies_link = [&](const Instruction& prior) {
                if (prior.Bits(16, 8) != return_reg || prior.Bits(12, 4) != 7) return false;
                if ((prior.words[0] & 0xff00ffffU) == 0x00007202U)
                    return prior.words[1] == *active_link && prior.words[2] == 0xf00;
                if ((prior.words[0] & 0xff00ffffU) == 0xff007224U)
                    return prior.words[1] == 0xff && prior.words[2] == (0x078e0000U | *active_link);
                return false;
            };
            bool linked = active_link && return_reg == *active_link;
            for (u32 prior = index; active_link && !linked && prior-- > active_entry;) {
                if (copies_link(code[prior])) linked = true;
                else if (code[prior].Bits(16, 8) == return_reg) break;
            }
            if (!main_exited || !linked ||
                (insn.words[0] & 0x00ffffffU) != 0x00007950 || return_reg >= 249 ||
                (insn.words[2] & ~0x3ffffU) != 0x03c00000 || !barrier_tokens.empty())
                return fail(index, "native return linkage mode");
            const u64 raw = insn.words[1] | (u64{insn.words[2] & 0x3ffff} << 32);
            const s64 relative = static_cast<s64>(raw << 14) >> 14;
            if (s64{index} * 16 + 16 + relative != 0)
                return fail(index, "native return relative base");
            emit(0xe32000000007000fULL);
            bool complete = !computed_branch;
            for (u32 target : call_targets) complete &= target <= index;
            if (complete) return finish(index);
            break;
        }
        case 0x945: case 0x941: case 0x942: case 0x948: // (+ WARPSYNC)
            // BSSY / BSYNC / BREAK only manage warp reconvergence: BSYNC waits for the other
            // threads and falls through, BREAK drops the thread from that wait. Eden runs threads
            // independently, so all three are no-ops (a SYNC would wrongly jump, and a branch
            // out of a lowered SSY region would leave its token on Eden's flow stack).
            break;
        case 0x947: { // BRA
            u64 branch_predicate = predicate;
            if (opcode == 0x947 && insn.Bits(87, 4) != 7) {
                if (insn.Bits(12, 4) != 7) {
                    relocations.push_back({static_cast<u32>(result.instructions.size()), index + 1});
                    emit(0xe24000000000000fULL | (u64{insn.Bits(12, 4) ^ 8U} << 16));
                }
                branch_predicate = u64{insn.Bits(87, 4)} << 16;
            } else if (insn.Bits(87, 4) != 7) {
                return fail(index, "native extra branch condition");
            }
            const u64 raw = insn.words[1] | (u64{insn.words[2] & 0x3ffff} << 32);
            const s64 relative = static_cast<s64>(raw << 14) >> 14;
            const s64 target = s64{index} * 16 + 16 + relative;
            if (target < 0 || target % 16) return fail(index, "native branch destination alignment");
            relocations.push_back({static_cast<u32>(result.instructions.size()), target / 16});
            emit(0xe24000000000000fULL | branch_predicate);
            if (computed_branch && target == s64{index} * 16 && branch_predicate == (u64{7} << 16))
                return finish(index); // the closing self-branch
            break;
        }
        case 0x94d: { // EXIT
            if (insn.Bits(84, 2)) {
                return fail(index, "native conditional/special EXIT");
            }
            if (insn.Bits(87, 4) != 7) {
                // @Pg EXIT Pc: exits when both hold; skip over a Pc-predicated EXIT unless Pg.
                if (native_predicate != 7) {
                    relocations.push_back({static_cast<u32>(result.instructions.size()), index + 1});
                    emit(0xe24000000000000fULL | (u64{native_predicate ^ 8U} << 16));
                }
                emit(0xe30000000000000fULL | (u64{insn.Bits(87, 4)} << 16));
                break;
            }
            result.instructions.push_back(0xe30000000000000fULL | predicate);
            if (insn.Bits(12, 4) != 7) break;
            if (!barrier_tokens.empty()) return fail(index, "native unclosed reconvergence scope");
            if (call_targets.empty()) {
                const bool pending_branch = std::any_of(relocations.begin(), relocations.end(),
                    [&](const Relocation& relocation) { return relocation.target > index; });
                if (!pending_branch && !computed_branch) return finish(index);
                break;
            }
            if (main_exited) {
                // A library function may end the shader instead of returning (Eden inlines
                // calls, so this is an ordinary exit at that point).
                bool complete = std::none_of(relocations.begin(), relocations.end(),
                    [&](const Relocation& relocation) { return relocation.target > index; });
                for (u32 target : call_targets) complete &= target <= index;
                if (complete && !computed_branch) return finish(index);
                break;
            }
            for (u32 target : call_targets) {
                if (target <= index) return fail(index, "native call target overlaps main program");
            }
            main_exited = true;
            break;
        }
        default:
            return fail(index, "unsupported native opcode");
        }
        if (!operand_error.empty()) return fail(index, operand_error);
        if (output_mask) {
            emit(0x38e8000000070000ULL | 239 | (u64{239} << 8) | (u64{output_mask} << 20));
            emit(0x38f0000000070000ULL | (u64{254} << 8) | (u64{output_mask} << 20));
        }
        if (guarded_uniform) restore_predicate(guard_mask);
    }
    return fail(static_cast<u32>(code.size()), main_exited ? "native program lacks captured library return" : "native program has no unconditional EXIT");
}

} // namespace Shader::Ampere
