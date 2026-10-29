// The pack stage of nxaot (docs/design/aot-recompiler.md sections 9.2 to 9.4, 13: src/build): every discovered function of a module lifted in
// function mode (src/lift/lower_calls.cpp) into one LLVM module, with the standard entries of the functions an indirect call or the host may
// enter, the dispatcher's lookup table, the function map, the line table (!pcsections) and the module descriptor of include/nxaot/aot_module.h;
// optimised by the tier's pipeline, written as one native object, and linked by lld (as a library) into the shared object the host loads.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
};

struct BuildStats {
    std::string module;
    uint64_t words = 0, functions = 0, standard_entries = 0, lookup_slots = 0, refused = 0, blocks = 0;
    uint64_t calls = 0, indirect_calls = 0, tail_calls = 0, indirect_tails = 0, returns = 0, svcs = 0, traps = 0, tables = 0, resume_sites = 0;
    uint64_t spills = 0, reloads = 0, object_bytes = 0, text_bytes = 0;
    uint64_t lines_lost = 0;  // line-table entries the backend could not keep (an access folded into another instruction)
    double t_discover = 0, t_plan = 0, t_lift = 0, t_opt = 0, t_codegen = 0, t_link = 0;
    uint64_t peak_rss_kib = 0;
    bool fpsr_flags = false;
    std::string cpu, features;
    std::vector<std::string> notes;  // [RED] lines and remarks
};

// Builds the object of one module. False with *error set on failure
bool BuildObject(const module::Image& image, const BuildOptions& options, const std::string& object_path, BuildStats* stats, std::string* error);

// Links objects into a shared object with lld (as a library: lld::elf::link). False with *error set on failure
bool LinkShared(const std::vector<std::string>& objects, const std::string& output, const std::string& soname, std::string* error);

}  // namespace nxaot::build
