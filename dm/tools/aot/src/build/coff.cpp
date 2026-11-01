#include "build/coff.h"

#include <cstring>
#include <limits>
#include <vector>

#include <llvm/ADT/StringExtras.h>
#include <llvm/BinaryFormat/COFF.h>
#include <llvm/BinaryFormat/ELF.h>
#include <llvm/Object/ELF.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MathExtras.h>

namespace nxaot::build {

namespace {

using ELFT = llvm::object::ELF64LE;
using Shdr = ELFT::Shdr;

enum class Kind { Drop, Text, Rdata, Data, Bss, Debug };

struct Reloc {
    uint32_t offset = 0, symbol = 0;
    uint16_t type = 0;
};

struct Section {
    std::string name;
    Kind kind = Kind::Drop;
    uint32_t flags = 0;
    uint64_t size = 0;
    std::string data;  // the ELF section's bytes, with the addends written in (empty for .bss)
    std::vector<Reloc> relocs;
    uint32_t number = 0;  // its COFF section number (1-based)
    uint32_t symbol = 0;  // the index of its section symbol
};

struct Symbol {
    std::string name;
    uint32_t value = 0;
    int32_t section = 0;  // COFF section number, 0 undefined
    uint16_t type = 0;
    uint8_t storage = 0;
    bool section_def = false;  // a section symbol: one auxiliary record (its section definition) follows
    const Section* sec = nullptr;
};

void Put16(std::string* s, uint16_t v) { s->append(reinterpret_cast<const char*>(&v), 2); }
void Put32(std::string* s, uint32_t v) { s->append(reinterpret_cast<const char*>(&v), 4); }

uint32_t AlignFlag(uint64_t align, std::string* error) {
    if (align <= 1) return llvm::COFF::IMAGE_SCN_ALIGN_1BYTES;
    if (!llvm::isPowerOf2_64(align) || align > 8192) {
        *error = "a section aligned to " + std::to_string(align) + " bytes, which COFF cannot express";
        return 0;
    }
    return static_cast<uint32_t>(llvm::Log2_64(align) + 1) << 20;
}

}  // namespace

bool ElfToCoff(llvm::StringRef elf, bool keep_debug, std::string* coff, CoffStats* stats, std::string* error) {
    const auto fail = [&](const std::string& what) {
        *error = "ELF to COFF: " + what;
        return false;
    };
    auto file = llvm::object::ELFFile<ELFT>::create(elf);
    if (!file) return fail(llvm::toString(file.takeError()));
    const llvm::object::ELFFile<ELFT>& ef = *file;
    if (ef.getHeader().e_type != llvm::ELF::ET_REL || ef.getHeader().e_machine != llvm::ELF::EM_X86_64) {
        return fail("not an x86-64 relocatable ELF object");
    }
    auto shdrs = ef.sections();
    if (!shdrs) return fail(llvm::toString(shdrs.takeError()));
    auto shstrtab = ef.getSectionStringTable(*shdrs);
    if (!shstrtab) return fail(llvm::toString(shstrtab.takeError()));
    const size_t n = shdrs->size();

    // ---- The sections: which are kept, and as what ----
    std::vector<Section> secs(n);
    const Shdr* symtab = nullptr;
    for (size_t i = 0; i < n; ++i) {
        const Shdr& s = (*shdrs)[i];
        auto name_or = ef.getSectionName(s, *shstrtab);
        if (!name_or) return fail(llvm::toString(name_or.takeError()));
        const llvm::StringRef name = *name_or;
        Section& out = secs[i];
        switch (s.sh_type) {
        case llvm::ELF::SHT_SYMTAB:
            symtab = &s;
            continue;
        case llvm::ELF::SHT_NULL:
        case llvm::ELF::SHT_STRTAB:
        case llvm::ELF::SHT_RELA:
        case llvm::ELF::SHT_GROUP:
        case llvm::ELF::SHT_LLVM_ADDRSIG:
        case llvm::ELF::SHT_SYMTAB_SHNDX:
            continue;
        case llvm::ELF::SHT_REL:
            return fail("a REL section (" + name.str() + "): x86-64 objects have RELA");
        default:
            break;
        }
        if (s.sh_flags & llvm::ELF::SHF_EXCLUDE) {
            ++stats->dropped_sections;  // the line marks (pack.cpp, "The line table"), read from the units before
            continue;
        }
        if (!(s.sh_flags & llvm::ELF::SHF_ALLOC)) {
            if (keep_debug && name.starts_with(".debug_")) {
                out.kind = Kind::Debug;
            } else {
                ++stats->dropped_sections;  // .comment, .note.GNU-stack, DWARF without debug information
                continue;
            }
        } else if (s.sh_type == llvm::ELF::SHT_X86_64_UNWIND || name == ".eh_frame") {
            ++stats->dropped_sections;  // ELF's unwind information
            continue;
        } else if (s.sh_flags & llvm::ELF::SHF_TLS) {
            return fail("a thread-local section (" + name.str() + "), which a pack does not have");
        } else if (s.sh_type == llvm::ELF::SHT_INIT_ARRAY || s.sh_type == llvm::ELF::SHT_FINI_ARRAY || s.sh_type == llvm::ELF::SHT_PREINIT_ARRAY) {
            return fail("an initializer array (" + name.str() + "), which a pack does not have (its DLL has no entry point)");
        } else if (s.sh_type != llvm::ELF::SHT_PROGBITS && s.sh_type != llvm::ELF::SHT_NOBITS) {
            return fail("section " + name.str() + " of ELF type " + llvm::utohexstr(s.sh_type) + ", which has no COFF form here");
        } else if (s.sh_flags & llvm::ELF::SHF_EXECINSTR) {
            out.kind = Kind::Text;
        } else if (s.sh_type == llvm::ELF::SHT_NOBITS) {
            out.kind = Kind::Bss;
        } else if ((s.sh_flags & llvm::ELF::SHF_WRITE) && !name.starts_with(".data.rel.ro")) {
            out.kind = Kind::Data;
        } else {
            out.kind = Kind::Rdata;  // read-only data, and .data.rel.ro: written by relocation only (the loader applies base relocations to it)
        }
        namespace C = llvm::COFF;
        const uint32_t align = AlignFlag(s.sh_addralign, error);
        if (!align) return fail(name.str() + ": " + *error);
        switch (out.kind) {
        case Kind::Text:
            out.name = ".text";
            out.flags = C::IMAGE_SCN_CNT_CODE | C::IMAGE_SCN_MEM_EXECUTE | C::IMAGE_SCN_MEM_READ | align;
            break;
        case Kind::Rdata:
            out.name = ".rdata";
            out.flags = C::IMAGE_SCN_CNT_INITIALIZED_DATA | C::IMAGE_SCN_MEM_READ | align;
            break;
        case Kind::Data:
            out.name = ".data";
            out.flags = C::IMAGE_SCN_CNT_INITIALIZED_DATA | C::IMAGE_SCN_MEM_READ | C::IMAGE_SCN_MEM_WRITE | align;
            break;
        case Kind::Bss:
            out.name = ".bss";
            out.flags = C::IMAGE_SCN_CNT_UNINITIALIZED_DATA | C::IMAGE_SCN_MEM_READ | C::IMAGE_SCN_MEM_WRITE | align;
            break;
        case Kind::Debug:
            out.name = name.str();
            out.flags = C::IMAGE_SCN_CNT_INITIALIZED_DATA | C::IMAGE_SCN_MEM_DISCARDABLE | C::IMAGE_SCN_MEM_READ | C::IMAGE_SCN_ALIGN_1BYTES;
            break;
        case Kind::Drop:
            break;
        }
        out.size = s.sh_size;
        if (out.kind != Kind::Bss) {
            auto bytes = ef.getSectionContents(s);
            if (!bytes) return fail(llvm::toString(bytes.takeError()));
            out.data.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        }
        if (out.size > std::numeric_limits<uint32_t>::max()) return fail(name.str() + " is larger than 4 GiB");
    }
    if (!symtab) return fail("no symbol table");

    // ---- The symbols: the kept sections' first (each with its section definition), then the ELF symbols in their order ----
    std::vector<Symbol> syms;
    uint32_t records = 0;
    uint32_t number = 0;
    for (Section& s : secs) {
        if (s.kind == Kind::Drop) continue;
        s.number = ++number;
        s.symbol = records;
        Symbol sym;
        sym.name = s.name;
        sym.section = static_cast<int32_t>(s.number);
        sym.storage = llvm::COFF::IMAGE_SYM_CLASS_STATIC;
        sym.section_def = true;
        sym.sec = &s;
        syms.push_back(sym);
        records += 2;
    }
    if (number > 0xFEFF) return fail(std::to_string(number) + " sections, more than a COFF object holds");
    auto esyms = ef.symbols(symtab);
    if (!esyms) return fail(llvm::toString(esyms.takeError()));
    auto strtab = ef.getStringTableForSymtab(*symtab);
    if (!strtab) return fail(llvm::toString(strtab.takeError()));
    constexpr uint32_t kNone = UINT32_MAX;
    std::vector<uint32_t> index_of(esyms->size(), kNone);  // ELF symbol -> COFF symbol record
    std::vector<std::string> names(esyms->size());
    for (size_t j = 1; j < esyms->size(); ++j) {
        const auto& es = (*esyms)[j];
        auto name_or = es.getName(*strtab);
        if (!name_or) return fail(llvm::toString(name_or.takeError()));
        names[j] = name_or->str();
        const uint8_t type = es.getType(), bind = es.getBinding();
        const uint16_t shndx = es.st_shndx;
        if (type == llvm::ELF::STT_FILE) continue;
        if (shndx == llvm::ELF::SHN_XINDEX) return fail("symbol " + names[j] + " has an extended section index");
        if (type == llvm::ELF::STT_SECTION) {
            if (shndx < n && secs[shndx].kind != Kind::Drop) index_of[j] = secs[shndx].symbol;
            continue;
        }
        if (shndx == llvm::ELF::SHN_ABS || shndx == llvm::ELF::SHN_COMMON) continue;  // an error only when a relocation names it
        Symbol sym;
        sym.name = names[j].empty() ? "$nxaot.sym." + std::to_string(j) : names[j];
        sym.storage = bind == llvm::ELF::STB_LOCAL ? llvm::COFF::IMAGE_SYM_CLASS_STATIC : llvm::COFF::IMAGE_SYM_CLASS_EXTERNAL;
        if (shndx == llvm::ELF::SHN_UNDEF) {
            if (bind == llvm::ELF::STB_WEAK) return fail("the weak undefined symbol " + names[j] + ", which COFF has no form for here");
            if (bind == llvm::ELF::STB_LOCAL) continue;
        } else {
            if (shndx >= n || secs[shndx].kind == Kind::Drop) continue;  // in a dropped section (.eh_frame's labels)
            if (es.st_value > std::numeric_limits<uint32_t>::max()) return fail("symbol " + names[j] + " lies beyond 4 GiB in its section");
            sym.section = static_cast<int32_t>(secs[shndx].number);
            sym.value = static_cast<uint32_t>(es.st_value);
            if (type == llvm::ELF::STT_FUNC) sym.type = llvm::COFF::IMAGE_SYM_DTYPE_FUNCTION << llvm::COFF::SCT_COMPLEX_TYPE_SHIFT;
        }
        index_of[j] = records;
        syms.push_back(sym);
        records += 1;
    }

    // ---- The relocations, the addends written into the sections' bytes ----
    for (size_t i = 0; i < n; ++i) {
        const Shdr& rs = (*shdrs)[i];
        if (rs.sh_type != llvm::ELF::SHT_RELA) continue;
        if (rs.sh_info >= n) return fail("a relocation section for no section");
        Section& out = secs[rs.sh_info];
        if (out.kind == Kind::Drop) continue;
        auto relas = ef.relas(rs);
        if (!relas) return fail(llvm::toString(relas.takeError()));
        for (const auto& r : *relas) {
            const uint32_t type = r.getType(false), sym = r.getSymbol(false);
            const uint64_t at = r.r_offset;
            const int64_t addend = r.r_addend;
            const auto site = [&] {
                return "a relocation of type " + std::to_string(type) + " at +0x" + llvm::utohexstr(at) + " of section " +
                       std::to_string(rs.sh_info) + " (" + out.name + ") against " + (sym < names.size() ? "'" + names[sym] + "'" : "?");
            };
            if (sym == 0 || sym >= index_of.size() || index_of[sym] == kNone) {
                return fail(site() + ": its symbol has no COFF form (an absolute or common symbol, or one in a dropped section)");
            }
            Reloc cr;
            cr.offset = static_cast<uint32_t>(at);
            cr.symbol = index_of[sym];
            const auto put = [&](uint64_t value, unsigned bytes) {
                if (out.kind == Kind::Bss || at + bytes > out.data.size()) return false;
                std::memcpy(out.data.data() + at, &value, bytes);
                return true;
            };
            bool ok = false;
            switch (type) {
            case llvm::ELF::R_X86_64_PC32:
            case llvm::ELF::R_X86_64_PLT32: {
                // S + A - P in ELF; S + addend - (P + 4) in COFF
                const int64_t v = addend + 4;
                if (v < INT32_MIN || v > INT32_MAX) return fail(site() + ": its addend does not fit 32 bits");
                ok = put(static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(v))), 4);
                cr.type = llvm::COFF::IMAGE_REL_AMD64_REL32;
                break;
            }
            case llvm::ELF::R_X86_64_64:
                ok = put(static_cast<uint64_t>(addend), 8);
                cr.type = llvm::COFF::IMAGE_REL_AMD64_ADDR64;
                break;
            case llvm::ELF::R_X86_64_32:
            case llvm::ELF::R_X86_64_32S: {
                // In a DWARF section: an offset into another DWARF section (its section symbol plus the addend), COFF's SECREL
                const auto& es = (*esyms)[sym];
                const bool to_debug = es.st_shndx < n && secs[es.st_shndx].kind == Kind::Debug;
                if (out.kind != Kind::Debug || !to_debug) {
                    return fail(site() + ": a 32-bit absolute address, which a DLL above 4 GiB cannot hold");
                }
                if (addend < 0 || addend > UINT32_MAX) return fail(site() + ": its addend does not fit 32 bits");
                ok = put(static_cast<uint64_t>(addend) + es.st_value, 4);
                cr.symbol = secs[es.st_shndx].symbol;  // the section's symbol: SECREL counts from the start of its section
                cr.type = llvm::COFF::IMAGE_REL_AMD64_SECREL;
                break;
            }
            default:
                return fail(site() + ": no COFF form (a GOT, TLS or 64-bit pc-relative relocation, which a pack's code does not use)");
            }
            if (!ok) return fail(site() + ": outside its section's bytes");
            out.relocs.push_back(cr);
        }
    }

    // ---- The file: header, section headers, each section's bytes and relocations, the symbol table, the string table ----
    std::string strings;
    const auto string_offset = [&](const std::string& s) {
        const uint32_t off = static_cast<uint32_t>(4 + strings.size());
        strings += s;
        strings += '\0';
        return off;
    };
    std::vector<const Section*> kept;
    for (const Section& s : secs) {
        if (s.kind != Kind::Drop) kept.push_back(&s);
    }
    uint64_t at = llvm::COFF::Header16Size + kept.size() * llvm::COFF::SectionSize;
    std::string headers, body;
    uint64_t relocations = 0;
    for (const Section* s : kept) {
        const uint64_t raw = s->kind == Kind::Bss ? 0 : s->data.size();
        const bool ovfl = s->relocs.size() >= 0xFFFF;
        const uint64_t nrel = s->relocs.size() + (ovfl ? 1 : 0);
        const uint64_t raw_at = raw ? at : 0;
        at += raw;
        const uint64_t rel_at = nrel ? at : 0;
        at += nrel * llvm::COFF::RelocationSize;
        if (at > std::numeric_limits<uint32_t>::max()) return fail("the object would be larger than 4 GiB");
        // Name: up to 8 bytes in place, else "/<offset>" into the string table
        char name[8] = {};
        if (s->name.size() <= 8) {
            std::memcpy(name, s->name.data(), s->name.size());
        } else {
            const uint32_t off = string_offset(s->name);
            if (off > 9'999'999) return fail("the string table is too large for a section's long name");
            const std::string ref = "/" + std::to_string(off);
            std::memcpy(name, ref.data(), ref.size());
        }
        headers.append(name, 8);
        Put32(&headers, 0);                                   // VirtualSize
        Put32(&headers, 0);                                   // VirtualAddress
        Put32(&headers, static_cast<uint32_t>(s->size));      // SizeOfRawData (a .bss's size)
        Put32(&headers, static_cast<uint32_t>(raw_at));       // PointerToRawData
        Put32(&headers, static_cast<uint32_t>(rel_at));       // PointerToRelocations
        Put32(&headers, 0);                                   // PointerToLinenumbers
        Put16(&headers, static_cast<uint16_t>(ovfl ? 0xFFFF : nrel));
        Put16(&headers, 0);                                   // NumberOfLinenumbers
        Put32(&headers, s->flags | (ovfl ? llvm::COFF::IMAGE_SCN_LNK_NRELOC_OVFL : 0));
        if (raw) body += s->data;
        if (ovfl) {  // the first relocation holds the count, itself included
            Put32(&body, static_cast<uint32_t>(nrel));
            Put32(&body, 0);
            Put16(&body, 0);
        }
        for (const Reloc& r : s->relocs) {
            Put32(&body, r.offset);
            Put32(&body, r.symbol);
            Put16(&body, r.type);
        }
        relocations += s->relocs.size();
    }
    std::string table;
    for (const Symbol& sym : syms) {
        if (sym.name.size() <= 8) {
            char name[8] = {};
            std::memcpy(name, sym.name.data(), sym.name.size());
            table.append(name, 8);
        } else {
            Put32(&table, 0);
            Put32(&table, string_offset(sym.name));
        }
        Put32(&table, sym.value);
        Put16(&table, static_cast<uint16_t>(sym.section));
        Put16(&table, sym.type);
        table += static_cast<char>(sym.storage);
        table += static_cast<char>(sym.section_def ? 1 : 0);
        if (sym.section_def) {  // the section definition: length, relocations, line numbers, checksum, number, selection
            const uint64_t nrel = sym.sec->relocs.size();
            Put32(&table, static_cast<uint32_t>(sym.sec->size));
            Put16(&table, static_cast<uint16_t>(nrel >= 0xFFFF ? 0xFFFF : nrel));
            Put16(&table, 0);
            Put32(&table, 0);
            Put16(&table, 0);
            table += '\0';
            table.append(3, '\0');
        }
    }
    std::string& o = *coff;
    o.clear();
    Put16(&o, llvm::COFF::IMAGE_FILE_MACHINE_AMD64);
    Put16(&o, static_cast<uint16_t>(kept.size()));
    Put32(&o, 0);  // TimeDateStamp: none, for a deterministic object
    Put32(&o, static_cast<uint32_t>(at));  // PointerToSymbolTable
    Put32(&o, records);
    Put16(&o, 0);  // SizeOfOptionalHeader
    Put16(&o, 0);  // Characteristics
    o += headers;
    o += body;
    o += table;
    Put32(&o, static_cast<uint32_t>(4 + strings.size()));
    o += strings;
    stats->sections = kept.size();
    stats->symbols = records;
    stats->relocations = relocations;
    return true;
}

}  // namespace nxaot::build
