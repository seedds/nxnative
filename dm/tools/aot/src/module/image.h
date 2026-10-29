// The module loader of nxaot (docs/design/aot-recompiler.md sections 1.1 to 1.4): one module, NRO or behaviour image, as one flat in-memory image
// with what the analysis reads from it. An NRO is read as the host's loader reads it (dm/host/loader.c: MOD0 at the offset in word +4, the dynamic
// section, the symbols, DT_INIT / DT_FINI and the init arrays, RELA / JMPREL / RELR relocations), plus .eh_frame_hdr, its FDEs and their LSDAs
// (.gcc_except_table call sites and landing pads), as dm/tools/nso_code.py fde_ranges reads them. A behaviour image is <name>.image.bin with its
// manifest <name>.aot.tsv (src/module/manifest.h); its layout comes from the manifest and everything else from the same parse. Either is keyed by
// the SHA-256 of its bytes (the NRO file, or image.bin, which for an NRO is the same bytes), as ro::LoadModule finds NROs (dm/host/hle_ro.c).
// Offsets are module offsets: modules are linked at 0.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "module/manifest.h"

namespace nxaot::module {

struct Symbol {
    uint64_t value = 0, size = 0;
    uint8_t type = 0, bind = 0;  // ELF64_ST_TYPE / ELF64_ST_BIND
    uint16_t shndx = 0;
    std::string name;
    bool Defined() const { return shndx != 0; }
};

enum class RelocTable { Rela, Jmprel, Relr };

struct Reloc {
    RelocTable table = RelocTable::Rela;
    uint64_t offset = 0;  // the slot
    uint32_t type = 0;    // R_AARCH64_*; RELR entries are R_AARCH64_RELATIVE
    uint32_t sym = 0;
    int64_t addend = 0;
    bool has_target = false;  // the module offset the slot holds once relocated (RELATIVE; a symbol the module defines)
    uint64_t target = 0;
};

struct CallSite {  // an LSDA call-site entry: the range of calls [start, start + length) and its landing pad (0: none)
    uint64_t start = 0, length = 0, landing_pad = 0;
    uint64_t action = 0;
};

struct Fde {
    uint64_t start = 0, end = 0;  // the function range it describes
    uint64_t offset = 0;          // of the FDE itself
    bool has_lsda = false;
    uint64_t lsda = 0;            // the LSDA's offset (in .gcc_except_table)
    uint64_t lp_start = 0;        // the landing pads' base (the function start unless the LSDA says otherwise)
    std::vector<CallSite> call_sites;
    std::string error;            // what could not be read of its CIE or LSDA
};

struct DataBlock {
    uint64_t lo = 0, hi = 0;
    std::string why;  // "nro header", "mod0", "manifest"
};

struct Image {
    std::string path, name;
    bool nro = false;               // read from an NRO file (false: a behaviour image with its manifest)
    std::vector<uint8_t> bytes;     // the image as the file holds it (bss not included)
    std::array<uint8_t, 32> sha256{};
    std::array<uint8_t, 32> module_id{};  // the NRO header's build ID (zeros for a behaviour image: the manifest has it as text)
    uint64_t text_off = 0, text_size = 0, ro_off = 0, ro_size = 0, data_off = 0, data_size = 0, bss_size = 0;

    uint64_t mod0 = 0, dynamic = 0, bss_start = 0, bss_end = 0, eh_frame_hdr = 0, eh_frame_hdr_end = 0;
    std::vector<Symbol> symbols;  // DT_SYMTAB, index 0 included (the null symbol)
    uint64_t init = 0, fini = 0;  // 0: none
    std::vector<uint64_t> init_array, fini_array, preinit_array;  // the functions they hold (relocation targets, else the slots' values)
    std::vector<Reloc> relocs;
    std::map<uint64_t, uint64_t> slot_targets;  // relocated 8-byte slots and the module offset each holds once relocated
    std::vector<Fde> fdes;        // sorted by start
    std::vector<DataBlock> data_blocks;  // sorted, in text
    std::optional<Manifest> manifest;
    std::vector<std::string> warnings;

    uint64_t TextEnd() const { return text_off + text_size; }
    bool InText(uint64_t off) const { return off >= text_off && off < TextEnd(); }
    bool InImage(uint64_t off, uint64_t n) const { return off <= bytes.size() && n <= bytes.size() - off; }
    std::optional<uint32_t> Word(uint64_t off) const;  // the instruction word at off (4-aligned, inside the image)
    std::optional<uint64_t> U64(uint64_t off) const;
    bool InData(uint64_t off) const;  // inside a data block
    std::string Sha256Hex() const;
    std::string ModuleIdHex() const;
};

// Reads an NRO (NRO0 at +0x10) or a manifest (a file whose first line is "# nxaot-manifest"; the image is the file named <stem>.image.bin beside
// <stem>.aot.tsv). nullopt with *error set when the module cannot be read
std::optional<Image> LoadModule(const std::string& path, std::string* error);

// The manifest of an NRO's behaviour image: the layout and the data blocks the loader found (the NRO header, MOD0 when it lies in the text);
// image.bin is the NRO's bytes unchanged
Manifest ManifestOf(const Image& image);

}  // namespace nxaot::module
