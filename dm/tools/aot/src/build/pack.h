// The pack stage of nxaot (docs/design/aot-recompiler.md sections 9.2 to 9.4, 10, 13: src/build): every discovered function of a module lifted in
// function mode (src/lift/lower_calls.cpp), with the standard entries of the functions an indirect call or the host may enter, the dispatcher's
// lookup table, the function map, the line table (!pcsections) and the module descriptor of include/nxaot/aot_module.h; optimised by the tier's
// pipeline and written as native objects, linked by lld (as a library) into the shared object the host loads.
//
// Compile units (section 10's strategy): at tiers C and W each guest function is a unit of its own (at H and H+, whose pipelines inline, a shard of
// neighbouring functions), lifted, optimised and compiled in an LLVM context and module of its own on one of the compile workers, into an object
// of its own; the tables, the descriptor and the code every unit shares (jit_entry, the slow-path wrappers, the module's base) are one more
// object. The functions reach each other through hidden symbols that lld resolves, so a unit's object depends on its own code and on the
// convention facts of what it calls, and not on how the units were spread over the workers: a pack is the same whatever the number of workers,
// and a unit's object can be cached (src/cache, section 13) and reused by the next pack.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "analysis/discover.h"
#include "lift/lifter.h"
#include "module/image.h"

namespace nxaot::build {

// The tiers of section 9.3. Without a profile the pack modes map onto one tier for every function: balanced -> W, max -> H+
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
    unsigned workers = 0;                           // compile workers; 0: from the CPUs this process may run on, bounded by free memory (--workers)
    std::string cache_dir;                          // the per-function cache (--cache, section 13); empty: none
};

struct BuildStats {
    std::string module;
    uint64_t words = 0, functions = 0, standard_entries = 0, lookup_slots = 0, refused = 0, blocks = 0;
    uint64_t calls = 0, indirect_calls = 0, tail_calls = 0, indirect_tails = 0, returns = 0, svcs = 0, traps = 0, tables = 0, resume_sites = 0;
    uint64_t spills = 0, reloads = 0, object_bytes = 0, text_bytes = 0;
    uint64_t lines_lost = 0;  // line-table entries the backend could not keep (an access folded into another instruction)
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
    uint64_t cache_hits = 0, cache_misses = 0, cache_bytes_read = 0, cache_bytes_written = 0;
    uint64_t peak_rss_kib = 0;
    bool fpsr_flags = false;
    std::string cpu, features;
    std::vector<std::string> notes;  // [RED] lines and remarks
};

// Compiles the module into objects, in link order: the tables' object, then one per unit (from the cache when options.cache_dir names one and
// it holds the unit's key; else compiled and, with a cache, stored). Objects that are not the cache's go to work_dir (created; the caller removes
// it). False with *error set on failure
bool BuildObjects(const module::Image& image, const BuildOptions& options, const std::string& work_dir, std::vector<std::string>* objects,
                  BuildStats* stats, std::string* error);

// One relocatable object of the whole module: BuildObjects, then lld's relocatable link of the objects into object_path (a work directory
// beside it is removed again)
bool BuildObject(const module::Image& image, const BuildOptions& options, const std::string& object_path, BuildStats* stats, std::string* error);

// Links objects into a shared object with lld (as a library: lld::elf::link). False with *error set on failure
bool LinkShared(const std::vector<std::string>& objects, const std::string& output, const std::string& soname, std::string* error);

// Links objects into one relocatable object (ld.lld -r)
bool LinkRelocatable(const std::vector<std::string>& objects, const std::string& output, std::string* error);

// The workers a pack runs with --workers 0: the CPUs this process may run on, at most one per kWorkerMemory of available memory, at least one
unsigned AutoWorkers();

}  // namespace nxaot::build
