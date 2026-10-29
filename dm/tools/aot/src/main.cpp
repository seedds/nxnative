// nxaot: the ahead-of-time ARM64 -> x86-64 recompiler (docs/design/aot-recompiler.md). Subcommands today:
//   nxaot discover <module.nro | name.aot.tsv> [--symbols-as-oracle] [--tsv <out>]
//                                                     function discovery, CFGs, jump tables, exclusive windows and summaries of a module
//                                                     (src/analysis/discover.h): one line per function and the summary; --tsv writes every
//                                                     row kind; --symbols-as-oracle takes no symbol as a source and compares with them
//   nxaot image <module.nro> <out dir> [<name>]       the behaviour-image form of an NRO: <name>.image.bin and its manifest <name>.aot.tsv
//                                                     (layout and data rows), the input form patch_nso.py --aot will write for NSOs
//   nxaot manifest <name.aot.tsv>                     parses a manifest and prints it back in canonical form (src/module/manifest.h)
//   nxaot module <module.nro | name.aot.tsv>          what the loader read: layout, MOD0, symbols, relocations, FDEs and LSDAs, data blocks
//   nxaot census <raw ARM64 code file> --base <hex>   phase 0's census of a code blob, as key<TAB>count lines (src/analysis/census.h)
//   nxaot opcodes                                     every dynarmic IR opcode with whether the lifter lowers it (group<TAB>opcode)
//   nxaot opcodes --reach [--samples N] [--check]     the opcodes the A64 frontend emits (src/analysis/reach.h), with the decoder entries that emit
//                                                     them; --check exits 1 when one of them is not lowered
//   nxaot version                                     the toolchain of nxaot.lock this build was configured with
// The lifter (src/lift) is exercised by nxaot_vec until the analysis and the pack stages exist (README.md, "What is not done yet").
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "analysis/census.h"
#include "analysis/discover.h"
#include "analysis/reach.h"
#include "dynarmic/ir/opcodes.h"
#include "lift/opcodes.h"
#include "module/image.h"
#include "module/manifest.h"
#include "nxaot_lock.h"

namespace {

int Usage() {
    std::fprintf(stderr,
                 "usage: nxaot discover <module.nro | name.aot.tsv> [--symbols-as-oracle] [--tsv <out>]\n"
                 "       nxaot image <module.nro> <out dir> [<name>]\n"
                 "       nxaot manifest <name.aot.tsv>\n"
                 "       nxaot module <module.nro | name.aot.tsv>\n"
                 "       nxaot census <raw ARM64 code file> --base <hex>\n"
                 "       nxaot opcodes [--reach [--samples N] [--check]]\n"
                 "       nxaot version\n");
    return 2;
}

std::string X(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%#llx", static_cast<unsigned long long>(v));
    return v ? buf : "0x0";
}

std::optional<nxaot::module::Image> Load(const std::string& path) {
    std::string error;
    auto m = nxaot::module::LoadModule(path, &error);
    if (!m) {
        std::fprintf(stderr, "nxaot: %s\n", error.c_str());
    }
    return m;
}

const char* ClassOf(const nxaot::analysis::Function& f) {
    return !f.summary.returns ? "noreturn" : f.summary.preserving ? "preserving" : "open";
}

int Discover(int argc, char** argv) {
    using namespace nxaot::analysis;
    std::string path, tsv;
    DiscoverOptions opts;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--symbols-as-oracle") {
            opts.symbols = false;
        } else if (a == "--tsv" && i + 1 < argc) {
            tsv = argv[++i];
        } else if (path.empty() && !a.empty() && a[0] != '-') {
            path = a;
        } else {
            return Usage();
        }
    }
    if (path.empty()) {
        return Usage();
    }
    const auto image = Load(path);
    if (!image) {
        return 1;
    }
    const Discovery d = Discover(*image, opts);
    std::vector<std::string> rows;
    const std::string mode = opts.symbols ? "symbols as a source" : "symbols as the oracle";
    rows.push_back("# nxaot discover\t" + image->name + "\t" + image->Sha256Hex() + "\t" + mode);
    size_t tables = 0, widths[9] = {}, unresolved = 0, blr = 0, call_edges = 0, preserving = 0, open = 0, noreturn = 0, taken = 0, exported = 0;
    size_t wins = 0, aba = 0, load = 0, unpaired = 0, across = 0;
    uint64_t primary[kProvCount] = {}, any[kProvCount] = {};
    std::printf("# nxaot discover %s (%s): SHA-256 %s, text %s bytes\n", path.c_str(), mode.c_str(), image->Sha256Hex().c_str(),
                X(image->text_size).c_str());
    std::printf("# offset\tsize\tprovenance\tblocks\tcalls\tindirect\twindows\thas_lsda\tclass\tname\n");
    for (const auto& [entry, f] : d.functions) {
        size_t f_blr = 0;
        for (const auto& [pc, target] : f.calls) {
            if (!target) ++f_blr;
            else ++call_edges;
        }
        blr += f_blr;
        unresolved += f.indirect.size();
        tables += f.tables.size();
        for (const auto& [br, jt] : f.tables) {
            ++widths[std::min(jt.width, 8u)];
            across += jt.across_calls;
        }
        wins += f.windows.size();
        unpaired += f.unpaired_exclusive;
        for (const auto& w : f.windows) {
            aba += w.kind == "aba";
            load += w.kind == "load";
        }
        const bool has_lsda = f.fde >= 0 && image->fdes[static_cast<size_t>(f.fde)].has_lsda;
        const char* cls = ClassOf(f);
        preserving += std::string(cls) == "preserving";
        open += std::string(cls) == "open";
        noreturn += std::string(cls) == "noreturn";
        taken += f.address_taken;
        exported += f.summary.exported;
        for (unsigned b = 0; b < kProvCount; ++b) {
            any[b] += f.prov >> b & 1;
        }
        for (unsigned b = 0; b < kProvCount; ++b) {
            if (f.prov >> b & 1) {
                ++primary[b];
                break;
            }
        }
        std::printf("%s\t%s\t%s\t%zu\t%zu\t%zu\t%zu\t%d\t%s\t%s\n", X(entry).c_str(), X(f.end - f.entry).c_str(), ProvList(f.prov).c_str(),
                    f.blocks.size(), f.calls.size(), f.indirect.size() + f_blr, f.windows.size(), has_lsda ? 1 : 0, cls,
                    f.name.empty() ? "-" : f.name.c_str());
        rows.push_back("fn\t" + X(entry) + "\t" + X(f.end - f.entry) + "\t" + X(f.limit) + "\t" + ProvList(f.prov) + "\t" + std::to_string(f.blocks.size()) +
                       "\t" + std::to_string(f.calls.size()) + "\t" + std::to_string(f.indirect.size()) + "\t" + std::to_string(f_blr) + "\t" +
                       std::to_string(f.windows.size()) + "\t" + (has_lsda ? "1" : "0") + "\t" + f.summary.reads.Str() + "\t" +
                       f.summary.writes.Str() + "\t" + f.summary.must.Str() + "\t" + cls + "\t" + (f.address_taken ? "1" : "0") + "\t" +
                       (f.summary.exported ? "1" : "0") + "\t" + (f.name.empty() ? "-" : f.name));
        for (const auto& [br, jt] : f.tables) {
            std::string targets;
            for (size_t i = 0; i < jt.targets.size(); ++i) {
                targets += (i ? "," : "") + X(jt.targets[i]);
            }
            rows.push_back("jt\t" + X(br) + "\t" + X(entry) + "\t" + X(jt.table) + "\t" + std::to_string(jt.width) + "\t" + (jt.is_signed ? "1" : "0") +
                           "\t" + X(jt.base) + "\t" + std::to_string(jt.scale) + "\t" + std::to_string(jt.index_lo) + "\t" + std::to_string(jt.count) +
                           "\t" + X(jt.region) + "\t" + (jt.across_calls ? "1" : "0") + "\t" + targets);
        }
        for (const auto& [br, why] : f.indirect) {
            rows.push_back("br\t" + X(br) + "\t" + X(entry) + "\tindirect\t" + why);
        }
        for (const auto& [pc, target] : f.calls) {
            rows.push_back("call\t" + X(pc) + "\t" + X(entry) + "\t" + (target ? X(target) : "blr"));
        }
        for (const auto& w : f.windows) {
            rows.push_back("win\t" + X(w.lo) + "\t" + X(w.hi) + "\t" + X(entry) + "\t" + w.kind);
        }
    }
    for (const uint64_t lp : d.landing_pads) {
        rows.push_back("pad\t" + X(lp));
    }
    const auto& c = d.coverage;
    for (const auto& [lo, hi] : c.uncovered_ranges) {
        rows.push_back("gap\t" + X(lo) + "\t" + X(hi) + "\tuncovered");
    }
    std::string prov_line = "summary\tprovenance";
    for (unsigned b = 0; b < kProvCount; ++b) {
        prov_line += "\t" + std::string(ProvName(b)) + "\t" + std::to_string(primary[b]) + "/" + std::to_string(any[b]);
    }
    std::vector<std::string> summary = {
        "summary\tfunctions\t" + std::to_string(d.functions.size()),
        prov_line,
        "summary\tcoverage\ttext_words\t" + std::to_string(c.text_words) + "\tdata_words\t" + std::to_string(c.data_words) + "\tcovered\t" +
            std::to_string(c.covered) + "\tpadding\t" + std::to_string(c.padding) + "\tuncovered\t" + std::to_string(c.uncovered) + "\tundefined\t" +
            std::to_string(c.undefined),
        "summary\tjump_tables\t" + std::to_string(tables) + "\tbyte\t" + std::to_string(widths[1]) + "\thalf\t" + std::to_string(widths[2]) +
            "\tword\t" + std::to_string(widths[4]) + "\tpast_calls\t" + std::to_string(across) + "\tbr_indirect\t" + std::to_string(unresolved) +
            "\tblr\t" + std::to_string(blr) + "\tregions_lifted\t" + std::to_string(d.regions_lifted),
        "summary\twindows\t" + std::to_string(wins) + "\tcas\t" + std::to_string(wins - aba - load) + "\taba\t" + std::to_string(aba) + "\tload\t" +
            std::to_string(load) + "\tunpaired\t" + std::to_string(unpaired),
        "summary\tclasses\tpreserving\t" + std::to_string(preserving) + "\topen\t" + std::to_string(open) + "\tnoreturn\t" + std::to_string(noreturn) +
            "\taddress_taken\t" + std::to_string(taken) + "\texported\t" + std::to_string(exported) + "\tcall_edges\t" + std::to_string(call_edges),
        "summary\tdiscovery\tlanding_pads\t" + std::to_string(d.landing_pads.size()) + "\trounds\t" + std::to_string(d.rounds),
    };
    if (!opts.symbols) {
        size_t found = 0;
        for (const auto& r : d.oracle) {
            found += r.found;
            rows.push_back("oracle\t" + X(r.value) + "\t" + X(r.size) + "\t" + (r.found ? "found" : "missed") + "\t" + X(r.found_size) + "\t" + r.name);
            if (!r.found) {
                std::printf("[RED] nxaot discover: the symbol %s at %s is no discovered function's start\n", r.name.c_str(), X(r.value).c_str());
            }
        }
        summary.push_back("summary\toracle\tsymbols\t" + std::to_string(d.oracle.size()) + "\tfound\t" + std::to_string(found) + "\tmissed\t" +
                          std::to_string(d.oracle.size() - found));
    }
    for (const auto& n : d.notes) {
        std::printf("%s\n", n.c_str());
    }
    for (const auto& w : image->warnings) {
        std::printf("%s\n", w.c_str());
    }
    for (const auto& s : summary) {
        std::printf("%s\n", s.c_str());
        rows.push_back(s);
    }
    if (!tsv.empty()) {
        std::FILE* f = std::fopen(tsv.c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "nxaot discover: cannot write %s\n", tsv.c_str());
            return 1;
        }
        for (const auto& r : rows) {
            std::fprintf(f, "%s\n", r.c_str());
        }
        std::fclose(f);
    }
    return 0;
}

int Image(int argc, char** argv) {
    if (argc < 4 || argc > 5) {
        return Usage();
    }
    const auto image = Load(argv[2]);
    if (!image) {
        return 1;
    }
    if (!image->nro) {
        std::fprintf(stderr, "nxaot image: %s is a behaviour image already\n", argv[2]);
        return 2;
    }
    std::string name = argc == 5 ? argv[4] : image->name;
    if (argc == 4 && name.size() > 4 && name.compare(name.size() - 4, 4, ".nro") == 0) {
        name.resize(name.size() - 4);
    }
    const std::string base = std::string(argv[3]) + "/" + name;
    nxaot::module::Manifest m = nxaot::module::ManifestOf(*image);
    m.module = name;
    std::FILE* bin = std::fopen((base + ".image.bin").c_str(), "wb");
    std::FILE* tsv = std::fopen((base + ".aot.tsv").c_str(), "wb");
    if (!bin || !tsv) {
        std::fprintf(stderr, "nxaot image: cannot write %s.image.bin / .aot.tsv\n", base.c_str());
        return 1;
    }
    std::fwrite(image->bytes.data(), 1, image->bytes.size(), bin);
    const std::string text = nxaot::module::WriteManifest(m);
    std::fwrite(text.data(), 1, text.size(), tsv);
    std::fclose(bin);
    std::fclose(tsv);
    std::printf("nxaot image: %s.image.bin (%zu bytes, SHA-256 %s) and %s.aot.tsv\n", base.c_str(), image->bytes.size(), image->Sha256Hex().c_str(),
                base.c_str());
    return 0;
}

int Manifest(int argc, char** argv) {
    if (argc != 3) {
        return Usage();
    }
    std::ifstream in{argv[2], std::ios::binary};
    if (!in) {
        std::fprintf(stderr, "nxaot manifest: cannot read %s\n", argv[2]);
        return 1;
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::string error;
    const auto m = nxaot::module::ParseManifest(text, &error);
    if (!m) {
        std::printf("[RED] nxaot manifest: %s: %s\n", argv[2], error.c_str());
        return 1;
    }
    std::fputs(nxaot::module::WriteManifest(*m).c_str(), stdout);
    return 0;
}

int Module(int argc, char** argv) {
    if (argc != 3) {
        return Usage();
    }
    const auto m = Load(argv[2]);
    if (!m) {
        return 1;
    }
    size_t funcs = 0, lsdas = 0, sites = 0, pads = 0, in_text = 0;
    for (const auto& s : m->symbols) {
        funcs += s.type == 2 && s.Defined();
    }
    for (const auto& f : m->fdes) {
        lsdas += f.has_lsda;
        sites += f.call_sites.size();
        for (const auto& cs : f.call_sites) {
            pads += cs.landing_pad != 0;
        }
    }
    size_t rela = 0, relr = 0, jmprel = 0;
    for (const auto& r : m->relocs) {
        rela += r.table == nxaot::module::RelocTable::Rela;
        relr += r.table == nxaot::module::RelocTable::Relr;
        jmprel += r.table == nxaot::module::RelocTable::Jmprel;
        in_text += r.has_target && m->InText(r.target);
    }
    std::printf("module\t%s\t%s\t%s\n", m->name.c_str(), m->nro ? "nro" : "image", m->Sha256Hex().c_str());
    std::printf("layout\ttext\t%s\t%s\tro\t%s\t%s\tdata\t%s\t%s\tbss\t%s\n", X(m->text_off).c_str(), X(m->text_size).c_str(), X(m->ro_off).c_str(),
                X(m->ro_size).c_str(), X(m->data_off).c_str(), X(m->data_size).c_str(), X(m->bss_size).c_str());
    std::printf("mod0\t%s\tdynamic\t%s\tbss\t%s\t%s\teh_frame_hdr\t%s\t%s\n", X(m->mod0).c_str(), X(m->dynamic).c_str(), X(m->bss_start).c_str(),
                X(m->bss_end).c_str(), X(m->eh_frame_hdr).c_str(), X(m->eh_frame_hdr_end).c_str());
    std::printf("symbols\t%zu\tfunctions\t%zu\n", m->symbols.size(), funcs);
    std::printf("dynamic\tinit\t%s\tfini\t%s\tinit_array\t%zu\tfini_array\t%zu\tpreinit_array\t%zu\n", X(m->init).c_str(), X(m->fini).c_str(),
                m->init_array.size(), m->fini_array.size(), m->preinit_array.size());
    std::printf("relocations\trela\t%zu\tjmprel\t%zu\trelr\t%zu\ttargets_in_text\t%zu\n", rela, jmprel, relr, in_text);
    std::printf("fdes\t%zu\tlsda\t%zu\tcall_sites\t%zu\tlanding_pads\t%zu\n", m->fdes.size(), lsdas, sites, pads);
    for (const auto& b : m->data_blocks) {
        std::printf("data\t%s\t%s\t%s\n", X(b.lo).c_str(), X(b.hi).c_str(), b.why.c_str());
    }
    for (const auto& w : m->warnings) {
        std::printf("%s\n", w.c_str());
    }
    return 0;
}

int Census(int argc, char** argv) {
    std::string path;
    uint64_t base = 0;
    bool have_base = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--base" && i + 1 < argc) {
            char* end = nullptr;
            base = std::strtoull(argv[++i], &end, 16);
            if (!end || *end) {
                std::fprintf(stderr, "nxaot census: bad --base %s\n", argv[i]);
                return 2;
            }
            have_base = true;
        } else if (path.empty() && !a.empty() && a[0] != '-') {
            path = a;
        } else {
            return Usage();
        }
    }
    if (path.empty() || !have_base) {
        return Usage();
    }
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        std::fprintf(stderr, "nxaot census: cannot read %s\n", path.c_str());
        return 1;
    }
    const std::vector<uint8_t> data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const auto counts = nxaot::analysis::Census(data.data(), data.size(), base);
    for (const auto& [key, n] : counts) {
        std::printf("%s\t%llu\n", key.c_str(), static_cast<unsigned long long>(n));
    }
    return 0;
}

int Opcodes(int argc, char** argv) {
    using Dynarmic::IR::Opcode;
    bool reach = false, check = false;
    size_t samples = 256;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--reach") {
            reach = true;
        } else if (a == "--check") {
            check = true;
        } else if (a == "--samples" && i + 1 < argc) {
            samples = std::strtoull(argv[++i], nullptr, 10);
        } else {
            return Usage();
        }
    }
    size_t per_group[4] = {};
    if (!reach) {
        for (size_t i = 0; i < Dynarmic::IR::OpcodeCount; ++i) {
            const auto op = static_cast<Opcode>(i);
            const auto group = nxaot::lift::GroupOf(op);
            ++per_group[static_cast<size_t>(group)];
            std::printf("%s\t%s\n", nxaot::lift::GroupName(group), nxaot::lift::OpcodeName(op));
        }
        std::printf("# %zu opcodes: %zu lowered, %zu fp_simd, %zu a32, %zu other\n", Dynarmic::IR::OpcodeCount, per_group[0], per_group[1], per_group[2],
                    per_group[3]);
        return 0;
    }
    // Every opcode with the A64 decoder entries that emitted it (at most four named), group<TAB>opcode<TAB>blocks<TAB>entries
    const auto r = nxaot::analysis::Reach(samples, 0x6E78616F74726561ull);
    size_t reached[4] = {}, not_lowered = 0;
    std::string missing;
    for (size_t i = 0; i < Dynarmic::IR::OpcodeCount; ++i) {
        const auto op = static_cast<Opcode>(i);
        const auto group = nxaot::lift::GroupOf(op);
        ++per_group[static_cast<size_t>(group)];
        const auto it = r.opcodes.find(op);
        if (it == r.opcodes.end()) {
            std::printf("%s\t%s\t0\t-\n", nxaot::lift::GroupName(group), nxaot::lift::OpcodeName(op));
            continue;
        }
        ++reached[static_cast<size_t>(group)];
        std::string names;
        for (size_t k = 0; k < it->second.matchers.size() && k < 4; ++k) {
            names += (k ? "," : "") + it->second.matchers[k];
        }
        if (it->second.matchers.size() > 4) {
            names += ",+" + std::to_string(it->second.matchers.size() - 4);
        }
        std::printf("%s\t%s\t%zu\t%s\n", nxaot::lift::GroupName(group), nxaot::lift::OpcodeName(op), it->second.blocks, names.c_str());
        if (group != nxaot::lift::OpcodeGroup::Lowered) {
            ++not_lowered;
            missing += std::string(" ") + nxaot::lift::OpcodeName(op);
        }
    }
    std::printf("# reach: %zu decoder entries, %zu words translated (%zu samples taken by a more specific entry); %zu opcodes emitted by the A64 "
                "frontend: %zu lowered, %zu fp_simd, %zu a32, %zu other\n",
                r.patterns, r.words, r.skipped, r.opcodes.size(), reached[0], reached[1], reached[2], reached[3]);
    if (check && not_lowered) {
        std::printf("[RED] nxaot opcodes --reach: %zu opcodes the A64 frontend emits are not lowered:%s\n", not_lowered, missing.c_str());
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return Usage();
    }
    const std::string cmd = argv[1];
    if (cmd == "discover") {
        return Discover(argc, argv);
    }
    if (cmd == "image") {
        return Image(argc, argv);
    }
    if (cmd == "manifest") {
        return Manifest(argc, argv);
    }
    if (cmd == "module") {
        return Module(argc, argv);
    }
    if (cmd == "census") {
        return Census(argc, argv);
    }
    if (cmd == "opcodes") {
        return Opcodes(argc, argv);
    }
    if (cmd == "version") {
        std::printf("llvm\t%s\ndynarmic\t%s\n", NXAOT_LOCK_LLVM, NXAOT_LOCK_DYNARMIC);
        return 0;
    }
    return Usage();
}
