// Glue to dynarmic's A64 frontend (docs/design/aot-recompiler.md sections 9.1 and 13, src/decode): its decoder and translator turn guest code into
// dynarmic IR, which the lifter (src/lift) lowers to LLVM IR and the census (src/analysis) inspects.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>

#include "dynarmic/ir/basic_block.h"

namespace Dynarmic::A64 {
struct TranslatorVisitor;
}

namespace nxaot::decode {

// Reads the instruction word at a guest address; nullopt when there is no code there
using CodeReader = std::function<std::optional<uint32_t>(uint64_t vaddr)>;

// The FPCR a block is translated for: blocks are specialised on it (section 7.1), 0 by default
constexpr uint32_t kDefaultFpcr = 0;

// The guest address each IR instruction of a block was translated from (the line table and debug locations of a pack, section 9.2)
using InstPcs = std::unordered_map<const Dynarmic::IR::Inst*, uint64_t>;

// One basic block from pc: dynarmic's A64::Translate loop (frontend/A64/translate/a64_translate.cpp) with nxaot's translation options (hints raise
// their exceptions, so that yield and wfe stay visible to the lifter; the counter is a wall clock) and the extra encodings of TranslateExtra.
// A word the reader does not supply ends the block with dynarmic's NoExecuteFault exception at that address. With pcs, every instruction the
// block holds is recorded with the guest address of the instruction it came from
Dynarmic::IR::Block TranslateBlock(uint64_t pc, const CodeReader& read, uint32_t fpcr = kDefaultFpcr, InstPcs* pcs = nullptr);

// One instruction alone, appended to block (the census's data-flow checks). False when the instruction ends a block
bool TranslateOne(Dynarmic::IR::Block& block, uint64_t pc, uint32_t word);

// The encodings dynarmic leaves to an interpreter (its Interpret terminal) that nxaot translates itself, into dynarmic IR with the frontend's own
// emitter: mrs xN, cntvct_el0 (dynarmic translates cntpct_el0 only; both read the guest counter, section 8). True when the word was one of them
bool TranslateExtra(Dynarmic::A64::TranslatorVisitor& v, uint32_t word);

// The word encodings TranslateExtra knows, for the census
bool IsMrsCntvct(uint32_t word);

}  // namespace nxaot::decode
