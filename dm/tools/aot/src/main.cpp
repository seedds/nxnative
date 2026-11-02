// nxaot: the ahead-of-time ARM64 -> x86-64 recompiler (docs/design/aot-recompiler.md). Subcommands today:
//   nxaot discover <module.nro | name.aot.tsv> [--symbols-as-oracle] [--tsv <out>] [--threads N] [--times] [--misses <file>] [--no-root <hex>]...
//                  [--memory]
//                                                     function discovery, CFGs, jump tables, exclusive windows and summaries of a module
//                                                     (src/analysis/discover.h): one line per function and the summary; --tsv writes every
//                                                     row kind; --symbols-as-oracle takes no symbol as a source and compares with them;
//                                                     --threads: the analysis's threads (0 or none: one per CPU; the result is the same);
//                                                     --times: the seconds of its steps on standard error; --misses and --no-root as for build;
//                                                     --memory: each table's bytes at its peak and the resident set at each stage, on standard
//                                                     error (src/analysis/memory.h; README.md, "Memory at scale")
//   nxaot plan <module.nro | name.aot.tsv> [--cc swifttail|ghc] [--all-entries] [--threads N] [--memory]
//                                                     the pack stage's convention facts (src/build/plan.h): the indirect view and per function
//                                                     PASS, WRITES, LIVE_AFTER, RV, a standard entry, its indirect sites; --memory as for discover
//   nxaot image <module.nro> <out dir> [<name>]       the behaviour-image form of an NRO: <name>.image.bin and its manifest <name>.aot.tsv
//                                                     (layout and data rows), the input form patch_nso.py --aot will write for NSOs
//   nxaot manifest <name.aot.tsv>                     parses a manifest and prints it back in canonical form (src/module/manifest.h)
//   nxaot module <module.nro | name.aot.tsv>          what the loader read: layout, MOD0, symbols, relocations, FDEs and LSDAs, data blocks
//   nxaot census <raw ARM64 code file> --base <hex>   phase 0's census of a code blob, as key<TAB>count lines (src/analysis/census.h)
//   nxaot opcodes                                     every dynarmic IR opcode with whether the lifter lowers it (group<TAB>opcode)
//   nxaot opcodes --reach [--samples N] [--check]     the opcodes the A64 frontend emits (src/analysis/reach.h), with the decoder entries that emit
//                                                     them; --check exits 1 when one of them is not lowered
//   nxaot version                                     the toolchain of nxaot.lock this build was configured with
//   nxaot build <module.nro | name.aot.tsv> [-o <object.o>] [--shared <lib.so>] [--cc swifttail|ghc] [--mode balanced|max] [--tier C|W|H|H+]
//               [--correctness] [--all-entries] [--cpu <llvm cpu>] [--fpsr-flags on|off] [--no-debug-info] [--no-line-table] [--spills] [--asm <file.s>]
//               [--no-safepoints] [--workers N] [--cache <dir>] [--time-passes] [--ir <file.ll>] [--ir-opt <file.ll>] [--profile <file>]
//               [--no-groups] [--no-split] [--split-above N] [--target linux|windows] [--isel-misses] [--misses <file>] [--no-root <hex>]...
//               [--memory] [--tiers-tsv <file>] [--gap-tier C|mode]
//                                                     the pack stage (src/build): every function of the module compiled, in units of
//                                                     neighbouring functions (a function over the split limit in parts) on --workers threads
//                                                     (0 or none: the analysis on one per CPU, then as many compile workers as the CPUs and
//                                                     the memory available after the analysis hold, logged as `aot: workers N (...)`;
//                                                     NXAOT_MEM_AVAILABLE_MIB, a test switch, stands for the available memory), or taken from
//                                                     the cache
//                                                     --cache (compiled units and the module's analysis); --profile gives each function its tier
//                                                     by the samples it holds (--gap-tier C: gap code at tier C in the balanced mode; --tiers-tsv
//                                                     writes each function's tier); with the tables' object (lookup table, function map, line table,
//                                                     descriptor: include/nxaot/aot_module.h) linked by lld into the shared object nxaot_host
//                                                     loads (--shared) and / or one relocatable object (-o); prints the module's numbers, the
//                                                     seconds of each stage and the caches' hits and misses. --target windows: a pack for the
//                                                     Windows host, a PE DLL (--shared <name>.dll) and / or one COFF object (-o): the same code,
//                                                     the host's calls in its convention, its imports bound by the host (README.md, "Windows packs").
//                                                     --misses: the host's JIT feedback for this module (dm/host/aot.h, "The JIT fallback's misses":
//                                                     DM_AOT_PROFILE's <module>.misses.tsv), roots of discovery (README.md, "JIT misses"), used when its
//                                                     image SHA-256 is the module's (else a [RED] line and it is ignored); --no-root (tests): an address
//                                                     none of discovery's own sources may make an entry, so that a miss naming it brings it back;
//                                                     --memory: the tables' peaks and the resident set at each stage (the link's included), as
//                                                     for discover. The tables' object's function map and lookup table go to the MC layer
//                                                     directly (`aot: tables' object: ... through MC`; README.md, "The tables' object through
//                                                     MC"); NXAOT_TABLES_IR, a test switch, takes them as LLVM constants, as --ir and --asm do,
//                                                     and NXAOT_TABLES_OBJECT=<file> copies the tables' object there (CTest tables)
//   nxaot lines <pack.so | pack.dll> <module.nro | name.aot.tsv>
//                                                     the line table of a pack against its code and its module (design 3.5; CTest lines): every
//                                                     entry's native address must start an x86 instruction that accesses memory, inside the
//                                                     body of its function, and its offset must name an A64 load, store or dc zva of that
//                                                     function's code (discovery's blocks); exit 1 on a mismatch
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "analysis/census.h"
#include "analysis/discover.h"
#include "analysis/memory.h"
#include "analysis/reach.h"
#include "build/pack.h"
#include "build/plan.h"
#include "dynarmic/ir/opcodes.h"
#include "lift/opcodes.h"
#include "module/image.h"
#include "module/manifest.h"
#include "nxaot_lock.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/FileSystem.h>

#include <llvm/IR/PassTimingInfo.h>
#include <llvm/MC/MCAsmInfo.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCDisassembler/MCDisassembler.h>
#include <llvm/MC/MCInst.h>
#include <llvm/MC/MCInstrInfo.h>
#include <llvm/MC/MCRegisterInfo.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/MCTargetOptions.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/COFF.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Pass.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>

#include "nxaot/aot_module.h"

namespace {

int Usage() {
    std::fprintf(stderr,
                 "usage: nxaot discover <module.nro | name.aot.tsv> [--symbols-as-oracle] [--tsv <out>] [--threads N] [--times] [--misses <file>]\n"
                 "                      [--no-root <hex>]... [--memory]\n"
                 "       nxaot plan <module.nro | name.aot.tsv> [--cc swifttail|ghc] [--all-entries] [--threads N] [--memory]\n"
                 "       nxaot image <module.nro> <out dir> [<name>]\n"
                 "       nxaot manifest <name.aot.tsv>\n"
                 "       nxaot module <module.nro | name.aot.tsv>\n"
                 "       nxaot census <raw ARM64 code file> --base <hex>\n"
                 "       nxaot opcodes [--reach [--samples N] [--check]]\n"
                 "       nxaot version\n"
                 "       nxaot build <module.nro | name.aot.tsv> [-o <object.o>] [--shared <lib.so>] [--cc swifttail|ghc] [--mode balanced|max]\n"
                 "                   [--tier C|W|H|H+] [--correctness] [--all-entries] [--cpu <llvm cpu>] [--fpsr-flags on|off] [--no-debug-info]\n"
                 "                   [--no-line-table] [--spills] [--asm <file.s>] [--no-safepoints] [--workers N] [--cache <dir>] [--time-passes]\n"
                 "                   [--ir <file.ll>] [--ir-opt <file.ll>] [--profile <file>] [--no-groups] [--no-split] [--split-above N]\n"
                 "                   [--target linux|windows]   (windows: a PE DLL --shared <name>.dll and / or a COFF object -o <object.obj>)\n"
                 "                   [--isel-misses] [--misses <file>] [--no-root <hex>]... [--memory] [--tiers-tsv <file>] [--gap-tier C|mode]\n"
                 "       nxaot lines <pack.so | pack.dll> <module.nro | name.aot.tsv>\n");
    return 2;
}

// --no-root's address (hexadecimal, 0x optional)
bool ParseHex(const std::string& s, uint64_t* v) {
    const char* p = s.c_str();
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) p += 2;
    char* end = nullptr;
    *v = std::strtoull(p, &end, 16);
    return *p && end && !*end;
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

// --misses (README.md, "JIT misses"): the host's file of this module's JIT misses, taken when its header names this image's SHA-256 (another image's, or none: a
// [RED] line, and the file is ignored), the code they prove in the image's data blocks taken out of them first (analysis::CarveMisses), as discovery, the lifter and
// the cache keys must all see it. False when the file cannot be read or a line does not parse (*error says which)
bool UseMisses(const std::string& path, nxaot::module::Image* image, std::vector<nxaot::analysis::Miss>* out, std::string* error) {
    std::string sha;
    std::vector<nxaot::analysis::Miss> rows;
    out->clear();
    if (!nxaot::build::ReadMisses(path, &rows, &sha, error)) {
        return false;
    }
    const std::string own = llvm::StringRef(image->Sha256Hex()).lower();
    if (sha != own) {
        std::printf("[RED] nxaot: the misses %s were recorded on %s (image SHA-256 %s), not on this module's image (%s): ignored\n", path.c_str(),
                    sha.empty() ? "an image the file does not name" : "another image", sha.empty() ? "none" : sha.c_str(), own.c_str());
        return true;
    }
    const auto runs = nxaot::analysis::CarveMisses(*image, rows);
    uint64_t words = 0;
    for (const auto& [lo, hi] : runs) words += (hi - lo) / 4;
    if (!runs.empty()) {
        std::printf("aot: misses %s: %llu words of data blocks are code the game ran there (%zu runs from %s): decoded as code\n", path.c_str(),
                    static_cast<unsigned long long>(words), runs.size(), X(runs.front().first).c_str());
    }
    *out = std::move(rows);
    return true;
}

int Discover(int argc, char** argv) {
    using namespace nxaot::analysis;
    std::string path, tsv, misses;
    DiscoverOptions opts;
    bool times = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        uint64_t held = 0;
        if (a == "--symbols-as-oracle") {
            opts.symbols = false;
        } else if (a == "--tsv" && i + 1 < argc) {
            tsv = argv[++i];
        } else if (a == "--threads" && i + 1 < argc) {
            opts.threads = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "--times") {
            times = true;
        } else if (a == "--misses" && i + 1 < argc) {
            misses = argv[++i];
        } else if (a == "--no-root" && i + 1 < argc && ParseHex(argv[i + 1], &held)) {
            opts.no_roots.insert(held);
            ++i;
        } else if (a == "--memory") {
            nxaot::memory::Registry::Get().Enable();
        } else if (path.empty() && !a.empty() && a[0] != '-') {
            path = a;
        } else {
            return Usage();
        }
    }
    if (path.empty()) {
        return Usage();
    }
    auto image = Load(path);
    nxaot::memory::Rss("the module loaded");
    if (!image) {
        return 1;
    }
    if (!misses.empty()) {
        std::string error;
        if (!UseMisses(misses, &*image, &opts.misses, &error)) {
            std::fprintf(stderr, "nxaot discover: %s\n", error.c_str());
            return 1;
        }
    }
    const Discovery d = Discover(*image, opts);
    if (times) {
        std::fprintf(stderr, "nxaot discover: %s\n", TimesLine(d.times).c_str());
    }
    nxaot::memory::Rss("after discovery");
    nxaot::memory::Registry::Get().Print(stderr);
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
    for (const auto& m : d.misses) {  // --misses: what became of each address (entry, std, resume, known, unserved and why)
        rows.push_back("miss\t" + X(m.offset) + "\t" + m.reasons + "\t" + m.result + (m.why.empty() ? "" : "\t" + m.why));
    }
    std::string prov_line = "summary\tprovenance";
    for (unsigned b = 0; b < kProvCount; ++b) {
        if ((1u << b) == kProvMiss && !any[b]) continue;  // only when misses made an entry (the line is as before without them)
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

// The convention facts of the pack stage (src/build/plan.h) as rows: the indirect view (what an indirect call passes, may find written, returns)
// and per function PASS, WRITES, LIVE_AFTER, RV, whether it has a standard entry and its indirect sites (blr, unbounded br): CTest plan
int PlanCmd(int argc, char** argv) {
    std::string path;
    nxaot::lift::Convention conv;
    bool all_entries = false;
    unsigned threads = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--threads" && i + 1 < argc) {
            threads = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "--cc" && i + 1 < argc) {
            const std::string v = argv[++i];
            if (v == "swifttail") conv.cc = nxaot::lift::CallConv::SwiftTail;
            else if (v == "ghc") conv.cc = nxaot::lift::CallConv::Ghc;
            else return Usage();
        } else if (a == "--all-entries") {
            all_entries = true;
        } else if (a == "--memory") {
            nxaot::memory::Registry::Get().Enable();
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
    nxaot::memory::Rss("the module loaded");
    nxaot::analysis::DiscoverOptions dopts;
    dopts.threads = threads;
    const nxaot::analysis::Discovery d = nxaot::analysis::Discover(*image, dopts);
    const nxaot::build::Plan plan = nxaot::build::MakePlan(*image, d, conv, all_entries, threads);
    const auto str = [](const nxaot::lift::Regs& r) { return nxaot::analysis::RegSet{r.x, r.v, r.sp, r.nzcv}.Str(); };
    std::printf("# nxaot plan\t%s\t%s\n", image->name.c_str(), image->Sha256Hex().c_str());
    std::printf("indirect\tpass\t%s\twrites\t%s\trv\t%u\n", str(plan.indirect_pass).c_str(), str(plan.indirect_writes).c_str(), plan.indirect_rv);
    for (const auto& [entry, ff] : plan.fns) {
        const auto& f = d.functions.at(entry);
        size_t indirect = f.indirect.size();
        for (const auto& [pc, target] : f.calls) indirect += target == 0;
        std::printf("fn\t%s\tpass\t%s\twrites\t%s\tlive_after\t%s\trv\t%u\tstd\t%d\tindirect\t%zu\n", X(entry).c_str(), str(ff.pass).c_str(),
                    str(ff.writes).c_str(), str(ff.live_after).c_str(), ff.rv, ff.host_callable ? 1 : 0, indirect);
    }
    nxaot::memory::Registry::Get().Print(stderr);
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

int Build(int argc, char** argv) {
    using nxaot::build::Tier;
    std::string path, object, shared, misses;
    nxaot::build::BuildOptions o;
    bool tier_set = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has = i + 1 < argc;
        uint64_t held = 0;
        if (a == "-o" && has) {
            object = argv[++i];
        } else if (a == "--shared" && has) {
            shared = argv[++i];
        } else if (a == "--cc" && has) {
            const std::string v = argv[++i];
            if (v == "swifttail") o.cc = nxaot::lift::CallConv::SwiftTail;
            else if (v == "ghc" || v == "preserve_none") o.cc = nxaot::lift::CallConv::Ghc;
            else return Usage();
        } else if (a == "--mode" && has) {
            o.mode = argv[++i];
            if (o.mode != "balanced" && o.mode != "max") return Usage();
        } else if (a == "--tier" && has) {
            const std::string v = argv[++i];
            tier_set = true;
            if (v == "C") o.tier = Tier::C;
            else if (v == "W") o.tier = Tier::W;
            else if (v == "H") o.tier = Tier::H;
            else if (v == "H+") o.tier = Tier::HPlus;
            else return Usage();
        } else if (a == "--correctness") {
            o.correctness = true;
        } else if (a == "--all-entries") {
            o.all_entries = true;
        } else if (a == "--cpu" && has) {
            o.cpu = argv[++i];
        } else if (a == "--fpsr-flags" && has) {
            const std::string v = argv[++i];
            o.fpsr_flags = v == "on" ? 1 : v == "off" ? 0 : -1;
        } else if (a == "--no-debug-info") {
            o.debug_info = false;
        } else if (a == "--no-line-table") {
            o.line_table = false;
        } else if (a == "--no-safepoints") {
            o.safepoints = false;  // no polls (section 6.2): the poll cost measurement
        } else if (a == "--time-passes") {
            llvm::TimePassesIsEnabled = true;  // LLVM's pass timers, printed to stderr at the end (one worker)
        } else if (a == "--ir" && has) {
            o.ir_path = argv[++i];
        } else if (a == "--ir-opt" && has) {
            o.ir_path = argv[++i];
            o.ir_after_passes = true;
        } else if (a == "--spills") {
            o.count_spills = true;
        } else if (a == "--asm" && has) {
            o.asm_path = argv[++i];
            o.count_spills = true;
        } else if (a == "--memory") {
            nxaot::memory::Registry::Get().Enable();
        } else if (a == "--workers" && has) {
            o.workers = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "--cache" && has) {
            o.cache_dir = argv[++i];
        } else if (a == "--no-groups") {
            o.groups = false;  // one function per unit at tiers C and W, as phase 5a compiled them (the measurement of the unit's fixed cost)
        } else if (a == "--no-split") {
            o.split = false;  // keep every function whole (the measurement of design 9.3's splitting)
        } else if (a == "--split-above" && has) {
            o.split_above = std::max<uint64_t>(8, std::strtoull(argv[++i], nullptr, 10));  // CTest split: most of the corpus in parts
        } else if (a == "--profile" && has) {
            o.profile_path = argv[++i];
        } else if (a == "--misses" && has) {
            misses = argv[++i];  // the host's JIT feedback (README.md, "JIT misses")
        } else if (a == "--no-root" && has && ParseHex(argv[i + 1], &held)) {
            o.no_roots.insert(held);  // tests: discovery's own sources do not make it an entry
            ++i;
        } else if (a == "--tiers-tsv" && has) {
            o.tiers_path = argv[++i];  // each function's tier, gap code marked (README.md, "Gap code at tier C")
        } else if (a == "--gap-tier" && has) {
            const std::string v = argv[++i];  // gap code at tier C in the balanced mode, or at the mode's tier (the default: README.md, "Gap code at tier C")
            if (v == "C") o.gap_tier_c = true;
            else if (v == "mode") o.gap_tier_c = false;
            else return Usage();
        } else if (a == "--isel-misses") {
            o.isel_misses = true;  // FastISel's misses by kind (tier C's instruction selection; README.md, "Phase 5d")
        } else if (a == "--target" && has) {
            const std::string v = argv[++i];
            if (v == "windows") o.windows = true;  // a PE DLL for the Windows host (README.md, "Windows packs")
            else if (v == "linux") o.windows = false;
            else return Usage();
        } else if (path.empty() && !a.empty() && a[0] != '-') {
            path = a;
        } else {
            return Usage();
        }
    }
    if (path.empty() || (object.empty() && shared.empty()) || (tier_set && !o.profile_path.empty())) {
        return Usage();  // --tier sets every function's tier, --profile each function's: one or the other
    }
    if (!o.windows && shared.size() >= 4 && shared.compare(shared.size() - 4, 4, ".dll") == 0) {
        std::fprintf(stderr, "nxaot build: %s is a DLL's name: a pack for the Windows host needs --target windows (README.md, \"Windows packs\")\n",
                     shared.c_str());
        return 2;
    }
    if (!tier_set) {
        o.tier = o.mode == "max" ? Tier::HPlus : Tier::W;  // no profile: balanced is tier W everywhere, max tier H+ (section 9.3)
    }
    const auto t_start = std::chrono::steady_clock::now();
    auto image = Load(path);
    if (!image) {
        return 1;
    }
    nxaot::memory::Rss("the module loaded");
    nxaot::build::BuildStats s;
    std::string error;
    if (!misses.empty() && !UseMisses(misses, &*image, &o.misses, &error)) {
        std::printf("[RED] nxaot build: %s: %s\n", path.c_str(), error.c_str());
        return 1;
    }
    // The objects that are not the cache's (the tables' object, and every unit without a cache) go to a folder beside the output, removed again
    const std::string work = (shared.empty() ? object : shared) + ".parts";
    std::vector<std::string> objects;
    bool ok = nxaot::build::BuildObjects(*image, o, work, &objects, &s, &error);
    const auto t_link = std::chrono::steady_clock::now();
    if (ok && o.windows) {
        // The DLL and / or the COFF object: the objects joined, converted and linked by lld-link (README.md, "Windows packs")
        ok = nxaot::build::LinkWindows(objects, work, shared, object, o.debug_info, &s, &error);
    } else {
        if (ok && !shared.empty()) {
            const std::string soname = shared.substr(shared.find_last_of('/') + 1);
            ok = nxaot::build::LinkShared(objects, shared, soname, &error);
        }
        if (ok && !object.empty()) {
            ok = nxaot::build::LinkRelocatable(objects, object, &error);  // one relocatable object of the whole module, as the shared object has it
        }
    }
    s.t_link = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_link).count();
    // The process's peak includes the link (lld runs in this process: its inputs and its output buffer): README.md, "Memory at scale"
    s.peak_rss_kib = std::max<uint64_t>(s.peak_rss_kib, nxaot::memory::RssPeak() >> 10);
    nxaot::memory::Rss("after the link");
    llvm::sys::fs::remove_directories(work);
    if (!ok) {
        std::printf("[RED] nxaot build: %s: %s\n", path.c_str(), error.c_str());
        return 1;
    }
    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    for (const auto& n : s.notes) {
        std::printf("%s\n", n.c_str());
    }
    const double analyse = s.t_discover + s.t_plan;
    const double compile = s.t_opt + s.t_codegen;
    const auto per_s = [&](double t) { return t > 0 ? static_cast<double>(s.words) / t : 0.0; };
    std::printf("aot: %s %llu instructions %llu functions\n", s.module.c_str(), static_cast<unsigned long long>(s.words),
                static_cast<unsigned long long>(s.functions));
    std::printf("aot: tier %s (mode %s), convention %s%s, cpu %s, fpsr flags %s\n", nxaot::build::TierName(o.tier), o.mode.c_str(),
                nxaot::lift::CallConvName(o.cc), o.correctness ? ", correctness mode" : "", s.cpu.c_str(), s.fpsr_flags ? "modelled" : "not modelled");
    std::printf("aot: tiers C %llu W %llu H %llu H+ %llu (functions)\n", static_cast<unsigned long long>(s.tier_functions[0]),
                static_cast<unsigned long long>(s.tier_functions[1]), static_cast<unsigned long long>(s.tier_functions[2]),
                static_cast<unsigned long long>(s.tier_functions[3]));
    // Lifting, verification, optimisation and code generation are CPU seconds summed over the units; the rest and "compile wall" wall clock
    std::printf("aot: seconds discover %.3f plan %.3f lift %.3f verify %.3f optimise %.3f codegen %.3f link %.3f; compile wall %.3f with %llu "
                "workers; total %.3f; peak memory %llu MiB\n",
                s.t_discover, s.t_plan, s.t_lift, s.t_verify, s.t_opt, s.t_codegen, s.t_link, s.t_compile_wall,
                static_cast<unsigned long long>(s.workers), total, static_cast<unsigned long long>(s.peak_rss_kib / 1024));
    std::printf("aot: instructions per second: analysis %.0f, lifting %.0f, tier %s (optimise and codegen) %.0f per core; compile %.0f, whole "
                "pack %.0f wall\n",
                per_s(analyse), per_s(s.t_lift), nxaot::build::TierName(o.tier), per_s(compile), per_s(s.t_compile_wall), per_s(total));
    std::printf("aot: units %llu (groups of neighbouring functions at tiers C and W, shards at H and H+, a unit per part of a split function), "
                "workers %llu\n",
                static_cast<unsigned long long>(s.units), static_cast<unsigned long long>(s.workers));
    std::printf("aot: split functions %llu into %llu parts, %llu transfers between parts, joined in %.3f s (design 9.3)\n",
                static_cast<unsigned long long>(s.split_functions), static_cast<unsigned long long>(s.split_parts),
                static_cast<unsigned long long>(s.splits), s.t_join);
    if (!o.profile_path.empty()) {
        std::printf("aot: profile %s: %llu samples at %llu offsets, %llu outside every function\n", o.profile_path.c_str(),
                    static_cast<unsigned long long>(s.profile_samples), static_cast<unsigned long long>(s.profile_offsets),
                    static_cast<unsigned long long>(s.profile_outside));
        // The samples' weights at tiers H and H+ (README.md, "The sample profile at tier H"); off with the test switch NXAOT_PROFILE_WEIGHTS=0
        if (s.profile_weights) {
            std::printf("aot: profile weights: %llu functions, %llu branches weighted, %llu entry counts, %llu call counts; %llu functions "
                        "sampled at their entry only, not weighed; %llu with samples profi found no flow for, not weighed\n",
                        static_cast<unsigned long long>(s.weighted_functions), static_cast<unsigned long long>(s.weighted_branches),
                        static_cast<unsigned long long>(s.entry_counts), static_cast<unsigned long long>(s.weighted_calls),
                        static_cast<unsigned long long>(s.entry_only_functions), static_cast<unsigned long long>(s.no_flow_functions));
        } else {
            std::printf("aot: profile weights: off (NXAOT_PROFILE_WEIGHTS=0): the profile chooses the tiers only\n");
        }
    }
    if (!o.cache_dir.empty()) {
        std::printf("aot: cache %s: %llu hits, %llu misses (units; functions %llu hit, %llu missed), %llu bytes read, %llu bytes written, keys "
                    "%.3f s\n",
                    o.cache_dir.c_str(), static_cast<unsigned long long>(s.cache_hits), static_cast<unsigned long long>(s.cache_misses),
                    static_cast<unsigned long long>(s.cache_hit_functions), static_cast<unsigned long long>(s.cache_miss_functions),
                    static_cast<unsigned long long>(s.cache_bytes_read), static_cast<unsigned long long>(s.cache_bytes_written), s.t_key);
        std::printf("aot: analysis cache: %s, %.3f s\n", s.analysis_cache.c_str(), s.t_analysis_cache);
    }
    const auto& dt = s.discover_times;
    std::printf("aot: discover seconds: classify %.3f walk %.3f tables %.3f (%llu tries, %llu skipped, %llu regions, %llu reused) summaries %.3f "
                "(%u runs, %u skipped, %llu rounds, %llu computed, %llu reused, %llu words, %llu translations) sources %.3f windows %.3f finish %.3f "
                "census %.3f\n",
                dt.classify, dt.walk, dt.tables, static_cast<unsigned long long>(dt.table_tries),
                static_cast<unsigned long long>(dt.table_tries_skipped), static_cast<unsigned long long>(dt.regions),
                static_cast<unsigned long long>(dt.regions_reused),
                dt.summaries, dt.summarise_runs, dt.summarise_skipped, static_cast<unsigned long long>(dt.summary_rounds),
                static_cast<unsigned long long>(dt.summary_computes), static_cast<unsigned long long>(dt.summary_reused),
                static_cast<unsigned long long>(dt.distinct_words),
                static_cast<unsigned long long>(dt.translations), dt.sources, dt.windows, dt.finish, s.t_census);
    std::printf("aot: sites: calls %llu, indirect calls %llu, tail calls %llu, indirect tail calls %llu, returns %llu, svc %llu, traps %llu, "
                "jump tables %llu, resume points %llu\n",
                static_cast<unsigned long long>(s.calls), static_cast<unsigned long long>(s.indirect_calls),
                static_cast<unsigned long long>(s.tail_calls), static_cast<unsigned long long>(s.indirect_tails),
                static_cast<unsigned long long>(s.returns), static_cast<unsigned long long>(s.svcs), static_cast<unsigned long long>(s.traps),
                static_cast<unsigned long long>(s.tables), static_cast<unsigned long long>(s.resume_sites));
    std::printf("aot: standard entries %llu, lookup slots %llu, refused %llu, blocks %llu, text %llu bytes, object %llu bytes, line entries lost %llu",
                static_cast<unsigned long long>(s.standard_entries), static_cast<unsigned long long>(s.lookup_slots),
                static_cast<unsigned long long>(s.refused), static_cast<unsigned long long>(s.blocks), static_cast<unsigned long long>(s.text_bytes),
                static_cast<unsigned long long>(s.object_bytes), static_cast<unsigned long long>(s.lines_lost));
    if (o.count_spills) {
        std::printf(", spills %llu, reloads %llu", static_cast<unsigned long long>(s.spills), static_cast<unsigned long long>(s.reloads));
    }
    std::printf("\n");
    std::printf("aot: line table %llu entries; gate sites %llu (design 1.1)\n", static_cast<unsigned long long>(s.line_entries),
                static_cast<unsigned long long>(s.gate_sites));
    if (o.isel_misses) {
        // Each miss sends work to SelectionDAG: a call alone, else the block up to the instruction, or a function's arguments; by the IR
        // instructions it sent there
        uint64_t total = 0, to_dag = 0;
        for (const auto& [kind, n] : s.isel_misses) {
            total += n.first;
            to_dag += n.second;
        }
        std::printf("aot: fastisel misses %llu, IR instructions to SelectionDAG %llu\n", static_cast<unsigned long long>(total),
                    static_cast<unsigned long long>(to_dag));
        std::vector<std::tuple<uint64_t, uint64_t, std::string>> rows;
        for (const auto& [kind, n] : s.isel_misses) rows.emplace_back(n.second, n.first, kind);
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a > b; });
        for (const auto& [w, n, kind] : rows) {
            std::printf("aot: fastisel miss %8llu %9llu  %s\n", static_cast<unsigned long long>(n), static_cast<unsigned long long>(w), kind.c_str());
        }
    }
    nxaot::memory::Registry::Get().Print(stdout);
    if (o.windows) {
        std::string list;
        for (const auto& i : s.imports) list += " " + i;
        std::printf("aot: windows: %zu imports bound by the host (nxaot_imports):%s; COFF object %llu sections, %llu symbol records, %llu "
                    "relocations\n",
                    s.imports.size(), list.c_str(), static_cast<unsigned long long>(s.coff_sections), static_cast<unsigned long long>(s.coff_symbols),
                    static_cast<unsigned long long>(s.coff_relocations));
    }
    return s.refused ? 1 : 0;
}

// The line table of a pack (design 3.5, include/nxaot/aot_module.h) against the code it describes and the module it was compiled from (CTest
// lines): read from the shared object's sections (.nxaot.fnmap, .nxaot.fnlines, .nxaot.lines: image-relative, so the file has them resolved), or
// from a Windows pack's DLL through its exported descriptor (CTest pack_windows), every
// entry's native address decoded by LLVM's x86 disassembler from the start of its function's body (it must start an instruction), and its guest
// offset checked against the module: an A64 load or store (the encoding group op0 = x1x0) or dc zva, in a block of the function the body compiles
// (discovery), or of another function (inlined into it at tiers H and H+, counted apart). The entries of one guest instruction in one body are
// every machine instruction LLVM made from its marked access (the selection DAG gives the IR instruction's !pcsections to every node it lowers it
// to: a cmpxchg's lock cmpxchg and the setcc of its success, an address's and, ...): the accesses none of whose entries loads or stores are counted
// and listed as notes (LLVM moved the mark off the memory instruction), not as errors of the table
int Lines(int argc, char** argv) {
    if (argc != 4) {
        return Usage();
    }
    const std::string pack = argv[2];
    const auto image = Load(argv[3]);
    if (!image) {
        return 1;
    }
    auto bin = llvm::object::ObjectFile::createObjectFile(pack);
    if (!bin) {
        std::printf("[RED] nxaot lines: %s: %s\n", pack.c_str(), llvm::toString(bin.takeError()).c_str());
        return 1;
    }
    const llvm::object::ObjectFile& obj = *bin->getBinary();
    struct Sec {
        uint64_t addr = 0;
        llvm::StringRef data;
        bool found = false;
    };
    std::map<std::string, Sec> secs;
    std::vector<Sec> code;
    for (const llvm::object::SectionRef& s : obj.sections()) {
        auto name = s.getName();
        auto data = s.getContents();
        if (!name || !data) {
            if (!name) llvm::consumeError(name.takeError());
            if (!data) llvm::consumeError(data.takeError());
            continue;
        }
        const Sec x{s.getAddress(), *data, true};
        secs[name->str()] = x;
        if (s.isText()) code.push_back(x);
    }
    Sec fnmap = secs[".nxaot.fnmap"], fnlines = secs[".nxaot.fnlines"], lines = secs[".nxaot.lines"];
    if (const auto* pe = llvm::dyn_cast<llvm::object::COFFObjectFile>(&obj)) {
        // A Windows pack (--target windows): its tables lie in .rdata, found through the exported descriptor, whose pointers the file holds
        // relocated for the image's preferred base (as the loader maps it there): each table's virtual address, its bytes in the file
        uint32_t rva = 0;
        bool exported = false;
        for (const llvm::object::ExportDirectoryEntryRef& e : pe->export_directories()) {
            llvm::StringRef name;
            if (llvm::Error err = e.getSymbolName(name)) {
                llvm::consumeError(std::move(err));
                continue;
            }
            if (name == "nxaot_module") {
                if (llvm::Error err = e.getExportRVA(rva)) {
                    llvm::consumeError(std::move(err));
                } else {
                    exported = true;
                }
            }
        }
        const uint64_t base = pe->getImageBase();
        const auto bytes_at = [&](uint64_t va, uint64_t size, Sec* out) {
            uintptr_t p = 0;
            if (va < base || va - base > UINT32_MAX) return false;
            if (llvm::Error err = pe->getRvaPtr(static_cast<uint32_t>(va - base), p)) {
                llvm::consumeError(std::move(err));
                return false;
            }
            const llvm::StringRef file = pe->getData();
            const char* at = reinterpret_cast<const char*>(p);
            if (at < file.begin() || size > static_cast<uint64_t>(file.end() - at)) return false;
            *out = Sec{va, llvm::StringRef(at, size), true};
            return true;
        };
        Sec desc_bytes;
        nxaot_module_desc d{};
        if (!exported || !bytes_at(base + rva, sizeof d, &desc_bytes)) {
            std::printf("[RED] nxaot lines: %s exports no nxaot_module descriptor\n", pack.c_str());
            return 1;
        }
        std::memcpy(&d, desc_bytes.data.data(), sizeof d);
        uint64_t total = 0;
        if (d.abi != NXAOT_MODULE_ABI || !bytes_at(reinterpret_cast<uint64_t>(d.fns), d.fn_count * sizeof(nxaot_fn_entry), &fnmap) ||
            (d.fn_lines && !bytes_at(reinterpret_cast<uint64_t>(d.fn_lines), d.fn_count * sizeof(nxaot_fn_lines), &fnlines))) {
            std::printf("[RED] nxaot lines: %s: its descriptor (ABI %u) names no function map or line table in the file\n", pack.c_str(), d.abi);
            return 1;
        }
        for (uint64_t i = 0; d.fn_lines && i < d.fn_count; ++i) {
            nxaot_fn_lines fl;
            std::memcpy(&fl, fnlines.data.data() + i * sizeof fl, sizeof fl);
            total = std::max<uint64_t>(total, static_cast<uint64_t>(fl.first) + fl.count);
        }
        if (d.lines && !bytes_at(reinterpret_cast<uint64_t>(d.lines), total * sizeof(nxaot_line_entry), &lines)) {
            std::printf("[RED] nxaot lines: %s: its line table lies outside the file\n", pack.c_str());
            return 1;
        }
    }
    if (!fnmap.found || !fnlines.found || !lines.found) {
        std::printf("[RED] nxaot lines: %s has no line table (.nxaot.fnmap, .nxaot.fnlines and .nxaot.lines, or a DLL's descriptor)\n", pack.c_str());
        return 1;
    }
    const size_t nfn = fnmap.data.size() / sizeof(nxaot_fn_entry), nlines = lines.data.size() / sizeof(nxaot_line_entry);
    if (fnlines.data.size() != nfn * sizeof(nxaot_fn_lines)) {
        std::printf("[RED] nxaot lines: %s: %zu functions in the map, %zu runs of lines\n", pack.c_str(), nfn, fnlines.data.size() / sizeof(nxaot_fn_lines));
        return 1;
    }
    const nxaot::analysis::Discovery d = nxaot::analysis::Discover(*image);
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetDisassembler();
    const std::string triple = llvm::sys::getProcessTriple();
    std::string err;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, err);
    if (!target) {
        std::printf("[RED] nxaot lines: %s\n", err.c_str());
        return 1;
    }
    std::unique_ptr<llvm::MCRegisterInfo> mri(target->createMCRegInfo(triple));
    llvm::MCTargetOptions mopts;
    std::unique_ptr<llvm::MCAsmInfo> mai(target->createMCAsmInfo(*mri, triple, mopts));
    std::unique_ptr<llvm::MCSubtargetInfo> sti(target->createMCSubtargetInfo(triple, llvm::sys::getHostCPUName(), ""));
    std::unique_ptr<llvm::MCInstrInfo> mii(target->createMCInstrInfo());
    llvm::MCContext mctx(llvm::Triple(triple), mai.get(), mri.get(), sti.get());
    std::unique_ptr<llvm::MCDisassembler> dis(target->createMCDisassembler(*sti, mctx));
    if (!dis) {
        std::printf("[RED] nxaot lines: no x86 disassembler in this LLVM\n");
        return 1;
    }
    const auto code_at = [&](uint64_t a) -> llvm::ArrayRef<uint8_t> {
        for (const Sec& s : code) {
            if (a >= s.addr && a - s.addr < s.data.size()) {
                return llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t*>(s.data.data()) + (a - s.addr), s.data.size() - (a - s.addr));
            }
        }
        return {};
    };
    const auto in_blocks = [](const nxaot::analysis::Function& f, uint64_t off) {
        auto it = f.blocks.upper_bound(off);
        if (it == f.blocks.begin()) return false;
        --it;
        return off >= it->second.start && off < it->second.end;
    };
    uint64_t checked = 0, bad = 0, bad_entries = 0, inlined = 0, with_lines = 0, accesses = 0, memory = 0;
    const auto report = [&](const std::string& what) {
        if (++bad <= 20) std::printf("[RED] nxaot lines: %s\n", what.c_str());
        bad_entries += what.rfind("entry ", 0) == 0;
    };
    // Whether the instruction at an address loads or stores: a prefix the disassembler gives as an instruction of its own (lock) is part of the next
    const auto accesses_memory = [&](uint64_t a, const llvm::MCInst& first, uint64_t first_size) {
        llvm::MCInst inst = first;
        uint64_t size = first_size;
        for (int k = 0; k < 4; ++k) {
            const llvm::MCInstrDesc& desc = mii->get(inst.getOpcode());
            if (desc.mayLoad() || desc.mayStore()) return true;
            if (!llvm::StringRef(mii->getName(inst.getOpcode())).ends_with("_PREFIX")) return false;
            a += size;
            const llvm::ArrayRef<uint8_t> bytes = code_at(a);
            if (bytes.empty() || dis->getInstruction(inst, size, bytes, a, llvm::nulls()) != llvm::MCDisassembler::Success) return false;
        }
        return false;
    };
    for (size_t i = 0; i < nfn; ++i) {
        nxaot_fn_entry fe;
        nxaot_fn_lines fl;
        std::memcpy(&fe, fnmap.data.data() + i * sizeof fe, sizeof fe);
        std::memcpy(&fl, fnlines.data.data() + i * sizeof fl, sizeof fl);
        const uint64_t body = fnmap.addr + i * sizeof fe + offsetof(nxaot_fn_entry, body) + static_cast<uint64_t>(static_cast<int64_t>(fe.body));
        if (static_cast<uint64_t>(fl.first) + fl.count > nlines) {
            report("the function at " + X(fe.offset) + " names lines past the table's end");
            continue;
        }
        with_lines += fl.count != 0;
        const auto f = d.functions.find(fe.offset);
        uint64_t pc = body;
        llvm::MCInst inst;
        uint64_t inst_size = 0;
        uint32_t last = 0;
        std::map<uint32_t, bool> loads_or_stores;  // per guest offset of the body: whether one of its entries' instructions loads or stores
        std::map<uint32_t, std::string> names;     // and its entries' instructions, for the report
        for (uint32_t k = 0; k < fl.count; ++k) {
            nxaot_line_entry e;
            std::memcpy(&e, lines.data.data() + (static_cast<size_t>(fl.first) + k) * sizeof e, sizeof e);
            ++checked;
            const std::string at = "entry " + std::to_string(fl.first + k) + " (function " + X(fe.offset) + ", native body+" + X(e.native) +
                                   ", guest " + X(e.offset) + ")";
            if (k && e.native < last) {
                report(at + ": the function's run is not sorted by native address");
                continue;
            }
            last = e.native;
            // The native side: an instruction starts there (decoded from the body's start on), and it loads or stores
            const uint64_t want = body + e.native;
            bool boundary = false;
            while (pc <= want) {
                const llvm::ArrayRef<uint8_t> bytes = code_at(pc);
                uint64_t size = 0;
                if (bytes.empty() || dis->getInstruction(inst, size, bytes, pc, llvm::nulls()) != llvm::MCDisassembler::Success || !size) {
                    break;
                }
                if (pc == want) {
                    boundary = true;
                    inst_size = size;
                    break;
                }
                pc += size;
            }
            if (!boundary) {
                report(at + ": " + X(want) + " starts no instruction of the function's code");
                continue;
            }
            loads_or_stores[e.offset] = loads_or_stores[e.offset] || accesses_memory(want, inst, inst_size);
            names[e.offset] += " " + std::string(mii->getName(inst.getOpcode())) + "@" + X(want);
            // The guest side: a load, store or dc zva of the function's code
            const auto w = image->InCode(e.offset) ? image->Word(e.offset) : std::nullopt;
            if (!w || !((*w & 0x0A000000u) == 0x08000000u || (*w & 0xFFFFFFE0u) == 0xD50B7420u)) {
                report(at + ": the guest word there (" + (w ? X(*w) : std::string("none")) + ") is no load, store or dc zva");
                continue;
            }
            if (f == d.functions.end() || !in_blocks(f->second, e.offset)) {
                const bool elsewhere = std::any_of(d.functions.begin(), d.functions.end(), [&](const auto& g) { return in_blocks(g.second, e.offset); });
                if (!elsewhere) {
                    report(at + ": the guest instruction is in no function's code");
                    continue;
                }
                ++inlined;
            }
        }
        for (const auto& [offset, mem] : loads_or_stores) {
            ++accesses;
            memory += mem;
            if (!mem && accesses - memory <= 20) {
                // LLVM's code generation moved the access's mark off its memory instruction (a load folded into the instruction that uses it, unfolded
                // again later: the unfolded load has no mark, and the mark is on an instruction made from the same node). A fault at that load is then
                // reported at the nearest entry before it, which says so: counted, not an error of the table
                const auto w = image->Word(offset);
                std::printf("nxaot lines: note: the function at %s, guest instruction %s (%s): no entry of the access is an x86 load or store:%s\n",
                            X(fe.offset).c_str(), X(offset).c_str(), w ? X(*w).c_str() : "?", names[offset].c_str());
            }
        }
    }
    std::printf("nxaot lines %s: %llu entries in %llu of %zu functions; %llu at loads and stores of their own function's code, %llu of a function "
                "inlined into it; %llu bad; %llu guest accesses, %llu with an x86 load or store among their entries\n",
                pack.c_str(), static_cast<unsigned long long>(checked), static_cast<unsigned long long>(with_lines), nfn,
                static_cast<unsigned long long>(checked - bad_entries - inlined), static_cast<unsigned long long>(inlined), static_cast<unsigned long long>(bad),
                static_cast<unsigned long long>(accesses), static_cast<unsigned long long>(memory));
    return bad || checked == 0 ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return Usage();
    }
    const std::string cmd = argv[1];
    if (cmd == "build") {
        return Build(argc, argv);
    }
    if (cmd == "lines") {
        return Lines(argc, argv);
    }
    if (cmd == "plan") {
        return PlanCmd(argc, argv);
    }
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
