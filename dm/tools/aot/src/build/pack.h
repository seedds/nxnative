// The pack stage of nxaot (docs/design/aot-recompiler.md sections 9.2 to 9.4, 10, 13: src/build): every discovered function of a module lifted in
// function mode (src/lift/lower_calls.cpp), with the standard entries of the functions an indirect call or the host may enter, the dispatcher's
// lookup table, the function map, the line table (!pcsections) and the module descriptor of include/nxaot/aot_module.h; optimised by the tier's
// pipeline and written as native objects, linked by lld (as a library) into the shared object the host loads; for the Windows host (--target
// windows) the same objects with the host's boundary in its convention, joined, converted to COFF (coff.h) and linked into a DLL (LinkWindows).
//
// Compile units (section 10's strategy): at tiers C and W a group of neighbouring functions of one tier, each optimised on its own (at H and H+,
// whose pipelines inline, a shard of neighbouring functions; a function over the size limit of section 9.3 is split into parts, a unit each),
// lifted, optimised and compiled in an LLVM context and module of its own on one of the compile workers, into an object of its own; the tables,
// the descriptor and the code every unit shares (jit_entry, the slow-path wrappers, the module's base) are one more object. The functions reach
// each other through hidden symbols that lld resolves, so a unit's object depends on its own code and on the convention facts of what it calls,
// and not on how the units were spread over the workers: a pack is the same whatever the number of workers, and a unit's object can be cached
// (src/cache, section 13) and reused by the next pack, as can the module's analysis (analysis_cache.h).
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "analysis/discover.h"
#include "lift/lifter.h"
#include "module/image.h"

namespace nxaot::build {

// The tiers of section 9.3. Without a profile the pack modes map onto one tier for every function: balanced -> W, max -> H+; with one
// (BuildOptions::profile_path) a sampled function is at H (max: H+) and the others at C (max: H). Gap code (a function only discovery's gap
// sweep found, analysis::IsGapCode) takes its mode's tier as any other function; with --gap-tier C (BuildOptions::gap_tier_c) it is at C in
// the balanced mode whatever --tier says, unless the profile sampled it (README.md, "Gap code at tier C")
enum class Tier : uint8_t { C, W, H, HPlus };
const char* TierName(Tier t);  // "C", "W", "H", "H+"

struct BuildOptions {
    lift::CallConv cc = lift::CallConv::SwiftTail;  // the default convention, chosen by the measurements (README.md, "Calls and returns")
    Tier tier = Tier::W;
    std::string mode = "balanced";                  // balanced | max (the pack modes; --tier overrides the mode's tier)
    bool correctness = false;                       // store and reload every register at every call and return (differential tests)
    bool all_entries = false;                       // a standard entry for every function (nxaot_funcdiff)
    bool debug_info = true;                         // DWARF line info, line = guest offset (section 9.2)
    bool line_table = true;                         // the compact line table (section 3.5)
    std::string cpu;                                // empty: this machine's CPU; else an LLVM CPU name (x86-64-v3)
    int fpsr_flags = -1;                            // model the cumulative FPSR flags: -1 as the census says (answer.fpsr_read), 0, 1
    bool count_spills = false;                      // also compile to assembly and count the register allocator's spills and reloads
    std::string asm_path;                           // also write that assembly here (and count them)
    bool no_inline = false;                         // keep every guest function out of line (nxaot_bench measures calls)
    bool safepoints = true;                         // polls at function entries and backward branches (section 6.2); off: --no-safepoints
    std::string ir_path;                            // also write the lifted LLVM IR, before the tier's passes (--ir)
    bool ir_after_passes = false;                   // write it after the tier's passes instead: what code generation is given (--ir-opt <file>)
    unsigned workers = 0;                           // compile workers; 0: from the CPUs this process may run on, bounded by free memory (--workers)
    std::string cache_dir;                          // the per-function cache (--cache, section 13); empty: none
    bool groups = true;                             // units of neighbouring functions at tiers C and W; off: one per function (--no-groups)
    bool split = true;                              // functions over a size limit split into parts (design 9.3); off: --no-split
    uint64_t split_above = 8'192;                   // that limit in instructions; parts of a quarter to a half of it (--split-above, tests)
    std::string profile_path;                       // sampled guest offsets (--profile, design 9.3: tier H where sampled, tier C elsewhere)
    std::vector<analysis::Miss> misses;             // the host's JIT feedback (--misses, ReadMisses; the image carved with analysis::CarveMisses before):
                                                    // discovery's roots, and with them in the analysis cache's key
    std::set<uint64_t> no_roots;                    // a test's --no-root (analysis::DiscoverOptions::no_roots), in the analysis cache's key too
    bool windows = false;                           // --target windows: a PE DLL for the Windows host ("Windows packs" in pack.cpp, coff.h)
    bool isel_misses = false;                       // count FastISel's misses by kind (--isel-misses; no cache: every unit compiled)
    std::string tiers_path;                         // also write each function's tier here (--tiers-tsv: README.md, "Gap code at tier C")
    bool gap_tier_c = false;                        // --gap-tier C: gap code at tier C in the balanced mode (off: the mode's tier, measured
                                                    // in README.md, "Gap code at tier C")
};

struct BuildStats {
    std::string module;
    uint64_t words = 0, functions = 0, standard_entries = 0, lookup_slots = 0, refused = 0, blocks = 0;
    uint64_t calls = 0, indirect_calls = 0, tail_calls = 0, indirect_tails = 0, returns = 0, svcs = 0, traps = 0, tables = 0, resume_sites = 0;
    uint64_t spills = 0, reloads = 0, object_bytes = 0, text_bytes = 0;
    uint64_t lines_lost = 0;  // line-table entries the backend could not keep (an access folded into another instruction)
    uint64_t line_entries = 0;  // the line table's entries (lines.o: one per native guest memory access)
    uint64_t gate_sites = 0;    // the gate sites compiled code calls (design 1.1: the manifest's obs and spin rows its functions hold)
    // Seconds. Lifting, verification, optimisation and code generation are summed over the units (CPU seconds of the workers), the others are
    // the wall clock of their stage
    double t_discover = 0, t_plan = 0, t_lift = 0, t_opt = 0, t_codegen = 0, t_link = 0;
    double t_census = 0;                      // of t_discover: the FPSR question (decision 5)
    double t_verify = 0;                      // the IR verifier over the lifted units
    double t_key = 0;                         // the units' cache keys (CPU seconds)
    double t_compile_wall = 0;                // the units and the tables' object on the workers: wall clock
    analysis::DiscoverTimes discover_times;   // of t_discover: discovery's steps
    uint64_t units = 0, workers = 0;
    uint64_t tier_functions[4] = {};          // functions compiled per tier (C, W, H, H+)
    uint64_t split_functions = 0, split_parts = 0, splits = 0;  // functions split for size, their parts, the transfers between parts
    double t_join = 0;                        // joining each split function's parts (ld.lld -r): wall clock
    uint64_t profile_samples = 0, profile_offsets = 0, profile_outside = 0;  // --profile: samples read, distinct offsets, offsets in no function
    uint64_t cache_hits = 0, cache_misses = 0, cache_bytes_read = 0, cache_bytes_written = 0;  // units
    uint64_t cache_hit_functions = 0, cache_miss_functions = 0;  // the functions of those units
    std::string analysis_cache;               // with --cache: "hit", or "miss (why)" (analysis_cache.h)
    double t_analysis_cache = 0;              // reading (and on a miss writing) the analysis cache's entry: wall clock, part of discover and plan
    uint64_t peak_rss_kib = 0;
    bool fpsr_flags = false;
    std::string cpu, features;
    std::vector<std::string> notes;  // [RED] lines and remarks
    std::vector<std::string> imports;  // --target windows: the run-time entries the pack imports, sorted (imports.o: nxaot_imports)
    uint64_t coff_sections = 0, coff_symbols = 0, coff_relocations = 0;  // --target windows: the COFF object lld-link is given (coff.h)
    // --isel-misses: FastISel's misses by kind, summed over the units: how many, and the IR instructions they handed to SelectionDAG
    std::map<std::string, std::pair<uint64_t, uint64_t>> isel_misses;
};

// Compiles the module into objects, in link order: the tables' object, then one per unit (from the cache when options.cache_dir names one and
// it holds the unit's key; else compiled and, with a cache, stored). Objects that are not the cache's go to work_dir (created; the caller removes
// it). False with *error set on failure
bool BuildObjects(const module::Image& image, const BuildOptions& options, const std::string& work_dir, std::vector<std::string>* objects,
                  BuildStats* stats, std::string* error);

// One relocatable object of the whole module: BuildObjects, then lld's relocatable link of the objects into object_path (a work directory
// beside it is removed again)
bool BuildObject(const module::Image& image, const BuildOptions& options, const std::string& object_path, BuildStats* stats, std::string* error);

// A misses file of the host (dm/host/aot.h, "The JIT fallback's misses": DM_AOT_PROFILE writes <folder>/<module>.misses.tsv): '#' comment lines, among them the
// header `# module <name>, build ID <id>, image SHA-256 <hex>`, whose SHA-256 goes to *sha256 ('' when the file has none), and one line `<module offset in hex>
// <reason> <entries>` per address and reason. False with *error naming the line that does not parse, or a file that cannot be read
bool ReadMisses(const std::string& path, std::vector<analysis::Miss>* out, std::string* sha256, std::string* error);

// Links objects into a shared object with lld (as a library: lld::elf::link). False with *error set on failure
bool LinkShared(const std::vector<std::string>& objects, const std::string& output, const std::string& soname, std::string* error);

// Links objects into one relocatable object (ld.lld -r)
bool LinkRelocatable(const std::vector<std::string>& objects, const std::string& output, std::string* error);

// A Windows pack (--target windows; README.md, "Windows packs"): BuildObjects' objects (ELF, imports.o among them) joined by ld.lld -r into one,
// converted to an AMD64 COFF object (coff.h), written to coff_object when it is not empty, and linked by lld's COFF driver (as a library:
// lld::coff::link) into the DLL dll when that is not empty: no entry point, no import, the descriptor nxaot_module and the import table
// nxaot_imports exported, the image deterministic (/Brepro), DWARF kept with debug_info. Work files go to work_dir. False with *error set
bool LinkWindows(const std::vector<std::string>& objects, const std::string& work_dir, const std::string& dll, const std::string& coff_object,
                 bool debug_info, BuildStats* stats, std::string* error);

// The run-time entries a pack may import (include/nxaot/aot_module.h, src/lift/runtime.h): what dm/host and src/rt export to it
bool IsHostImport(const std::string& name);

// The analysis's threads with --workers 0: one per CPU this process may run on (its result is the same with any number)
unsigned AnalysisThreads();

// The compile's workers once the analysis is done (README.md, "Memory at scale"): --workers, else as many as the memory available now holds
// beside the tables' object (which grows with the module's functions), at the measured peak of a worker at the units' tiers, at most one per
// CPU this process may run on, at least one; never more than the units and the tables' object. *line is the log's `aot: workers N (...)` line
unsigned CompileWorkers(const BuildOptions& o, size_t units, const std::set<Tier>& tiers, size_t functions, std::string* line);

// What the analysis of the module needs at its peak (README.md, "Memory at scale": measured per text word, the image besides)
uint64_t AnalysisPeak(const module::Image& image);

}  // namespace nxaot::build
