// The object of a Windows pack in COFF (README.md, "Windows packs"; docs/design/aot-recompiler.md 9.4): the pack stage compiles a Windows pack's units
// as it compiles a Linux pack's, as ELF objects of the Linux triple (src/build/pack.cpp, "Windows packs": LLVM 18 cannot compile the convention of
// section 2.3 for a Windows triple, nor write the line marks' !pcsections into COFF), joins them with ld.lld -r into one relocatable object, and
// converts that object here into an AMD64 COFF object for lld's COFF driver, which links the DLL. Only what such an object holds is converted:
//   sections   code (SHF_EXECINSTR) -> .text, read-only data and the data that only relocation writes (.data.rel.ro) -> .rdata, other writable data
//              -> .data, SHT_NOBITS -> .bss, each with its alignment; the DWARF sections (.debug_*) under their own names, discardable, when debug
//              information is kept; dropped: .eh_frame (ELF's unwind information: Windows unwinds through .pdata, which the pack does not have),
//              SHF_EXCLUDE sections (the line marks, read before), comments, notes, group and address-significance sections (ld.lld -r kept
//              one copy of each comdat group, so its members are plain sections here). A TLS section, an initializer array or a section of
//              another kind fails the conversion
//   symbols    every symbol of a kept section (global and weak: external; local: static; functions typed as functions), undefined symbols as
//              externals, sections' symbols as COFF section symbols with their section definitions
//   relocations R_X86_64_PC32 and R_X86_64_PLT32 -> IMAGE_REL_AMD64_REL32 (COFF's REL32 counts from the end of the field, so the addend
//              written in place is ELF's + 4), R_X86_64_64 -> IMAGE_REL_AMD64_ADDR64 (a base relocation in the DLL), R_X86_64_32 in a DWARF
//              section -> IMAGE_REL_AMD64_SECREL (DWARF's offsets into another DWARF section); the addends go into the section's bytes (COFF
//              relocations have none). Any other relocation (a GOT or TLS one, an absolute 32-bit address in loaded data) fails the
//              conversion, naming its section, offset and symbol. More than 65,535 relocations in a section use COFF's
//              IMAGE_SCN_LNK_NRELOC_OVFL form
// The output is deterministic: no time stamp, the input's order kept.
#pragma once

#include <cstdint>
#include <string>

#include <llvm/ADT/StringRef.h>

namespace nxaot::build {

struct CoffStats {
    uint64_t sections = 0, symbols = 0, relocations = 0;  // the COFF object's (symbols: records, auxiliary ones included)
    uint64_t dropped_sections = 0;                         // ELF sections not converted (.eh_frame, the line marks, notes, ...)
};

// The ELF64 x86-64 relocatable object elf as an AMD64 COFF object in *coff; keep_debug: the DWARF sections too. False with *error set
bool ElfToCoff(llvm::StringRef elf, bool keep_debug, std::string* coff, CoffStats* stats, std::string* error);

}  // namespace nxaot::build
