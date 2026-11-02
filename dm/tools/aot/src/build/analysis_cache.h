// The analysis of a module kept for the next pack (docs/design/aot-recompiler.md section 13; README.md, "The analysis cache"): what the pack
// stage takes from discovery and the plan (src/analysis, src/build/plan.h), in the per-function cache's directory under the SHA-256 of
// everything they depend on: the module's image and, for a behaviour image, its manifest file; the tool (nxaot.lock, LLVM, this executable's
// SHA-256: the chain version until AOT_CHAIN exists); the convention and the options the plan reads. A warm repack with nothing changed then
// runs neither discovery nor the plan: phase 5a measured them as most of a warm repack (3.89 of the benchmark's 4.32 s).
//
// What is kept: per function its entry, limit, the bounds of its blocks (the gate sites and the returns' table read them), its calls' sites and
// whether it is gap code (analysis::IsGapCode: the tier rule's, README.md "Gap code at tier C"), and for a function the pack may split (more
// than split_above instructions) its blocks' successors and exclusive windows too; the census's FPSR answer; discovery's notes; the whole plan.
// An entry is one file, <dir>/analysis/<key>.bin, written through a temporary name and renamed, its payload followed by its own SHA-256: a
// file that is short, does not parse, names another key or fails its checksum is ignored (and replaced), never trusted.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "analysis/discover.h"
#include "build/plan.h"
#include "module/image.h"

namespace nxaot::build {

struct CachedAnalysis {
    analysis::Discovery d;  // functions: entry, limit, end, name, blocks (bounds; successors where kept), calls, windows (where kept), and a
                            // provenance of kProvGap for gap code (0 for the others: their provenance is not kept)
    Plan plan;
    bool reads_fpsr = false;  // the census's answer (analysis::ReadsFpsr)
};

// The key of a module's analysis: tool is the toolchain's identity (pack.cpp, ToolIdentity); split_above, the size above which an entry keeps
// a function's edges (BuildOptions::split_above, UINT64_MAX without splitting)
std::string AnalysisKey(const module::Image& image, const lift::Convention& conv, bool all_entries, uint64_t split_above, const std::string& tool);

// The entry's path in a cache directory
std::string AnalysisPath(const std::string& cache_dir, const std::string& key);

// Reads an entry: false (and *why says why, for the log) when there is none or it cannot be trusted
bool LoadAnalysis(const std::string& path, const std::string& key, CachedAnalysis* out, std::string* why);

// Writes an entry (through a temporary name); false with *error set when it cannot
bool StoreAnalysis(const std::string& path, const std::string& key, const analysis::Discovery& d, const Plan& plan, bool reads_fpsr,
                   uint64_t split_above, std::string* error);

}  // namespace nxaot::build
