#include "analysis/discover.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

#include "analysis/insn.h"
#include "analysis/jumptable.h"
#include "analysis/summary.h"

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
    Discoverer(const module::Image& image, const DiscoverOptions& options) : img(image), opts(options) {
        const size_t n = img.text_size / 4;
        insns.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const uint64_t pc = img.text_off + 4 * i;
            insns[i] = ClassifyWord(*img.Word(pc), pc);
        }
    }

    Discovery Run() {
        Sources();
        for (out.rounds = 1; out.rounds < 10000; ++out.rounds) {
            if (WalkDirty()) continue;
            if (Verify()) continue;
            if (Pads()) continue;
            if (Tables({})) continue;
            if (Pending()) continue;
            if (Addresses()) continue;
            // The code-derived sources are exhausted. The summaries now let regions continue past calls, for the brs that a region without
            // calls did not bound; only then the gap sweep, so that cases of a table not bounded yet do not become entries of their own
            Taken();
            Summarise(img, fns);
            if (CutNoreturn()) continue;
            if (Tables([this](uint64_t pc) { return EffectOf(pc); })) continue;
            if (Gaps()) continue;
            break;
        }
        Taken();
        Summarise(img, fns);
        for (auto& [entry, f] : fns) {
            FindWindows(img, f);
        }
        Names();
        Cover();
        Oracle();
        return std::move(out);
    }

private:
    // ---- Helpers ----
    bool Code(uint64_t pc) const { return img.InText(pc) && !(pc & 3) && !img.InData(pc); }
    const Insn& At(uint64_t pc) const { return insns[(pc - img.text_off) / 4]; }
    bool Inside(const Function& f, uint64_t pc) const { return pc >= f.entry && pc < f.limit && Code(pc); }
    // A jump-table target: a decoded instruction of the function, or the entry of another function (a computed tail call: a table of pointers)
    bool ValidTarget(const Function& f, uint64_t t) const { return Code(t) && At(t).kind != Kind::Undefined && (Inside(f, t) || fns.count(t)); }

    bool AddEntry(uint64_t pc, uint32_t prov) {
        if (!Code(pc)) {
            return false;
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
            if (!Code(fde.start)) {
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
        f.end = covered.empty() ? f.entry : *covered.rbegin() + 4;
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
    bool Tables(const CallEffects& calls) {
        bool found = false;
        const bool across = static_cast<bool>(calls);
        const unsigned round = across ? summary_round : 0;  // regions past calls are tried again whenever the summaries were computed again
        for (auto& [entry, f] : fns) {
            for (auto& [br, why] : f.indirect) {
                const auto key = std::make_tuple(entry, br, across);
                const auto tried_it = tried.find(key);
                if (tried_it != tried.end() && tried_it->second == std::make_pair(version[entry], round)) continue;
                tried[key] = {version[entry], round};
                const auto valid = [&](uint64_t t) { return ValidTarget(f, t); };
                TableResult r = ResolveJumpTable(img, f, br, opts, valid, calls, false);
                out.regions_lifted += r.regions;
                bool tentative = false;
                if (!r.table && !f.spec_live.empty() && !f.unverified.count(br)) {
                    // The region may need code the function's walk did not reach yet (a switch in a loop: its cases branch back to the
                    // dispatch): a table found while ignoring that code is kept only if it verifies once its cases are walked (Verify)
                    TableResult o = ResolveJumpTable(img, f, br, opts, valid, calls, true);
                    out.regions_lifted += o.regions;
                    if (o.table) {
                        r = o;
                        tentative = true;
                    }
                }
                if (r.table) {
                    f.tables[br] = *r.table;
                    for (const uint64_t t : r.table->targets) {
                        if (!Inside(f, t)) pending[t] |= kProvJumpTable;  // another function's entry: its address is taken by the table
                    }
                    if (tentative) f.tentative.insert(br);
                    dirty.insert(entry);
                    found = true;
                } else {
                    why = r.why;
                }
            }
        }
        if (across) ++summary_round;
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
                const TableResult r = ResolveJumpTable(img, f, br, opts, valid, calls, false);
                out.regions_lifted += r.regions;
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
            if (!Code(t)) continue;
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
    Discovery out;
    std::map<uint64_t, Function>& fns = out.functions;
    std::vector<Insn> insns;
    std::set<uint64_t> dirty;
    bool limits_stale = false;
    std::map<uint64_t, uint32_t> pending;  // entries code names: bl and tail-call targets, landing pads and table targets outside
    std::map<uint64_t, uint64_t> refs;     // text addresses adr / adrp + add build, and where
    std::map<uint64_t, uint64_t> version;  // walks of each function
    std::map<std::tuple<uint64_t, uint64_t, bool>, std::pair<uint64_t, unsigned>> tried;  // (function, br, past calls) -> (walk, summary round)
    unsigned summary_round = 0;
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
