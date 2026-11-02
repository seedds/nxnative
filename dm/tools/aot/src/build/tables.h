// The tables' object's rows through the MC layer (README.md, "The tables' object through MC"): the function map and the lookup table of a pack
// (src/build/pack.cpp, CompileTables) hold a row per function and per lookup slot, each with a 32-bit field holding a body's or a standard
// entry's address less the field's own (a hidden symbol another unit defines: a relocation per row). As LLVM IR each such field was a constant
// expression of five constants (trunc (sub (ptrtoint body), (ptrtoint (gep table, 0, i, 1))))) beside a declaration of every body and entry, and
// code generation lowered each to an MCExpr tree of five nodes: about 2,400 bytes per function. Here the module keeps the tables as
// zero-initialised placeholders of the same type, section and alignment (the AsmPrinter emits their symbol, section, alignment, label and size
// as before), and the object streamer, when the AsmPrinter emits a placeholder's zeros, emits the rows instead: plain fields as the same
// emitIntValue calls, each address field as a 4-byte pc-relative fixup against the symbol (R_X86_64_PC32 with addend 0, the relocation the
// constant expression came to), the symbol registered and hidden where the AsmPrinter registered and hid the declaration's. The object is the
// same bytes (CTest tables compares it with the IR path's).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <llvm/ADT/StringRef.h>

namespace llvm {
class Module;
class TargetMachine;
class Triple;
}  // namespace llvm

namespace nxaot::build {

// What a table's rows are written with
class RowWriter {
public:
    virtual ~RowWriter() = default;
    // A plain field of 1 to 8 bytes (the AsmPrinter's emitIntValue of a ConstantInt)
    virtual void Int(uint64_t value, unsigned bytes) = 0;
    // A 32-bit field holding the address of the hidden symbol `symbol` less the field's own (a relocation R_X86_64_PC32 against it)
    virtual void Rel32(llvm::StringRef symbol) = 0;
};

// A table whose rows go to the MC layer: its placeholder in the module is a constant, zero-initialised global of `bytes` bytes in `section`
struct RowTable {
    std::string section;
    uint64_t bytes = 0;
    std::function<void(RowWriter&)> rows;  // writes exactly `bytes` bytes
    bool emitted = false;
};

// What the rows took, for --memory: the bytes the rows hold in their sections' fragments, their fixups and relocations, the symbols the
// rows registered (the bodies and standard entries), and an estimate of the MC layer's memory for them
struct RowStats {
    uint64_t bytes = 0, relocations = 0, symbols = 0, symbol_name_bytes = 0;
    uint64_t Estimate() const;
};

// Whether the rows of objects for this triple can go through the MC layer: LLVM 18's x86-64 ELF object streamer is MCELFStreamer itself, which
// RowStreamer derives from (another LLVM may give x86 ELF a streamer of its own, which RowStreamer would have to derive from instead).
// Otherwise the tables are LLVM constants (pack.cpp's IR path)
bool RowsThroughMC(const llvm::Triple& triple);

// Code generation of m into *object, as tm.addPassesToEmitFile(ObjectFile) does it (the same passes, AsmPrinter and MC layer), with each
// table's rows emitted in place of its placeholder's zeros; the object writer writes into *object itself (no copy out of a buffer). False with
// *error set (also when a table's placeholder was not met or its rows did not fill it)
bool EmitObjectWithRows(llvm::Module& m, llvm::TargetMachine& tm, std::vector<RowTable>* tables, std::string* object, RowStats* stats,
                        std::string* error);

}  // namespace nxaot::build
