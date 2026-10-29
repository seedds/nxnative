#include "decode/translate.h"

#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/frontend/A64/decoder/a64.h"
#include "dynarmic/frontend/A64/translate/impl/impl.h"
#include "dynarmic/ir/terminal.h"

namespace nxaot::decode {

using namespace Dynarmic;

namespace {

A64::TranslationOptions Options() {
    A64::TranslationOptions options;
    options.define_unpredictable_behaviour = false;  // as dynarmic's JIT translates (a64_interface.cpp), so the vector harness compares like with like
    options.wall_clock_cntpct = true;
    options.hook_hint_instructions = true;
    return options;
}

// One word through the visitor: the extra encodings first, then dynarmic's decoder; a word neither knows goes to dynarmic's Interpret terminal
bool Visit(A64::TranslatorVisitor& visitor, uint32_t word) {
    if (TranslateExtra(visitor, word)) {
        return true;
    }
    if (auto decoder = A64::Decode<A64::TranslatorVisitor>(word)) {
        return decoder->get().call(visitor, word);
    }
    return visitor.InterpretThisInstruction();
}

}  // namespace

bool IsMrsCntvct(uint32_t word) {
    // mrs xT, cntvct_el0 = S3_3_C14_C0_2
    return (word & 0xFFFFFFE0u) == 0xD53BE040u;
}

bool TranslateExtra(A64::TranslatorVisitor& v, uint32_t word) {
    if (IsMrsCntvct(word)) {
        v.X(64, static_cast<A64::Reg>(word & 31), v.ir.GetCNTPCT());
        return true;
    }
    return false;
}

IR::Block TranslateBlock(uint64_t pc, const CodeReader& read, uint32_t fpcr, InstPcs* pcs) {
    const A64::LocationDescriptor descriptor{pc, FP::FPCR{fpcr}};
    IR::Block block{descriptor};
    A64::TranslatorVisitor visitor{block, descriptor, Options()};
    bool should_continue = true;
    do {
        const u64 at = visitor.ir.current_location->PC();
        const IR::Inst* last = block.empty() ? nullptr : &block.back();
        if (const auto word = read(at)) {
            should_continue = Visit(visitor, *word);
        } else {
            should_continue = visitor.RaiseException(A64::Exception::NoExecuteFault);
        }
        if (pcs) {
            for (auto it = block.rbegin(); it != block.rend() && &*it != last; ++it) {
                (*pcs)[&*it] = at;
            }
        }
        visitor.ir.current_location = visitor.ir.current_location->AdvancePC(4);
        block.CycleCount()++;
    } while (should_continue);
    block.SetEndLocation(*visitor.ir.current_location);
    return block;
}

bool TranslateOne(IR::Block& block, uint64_t pc, uint32_t word) {
    const A64::LocationDescriptor descriptor{pc, FP::FPCR{kDefaultFpcr}};
    A64::TranslatorVisitor visitor{block, descriptor, Options()};
    const bool should_continue = Visit(visitor, word);
    block.SetEndLocation(descriptor.AdvancePC(4));
    return should_continue;
}

}  // namespace nxaot::decode
