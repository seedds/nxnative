// The measurements of phase 1 (docs/design/aot-recompiler.md section 14) besides the NaN chains: the two calling conventions of section 2.3 on
// synthetic call chains (ns and TSC cycles per call and return) and on the corpus (spills and reloads), the dispatcher's cost per blr on an
// inline-cache hit and miss (section 4), a jump table, the integer throughput of the corpus's int_ops natively and under dynarmic, and the pack
// stage's throughput per stage and tier over the corpus (section 10). Every module is built by the pack stage (src/build) into a shared object
// and run by the native run time (src/rt), as nxaot build and nxaot_host do.
#pragma once

#include <cstdint>
#include <string>

namespace nxaot::bench {

struct CallsOptions {
    uint64_t iterations = 20'000'000;  // loop iterations of each synthetic program
    uint64_t rounds = 200;             // int_ops_rounds of the throughput run
    int repeats = 5;                   // best of
    std::string work;                  // where the objects and shared objects go
    std::string nxaot_run;             // the dynarmic reference runner (its run times)
    std::string corpus;                // dm/tools/aot/corpus
    std::string cpu;                   // the target CPU of the builds (empty: this machine's)
    bool calls = true, dispatch = true, int_ops = true, pack = true, spills = true;
};

int RunCalls(const CallsOptions& o);

}  // namespace nxaot::bench
