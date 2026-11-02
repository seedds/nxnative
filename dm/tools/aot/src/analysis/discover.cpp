#include "analysis/discover.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <optional>
#include <set>
#include <thread>
#include <tuple>
#include <utility>

#include "analysis/insn.h"
#include "analysis/jumptable.h"
#include "analysis/parallel.h"
#include "analysis/summary.h"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/SHA256.h>

namespace nxaot::analysis {

namespace {

constexpr const char* kProvNames[kProvCount] = {"start", "fde", "sym", "init", "fini", "init_array", "fini_array",
                                                "reloc", "adr", "call", "tail", "lsda", "jt", "gap", "miss"};
constexpr uint32_t kNop = 0xD503201Fu;
constexpr uint32_t kTakenProv = kProvReloc | kProvAdr | kProvInit | kProvFini | kProvInitArray | kProvFiniArray | kProvJumpTable;

std::string Hex(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%#llx", static_cast<unsigned long long>(v));
    return buf;
}

class Discoverer {
public:
    Discoverer(const module::Image& image, const DiscoverOptions& options)
            : img(image), opts(options), cache(std::make_shared<InsnCache>(image)), pool(std::make_unique<Pool>(options.threads)) {
        out.times.threads = pool->Threads();
        const Timer t{out.times.classify};
        const size_t n = img.text_size / 4;
        insns.resize(n);
        // Each word alone: in chunks on the pool
        constexpr size_t kChunk = 1 << 14;
        pool->For((n + kChunk - 1) / kChunk, [&](size_t c) {
            for (size_t i = c * kChunk; i < std::min(n, (c + 1) * kChunk); ++i) {
                const uint64_t pc = img.text_off + 4 * i;
                insns[i] = ClassifyWord(*img.Word(pc), pc);
            }
        });
        aux_insns.resize(img.aux_size / 4);
        for (size_t i = 0; i < aux_insns.size(); ++i) {
            const uint64_t pc = img.aux_off + 4 * i;
            aux_insns[i] = ClassifyWord(*img.Word(pc), pc);
        }
    }

    Discovery Run() {
        DiscoverTimes& tm = out.times;
        // Each step timed into its DiscoverTimes field (the steps that change nothing are cheap; the loop's order is the algorithm's)
        const auto timed = [](double& into, auto&& step) {
            const Timer t{into};
            return step();
        };
        timed(tm.sources, [&] { Sources(); return true; });
        for (out.rounds = 1; out.rounds < 10000; ++out.rounds) {
            if (timed(tm.walk, [&] { return WalkDirty(); })) continue;
            if (timed(tm.tables, [&] { return Verify(); })) continue;
            if (timed(tm.sources, [&] { return Pads(); })) continue;
            if (timed(tm.tables, [&] { return Tables({}); })) continue;
            if (timed(tm.sources, [&] { return Pending(); })) continue;
            if (timed(tm.sources, [&] { return Addresses(); })) continue;
            if (timed(tm.sources, [&] { return MissRoots(); })) continue;
            // The code-derived sources are exhausted. The summaries now let regions continue past calls, for the brs that a region without
            // calls did not bound; only then the gap sweep, so that cases of a table not bounded yet do not become entries of their own
            timed(tm.summaries, [&] { Taken(); RunSummaries(); return true; });
            if (timed(tm.sources, [&] { return CutNoreturn(); })) continue;
            if (timed(tm.tables, [&] { return Tables([this](uint64_t pc) { return EffectOf(pc); }); })) continue;
            if (timed(tm.sources, [&] { return Gaps(); })) continue;
            break;
        }
        timed(tm.summaries, [&] { Taken(); RunSummaries(); return true; });
        timed(tm.windows, [&] {
            // Each function's own windows: on the pool
            std::vector<Function*> all;
            for (auto& [entry, f] : fns) all.push_back(&f);
            pool->For(all.size(), [&](size_t k) { FindWindows(img, *all[k]); }, 64);
            return true;
        });
        timed(tm.finish, [&] { Names(); Cover(); Oracle(); MissReport(); return true; });
        tm.distinct_words = cache->Distinct();
        tm.translations = cache->Translations();
        tm.regions = out.regions_lifted;
        tm.table_tries = out.table_tries;
        tm.table_tries_skipped = out.table_tries_skipped;
        tm.regions_reused = out.regions_reused;
        out.insns = cache;
        return std::move(out);
    }

private:
    // Adds the seconds of its scope to a DiscoverTimes field
    struct Timer {
        double& into;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Timer() { into += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }
    };

    // The summaries are a function of every function's code as its last walk found it (blocks, roots, pads: its walk state), of its
    // address-taken and exported flags and of the set of functions: when none of those changed since the last run (the run after the loop,
    // often), a new run would give the same summaries, and is not made
    void RunSummaries() {
        std::vector<std::tuple<uint64_t, uint64_t, bool, bool>> key;
        key.reserve(fns.size());
        for (const auto& [entry, f] : fns) key.emplace_back(entry, walk_state[entry].changes, f.address_taken, f.summary.exported);
        if (summarised_key && *summarised_key == key) {
            ++out.times.summarise_skipped;
            return;
        }
        ++out.times.summarise_runs;
        Summarise(img, fns, cache.get(), &summary_memo, &out.times.summary_rounds, &out.times.summary_computes, pool.get(),
                  &out.times.summary_sweeps);
        out.times.summary_reused = summary_memo.hits;
        summarised_key = std::move(key);
    }

    // ---- Helpers ----
    // Code: the text outside its data blocks (TextCode), and the aux region of a behaviour image (AuxCode: the frame step's stubs, design 1.1).
    // Functions start in the text only. The aux region is reached by the branches the cuts put into the text (a stub's entry) and leaves by its
    // own branches back: its blocks belong to every function whose walk reaches them (Inside), so a stub and the code around its site are one
    // function, as on Apple, where the stub is patch-area code between two instructions of that function
    bool TextCode(uint64_t pc) const { return img.InText(pc) && !(pc & 3) && !img.InData(pc); }
    bool AuxCode(uint64_t pc) const { return img.InAux(pc) && !(pc & 3); }
    bool Code(uint64_t pc) const { return TextCode(pc) || AuxCode(pc); }
    const Insn& At(uint64_t pc) const { return AuxCode(pc) ? aux_insns[(pc - img.aux_off) / 4] : insns[(pc - img.text_off) / 4]; }
    bool Inside(const Function& f, uint64_t pc) const { return (pc >= f.entry && pc < f.limit && TextCode(pc)) || AuxCode(pc); }
    // A jump-table target: a decoded instruction of the function, or the entry of another function (a computed tail call: a table of pointers)
    bool ValidTarget(const Function& f, uint64_t t) const { return Code(t) && At(t).kind != Kind::Undefined && (Inside(f, t) || fns.count(t)); }

    bool AddEntry(uint64_t pc, uint32_t prov) {
        if (!TextCode(pc)) {
            return false;  // the aux region's code is part of the functions that reach it
        }
        if (!(prov & kProvMiss) && opts.no_roots.count(pc)) {
            return false;  // a test's --no-root: none of discovery's own sources makes it an entry (a miss does)
        }
        auto [it, inserted] = fns.try_emplace(pc);
        it->second.entry = pc;
        it->second.prov |= prov;
        if (inserted) {
            limits_stale = true;
            if (const auto f = fde_start.find(pc); f != fde_start.end()) it->second.fde = f->second;  // an FDE's function a miss brought back
        }
        return inserted;
    }

    void Limits() {
        if (!limits_stale) return;
        limits_stale = false;
        for (auto it = fns.begin(); it != fns.end(); ++it) {
            const auto next = std::next(it);
            uint64_t limit = next == fns.end() ? img.TextEnd() : next->first;
            Function& f = it->second;
            if (f.fde >= 0) limit = std::min(limit, img.fdes[static_cast<size_t>(f.fde)].end);
            if (limit != f.limit) {
                f.limit = limit;
                dirty.insert(f.entry);
            }
        }
    }

    // ---- The sources whose entries need no code: the module's entry, FDEs, symbols, the dynamic section, relocations ----
    void Sources() {
        AddEntry(img.text_off, kProvStart);
        for (size_t i = 0; i < img.fdes.size(); ++i) {
            const auto& fde = img.fdes[i];
            if (!TextCode(fde.start)) {
                out.notes.push_back("[RED] discover: the FDE at " + Hex(fde.offset) + " starts at " + Hex(fde.start) + ", not an instruction of the text");
                continue;
            }
            fde_start[fde.start] = static_cast<int>(i);
            AddEntry(fde.start, kProvFde);
            if (const auto it = fns.find(fde.start); it != fns.end()) it->second.fde = static_cast<int>(i);
        }
        if (opts.symbols) {
            for (const auto& s : img.symbols) {
                if (s.type == 2 /* STT_FUNC */ && s.Defined() && img.InText(s.value)) {
                    AddEntry(s.value, kProvSym);
                    sym_end[s.value] = std::max(sym_end[s.value], s.value + s.size);
                }
            }
        }
        if (img.init) AddEntry(img.init, kProvInit);
        if (img.fini) AddEntry(img.fini, kProvFini);
        for (const uint64_t a : img.init_array) AddEntry(a, kProvInitArray);
        for (const uint64_t a : img.preinit_array) AddEntry(a, kProvInitArray);
        for (const uint64_t a : img.fini_array) AddEntry(a, kProvFiniArray);
        for (const auto& r : img.relocs) {
            if (r.has_target && img.InText(r.target)) AddEntry(r.target, kProvReloc);
        }
        // The host's misses (DiscoverOptions::misses, design 1.3: the JIT feedback): the entries now; the resume points once the functions around them
        // are known (MissRoots); the others are only reported (MissReport)
        for (const Miss& m : opts.misses) {
            switch (MissKindOf(m.reason)) {
            case MissKind::Entry:
                AddEntry(m.offset, kProvMiss);
                if (MissNeedsStdEntry(m.reason)) miss_std.insert(m.offset);
                break;
            case MissKind::Resume:
                miss_resume.insert(m.offset);
                break;
            case MissKind::Unserved:
                break;
            }
        }
        Limits();
    }

    // ---- The misses' resume points (nonlocal, design 3.3): a root of the function whose range holds one, so that its walk reaches it as a block of
    // its own, where the pending check after each of its calls can resume; at a function's entry, or in no function's range, an entry the dispatcher
    // enters (a standard entry) ----
    bool MissRoots() {
        bool changed = false;
        for (const uint64_t a : miss_resume) {
            if (!TextCode(a)) continue;  // not code: reported (MissReport)
            if (const auto e = fns.find(a); e != fns.end()) {
                e->second.prov |= kProvMiss;
                miss_std.insert(a);
                continue;
            }
            const auto it = fns.upper_bound(a);
            Function* f = it == fns.begin() ? nullptr : &std::prev(it)->second;
            if (!f || a >= f->limit) {
                if (AddEntry(a, kProvMiss)) changed = true;
                miss_std.insert(a);
                continue;
            }
            if (f->roots.insert(a).second) {
                dirty.insert(f->entry);
                changed = true;
            }
        }
        if (changed) Limits();
        return changed;
    }

    // ---- The walk: a function's instructions from its entry and roots, by the direct branches, inside [entry, limit) ----
    // What a walk finds for the whole discovery: entries code names (pending) and text addresses adr / adrp + add build (refs), in the order
    // the walk found them; WalkDirty applies them in function order
    struct WalkOut {
        std::vector<std::pair<uint64_t, uint32_t>> pending;
        std::vector<std::pair<uint64_t, uint64_t>> refs;
    };

    // Reads the image, the set of functions and the summaries, and writes only the function, its walk count and state, and out: walks of
    // different functions run at once (WalkDirty)
    void Walk(Function& f, WalkOut& wout) {
        f.blocks.clear();
        f.calls.clear();
        f.indirect.clear();
        f.words = f.undefined = 0;
        f.end = f.entry;
        std::set<uint64_t> leaders{f.entry}, covered;
        std::deque<uint64_t> work{f.entry};
        for (const uint64_t r : f.roots) {
            if (Inside(f, r) && !f.speculative.count(r)) {
                leaders.insert(r);
                work.push_back(r);
            }
        }
        auto branch_to = [&](uint64_t t) {
            if (Inside(f, t) && !(fns.count(t) && t != f.entry)) {
                leaders.insert(t);
                work.push_back(t);
                return true;
            }
            return false;
        };
        // First from the entry and the known roots, then from the speculative roots that walk did not reach
        f.spec_live.clear();
        for (int phase = 0; phase < 2; ++phase) {
            if (phase == 1) {
                for (const uint64_t r : f.speculative) {
                    if (Inside(f, r) && !covered.count(r)) {
                        f.spec_live.insert(r);
                        leaders.insert(r);
                        work.push_back(r);
                    }
                }
            }
        while (!work.empty()) {
            uint64_t pc = work.front();
            work.pop_front();
            while (Inside(f, pc)) {
                if (covered.count(pc)) {
                    leaders.insert(pc);
                    break;
                }
                covered.insert(pc);
                const Insn& i = At(pc);
                if (i.kind == Kind::Normal) {
                    pc += 4;
                    continue;
                }
                switch (i.kind) {
                case Kind::B:
                    if (!branch_to(i.target)) wout.pending.emplace_back(i.target, kProvTail);
                    break;
                case Kind::BCond:
                    if (!branch_to(i.target)) wout.pending.emplace_back(i.target, kProvTail);
                    leaders.insert(pc + 4);
                    work.push_back(pc + 4);
                    break;
                case Kind::BL:
                    wout.pending.emplace_back(i.target, kProvCall);
                    if (!Returns(i.target)) break;
                    [[fallthrough]];
                case Kind::BLR:
                case Kind::SVC:
                    leaders.insert(pc + 4);
                    work.push_back(pc + 4);
                    if (const auto pad = f.pads.find(pc); pad != f.pads.end()) {
                        leaders.insert(pad->second);
                        work.push_back(pad->second);
                    }
                    break;
                case Kind::BR:
                    if (const auto t = f.tables.find(pc); t != f.tables.end()) {
                        for (const uint64_t target : t->second.targets) branch_to(target);
                    }
                    break;
                default:
                    break;  // ret, a trap, an undefined word
                }
                break;
            }
        }
        }
        // Blocks: a leader, or the instruction after one that ends a block, starts one
        Block* cur = nullptr;
        for (const uint64_t pc : covered) {
            const bool starts = !cur || leaders.count(pc) || cur->end != pc;
            if (starts) {
                cur = &f.blocks[pc];
                cur->start = pc;
            }
            cur->end = pc + 4;
            const Insn& i = At(pc);
            ++f.words;
            if (i.kind == Kind::Undefined) ++f.undefined;
            if (i.Ends()) {
                Finish(f, *cur, i, pc);
                cur = nullptr;
            }
        }
        for (auto& [start, b] : f.blocks) {
            if (b.succ.empty() && b.term == Term::Fall) {
                const Insn& last = At(b.end - 4);
                if (!last.Ends()) {
                    if (covered.count(b.end)) b.succ.emplace_back(b.end, Edge::Fall);
                    else b.term = Term::Out;  // runs into another function, a data block or the end of the text
                }
            }
        }
        // The end of its text: the aux region's blocks lie past the data, outside the function's own range
        f.end = f.entry;
        for (auto it = covered.rbegin(); it != covered.rend(); ++it) {
            if (TextCode(*it)) {
                f.end = *it + 4;
                break;
            }
        }
        // adr and adrp + add that build text addresses, per block (an adrp's page is forgotten when its register is written)
        for (const auto& [start, b] : f.blocks) {
            uint64_t page[32];
            uint32_t have = 0;
            for (uint64_t pc = b.start; pc < b.end; pc += 4) {
                const Insn& i = At(pc);
                if (i.adr) {
                    wout.refs.emplace_back(i.target, pc);
                } else if (i.add_imm && (have >> i.rn & 1)) {
                    wout.refs.emplace_back(page[i.rn] + i.target, pc);
                }
                if (i.kind == Kind::Normal) have &= ~(1u << i.rd);
                if (i.adrp) {
                    page[i.rd] = i.target;
                    have |= 1u << i.rd;
                }
            }
        }
        ++version.find(f.entry)->second;
        const std::array<uint8_t, 32> digest = StateDigest(f);
        WalkState& ws = walk_state.find(f.entry)->second;
        if (ws.digest != digest) {
            ws.digest = digest;
            ++ws.changes;
        }
        f.state = ws.changes;
    }

    // What a jump-table region reads of its function (jumptable.cpp: the blocks and their edges, the known tables, the roots, the limit), as a
    // SHA-256: a walk that gives the same state again (a function walked again because a neighbour's limit or a callee's return changed) does
    // not make its brs be tried again
    static std::array<uint8_t, 32> StateDigest(const Function& f) {
        std::vector<uint64_t> w{f.entry, f.limit};
        for (const auto& [start, b] : f.blocks) {
            w.insert(w.end(), {b.start, b.end, static_cast<uint64_t>(b.term), b.target, b.succ.size()});
            for (const auto& [t, kind] : b.succ) w.insert(w.end(), {t, static_cast<uint64_t>(kind)});
        }
        w.push_back(~0ull);
        for (const auto& [br, jt] : f.tables) {
            w.insert(w.end(), {br, jt.table, jt.width, jt.is_signed, jt.base, static_cast<uint64_t>(jt.scale), jt.index_lo, jt.count, jt.region,
                               jt.across_calls, jt.writable, jt.targets.size()});
            w.insert(w.end(), jt.targets.begin(), jt.targets.end());
        }
        for (const std::set<uint64_t>* s : {&f.roots, &f.speculative, &f.spec_live}) {
            w.push_back(~0ull);
            w.insert(w.end(), s->begin(), s->end());
        }
        w.push_back(~0ull);
        for (const auto& [call, pad] : f.pads) w.insert(w.end(), {call, pad});  // the summaries read them too (RunSummaries)
        llvm::SHA256 sha;
        sha.update(llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t*>(w.data()), w.size() * sizeof(uint64_t)));
        return sha.final();
    }

    void Finish(Function& f, Block& b, const Insn& i, uint64_t pc) {
        const uint64_t next = pc + 4;
        switch (i.kind) {
        case Kind::B:
            b.target = i.target;
            if (Inside(f, i.target) && !(fns.count(i.target) && i.target != f.entry)) {
                b.term = Term::Branch;
                b.succ.emplace_back(i.target, Edge::Branch);
            } else {
                b.term = Term::Tail;
            }
            break;
        case Kind::BCond:
            b.target = i.target;
            if (Inside(f, i.target) && !(fns.count(i.target) && i.target != f.entry)) {
                b.term = Term::Cond;
                b.succ.emplace_back(i.target, Edge::Branch);
            } else {
                b.term = Term::CondTail;
            }
            if (Inside(f, next)) b.succ.emplace_back(next, Edge::Fall);
            break;
        case Kind::BL:
        case Kind::BLR:
            b.term = i.kind == Kind::BL ? Term::Call : Term::CallReg;
            b.target = i.kind == Kind::BL ? i.target : 0;
            f.calls.emplace_back(pc, b.target);
            if (Inside(f, next) && (i.kind == Kind::BLR || Returns(i.target))) b.succ.emplace_back(next, Edge::Return);
            if (const auto pad = f.pads.find(pc); pad != f.pads.end() && Inside(f, pad->second)) b.succ.emplace_back(pad->second, Edge::Unwind);
            break;
        case Kind::SVC:
            b.term = Term::Svc;
            if (Inside(f, next)) b.succ.emplace_back(next, Edge::Escape);
            break;
        case Kind::BR:
            if (i.rn == 30) {
                b.term = Term::Ret;  // br x30 returns, as ret does (aot-recompiler.md 3.3)
            } else if (const auto t = f.tables.find(pc); t != f.tables.end()) {
                b.term = Term::Table;
                std::set<uint64_t> seen;
                for (const uint64_t target : t->second.targets) {
                    if (seen.insert(target).second && Inside(f, target)) b.succ.emplace_back(target, Edge::Table);
                }
            } else {
                b.term = Term::Indirect;
                f.indirect[pc] = "not bounded yet";
            }
            break;
        case Kind::RET:
            b.term = Term::Ret;
            break;
        default:
            b.term = Term::Trap;
            break;
        }
    }

    // A bl to a function with no return path does not come back: what follows it is not that function's code (often the next function,
    // which then has no other entry: the gap sweep finds it)
    bool Returns(uint64_t callee) const {
        const auto it = std::lower_bound(summarised.begin(), summarised.end(), callee);
        if (it == summarised.end() || *it != callee) return true;  // not summarised yet (a function found since): taken to return
        return summarised_returns[static_cast<size_t>(it - summarised.begin())];
    }

    // After the summaries: walk again every function with a call whose fall-through disagrees with them, either way. The functions the
    // summaries cover and whether each returns are kept in address order (their summaries change only at the next run, after which this
    // runs again)
    bool CutNoreturn() {
        summarised.clear();
        summarised_returns.clear();
        std::vector<const Function*> all;
        for (const auto& [entry, f] : fns) {
            summarised.push_back(entry);
            summarised_returns.push_back(f.summary.returns);
            all.push_back(&f);
        }
        std::vector<uint8_t> again(all.size());
        pool->For(all.size(), [&](size_t k) {
            const Function& f = *all[k];
            for (const auto& [start, b] : f.blocks) {
                if (b.term != Term::Call) continue;
                const bool has = std::any_of(b.succ.begin(), b.succ.end(), [](const auto& s) { return s.second == Edge::Return; });
                if (has != (Returns(b.target) && Inside(f, b.end))) again[k] = 1;
            }
        }, 256);
        for (size_t k = 0; k < all.size(); ++k) {
            if (again[k]) dirty.insert(all[k]->entry);
        }
        return !dirty.empty();
    }

    bool WalkDirty() {
        Limits();
        if (dirty.empty()) return false;
        const std::set<uint64_t> todo = std::move(dirty);
        dirty.clear();
        // The walks on the pool (each writes its own function and output; the maps' slots exist before), their entries and addresses
        // applied in function order afterwards, as the walks one after another made them
        std::vector<Function*> walks;
        for (const uint64_t e : todo) {
            if (const auto it = fns.find(e); it != fns.end()) {
                walks.push_back(&it->second);
                version[e];
                walk_state[e];
            }
        }
        std::vector<WalkOut> outs(walks.size());
        pool->For(walks.size(), [&](size_t k) { Walk(*walks[k], outs[k]); }, 8);
        for (const WalkOut& o : outs) {
            for (const auto& [t, prov] : o.pending) pending[t] |= prov;
            for (const auto& [t, pc] : o.refs) refs[t] = pc;
        }
        return true;
    }

    // ---- LSDA landing pads: roots of the FDE's function, and the calls of each call-site range lead to them ----
    bool Pads() {
        bool changed = false;
        for (const auto& fde : img.fdes) {
            if (!fde.has_lsda) continue;
            const auto it = fns.find(fde.start);
            if (it == fns.end()) continue;
            Function& f = it->second;
            for (const auto& cs : fde.call_sites) {
                if (!cs.landing_pad) continue;
                if (std::find(out.landing_pads.begin(), out.landing_pads.end(), cs.landing_pad) == out.landing_pads.end()) {
                    out.landing_pads.push_back(cs.landing_pad);
                }
                if (!Inside(f, cs.landing_pad)) {
                    pending[cs.landing_pad] |= kProvLsda;
                    continue;
                }
                if (f.roots.insert(cs.landing_pad).second) changed = true;
                for (const auto& [pc, target] : f.calls) {
                    if (pc >= cs.start && pc < cs.start + cs.length && !f.pads.count(pc)) {
                        f.pads[pc] = cs.landing_pad;
                        changed = true;
                    }
                }
            }
            if (changed) dirty.insert(f.entry);
        }
        return changed;
    }

    // ---- Jump tables on the lifted IR ----
    // A try of a br's regions, with what it read besides the function's walk: the effects of the calls its regions went past (their callees'
    // summaries; past calls only) and the validity of the targets it checked (a table target may be another function's entry)
    struct Tried {
        uint64_t version = 0;  // the function's walk
        uint64_t state = 0;    // its walk state (WalkState::changes)
        std::vector<std::pair<uint64_t, std::optional<lift::CallEffect>>> effects;
        std::vector<std::pair<uint64_t, bool>> valid;
    };
    struct WalkState {
        std::array<uint8_t, 32> digest{};
        uint64_t changes = 0;  // walks that changed the digest
    };
    static bool SameEffect(const std::optional<lift::CallEffect>& a, const std::optional<lift::CallEffect>& b) {
        return a.has_value() == b.has_value() && (!a || (a->x == b->x && a->v == b->v && a->sp == b->sp && a->nzcv == b->nzcv));
    }
    // Whether every fact the try read is the same now: ResolveJumpTable is a function of the walk and of those facts, so a try on the same
    // ones would come out as the last did
    bool Unchanged(const Function& f, const Tried& t) const {
        for (const auto& [pc, e] : t.effects) {
            if (!SameEffect(EffectOf(pc), e)) return false;
        }
        for (const auto& [target, v] : t.valid) {
            if (ValidTarget(f, target) != v) return false;
        }
        return true;
    }

    // The brs of one function (TablesOf's results): what goes to the whole discovery once every function is done, in function order
    struct TablesOut {
        std::vector<uint64_t> targets_outside;  // table targets that are other functions' entries
        bool found = false;
        uint64_t tries = 0, skipped = 0, regions = 0, reused = 0;
    };

    // Tries the unbounded brs of one function. It reads the image, the function, the set of functions and the summaries, and writes only the
    // function's tables, its tentative set and its brs' reasons, its own tries and its own regions: functions are tried on several threads
    // (each br's regions are lifted in an LLVM context of their own), and their results applied in function order afterwards, as they were
    void TablesOf(Function& f, const CallEffects& calls, uint64_t walk, uint64_t state, std::map<std::pair<uint64_t, bool>, Tried>& tries,
                  RegionMemo& memo, TablesOut& out_f) const {
        const bool across = static_cast<bool>(calls);
        for (auto& [br, why] : f.indirect) {
            // When a br is tried again (without calls: after a walk of its function; past calls: after every computation of the summaries,
            // which in a module with brs no region bounds, as indirect tail calls through a register are, lifted every such br's regions
            // again in every round), the try is skipped if the walk gave the same state and every call effect and target validity the
            // last try read is the same: ResolveJumpTable is a function of those, and would fail again as the last try did
            const auto key = std::make_pair(br, across);
            const auto tried_it = tries.find(key);
            if (tried_it != tries.end()) {
                const Tried& last = tried_it->second;
                if (!across && last.version == walk) continue;
                if (last.state == state && Unchanged(f, last)) {
                    ++out_f.skipped;
                    continue;
                }
            }
            Tried& rec = tries[key];
            rec = Tried{walk, state, {}, {}};
            const auto valid = [&](uint64_t t) {
                const bool v = ValidTarget(f, t);
                rec.valid.emplace_back(t, v);
                return v;
            };
            CallEffects seen;
            if (across) {
                seen = [&](uint64_t pc) {
                    const auto e = calls(pc);
                    rec.effects.emplace_back(pc, e);
                    return e;
                };
            }
            ++out_f.tries;
            TableResult r = ResolveJumpTable(img, f, br, opts, valid, seen, false, &memo);
            out_f.regions += r.regions;
            out_f.reused += r.reused;
            bool tentative = false;
            if (!r.table && !f.spec_live.empty() && !f.unverified.count(br)) {
                // The region may need code the function's walk did not reach yet (a switch in a loop: its cases branch back to the
                // dispatch): a table found while ignoring that code is kept only if it verifies once its cases are walked (Verify)
                TableResult o = ResolveJumpTable(img, f, br, opts, valid, seen, true, &memo);
                out_f.regions += o.regions;
                out_f.reused += o.reused;
                if (o.table) {
                    r = o;
                    tentative = true;
                }
            }
            if (r.table) {
                f.tables[br] = *r.table;
                for (const uint64_t t : r.table->targets) {
                    if (!Inside(f, t)) out_f.targets_outside.push_back(t);  // another function's entry: its address is taken by the table
                }
                if (tentative) f.tentative.insert(br);
                out_f.found = true;
            } else {
                why = r.why;
            }
        }
    }

    bool Tables(const CallEffects& calls) {
        // Each function's containers exist before the threads start (each thread only touches the function it took)
        struct Job {
            Function* f;
            uint64_t walk, state;
            std::map<std::pair<uint64_t, bool>, Tried>* tries;
            RegionMemo* memo;
        };
        std::vector<Job> jobs;
        for (auto& [entry, f] : fns) {
            if (!f.indirect.empty()) jobs.push_back({&f, version[entry], walk_state[entry].changes, &tried[entry], &region_memo[entry]});
        }
        std::vector<TablesOut> outs(jobs.size());
        pool->For(jobs.size(), [&](size_t i) {
            TablesOf(*jobs[i].f, calls, jobs[i].walk, jobs[i].state, *jobs[i].tries, *jobs[i].memo, outs[i]);
        }, 2, 1);
        bool found = false;
        for (size_t i = 0; i < jobs.size(); ++i) {
            const TablesOut& o = outs[i];
            for (const uint64_t t : o.targets_outside) pending[t] |= kProvJumpTable;
            if (o.found) {
                dirty.insert(jobs[i].f->entry);
                found = true;
            }
            out.table_tries += o.tries;
            out.table_tries_skipped += o.skipped;
            out.regions_lifted += static_cast<unsigned>(o.regions);
            out.regions_reused += o.reused;
        }
        return found;
    }

    // ---- Tentative tables: with their cases walked, the table must come out of a closed region that includes them, as it went in ----
    bool Verify() {
        bool changed = false;
        for (auto& [entry, f] : fns) {
            for (auto it = f.tentative.begin(); it != f.tentative.end();) {
                const uint64_t br = *it;
                const JumpTable tentative = f.tables.at(br);
                const auto valid = [&](uint64_t t) { return ValidTarget(f, t); };
                const CallEffects calls = tentative.across_calls ? CallEffects([this](uint64_t pc) { return EffectOf(pc); }) : CallEffects{};
                // Derived again with every table of the function known, this one included: the region now follows its cases back to the br
                const TableResult r = ResolveJumpTable(img, f, br, opts, valid, calls, false, &region_memo[f.entry]);
                out.regions_lifted += r.regions;
                out.regions_reused += r.reused;
                const bool same = r.table && std::all_of(r.table->targets.begin(), r.table->targets.end(), [&](uint64_t t) {
                                      return std::find(tentative.targets.begin(), tentative.targets.end(), t) != tentative.targets.end();
                                  });
                if (!same) {
                    f.tables.erase(br);
                    f.unverified.insert(br);
                    f.indirect[br] = "a table found without its cases did not verify with them (" + (r.table ? std::string("other targets") : r.why) + ")";
                    dirty.insert(entry);
                    changed = true;
                }
                it = f.tentative.erase(it);
            }
        }
        return changed;
    }

    std::optional<lift::CallEffect> EffectOf(uint64_t call_pc) const {
        const Insn& i = At(call_pc);
        if (i.kind != Kind::BL) return std::nullopt;  // blr: no single callee to take the effects of
        const auto it = fns.find(i.target);
        if (it == fns.end() || !it->second.summary.returns) return std::nullopt;
        const Summary& s = it->second.summary;
        lift::CallEffect e;
        e.x = s.writes.x | (1u << 30);
        e.v = s.writes.v;
        e.sp = s.writes.sp;
        e.nzcv = true;
        return e;
    }

    // ---- New entries from code: bl targets, tail calls, landing pads and table targets outside their function ----
    bool Pending() {
        bool added = false;
        for (const auto& [t, prov] : pending) {
            if (AddEntry(t, prov)) added = true;
            else if (fns.count(t)) fns[t].prov |= prov;
        }
        pending.clear();
        if (added) Limits();
        return added;
    }

    // ---- adr / adrp + add text addresses: a label inside code a function reaches (a table base, a resume point), else an entry ----
    bool Addresses() {
        bool added = false;
        for (const auto& [t, pc] : refs) {
            if (!TextCode(t)) continue;
            if (fns.count(t)) {
                fns[t].prov |= kProvAdr;
                continue;
            }
            Function* owner = nullptr;
            auto it = fns.upper_bound(t);
            if (it != fns.begin()) owner = &std::prev(it)->second;
            if (owner && t < owner->end && Reached(*owner, t)) continue;  // a label of code the function reaches
            if (Function* o = Owner(t)) {
                if (o->roots.insert(t).second) {  // a label inside a function's FDE or symbol range: a root of it (a resume point, a table base)
                    o->speculative.insert(t);
                    dirty.insert(o->entry);
                    added = true;
                }
                continue;
            }
            if (AddEntry(t, kProvAdr)) added = true;
        }
        if (added) Limits();
        return added;
    }

    // The function whose own range holds pc: its FDE's range, or its symbol's size when symbols are a source; never past its limit
    Function* Owner(uint64_t pc) {
        auto it = fns.upper_bound(pc);
        if (it == fns.begin()) return nullptr;
        Function& f = std::prev(it)->second;
        if (pc >= f.limit) return nullptr;
        if (f.fde >= 0 && pc < img.fdes[static_cast<size_t>(f.fde)].end) return &f;
        if (const auto e = sym_end.find(f.entry); e != sym_end.end() && pc < e->second) return &f;
        return nullptr;
    }

    static bool Reached(const Function& f, uint64_t pc) {
        auto it = f.blocks.upper_bound(pc);
        if (it == f.blocks.begin()) return false;
        --it;
        return pc >= it->second.start && pc < it->second.end;
    }

    // ---- The gap sweep: the first word after padding of every stretch no function reached becomes a candidate entry ----
    bool Gaps() {
        const std::vector<uint8_t> covered = Covered();
        // The candidates, found in chunks on the pool: they depend only on the covered words and on the words themselves (and gap_tried,
        // which only the loop below changes, for the candidate it takes)
        constexpr size_t kChunk = 1 << 14;
        const size_t n = insns.size();
        std::vector<std::vector<uint64_t>> found((n + kChunk - 1) / kChunk);
        pool->For(found.size(), [&](size_t c) {
            for (size_t i = c * kChunk; i < std::min(n, (c + 1) * kChunk); ++i) {
                const uint64_t pc = img.text_off + 4 * i;
                if (covered[i] || !Code(pc) || insns[i].word == kNop || insns[i].word == 0 || insns[i].kind == Kind::Undefined) continue;
                // One candidate per stretch no function reached, padding included (a nop inside a function, as lld's adrp + add -> nop +
                // adr relaxation leaves, is no function boundary): the first word that is no padding, after covered code, data or the
                // text's start. An undefined word ends the stretch: what follows it may be code again
                size_t j = i;
                while (j > 0 && !covered[j - 1] && Code(img.text_off + 4 * (j - 1)) && insns[j - 1].kind != Kind::Undefined &&
                       (insns[j - 1].word == kNop || insns[j - 1].word == 0)) {
                    --j;
                }
                const bool starts = j == 0 || covered[j - 1] || !Code(img.text_off + 4 * (j - 1)) || insns[j - 1].kind == Kind::Undefined;
                if (starts) found[c].push_back(pc);
            }
        });
        bool added = false;
        for (const auto& chunk : found) {
            for (const uint64_t pc : chunk) {
                if (gap_tried.count(pc)) continue;
                gap_tried.insert(pc);
                if (Function* o = Owner(pc)) {
                    // Code inside a function's own range that its walk did not reach (the cases of a table not bounded, code after a call
                    // that does not return): a root of that function, not an entry of its own
                    if (o->roots.insert(pc).second) {
                        o->speculative.insert(pc);
                        dirty.insert(o->entry);
                        added = true;
                    }
                    continue;
                }
                if (AddEntry(pc, kProvGap)) added = true;
            }
        }
        if (added) Limits();
        return added;
    }

    // The text words some function's walk reached (the aux region's blocks are not text). Functions' text blocks lie in their own
    // [entry, limit), which do not overlap: each function's are marked on the pool
    std::vector<uint8_t> Covered() {
        std::vector<uint8_t> covered(insns.size());
        std::vector<const Function*> all;
        for (const auto& [entry, f] : fns) all.push_back(&f);
        pool->For(all.size(), [&](size_t k) {
            for (const auto& [start, b] : all[k]->blocks) {
                if (!img.InText(b.start)) continue;
                for (uint64_t pc = b.start; pc < b.end; pc += 4) covered[(pc - img.text_off) / 4] = 1;
            }
        }, 256);
        return covered;
    }

    void Taken() {
        for (auto& [entry, f] : fns) {
            f.address_taken = (f.prov & kTakenProv) != 0 || miss_std.count(entry);  // a miss the dispatcher or the host entered by its address
            f.summary.exported = false;
        }
        for (const auto& s : img.symbols) {
            if (s.type == 2 && s.Defined() && (s.bind == 1 || s.bind == 2) && fns.count(s.value)) fns[s.value].summary.exported = true;
        }
    }

    void Names() {
        for (const auto& s : img.symbols) {
            if (s.type != 2 || !s.Defined()) continue;
            const auto it = fns.find(s.value);
            if (it != fns.end() && (it->second.name.empty() || s.bind != 0)) it->second.name = s.name;
        }
    }

    void Cover() {
        Coverage& c = out.coverage;
        c = {};
        std::vector<bool> covered(insns.size());
        for (const auto& [entry, f] : fns) {
            c.undefined += f.undefined;
            for (const auto& [start, b] : f.blocks) {
                if (!img.InText(b.start)) continue;  // the aux region's blocks: not text
                for (uint64_t pc = b.start; pc < b.end; pc += 4) covered[(pc - img.text_off) / 4] = true;
            }
        }
        c.text_words = insns.size();
        for (size_t i = 0; i < insns.size(); ++i) {
            const uint64_t pc = img.text_off + 4 * i;
            if (img.InData(pc)) {
                ++c.data_words;
            } else if (covered[i]) {
                ++c.covered;
            } else if (insns[i].word == kNop || insns[i].word == 0) {
                ++c.padding;
            } else {
                ++c.uncovered;
                if (!c.uncovered_ranges.empty() && c.uncovered_ranges.back().second == pc) c.uncovered_ranges.back().second = pc + 4;
                else c.uncovered_ranges.emplace_back(pc, pc + 4);
            }
        }
    }

    void Oracle() {
        if (opts.symbols) return;
        for (const auto& s : img.symbols) {
            if (s.type != 2 || !s.Defined() || !img.InText(s.value)) continue;
            OracleRow r{s.value, s.size, s.name, false, 0};
            if (const auto it = fns.find(s.value); it != fns.end()) {
                r.found = true;
                r.found_size = it->second.end - it->second.entry;
            }
            out.oracle.push_back(r);
        }
    }

    // ---- What became of the misses (Discovery::misses, miss_resumes), and the summary line and the [RED] lines of the ones a pack cannot serve ----
    void MissReport() {
        if (opts.misses.empty()) return;
        std::map<uint64_t, std::set<std::string>> by;  // address -> its reasons
        for (const Miss& m : opts.misses) by[m.offset].insert(m.reason);
        std::set<uint64_t> resumed;
        for (const uint64_t a : miss_resume) {
            if (fns.count(a)) continue;  // an entry
            const auto it = fns.upper_bound(a);
            if (it == fns.begin()) continue;
            const Function& f = std::prev(it)->second;
            if (a < f.limit && Reached(f, a)) {
                out.miss_resumes[f.entry].push_back(a);  // miss_resume is ordered: each function's list is sorted
                resumed.insert(a);
            }
        }
        size_t entries = 0, resumes = 0, unserved = 0;
        std::map<std::string, size_t> unserved_by;
        std::vector<std::string> red;
        for (const auto& [a, reasons] : by) {
            MissResult r;
            r.offset = a;
            bool code = false;  // a reason that asks for an entry or a resume point
            for (const std::string& w : reasons) {
                r.reasons += (r.reasons.empty() ? "" : ",") + w;
                code |= MissKindOf(w) != MissKind::Unserved;
            }
            const auto f = fns.find(a);
            if (code && f != fns.end()) {
                // Discovery's own sources: an entry they make (not one only a miss made, nor one a test's --no-root held back), with a standard entry when they
                // give it one (address taken, exported, the module's entry)
                const bool own = (f->second.prov & ~kProvMiss) != 0 && !opts.no_roots.count(a);
                const bool own_std = own && ((f->second.prov & kTakenProv) || f->second.summary.exported || a == img.text_off);
                r.result = !own ? "entry" : miss_std.count(a) && !own_std ? "std" : "known";
                entries += r.result != "known";
            } else if (resumed.count(a)) {
                r.result = "resume";
                resumes++;
            } else {
                r.result = "unserved";
                std::string word;
                for (const std::string& w : reasons) {
                    if (MissKindOf(w) == MissKind::Unserved) { word = w; break; }
                }
                if (code) {
                    word = *reasons.begin();
                    r.why = !img.InText(a) || (a & 3) ? "not an instruction of the text"
                          : img.InData(a)            ? "in a data block, where its control flow reaches no instruction"
                                                     : "in no function's code";
                } else {
                    r.why = word == "fpcr"           ? "msr fpcr left the FPCR the module is compiled for (design 7.1)"
                          : word == "refused"        ? "a function the lifter refused"
                          : word == "untranslated"   ? "an encoding the lifter does not translate"
                                                     : "a reason this nxaot does not know";
                }
                unserved++;
                unserved_by[word]++;
                if (red.size() < 32) red.push_back("[RED] discover: the JIT miss at " + Hex(a) + " (" + r.reasons + ") stays in the JIT fallback: " + r.why);
            }
            out.misses.push_back(std::move(r));
        }
        std::string why;
        for (const auto& [w, n] : unserved_by) why += (why.empty() ? "" : ", ") + w + " " + std::to_string(n);
        if (unserved > red.size()) red.push_back("[RED] discover: and " + std::to_string(unserved - red.size()) + " more misses the pack does not serve");
        red.insert(red.begin(), "aot: misses: " + std::to_string(by.size()) + " addresses, " + std::to_string(entries) + " new entries, " + std::to_string(resumes) +
                                    " resume points, " + std::to_string(unserved) + " still unserved (" + (why.empty() ? "none" : why) + ")");
        out.notes.insert(out.notes.begin(), red.begin(), red.end());
    }

    const module::Image& img;
    DiscoverOptions opts;
    std::shared_ptr<InsnCache> cache;  // the instruction facts the summaries read (summary.h), kept for the plan
    std::unique_ptr<Pool> pool;        // the analysis's threads (DiscoverOptions::threads)
    std::optional<std::vector<std::tuple<uint64_t, uint64_t, bool, bool>>> summarised_key;  // RunSummaries: what the last summaries saw
    Discovery out;
    std::map<uint64_t, Function>& fns = out.functions;
    std::vector<Insn> insns;
    std::vector<Insn> aux_insns;           // the aux region's words (Image::aux_off)
    std::set<uint64_t> dirty;
    bool limits_stale = false;
    std::map<uint64_t, uint32_t> pending;  // entries code names: bl and tail-call targets, landing pads and table targets outside
    std::map<uint64_t, uint64_t> refs;     // text addresses adr / adrp + add build, and where
    std::map<uint64_t, uint64_t> version;  // walks of each function
    std::map<uint64_t, std::map<std::pair<uint64_t, bool>, Tried>> tried;  // function -> (br, past calls) -> the last try
    std::map<uint64_t, WalkState> walk_state;                     // per function: its last walk's state (StateDigest)
    std::map<uint64_t, RegionMemo> region_memo;                   // per function: the jump-table regions analysed so far
    SummaryMemo summary_memo;                                     // the summaries computed so far
    std::set<uint64_t> gap_tried;
    std::map<uint64_t, uint64_t> sym_end;  // symbol entries and their ends, when symbols are a source
    std::map<uint64_t, int> fde_start;     // the FDE that starts at each address (an entry made after Sources, by a miss, gets it)
    std::set<uint64_t> miss_std;           // misses the dispatcher or the host enters by their address: a standard entry (Taken)
    std::set<uint64_t> miss_resume;        // the misses' resume points (MissRoots)
    std::vector<uint64_t> summarised;      // the functions the last summaries cover, in address order: calls to those without a return path
    std::vector<uint8_t> summarised_returns;  // do not fall through (Returns)
};

}  // namespace

MissKind MissKindOf(const std::string& reason) {
    if (reason == "indirect" || reason == "call" || reason == "host" || reason == "nocode") return MissKind::Entry;
    if (reason == "nonlocal") return MissKind::Resume;
    return MissKind::Unserved;
}

bool MissNeedsStdEntry(const std::string& reason) {
    return reason == "indirect" || reason == "host" || reason == "nonlocal";
}

std::vector<std::pair<uint64_t, uint64_t>> CarveMisses(module::Image& image, const std::vector<Miss>& misses) {
    const auto in_data = [&](uint64_t pc) { return image.InText(pc) && !(pc & 3) && image.InData(pc); };
    std::set<uint64_t> words;
    std::deque<uint64_t> work;
    for (const Miss& m : misses) {
        if (MissKindOf(m.reason) != MissKind::Unserved && in_data(m.offset)) work.push_back(m.offset);
    }
    constexpr size_t kMax = size_t{1} << 16;  // words in all: a walk that ran into real data (whose words rarely decode for long) stops here
    while (!work.empty() && words.size() < kMax) {
        uint64_t pc = work.front();
        work.pop_front();
        while (in_data(pc) && !words.count(pc) && words.size() < kMax) {
            const auto w = image.Word(pc);
            if (!w) break;
            const Insn i = ClassifyWord(*w, pc);
            if (i.kind == Kind::Undefined) break;
            words.insert(pc);
            if (i.kind == Kind::Normal || i.kind == Kind::BLR || i.kind == Kind::SVC) {
                pc += 4;
                continue;
            }
            if (i.kind == Kind::BCond || i.kind == Kind::BL) {
                work.push_back(i.target);
                pc += 4;
                continue;
            }
            if (i.kind == Kind::B) work.push_back(i.target);
            break;  // b, br, ret, a trap
        }
    }
    std::vector<std::pair<uint64_t, uint64_t>> runs;
    for (const uint64_t pc : words) {
        if (!runs.empty() && runs.back().second == pc) runs.back().second = pc + 4;
        else runs.emplace_back(pc, pc + 4);
    }
    if (runs.empty()) return runs;
    // Each data block without them (blocks may overlap: each is cut on its own)
    std::vector<module::DataBlock> blocks;
    for (const auto& b : image.data_blocks) {
        uint64_t lo = b.lo;
        for (const auto& [rlo, rhi] : runs) {
            if (rhi <= lo || rlo >= b.hi) continue;
            if (rlo > lo) blocks.push_back({lo, rlo, b.why});
            lo = std::max(lo, rhi);
        }
        if (lo < b.hi) blocks.push_back({lo, b.hi, b.why});
    }
    image.data_blocks = std::move(blocks);
    image.IndexData();
    return runs;
}

const char* ProvName(unsigned bit) {
    return bit < kProvCount ? kProvNames[bit] : "?";
}

std::string ProvList(uint32_t mask) {
    std::string s;
    for (unsigned b = 0; b < kProvCount; ++b) {
        if (mask >> b & 1) s += (s.empty() ? "" : ",") + std::string(kProvNames[b]);
    }
    return s.empty() ? "-" : s;
}

std::string RegSet::Str() const {
    std::string s;
    auto ranges = [&](uint32_t bits, char prefix, unsigned n) {
        for (unsigned i = 0; i < n;) {
            if (!(bits >> i & 1)) {
                ++i;
                continue;
            }
            unsigned j = i;
            while (j + 1 < n && (bits >> (j + 1) & 1)) ++j;
            s += (s.empty() ? "" : ",") + std::string(1, prefix) + std::to_string(i);
            if (j > i) s += "-" + std::string(1, prefix) + std::to_string(j);
            i = j + 1;
        }
    };
    ranges(x, 'x', 31);
    if (sp) s += s.empty() ? "sp" : ",sp";
    ranges(v, 'v', 32);
    if (nzcv) s += s.empty() ? "nzcv" : ",nzcv";
    return s.empty() ? "-" : s;
}

Discovery Discover(const module::Image& image, const DiscoverOptions& options) {
    const auto cpu = [] {
        timespec ts{};
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
        return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
    };
    const double c0 = cpu();
    Discovery d = Discoverer{image, options}.Run();
    d.times.cpu = cpu() - c0;
    return d;
}

std::string TimesLine(const DiscoverTimes& t) {
    char buf[1024];
    std::snprintf(buf, sizeof buf,
                  "threads %u; seconds classify %.3f walk %.3f tables %.3f (%llu tries, %llu skipped, %llu regions, %llu reused) summaries %.3f "
                  "(%u runs, %u skipped, %llu rounds, %llu sweeps, %llu computed, %llu reused, %llu words, %llu translations) sources %.3f "
                  "windows %.3f finish %.3f; CPU %.3f",
                  t.threads, t.classify, t.walk, t.tables, static_cast<unsigned long long>(t.table_tries),
                  static_cast<unsigned long long>(t.table_tries_skipped), static_cast<unsigned long long>(t.regions),
                  static_cast<unsigned long long>(t.regions_reused), t.summaries, t.summarise_runs, t.summarise_skipped,
                  static_cast<unsigned long long>(t.summary_rounds), static_cast<unsigned long long>(t.summary_sweeps),
                  static_cast<unsigned long long>(t.summary_computes), static_cast<unsigned long long>(t.summary_reused),
                  static_cast<unsigned long long>(t.distinct_words), static_cast<unsigned long long>(t.translations), t.sources, t.windows,
                  t.finish, t.cpu);
    return buf;
}

}  // namespace nxaot::analysis
