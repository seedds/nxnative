// Function discovery over a whole module (docs/design/aot-recompiler.md section 1.3): the entries from every source the design lists, in its order
// (FDEs, symbols, DT_INIT / DT_FINI and the init and fini arrays, relocation targets in text, adr and adrp + add, bl targets and b targets that
// leave their function, LSDA landing pads, jump-table targets, the gap sweep; then the host's JIT feedback, the misses, when it is given), the
// provenance of each, a CFG per function from dynarmic's decoder table and the direct branches, jump tables recovered on the lifted IR
// (src/analysis/jumptable.h), exclusive windows, the interprocedural summaries of section 2.3 (src/analysis/summary.h) and the coverage of the
// text. `nxaot discover` prints it (src/main.cpp).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "module/image.h"

namespace nxaot::analysis {

// Where a function entry came from, in the order discovery takes the sources (the first bit set is an entry's primary provenance)
enum Prov : uint32_t {
    kProvStart = 1u << 0,       // the module's entry, the first word of the text
    kProvFde = 1u << 1,         // an FDE of .eh_frame_hdr
    kProvSym = 1u << 2,         // an STT_FUNC symbol of DT_SYMTAB, exported or local
    kProvInit = 1u << 3,        // DT_INIT
    kProvFini = 1u << 4,        // DT_FINI
    kProvInitArray = 1u << 5,   // DT_INIT_ARRAY, DT_PREINIT_ARRAY
    kProvFiniArray = 1u << 6,   // DT_FINI_ARRAY
    kProvReloc = 1u << 7,       // a relocation's target in text (RELA, JMPREL, RELR): vtables, pointer tables
    kProvAdr = 1u << 8,         // an address built in code: adr, adrp + add
    kProvCall = 1u << 9,        // a bl target
    kProvTail = 1u << 10,       // a b (or b.cond, cbz, tbz) target outside the function: a tail call
    kProvLsda = 1u << 11,       // an LSDA landing pad outside its function
    kProvJumpTable = 1u << 12,  // a jump-table target outside the function of the br
    kProvGap = 1u << 13,        // the gap sweep: code after a ret, b or br that nothing else named
    kProvMiss = 1u << 14,       // a miss of the host's JIT fallback (DiscoverOptions::misses): an address the game ran with no compiled entry there
};
constexpr uint32_t kProvCount = 15;
const char* ProvName(unsigned bit);   // "start", "fde", ...
std::string ProvList(uint32_t mask);  // "fde,sym,call", in source order; "-" when empty

// ---- The host's JIT feedback (design 1.3 and 11; README.md, "JIT misses") ----
// A miss: a guest address the x86-64 engine's JIT fallback ran because the pack had no compiled code there, with why and how often (dm/host/aot.h, "The JIT
// fallback's misses": DM_AOT_PROFILE writes <folder>/<module>.misses.tsv, `<module offset> <reason> <entries>` lines; nxaot build / discover --misses)
struct Miss {
    uint64_t offset = 0;
    std::string reason;   // the host's word: indirect, call, host, nocode, nonlocal, fpcr, refused, untranslated, other
    uint64_t count = 0;   // entries into the JIT fallback there (informative: neither discovery nor a cache key reads it)
};
// What discovery makes of a miss, by its reason: a function entry (indirect, call, host, nocode: a call, a branch or the host reached it), a resume point of the function
// whose code holds it (nonlocal: a non-local transfer no native frame took, design 3.3 and 3.4; an entry when no function holds it), nothing (fpcr, refused, untranslated
// and any other word: msr fpcr, a function the lifter refused, an encoding it does not translate: a pack cannot serve them, the JIT keeps them)
enum class MissKind : uint8_t { Entry, Resume, Unserved };
MissKind MissKindOf(const std::string& reason);
bool MissNeedsStdEntry(const std::string& reason);   // indirect, host, nonlocal: the dispatcher or the host enters it by its address (a standard entry, a lookup slot)
// The code a miss proves in a data block: the host ran it, so from each miss's address of kind Entry or Resume that lies in a data block (a stretch the manifest or the
// loader took for data, as nso_code.data_blocks takes one where more than a quarter of the words decode as nothing), the words its control flow reaches there (falling
// through, both ways of a conditional branch, b and bl targets, the return after a call) up to a ret, br, trap or undefined word leave the data blocks, and discovery,
// the lifter and the cache keys see them as code. Returns those words as [lo, hi) runs, in address order
std::vector<std::pair<uint64_t, uint64_t>> CarveMisses(module::Image& image, const std::vector<Miss>& misses);
// What discovery made of one miss address (Discovery::misses): "entry" (a function entry discovery's own sources do not give), "std" (a known function that gets a
// standard entry, which the dispatcher or the host needed), "resume" (a resume point of the function holding it), "known" (the pack serves it without the misses: an
// earlier pack's), "unserved" (with why)
struct MissResult {
    uint64_t offset = 0;
    std::string reasons;   // its reasons, comma-separated
    std::string result, why;
};

// A set of guest registers: x0..x30, SP, v0..v31 (whole registers), NZCV
struct RegSet {
    uint32_t x = 0, v = 0;
    bool sp = false, nzcv = false;
    bool operator==(const RegSet&) const = default;
    RegSet operator|(const RegSet& o) const { return {x | o.x, v | o.v, sp || o.sp, nzcv || o.nzcv}; }
    RegSet operator&(const RegSet& o) const { return {x & o.x, v & o.v, sp && o.sp, nzcv && o.nzcv}; }
    RegSet operator-(const RegSet& o) const { return {x & ~o.x, v & ~o.v, sp && !o.sp, nzcv && !o.nzcv}; }
    bool Empty() const { return !x && !v && !sp && !nzcv; }
    static RegSet All() { return {0x7FFFFFFFu, 0xFFFFFFFFu, true, true}; }
    std::string Str() const;  // "x0-x3,x30,sp,v0,nzcv"; "-" when empty
};

enum class Edge : uint8_t {
    Branch,  // b, b.cond, cbz, tbz taken
    Fall,    // falls through
    Return,  // the instruction after a call (bl, blr), when the call returns
    Escape,  // the instruction after an svc
    Table,   // a jump-table target of the block's br
    Unwind,  // a call to its landing pad (the LSDA call site that covers the call): the unwinder enters the pad
};

enum class Term : uint8_t {
    Fall,      // the block runs into a block start
    Branch,    // b inside the function
    Cond,      // b.cond, cbz, tbz
    Call,      // bl
    CallReg,   // blr
    Svc,
    Ret,       // ret, br x30
    Table,     // br through a recovered jump table
    Indirect,  // br that discovery could not bound: it leaves through the dispatcher
    Tail,      // b to another function
    CondTail,  // b.cond, cbz, tbz to another function (falls through otherwise)
    Trap,      // brk, hlt, udf, a word that does not decode
    Out,       // runs into another function, a data block or the end of the text
};

struct Block {
    uint64_t start = 0, end = 0;  // [start, end)
    Term term = Term::Fall;
    uint64_t target = 0;          // the call or branch target (Call, Branch, Cond, Tail, CondTail)
    std::vector<std::pair<uint64_t, Edge>> succ;
};

struct JumpTable {
    uint64_t br = 0;
    uint64_t table = 0;       // the address of the first entry read (index lo)
    unsigned width = 0;       // entry bytes: 1, 2, 4, 8
    bool is_signed = false;   // entries sign-extended
    uint64_t base = 0;        // the target of an entry of 0
    int64_t scale = 0;        // what one unit of an entry adds to the target
    uint64_t index_lo = 0, count = 0;
    std::vector<uint64_t> targets;  // per index, in order
    uint64_t region = 0;      // the block the lifted region started at
    bool across_calls = false;  // the region continued past calls by their summaries
    bool writable = false;    // the entries lie in writable data (a table of pointers): the dispatcher must check them, design section 4
};

struct Window {  // an exclusive window: ld(a)x(r|p) at lo ... st(l)x(r|p) at hi - 4
    uint64_t lo = 0, hi = 0;
    std::string kind;  // "cas" (no other load: compare-and-swap semantics hold), "aba" (a load reads through the loaded value), "load"
};

struct Summary {
    RegSet reads;        // may be read before written (calls: the callee's reads)
    RegSet writes;       // may hold another value at return than at entry (calls included; registers restored from their slots excluded)
    RegSet must;         // written on every path to every return (and not restored)
    RegSet restored;     // of x19..x29, SP and the low halves of v8..v15: restored on every return path
    bool preserving = false;  // x19..x29, SP and v8..v15 (low halves) restored on every return path from its own prologue slots
    bool returns = false;     // has a return path (ret, br x30, or a tail call to a function that returns)
    bool exported = false;    // its symbol is in DT_SYMTAB with default visibility
};

struct Function {
    uint64_t entry = 0;
    uint64_t limit = 0;   // the walk stays in [entry, limit): the next entry, or the FDE's end
    uint64_t end = 0;     // after the last instruction the walk reached
    uint32_t prov = 0;
    std::string name;     // the symbol at the entry, when there is one
    int fde = -1;         // index into Image::fdes of the FDE that starts at the entry
    std::set<uint64_t> roots;          // block starts entered from outside the function's branches: landing pads, labels (adr targets), and
                                       // code of its FDE or symbol range that no known edge reaches (speculative)
    std::set<uint64_t> speculative;    // roots inside the function's own range that the gap sweep or an adr found: the cases of a table not
                                       // bounded yet, code after a call that does not return
    std::set<uint64_t> spec_live;      // the speculative roots the last walk still needed (no known edge reached them)
    std::set<uint64_t> tentative;      // brs whose table was found by ignoring the speculative code and waits for its verification
    std::set<uint64_t> unverified;     // brs whose tentative table did not verify: not tried that way again
    std::map<uint64_t, Block> blocks;  // by start
    std::vector<std::pair<uint64_t, uint64_t>> calls;  // (pc, target), target 0 for blr
    std::map<uint64_t, JumpTable> tables;              // by the br's pc
    std::map<uint64_t, uint64_t> pads;                 // call pc -> the landing pad of the LSDA call site covering it
    std::map<uint64_t, std::string> indirect;          // br sites left for the dispatcher, with why
    std::vector<Window> windows;
    unsigned unpaired_exclusive = 0;
    size_t words = 0;      // instructions the walk reached
    size_t undefined = 0;  // words inside the function that do not decode
    bool address_taken = false;  // its address is taken: a relocation, an adr / adrp pair, an init or fini array, another function's jump table
    Summary summary;
    uint64_t state = 0;  // discovery: how many of its walks changed what the analyses read of it (the summaries' memo, summary.h)
};

struct Coverage {
    uint64_t text_words = 0, data_words = 0, covered = 0, padding = 0, uncovered = 0, undefined = 0;
    std::vector<std::pair<uint64_t, uint64_t>> uncovered_ranges;  // [lo, hi) of words outside data blocks no function reached that are not padding
};

struct OracleRow {  // --symbols-as-oracle: a symbol against discovery
    uint64_t value = 0, size = 0;
    std::string name;
    bool found = false;        // a discovered function starts there
    uint64_t found_size = 0;
};

struct DiscoverOptions {
    bool symbols = true;          // take STT_FUNC symbols as a source (false: --symbols-as-oracle, they are only compared with)
    unsigned max_table = 4096;    // the most entries a jump table may have
    unsigned region_depth = 12;   // the dominators of a br block a jump-table region may start at
    unsigned threads = 0;         // the analysis's threads: walks, jump tables, summaries, windows, scans (the result is the same with any
                                  // number: README.md, "Phase 5c"); 0: one per CPU
    std::vector<Miss> misses;     // the host's JIT feedback (--misses): roots of discovery by their MissKind (the image carved first, CarveMisses)
    std::set<uint64_t> no_roots;  // a test's option (--no-root): addresses none of discovery's own sources makes an entry (a miss still does)
};

// Where discovery's time goes (seconds of each step over all rounds; `nxaot build` and `nxaot discover --times` print them)
struct DiscoverTimes {
    double classify = 0;   // every text word through ClassifyWord (dynarmic's decoder table)
    double walk = 0;       // the walks of the dirty functions
    double tables = 0;     // jump tables: regions lifted and analysed (Tables, Verify)
    double summaries = 0;  // the interprocedural summaries (every Summarise)
    double sources = 0;    // the other steps: sources, pads, pending entries, addresses, the gap sweep, the no-return cuts
    double windows = 0;    // exclusive windows
    double finish = 0;     // names, coverage, the oracle
    double cpu = 0;        // the process's CPU seconds over the whole discovery (every thread's)
    unsigned summarise_runs = 0;     // Summarise calls
    unsigned summarise_skipped = 0;  // calls not made: nothing they read had changed since the last
    uint64_t summary_rounds = 0;     // fixed-point rounds over all of them
    uint64_t summary_sweeps = 0;     // passes over the functions' levels within those rounds (summary.cpp, Run)
    uint64_t summary_computes = 0;   // summaries computed over all of them
    uint64_t summary_reused = 0;     // summaries taken from the memo of earlier runs (summary.h, SummaryMemo)
    uint64_t distinct_words = 0;     // distinct instruction words the summaries translated (InsnCache)
    uint64_t translations = 0;       // dynarmic translations they made
    uint64_t regions = 0, table_tries = 0, table_tries_skipped = 0, regions_reused = 0;  // Discovery's counts of the same names
    unsigned threads = 1;            // the threads the analysis ran on (DiscoverOptions::threads)
};

class InsnCache;  // src/analysis/summary.h

struct Discovery {
    std::map<uint64_t, Function> functions;
    Coverage coverage;
    std::vector<uint64_t> landing_pads;
    std::vector<OracleRow> oracle;  // with DiscoverOptions::symbols false
    std::vector<std::string> notes; // [RED] lines and remarks, in order
    unsigned regions_lifted = 0, rounds = 0;
    uint64_t table_tries = 0;          // brs given to ResolveJumpTable (each lifts up to region_depth + 1 regions)
    uint64_t table_tries_skipped = 0;  // tries not made: the last one read the same walk state and facts
    uint64_t regions_reused = 0;       // regions whose analysis came from the memo (jumptable.h, RegionMemo) instead of a lifting
    DiscoverTimes times;
    std::shared_ptr<InsnCache> insns;  // the per-instruction facts discovery computed, for the pack's plan (src/build/plan.cpp)
    // With misses (DiscoverOptions::misses): what became of each address, by offset, and the resume points they give each function (function entry -> the
    // addresses, sorted), which the pack makes continuations of the function's call sites (design 3.3, as a setjmp's; src/build/pack.cpp). The summary line
    // `aot: misses: ...` is the first of the notes
    std::vector<MissResult> misses;
    std::map<uint64_t, std::vector<uint64_t>> miss_resumes;
};

Discovery Discover(const module::Image& image, const DiscoverOptions& options = {});

// The times as one line: "threads N; seconds classify .. walk .. tables .. (tries ..) summaries .. (runs .., rounds .., sweeps .., ...) sources ..
// windows .. finish ..; CPU .." (nxaot discover --times)
std::string TimesLine(const DiscoverTimes& t);

}  // namespace nxaot::analysis
