#include "module/image.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/SHA256.h>

namespace nxaot::module {

namespace {

// ELF dynamic tags and relocation types the loader reads (as dm/host/loader.c)
enum : int64_t {
    DT_NULL = 0, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8, DT_STRSZ = 10, DT_INIT = 12, DT_FINI = 13,
    DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_FINI_ARRAY = 26, DT_INIT_ARRAYSZ = 27, DT_FINI_ARRAYSZ = 28, DT_PREINIT_ARRAY = 32,
    DT_PREINIT_ARRAYSZ = 33, DT_RELRSZ = 35, DT_RELR = 36, DT_GNU_HASH = 0x6ffffef5,
};
enum : uint32_t { R_AARCH64_ABS64 = 257, R_AARCH64_GLOB_DAT = 1025, R_AARCH64_JUMP_SLOT = 1026, R_AARCH64_RELATIVE = 1027 };

std::string Hex(const uint8_t* p, size_t n) {
    std::string s;
    char buf[4];
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(buf, sizeof buf, "%02x", p[i]);
        s += buf;
    }
    return s;
}

std::string X(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%#llx", static_cast<unsigned long long>(v));
    return buf;
}

// Bounds-checked little-endian reads from the image; a read outside it fails the reader (ok = false) and returns 0
struct Reader {
    const std::vector<uint8_t>& b;
    uint64_t p = 0;
    bool ok = true;

    template<typename T>
    T Get() {
        if (!ok || p > b.size() || sizeof(T) > b.size() - p) {
            ok = false;
            return T{};
        }
        T v;
        std::memcpy(&v, b.data() + p, sizeof v);
        p += sizeof v;
        return v;
    }
    uint64_t Uleb() {
        uint64_t r = 0;
        for (unsigned shift = 0; ok; shift += 7) {
            const uint8_t byte = Get<uint8_t>();
            if (shift < 64) {
                r |= static_cast<uint64_t>(byte & 0x7F) << shift;
            }
            if (!(byte & 0x80)) {
                break;
            }
        }
        return r;
    }
    int64_t Sleb() {
        int64_t r = 0;
        unsigned shift = 0;
        uint8_t byte = 0;
        do {
            byte = Get<uint8_t>();
            if (shift < 64) {
                r |= static_cast<int64_t>(byte & 0x7F) << shift;
            }
            shift += 7;
        } while (ok && (byte & 0x80));
        if (shift < 64 && (byte & 0x40)) {
            r |= -(static_cast<int64_t>(1) << shift);
        }
        return r;
    }
    std::string CString() {
        std::string s;
        while (ok) {
            const char c = static_cast<char>(Get<uint8_t>());
            if (!c) {
                break;
            }
            s += c;
        }
        return s;
    }
    // A DWARF pointer encoding (DW_EH_PE_*): the format in the low nibble, the application in bits 4..6; base for datarel
    uint64_t Encoded(uint8_t enc, uint64_t datarel_base) {
        const uint64_t at = p;
        uint64_t v = 0;
        switch (enc & 0x0F) {
        case 0x00: v = Get<uint64_t>(); break;                                   // absptr
        case 0x01: v = Uleb(); break;                                            // uleb128
        case 0x02: v = Get<uint16_t>(); break;                                   // udata2
        case 0x03: v = Get<uint32_t>(); break;                                   // udata4
        case 0x04: v = Get<uint64_t>(); break;                                   // udata8
        case 0x09: v = static_cast<uint64_t>(Sleb()); break;                     // sleb128
        case 0x0A: v = static_cast<uint64_t>(static_cast<int64_t>(Get<int16_t>())); break;
        case 0x0B: v = static_cast<uint64_t>(static_cast<int64_t>(Get<int32_t>())); break;
        case 0x0C: v = Get<uint64_t>(); break;
        default: ok = false; return 0;
        }
        switch (enc & 0x70) {
        case 0x00: break;
        case 0x10: v += at; break;            // pcrel
        case 0x30: v += datarel_base; break;  // datarel
        default: ok = false; return 0;        // textrel, funcrel, aligned: not produced by the toolchains of the Switch
        }
        return v;
    }
};

bool ReadFile(const std::string& path, std::vector<uint8_t>* out) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::array<uint8_t, 32> Sha256(const std::vector<uint8_t>& bytes) {
    const auto h = llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(bytes.data(), bytes.size()));
    std::array<uint8_t, 32> out{};
    std::copy(h.begin(), h.end(), out.begin());
    return out;
}

// The number of DT_SYMTAB entries: DT_HASH's nchain, else the highest index DT_GNU_HASH reaches, else the distance to DT_STRTAB when the string
// table follows the symbols (the host's rule, loader.c parse_dynamic)
uint64_t SymbolCount(const Image& m, uint64_t symtab, uint64_t strtab, uint64_t hash, uint64_t gnu_hash) {
    Reader r{m.bytes};
    if (hash) {
        r.p = hash + 4;
        const uint32_t nchain = r.Get<uint32_t>();
        if (r.ok) {
            return nchain;
        }
    }
    if (gnu_hash) {
        r.p = gnu_hash;
        const uint32_t nbuckets = r.Get<uint32_t>(), symoffset = r.Get<uint32_t>(), bloom_size = r.Get<uint32_t>();
        r.Get<uint32_t>();
        const uint64_t buckets = r.p + 8ull * bloom_size, chains = buckets + 4ull * nbuckets;
        uint32_t last = 0;
        r.p = buckets;
        for (uint32_t i = 0; i < nbuckets && r.ok; ++i) {
            last = std::max(last, r.Get<uint32_t>());
        }
        if (r.ok && last >= symoffset) {
            r.p = chains + 4ull * (last - symoffset);
            while (r.ok && !(r.Get<uint32_t>() & 1)) {
                ++last;
            }
            if (r.ok) {
                return last + 1ull;
            }
        } else if (r.ok) {
            return symoffset;
        }
    }
    return strtab > symtab ? (strtab - symtab) / 24 : 0;
}

void ParseFdes(Image& m) {
    if (m.eh_frame_hdr_end < m.eh_frame_hdr + 12 || !m.InImage(m.eh_frame_hdr, 4)) {
        return;  // no .eh_frame_hdr (an empty range: a module built without unwind tables)
    }
    Reader r{m.bytes, m.eh_frame_hdr};
    const uint8_t version = r.Get<uint8_t>(), ptr_enc = r.Get<uint8_t>(), count_enc = r.Get<uint8_t>(), table_enc = r.Get<uint8_t>();
    if (version != 1 || table_enc == 0xFF || count_enc == 0xFF) {
        m.warnings.push_back("[RED] .eh_frame_hdr at " + X(m.eh_frame_hdr) + " is not version 1 with a search table: no FDEs read");
        return;
    }
    r.Encoded(ptr_enc, m.eh_frame_hdr);
    const uint64_t count = r.Encoded(count_enc, m.eh_frame_hdr);
    struct CieInfo {
        bool ok = false;
        uint8_t fde_enc = 0, lsda_enc = 0xFF;
        bool z = false;
    };
    std::vector<std::pair<uint64_t, CieInfo>> cies;
    auto cie_of = [&](uint64_t cie) -> CieInfo {
        for (const auto& [at, info] : cies) {
            if (at == cie) {
                return info;
            }
        }
        CieInfo info;
        Reader c{m.bytes, cie};
        uint64_t len = c.Get<uint32_t>();
        if (len == 0xFFFFFFFFu) {
            len = c.Get<uint64_t>();
        }
        const uint32_t id = c.Get<uint32_t>();
        const uint8_t ver = c.Get<uint8_t>();
        const std::string aug = c.CString();
        if (aug.find("eh") != std::string::npos) {
            c.Get<uint64_t>();
        }
        c.Uleb();
        c.Sleb();
        if (ver == 1) {
            c.Get<uint8_t>();
        } else {
            c.Uleb();
        }
        if (!aug.empty() && aug[0] == 'z') {
            info.z = true;
            c.Uleb();
            for (size_t i = 1; i < aug.size() && c.ok; ++i) {
                switch (aug[i]) {
                case 'R': info.fde_enc = c.Get<uint8_t>(); break;
                case 'L': info.lsda_enc = c.Get<uint8_t>(); break;
                case 'P': {
                    const uint8_t pe = c.Get<uint8_t>();
                    c.Encoded(pe & 0x7F, m.eh_frame_hdr);
                    break;
                }
                default: break;  // 'S' (signal frame), 'B' (branch protection): no data
                }
            }
        }
        info.ok = c.ok && id == 0 && len > 0;
        cies.emplace_back(cie, info);
        return info;
    };
    for (uint64_t i = 0; i < count && r.ok; ++i) {
        r.Encoded(table_enc, m.eh_frame_hdr);
        const uint64_t f = r.Encoded(table_enc, m.eh_frame_hdr);
        if (!r.ok) {
            break;
        }
        Fde fde;
        fde.offset = f;
        Reader fr{m.bytes, f};
        const uint32_t len = fr.Get<uint32_t>();
        const uint64_t cie_field = fr.p;
        const uint32_t cie_ptr = fr.Get<uint32_t>();
        const CieInfo cie = len && len != 0xFFFFFFFFu ? cie_of(cie_field - cie_ptr) : CieInfo{};
        if (!fr.ok || !cie.ok) {
            m.warnings.push_back("[RED] FDE at " + X(f) + ": its CIE cannot be read");
            continue;
        }
        fde.start = fr.Encoded(cie.fde_enc, m.eh_frame_hdr);
        fde.end = fde.start + fr.Encoded(cie.fde_enc & 0x0F, m.eh_frame_hdr);
        if (cie.z) {
            const uint64_t aug_len = fr.Uleb(), aug_end = fr.p + aug_len;
            if (cie.lsda_enc != 0xFF && aug_len) {
                fde.lsda = fr.Encoded(cie.lsda_enc, m.eh_frame_hdr);
                fde.has_lsda = fr.ok && fde.lsda != 0;
            }
            fr.p = aug_end;
        }
        if (!fr.ok) {
            m.warnings.push_back("[RED] FDE at " + X(f) + ": cannot be read");
            continue;
        }
        if (fde.has_lsda) {
            // The LSDA (.gcc_except_table): the landing pads' base, the type table (skipped), the call-site table
            Reader l{m.bytes, fde.lsda};
            const uint8_t lpstart_enc = l.Get<uint8_t>();
            fde.lp_start = lpstart_enc == 0xFF ? fde.start : l.Encoded(lpstart_enc, m.eh_frame_hdr);
            const uint8_t ttype_enc = l.Get<uint8_t>();
            if (ttype_enc != 0xFF) {
                l.Uleb();
            }
            const uint8_t cs_enc = l.Get<uint8_t>();
            const uint64_t cs_len = l.Uleb(), cs_end = l.p + cs_len;
            while (l.ok && l.p < cs_end) {
                CallSite cs;
                const uint64_t start = l.Encoded(cs_enc, 0), length = l.Encoded(cs_enc, 0), lp = l.Encoded(cs_enc, 0);
                cs.action = l.Uleb();
                cs.start = fde.lp_start + start;
                cs.length = length;
                cs.landing_pad = lp ? fde.lp_start + lp : 0;
                if (l.ok) {
                    fde.call_sites.push_back(cs);
                }
            }
            if (!l.ok) {
                fde.error = "LSDA at " + X(fde.lsda) + " cannot be read whole";
                m.warnings.push_back("[RED] FDE at " + X(f) + ": " + fde.error);
            }
        }
        m.fdes.push_back(std::move(fde));
    }
    std::sort(m.fdes.begin(), m.fdes.end(), [](const Fde& a, const Fde& b) { return a.start < b.start; });
}

// Everything after the layout: MOD0, the dynamic section, symbols, relocations, the init and fini arrays, the FDEs
void ParseModule(Image& m) {
    Reader r{m.bytes, 4};
    m.mod0 = r.Get<uint32_t>();
    if (!r.ok || !m.InImage(m.mod0, 28) || std::memcmp(m.bytes.data() + m.mod0, "MOD0", 4) != 0) {
        m.warnings.push_back("[RED] no MOD0 at the offset word +4 names: no dynamic section, symbols or unwind entries read");
        return;
    }
    r.p = m.mod0 + 4;
    auto rel = [&]() { return m.mod0 + static_cast<uint64_t>(static_cast<int64_t>(r.Get<int32_t>())); };
    m.dynamic = rel();
    m.bss_start = rel();
    m.bss_end = rel();
    m.eh_frame_hdr = rel();
    m.eh_frame_hdr_end = rel();

    uint64_t symtab = 0, strtab = 0, hash = 0, gnu_hash = 0, rela = 0, relasz = 0, jmprel = 0, pltsz = 0, relr = 0, relrsz = 0;
    uint64_t init_array = 0, init_arraysz = 0, fini_array = 0, fini_arraysz = 0, preinit_array = 0, preinit_arraysz = 0;
    Reader d{m.bytes, m.dynamic};
    for (unsigned n = 0; n < 4096; ++n) {
        const int64_t tag = d.Get<int64_t>();
        const uint64_t val = d.Get<uint64_t>();
        if (!d.ok) {
            m.warnings.push_back("[RED] the dynamic section at " + X(m.dynamic) + " runs past the image");
            break;
        }
        if (tag == DT_NULL) {
            break;
        }
        switch (tag) {
        case DT_SYMTAB: symtab = val; break;
        case DT_STRTAB: strtab = val; break;
        case DT_HASH: hash = val; break;
        case DT_GNU_HASH: gnu_hash = val; break;
        case DT_RELA: rela = val; break;
        case DT_RELASZ: relasz = val; break;
        case DT_JMPREL: jmprel = val; break;
        case DT_PLTRELSZ: pltsz = val; break;
        case DT_RELR: relr = val; break;
        case DT_RELRSZ: relrsz = val; break;
        case DT_INIT: m.init = val; break;
        case DT_FINI: m.fini = val; break;
        case DT_INIT_ARRAY: init_array = val; break;
        case DT_INIT_ARRAYSZ: init_arraysz = val; break;
        case DT_FINI_ARRAY: fini_array = val; break;
        case DT_FINI_ARRAYSZ: fini_arraysz = val; break;
        case DT_PREINIT_ARRAY: preinit_array = val; break;
        case DT_PREINIT_ARRAYSZ: preinit_arraysz = val; break;
        default: break;
        }
    }

    if (symtab) {
        const uint64_t n = SymbolCount(m, symtab, strtab, hash, gnu_hash);
        Reader s{m.bytes, symtab};
        for (uint64_t i = 0; i < n && i < (1u << 24); ++i) {
            Symbol sym;
            const uint32_t name = s.Get<uint32_t>();
            const uint8_t info = s.Get<uint8_t>();
            s.Get<uint8_t>();
            sym.shndx = s.Get<uint16_t>();
            sym.value = s.Get<uint64_t>();
            sym.size = s.Get<uint64_t>();
            if (!s.ok) {
                m.warnings.push_back("[RED] the symbol table runs past the image");
                break;
            }
            sym.type = info & 15;
            sym.bind = info >> 4;
            Reader nr{m.bytes, strtab + name};
            sym.name = nr.CString();
            m.symbols.push_back(std::move(sym));
        }
    }

    auto read_rela = [&](uint64_t at, uint64_t bytes, RelocTable table) {
        Reader t{m.bytes, at};
        for (uint64_t i = 0; i < bytes / 24; ++i) {
            Reloc rl;
            rl.table = table;
            rl.offset = t.Get<uint64_t>();
            const uint64_t info = t.Get<uint64_t>();
            rl.addend = t.Get<int64_t>();
            if (!t.ok) {
                m.warnings.push_back("[RED] a relocation table runs past the image");
                return;
            }
            rl.type = static_cast<uint32_t>(info);
            rl.sym = static_cast<uint32_t>(info >> 32);
            if (rl.type == R_AARCH64_RELATIVE) {
                rl.has_target = true;
                rl.target = static_cast<uint64_t>(rl.addend);
            } else if ((rl.type == R_AARCH64_ABS64 || rl.type == R_AARCH64_GLOB_DAT || rl.type == R_AARCH64_JUMP_SLOT) && rl.sym < m.symbols.size() &&
                       m.symbols[rl.sym].Defined()) {
                rl.has_target = true;
                rl.target = m.symbols[rl.sym].value + (rl.type == R_AARCH64_JUMP_SLOT ? 0 : static_cast<uint64_t>(rl.addend));
            }
            m.relocs.push_back(rl);
        }
    };
    if (relasz) {
        read_rela(rela, relasz, RelocTable::Rela);
    }
    if (pltsz) {
        read_rela(jmprel, pltsz, RelocTable::Jmprel);
    }
    if (relrsz) {
        // DT_RELR (loader.c relr_apply): even entries are slots, odd ones bitmaps of the 63 slots after the last; the slot holds the target
        Reader t{m.bytes, relr};
        uint64_t where = 0;
        bool have = false;
        for (uint64_t i = 0; i < relrsz / 8; ++i) {
            uint64_t v = t.Get<uint64_t>();
            if (!t.ok) {
                m.warnings.push_back("[RED] DT_RELR runs past the image");
                break;
            }
            auto add = [&](uint64_t slot) {
                Reloc rl;
                rl.table = RelocTable::Relr;
                rl.offset = slot;
                rl.type = R_AARCH64_RELATIVE;
                if (const auto value = m.U64(slot)) {
                    rl.has_target = true;
                    rl.target = *value;
                    rl.addend = static_cast<int64_t>(*value);
                }
                m.relocs.push_back(rl);
            };
            if (!(v & 1)) {
                add(v);
                where = v + 8;
                have = true;
                continue;
            }
            if (!have) {
                m.warnings.push_back("[RED] DT_RELR bitmap entry with no start before it");
                break;
            }
            for (unsigned b = 0; (v >>= 1); ++b) {
                if (v & 1) {
                    add(where + 8ull * b);
                }
            }
            where += 63 * 8;
        }
    }

    for (const auto& rl : m.relocs) {
        if (rl.has_target) {
            m.slot_targets[rl.offset] = rl.target;
        }
    }
    // The arrays hold function pointers the loader relocates: the relocation's target where one names the slot, else the slot's value
    auto array = [&](uint64_t at, uint64_t bytes, std::vector<uint64_t>& out) {
        for (uint64_t slot = at; slot < at + bytes; slot += 8) {
            if (const auto it = m.slot_targets.find(slot); it != m.slot_targets.end()) {
                out.push_back(it->second);
            } else if (const auto v = m.U64(slot)) {
                out.push_back(*v);
            }
        }
    };
    array(init_array, init_arraysz, m.init_array);
    array(fini_array, fini_arraysz, m.fini_array);
    array(preinit_array, preinit_arraysz, m.preinit_array);
    ParseFdes(m);
}

// The data blocks an NRO has in its text: the header after the entry branch (word +4 is MOD0's offset, +8 the "HOMEBREW" tag or padding, then
// the NRO header to +0x80: nso_code.py data_blocks skips +8..+0x80 for the rewriter, which keeps the word at +4 as it is; here it is data), and MOD0
// with libnx's LNY extension records when the linker put it in the text, as libnx's crt0 does
void NroDataBlocks(Image& m) {
    m.data_blocks.push_back({4, 0x80, "nro header"});
    if (m.mod0 && m.InText(m.mod0)) {
        uint64_t end = m.mod0 + 28;
        while (m.InImage(end, 12) && end < m.TextEnd() && std::memcmp(m.bytes.data() + end, "LNY", 3) == 0) {
            end += 12;
        }
        m.data_blocks.push_back({m.mod0, std::min(end, m.TextEnd()), "mod0"});
    }
}

}  // namespace

std::optional<uint32_t> Image::Word(uint64_t off) const {
    if (off & 3 || !InImage(off, 4)) {
        return std::nullopt;
    }
    uint32_t w;
    std::memcpy(&w, bytes.data() + off, 4);
    return w;
}

std::optional<uint64_t> Image::U64(uint64_t off) const {
    if (!InImage(off, 8)) {
        return std::nullopt;
    }
    uint64_t v;
    std::memcpy(&v, bytes.data() + off, 8);
    return v;
}

bool Image::InData(uint64_t off) const {
    for (const auto& b : data_blocks) {
        if (off >= b.lo && off < b.hi) {
            return true;
        }
    }
    return false;
}

std::string Image::Sha256Hex() const {
    return Hex(sha256.data(), sha256.size());
}

std::string Image::ModuleIdHex() const {
    return Hex(module_id.data(), module_id.size());
}

std::optional<Image> LoadModule(const std::string& path, std::string* error) {
    Image m;
    m.path = path;
    std::vector<uint8_t> file;
    if (!ReadFile(path, &file)) {
        *error = "cannot read " + path;
        return std::nullopt;
    }
    const std::string base = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
    static const char kManifestTag[] = "# nxaot-manifest";
    if (file.size() >= sizeof kManifestTag - 1 && std::memcmp(file.data(), kManifestTag, sizeof kManifestTag - 1) == 0) {
        const std::string text(file.begin(), file.end());
        std::string why;
        auto manifest = ParseManifest(text, &why);
        if (!manifest) {
            *error = path + ": " + why;
            return std::nullopt;
        }
        if (!manifest->layout) {
            *error = path + ": no layout row";
            return std::nullopt;
        }
        const std::string suffix = ".aot.tsv";
        if (path.size() <= suffix.size() || path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) {
            *error = path + ": a manifest is named <name>.aot.tsv, beside <name>.image.bin";
            return std::nullopt;
        }
        const std::string image_path = path.substr(0, path.size() - suffix.size()) + ".image.bin";
        if (!ReadFile(image_path, &m.bytes)) {
            *error = "cannot read " + image_path + ", the image of " + path;
            return std::nullopt;
        }
        m.sha256 = Sha256(m.bytes);
        if (m.Sha256Hex() != manifest->sha256) {
            *error = image_path + ": SHA-256 " + m.Sha256Hex() + ", the manifest names " + manifest->sha256;
            return std::nullopt;
        }
        const auto& l = *manifest->layout;
        if (l.image_size != m.bytes.size()) {
            *error = path + ": layout image_size " + X(l.image_size) + ", image.bin has " + X(m.bytes.size()) + " bytes";
            return std::nullopt;
        }
        m.name = manifest->module;
        m.text_off = 0;
        m.text_size = l.text_end;
        m.ro_off = l.ro_off;
        m.ro_size = l.data_off - l.ro_off;
        m.data_off = l.data_off;
        m.data_size = l.image_size - l.data_off;
        m.bss_size = l.bss;
        for (const auto& r : manifest->data) {
            m.data_blocks.push_back({r.lo, r.hi, "manifest"});
        }
        m.manifest = std::move(manifest);
        ParseModule(m);
    } else {
        if (file.size() < 0x80 || std::memcmp(file.data() + 0x10, "NRO0", 4) != 0) {
            *error = path + ": neither an NRO (NRO0 at +0x10) nor a manifest (# nxaot-manifest)";
            return std::nullopt;
        }
        m.nro = true;
        m.bytes = std::move(file);
        m.sha256 = Sha256(m.bytes);
        m.name = base;
        Reader h{m.bytes, 0x18};
        const uint32_t size = h.Get<uint32_t>();
        h.p = 0x20;
        m.text_off = h.Get<uint32_t>();
        m.text_size = h.Get<uint32_t>();
        m.ro_off = h.Get<uint32_t>();
        m.ro_size = h.Get<uint32_t>();
        m.data_off = h.Get<uint32_t>();
        m.data_size = h.Get<uint32_t>();
        m.bss_size = h.Get<uint32_t>();
        std::memcpy(m.module_id.data(), m.bytes.data() + 0x40, 32);
        if (size > m.bytes.size() || m.text_off != 0 || m.ro_off < m.text_off + m.text_size || m.data_off < m.ro_off + m.ro_size ||
            !m.InImage(m.data_off, m.data_size)) {
            *error = path + ": the NRO header's size or segments do not fit the file";
            return std::nullopt;
        }
        ParseModule(m);
        NroDataBlocks(m);
    }
    std::sort(m.data_blocks.begin(), m.data_blocks.end(), [](const DataBlock& a, const DataBlock& b) { return a.lo < b.lo; });
    return m;
}

Manifest ManifestOf(const Image& image) {
    Manifest m;
    m.module = image.name;
    m.build_id = image.ModuleIdHex();
    m.sha256 = image.Sha256Hex();
    Manifest::Layout l;
    l.text_end = image.TextEnd();
    l.ro_off = image.ro_off;
    l.data_off = image.data_off;
    l.bss = image.bss_size;
    l.image_size = image.bytes.size();
    m.layout = l;
    for (const auto& b : image.data_blocks) {
        m.data.push_back({b.lo, b.hi});
    }
    return m;
}

}  // namespace nxaot::module
