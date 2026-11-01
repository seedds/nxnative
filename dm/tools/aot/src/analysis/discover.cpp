#include "analysis/discover.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <optional>
#include <set>
#include <thread>
#include <tuple>
#include <utility>

#include "analysis/insn.h"
#include "analysis/jumptable.h"
#include "analysis/summary.h"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/SHA256.h>

namespace nxaot::analysis {

namespace {

constexpr const char* kProvNames[kProvCount] = {"start", "fde", "sym", "init", "fini", "init_array", "fini_array",
                                                "reloc", "adr", "call", "tail", "lsda", "jt", "gap"};
constexpr uint32_t kNop = 0xD503201Fu;
constexpr uint32_t kTakenProv = kProvReloc | kProvAdr | kProvInit | kProvFini | kProvInitArray | kProvFiniArray | kProvJumpTable;

std::string Hex(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%#llx", static_cast<unsigned long long>(v));
    return buf;
}

class Discoverer {
public:
    Discoverer(const module::Image& image, const DiscoverOptions& options) : img(image), opts(options), cache(std::make_shared<InsnCache>(image)) {
        const Timer t{out.times.classify};
        const size_t n = img.text_size / 4;
        insns.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const uint64_t pc = img.text_off + 4 * i;
            insns[i] = ClassifyWord(*img.Word(pc), pc);
        }
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
            for (auto& [entry, f] : fns) {
                FindWindows(img, f);
            }
            return true;
        });
        timed(tm.finish, [&] { Names(); Cover(); Oracle(); return true; });
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
        Summarise(img, fns, cache.get(), &summary_memo, &out.times.summary_rounds, &out.times.summary_computes);
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
        auto [it, inserted] = fns.try_emplace(pc);
        it->second.entry = pc;
        it->second.prov |= prov;
        if (inserted) {
            limits_stale = true;
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
            AddEntry(fde.start, kProvFde);
            fns[fde.start].fde = static_cast<int>(i);
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
        Limits();
    }

    // ---- The walk: a function's instructions from its entry and roots, by the direct branches, inside [entry, limit) ----
    void Walk(Function& f) {
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
                    if (!branch_to(i.target)) pending[i.target] |= kProvTail;
                    break;
                case Kind::BCond:
                    if (!branch_to(i.target)) pending[i.target] |= kProvTail;
                    leaders.insert(pc + 4);
                    work.push_back(pc + 4);
                    break;
                case Kind::BL:
                    pending[i.target] |= kProvCall;
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
                    refs[i.target] = pc;
                } else if (i.add_imm && (have >> i.rn & 1)) {
                    refs[page[i.rn] + i.target] = pc;
                }
                if (i.kind == Kind::Normal) have &= ~(1u << i.rd);
                if (i.adrp) {
                    page[i.rd] = i.target;
                    have |= 1u << i.rd;
                }
            }
        }
        ++version[f.entry];
        const std::array<uint8_t, 32> digest = StateDigest(f);
        WalkState& ws = walk_state[f.entry];
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
        if (!summarised.count(callee)) return true;  // not summarised yet (a function found since): taken to return
        return fns.at(callee).summary.returns;
    }

    // After the summaries: walk again every function with a call whose fall-through disagrees with them, either way
    bool CutNoreturn() {
        summarised.clear();
        for (const auto& [entry, f] : fns) summarised.insert(entry);
        for (auto& [entry, f] : fns) {
            for (const auto& [start, b] : f.blocks) {
                if (b.term != Term::Call) continue;
                const bool has = std::any_of(b.succ.begin(), b.succ.end(), [](const auto& s) { return s.second == Edge::Return; });
                if (has != (Returns(b.target) && Inside(f, b.end))) dirty.insert(entry);
            }
        }
        return !dirty.empty();
    }

    bool WalkDirty() {
        Limits();
        if (dirty.empty()) return false;
        const std::set<uint64_t> todo = std::move(dirty);
        dirty.clear();
        for (const uint64_t e : todo) {
            if (const auto it = fns.find(e); it != fns.end()) Walk(it->second);
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
        std::atomic<size_t> next{0};
        const auto work = [&] {
            for (size_t i; (i = next.fetch_add(1)) < jobs.size();) {
                TablesOf(*jobs[i].f, calls, jobs[i].walk, jobs[i].state, *jobs[i].tries, *jobs[i].memo, outs[i]);
            }
        };
        const unsigned threads = static_cast<unsigned>(std::min<size_t>(Threads(), jobs.size()));
        if (threads <= 1) {
            work();
        } else {
            std::vector<std::thread> pool;
            for (unsigned i = 0; i < threads; ++i) pool.emplace_back(work);
            for (auto& th : pool) th.join();
        }
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

    // The threads discovery tries jump tables on (DiscoverOptions::threads; 0: one per CPU)
    unsigned Threads() const {
        if (opts.threads) return opts.threads;
        const unsigned h = std::thread::hardware_concurrency();
        return h ? h : 1;
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
        std::vector<bool> covered(insns.size());
        for (const auto& [entry, f] : fns) {
            for (const auto& [start, b] : f.blocks) {
                if (!img.InText(b.start)) continue;  // the aux region's blocks
                for (uint64_t pc = b.start; pc < b.end; pc += 4) covered[(pc - img.text_off) / 4] = true;
            }
        }
        bool added = false;
        for (size_t i = 0; i < insns.size(); ++i) {
            const uint64_t pc = img.text_off + 4 * i;
            if (covered[i] || !Code(pc) || insns[i].word == kNop || insns[i].word == 0 || insns[i].kind == Kind::Undefined) continue;
            // One candidate per stretch no function reached, padding included (a nop inside a function, as lld's adrp + add -> nop + adr
            // relaxation leaves, is no function boundary): the first word that is no padding, after covered code, data or the text's start.
            // An undefined word ends the stretch: what follows it may be code again
            size_t j = i;
            while (j > 0 && !covered[j - 1] && Code(img.text_off + 4 * (j - 1)) && insns[j - 1].kind != Kind::Undefined &&
                   (insns[j - 1].word == kNop || insns[j - 1].word == 0)) {
                --j;
            }
            const bool starts = j == 0 || covered[j - 1] || !Code(img.text_off + 4 * (j - 1)) || insns[j - 1].kind == Kind::Undefined;
            if (!starts || gap_tried.count(pc)) continue;
            gap_tried.insert(pc);
            if (Function* o = Owner(pc)) {
                // Code inside a function's own range that its walk did not reach (the cases of a table not bounded, code after a call that
                // does not return): a root of that function, not an entry of its own
                if (o->roots.insert(pc).second) {
                    o->speculative.insert(pc);
                    dirty.insert(o->entry);
                    added = true;
                }
                continue;
            }
            if (AddEntry(pc, kProvGap)) added = true;
        }
        if (added) Limits();
        return added;
    }

    void Taken() {
        for (auto& [entry, f] : fns) {
            f.address_taken = (f.prov & kTakenProv) != 0;
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

    const module::Image& img;
    DiscoverOptions opts;
    std::shared_ptr<InsnCache> cache;  // the instruction facts the summaries read (summary.h), kept for the plan
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
    std::set<uint64_t> summarised;         // the functions the last summaries cover: calls to those without a return path do not fall through
};

}  // namespace

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
    return Discoverer{image, options}.Run();
}

}  // namespace nxaot::analysis
