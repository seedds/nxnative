// The interprocedural facts of docs/design/aot-recompiler.md section 2.3, over discovery's functions (no code generation): per function the
// registers it may read before writing them, may change, must write, and whether it is preserving (x19..x29, SP and the low halves of v8..v15
// back at every return, from the slots its own prologue saved them to, SP at its entry value), as a fixed point over the call graph (direct calls
// by their callee; blr and unresolved br by the union over the address-taken functions; svc by the SVC convention). Also the exclusive windows
// of section 1.3 under the rewriter's rules, classified as in section 6.5. The register effects of each instruction come from dynarmic's IR of it.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "analysis/discover.h"
#include "analysis/parallel.h"
#include "module/image.h"

namespace nxaot::analysis {

// The effects of one instruction on the registers (uses before its own writes, writes), from dynarmic's IR of it
struct RegFx {
    RegSet use, def;
};
RegFx InstructionFx(uint64_t pc, uint32_t word);

// What the summaries (and the pack's plan, src/build/plan.cpp) need of every instruction, again and again over the fixed points: its register
// effects (InstructionFx) and the preservation analysis' view of its dynarmic IR. Both are a function of the word alone, except immediates that
// are the pc plus a constant (adr, adrp, bl's return address, literal addresses), which are kept relative: so each distinct word is translated
// by dynarmic once (twice, to find those immediates) for the whole module, instead of once per visit. A word whose IR depends on its pc in any
// other way is translated at each visit, as before. One per module, kept by Discovery for the plan. Prepare makes the facts of every
// instruction of the functions it is given on the pool's threads; afterwards Fx and the summaries' interpretation of those instructions only
// read the cache, from any number of threads. An instruction not prepared is made at its first use, on one thread only
class InsnCache {
public:
    explicit InsnCache(const module::Image& image);
    ~InsnCache();
    InsnCache(const InsnCache&) = delete;
    InsnCache& operator=(const InsnCache&) = delete;
    const RegFx& Fx(uint64_t pc);  // InstructionFx of the word at pc; RegFx{} where the image has no word
    // The facts of every instruction of the functions' blocks, the distinct new words translated on the pool; a function whose walk state
    // (Function::state, when not 0) is the one it was last prepared with is skipped
    void Prepare(const std::vector<const Function*>& functions, Pool* pool);
    uint64_t Distinct() const;     // distinct words seen
    uint64_t Translations() const; // dynarmic translations made
    // Discovery's end: the summaries' programs dropped, the effects kept (the plan reads Fx only); a later use of a program translates the
    // word again (README.md, "Memory at scale")
    void KeepEffects();
    void NoteMemory(const std::string& at) const;  // its tables' bytes (src/analysis/memory.h; --memory)
    uint64_t Bytes() const;                        // all of them
    struct Impl;                   // summary.cpp
    Impl& Internal() { return *impl; }

private:
    std::unique_ptr<Impl> impl;
};

// The summaries of earlier runs, per function by everything its summary is computed from: its code as discovery's walks left it
// (Function::state, with its exported flag) and what it reads of its callees' summaries (and of the address-taken functions' union for blr and
// unbounded br). Each run of the fixed point starts afresh, and its rounds mostly compute what the last run computed in the same order; a
// function's summary for inputs seen before is taken from here. One per discovery
struct SummaryMemo {
    // per function up to 8 records back to back, oldest first: the length of its inputs, the inputs (the words summary.cpp's EmitInputs
    // makes of them) and the summary computed from them (five words): README.md, "Memory at scale"
    std::map<uint64_t, std::vector<uint64_t>> by_function;
    uint64_t hits = 0;
};
uint64_t BytesOf(const SummaryMemo& memo);  // its bytes (src/analysis/memory.h)

// Fills Function::summary for every function; address_taken must be set already. cache: the module's instruction cache (null: a cache of this
// call's own); memo: null, or the summaries of earlier runs (Function::state must then name each function's code). rounds and computes, when
// given, count the fixed point's rounds and the summaries it computed (DiscoverTimes); sweeps, the passes over the functions within the rounds.
// pool: null (one thread), or the threads the summaries of a round are computed on: the result is the same with any number (summary.cpp, Run)
void Summarise(const module::Image& image, std::map<uint64_t, Function>& functions, InsnCache* cache = nullptr, SummaryMemo* memo = nullptr,
               uint64_t* rounds = nullptr, uint64_t* computes = nullptr, Pool* pool = nullptr, uint64_t* sweeps = nullptr);

// Fills Function::windows and unpaired_exclusive
void FindWindows(const module::Image& image, Function& fn);

// What an indirect call (blr, a br discovery could not bound) may find changed when the callee is not one of the module's functions: a host
// function through the host-function page (a GOT slot the loader bound to an import), a function of another pack, code the JIT fallback runs.
// Those follow AAPCS64 and leave their results in its result registers: x0..x7, x8 (the indirect result location) and v0..v3. The module's own
// write set cannot stand for them: a module whose functions never write v0 had its call sites keep their own v0 after a host function returned
// a float there (sqrtf through a GOT slot). The summaries' indirect callee and the plan's indirect view (src/build/plan.cpp) include these
inline RegSet ForeignWrites() {
    RegSet r;
    r.x = 0x1FFu;
    r.v = 0xFu;
    return r;
}

}  // namespace nxaot::analysis
