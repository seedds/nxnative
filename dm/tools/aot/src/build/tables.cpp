#include "build/tables.h"

#include <cstring>
#include <memory>

#include <llvm/BinaryFormat/ELF.h>
#include <llvm/CodeGen/AsmPrinter.h>
#include <llvm/CodeGen/MachineModuleInfo.h>
#include <llvm/CodeGen/Passes.h>
#include <llvm/CodeGen/TargetPassConfig.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/MCAsmBackend.h>
#include <llvm/MC/MCAssembler.h>
#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCDwarf.h>
#include <llvm/MC/MCELFObjectWriter.h>
#include <llvm/MC/MCELFStreamer.h>
#include <llvm/MC/MCExpr.h>
#include <llvm/MC/MCFixup.h>
#include <llvm/MC/MCFragment.h>
#include <llvm/MC/MCInstrInfo.h>
#include <llvm/MC/MCObjectWriter.h>
#include <llvm/MC/MCRegisterInfo.h>
#include <llvm/MC/MCSection.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/MCSymbolELF.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Triple.h>

namespace nxaot::build {

namespace {

// The object streamer of an x86-64 ELF object (LLVM 18's Target::createMCObjectStreamer makes a plain MCELFStreamer for it: X86's MC library
// defines an object streamer of its own for COFF only, and its object target streamer is COFF's), which writes a table's rows where the
// AsmPrinter emits its placeholder's zeros. Everything else goes to MCELFStreamer unchanged
class RowStreamer final : public llvm::MCELFStreamer, public RowWriter {
public:
    RowStreamer(llvm::MCContext& ctx, std::unique_ptr<llvm::MCAsmBackend> backend, std::unique_ptr<llvm::MCObjectWriter> writer,
                std::unique_ptr<llvm::MCCodeEmitter> emitter, std::vector<RowTable>* tables, RowStats* stats)
        : MCELFStreamer(ctx, std::move(backend), std::move(writer), std::move(emitter)), tables_{tables}, stats_{stats} {}

    // The AsmPrinter emits a zero-initialised global's initializer as emitZeros(size), which comes here: in a table's section, with the
    // table's size, the rows instead (once per table)
    using MCELFStreamer::emitFill;
    void emitFill(const llvm::MCExpr& bytes, uint64_t value, llvm::SMLoc loc) override {
        int64_t n = 0;
        if (value == 0 && bytes.evaluateAsAbsolute(n) && n > 0 && getCurrentSectionOnly()) {
            const llvm::StringRef section = getCurrentSectionOnly()->getName();
            for (RowTable& t : *tables_) {
                if (!t.emitted && t.bytes == static_cast<uint64_t>(n) && section == t.section) {
                    t.emitted = true;
                    written_ = 0;
                    t.rows(*this);
                    if (written_ != t.bytes) short_ = t.section;
                    return;
                }
            }
        }
        MCELFStreamer::emitFill(bytes, value, loc);
    }

    // A plain field: what the AsmPrinter emits for a ConstantInt field of a constant struct
    void Int(uint64_t value, unsigned bytes) override {
        emitIntValue(value, bytes);
        written_ += bytes;
        stats_->bytes += bytes;
    }

    // An address field: the AsmPrinter lowered trunc (sub (ptrtoint symbol), (ptrtoint field)) to the MCExpr (symbol - table) + (-offset)
    // and emitted it with emitValue (MCObjectStreamer::emitValueImpl: its symbols registered, a data fragment, its pending labels and a
    // .loc's line entry, then a 4-byte data fixup); the assembler, finding the subtracted symbol in the fixup's own section, made it the
    // pc-relative R_X86_64_PC32 against symbol with addend 0. The same steps here with that pc-relative fixup itself: one MCSymbolRefExpr per
    // row in place of five MCExpr nodes, and no IR constants. The symbol's .hidden, which the AsmPrinter emitted for the declaration after the
    // globals (by then registered), is set here
    void Rel32(llvm::StringRef name) override {
        llvm::MCContext& ctx = getContext();
        auto* sym = llvm::cast<llvm::MCSymbolELF>(ctx.getOrCreateSymbol(name));
        if (!sym->isRegistered()) {
            ++stats_->symbols;
            stats_->symbol_name_bytes += name.size();
        }
        visitUsedSymbol(*sym);
        sym->setVisibility(llvm::ELF::STV_HIDDEN);
        llvm::MCDataFragment* df = getOrCreateDataFragment();
        flushPendingLabels(df, df->getContents().size());
        llvm::MCDwarfLineEntry::make(this, getCurrentSectionOnly());
        df->getFixups().push_back(
            llvm::MCFixup::create(static_cast<uint32_t>(df->getContents().size()), llvm::MCSymbolRefExpr::create(sym, ctx), llvm::FK_PCRel_4));
        df->getContents().resize(df->getContents().size() + 4, 0);
        written_ += 4;
        stats_->bytes += 4;
        ++stats_->relocations;
    }

    const std::string& Short() const { return short_; }

private:
    std::vector<RowTable>* tables_;
    RowStats* stats_;
    uint64_t written_ = 0;
    std::string short_;  // a table whose rows did not fill its placeholder
};

// The object writer's output stream over a string, as raw_svector_ostream is over a SmallVector (unbuffered; pwrite patches bytes already
// written): the object goes into the caller's string without the copy Finish makes of a SmallVector's (15 MiB at 150,000 functions)
class StringStream final : public llvm::raw_pwrite_stream {
public:
    explicit StringStream(std::string* s) : s_{s} { SetUnbuffered(); }
    void reserveExtraSpace(uint64_t extra) override { s_->reserve(s_->size() + extra); }

private:
    void write_impl(const char* p, size_t n) override { s_->append(p, n); }
    void pwrite_impl(const char* p, size_t n, uint64_t offset) override { std::memcpy(s_->data() + offset, p, n); }
    uint64_t current_pos() const override { return s_->size(); }
    std::string* s_;
};

}  // namespace

uint64_t RowStats::Estimate() const {
    // Per relocation: its fixup in the fragment's vector and its entry in the ELF writer's (vectors that double: up to twice each), an
    // MCSymbolRefExpr in the MCContext's allocator, and its .rela entry (24 bytes) in the object. Per symbol: the MCSymbolELF, its name in the
    // context's three string maps (the name and a 16-byte head each, and a 16-byte bucket), the ELF writer's symbol data (32 bytes, a vector)
    // and string table map (24 bytes a slot, at most three quarters full), its .symtab entry (24) and its name in .strtab. The rows' bytes in
    // their fragments' vectors (up to twice)
    const uint64_t per_relocation = 2 * sizeof(llvm::MCFixup) + sizeof(llvm::MCSymbolRefExpr) + 2 * sizeof(llvm::ELFRelocationEntry) + 24;
    const uint64_t per_symbol = sizeof(llvm::MCSymbolELF) + 3 * (16 + 1 + 16) + 2 * 32 + 2 * 24 + 24 + 1;
    return 2 * bytes + relocations * per_relocation + symbols * per_symbol + 4 * symbol_name_bytes;
}

bool RowsThroughMC(const llvm::Triple& t) {
#if LLVM_VERSION_MAJOR == 18
    return t.getArch() == llvm::Triple::x86_64 && t.isOSBinFormatELF();
#else
    (void)t;
    return false;
#endif
}

bool EmitObjectWithRows(llvm::Module& m, llvm::TargetMachine& tm, std::vector<RowTable>* tables, std::string* object, RowStats* stats,
                        std::string* error) {
    if (!RowsThroughMC(tm.getTargetTriple())) {
        *error = "the tables' rows go through the MC layer for x86-64 ELF objects of LLVM 18 only";
        return false;
    }
    auto& ltm = static_cast<llvm::LLVMTargetMachine&>(tm);
    object->clear();
    StringStream os{object};
    llvm::legacy::PassManager pm;
    // LLVMTargetMachine::addPassesToEmitFile's steps for an object file (addPassesToGenerateCode, then addAsmPrinter with createMCStreamer's
    // object streamer: the target's code emitter and assembler backend, the backend's object writer, RelaxAll from the MC options), with
    // RowStreamer as that streamer
    auto* mmiwp = new llvm::MachineModuleInfoWrapperPass(&ltm);
    llvm::TargetPassConfig* config = ltm.createPassConfig(pm);
    config->setDisableVerify(true);
    pm.add(config);
    pm.add(mmiwp);
    if (config->addISelPasses()) {
        *error = "the target machine cannot select instructions for the tables' object";
        return false;
    }
    config->addMachinePasses();
    config->setInitialized();
    llvm::MCContext& ctx = mmiwp->getMMI().getContext();
    const llvm::Target& target = ltm.getTarget();
    std::unique_ptr<llvm::MCCodeEmitter> emitter(target.createMCCodeEmitter(*ltm.getMCInstrInfo(), ctx));
    std::unique_ptr<llvm::MCAsmBackend> backend(target.createMCAsmBackend(*ltm.getMCSubtargetInfo(), *ltm.getMCRegisterInfo(), ltm.Options.MCOptions));
    if (!emitter || !backend) {
        *error = "the target machine has no code emitter or assembler backend";
        return false;
    }
    std::unique_ptr<llvm::MCObjectWriter> writer = backend->createObjectWriter(os);
    auto streamer = std::make_unique<RowStreamer>(ctx, std::move(backend), std::move(writer), std::move(emitter), tables, stats);
    if (ltm.Options.MCOptions.MCRelaxAll) {
        streamer->getAssembler().setRelaxAll(true);
    }
    const RowStreamer* rows = streamer.get();
    llvm::AsmPrinter* printer = target.createAsmPrinter(ltm, std::move(streamer));
    if (!printer) {
        *error = "the target machine has no AsmPrinter";
        return false;
    }
    pm.add(printer);
    pm.add(llvm::createFreeMachineFunctionPass());
    pm.run(m);
    if (!rows->Short().empty()) {
        *error = "the rows of " + rows->Short() + " did not fill its placeholder";
        return false;
    }
    for (const RowTable& t : *tables) {
        if (!t.emitted && t.bytes) {
            *error = "the AsmPrinter emitted no placeholder for the rows of " + t.section;
            return false;
        }
    }
    return true;
}

}  // namespace nxaot::build
