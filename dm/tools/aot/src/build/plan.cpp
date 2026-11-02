#include "build/plan.h"

#include <algorithm>
#include <memory>
#include <numeric>
#include <unordered_map>

#include "analysis/memory.h"
#include "analysis/parallel.h"
#include "analysis/summary.h"

namespace nxaot::build {

using analysis::Edge;
using analysis::Term;
using lift::Regs;

namespace {

constexpr uint32_t kCalleeSavedV = 0x0000FF00u;  // v8..v15: AAPCS64 keeps only their low halves; a d8 restore zeroes the high half
const Regs kHostReads{0x3u, 0xFu, true, false};  // what the host and the JIT read after a guest function they called returns: x0, x1, v0..v3, SP
const Regs kSvcReads = Regs::X(0xFFu);            // the SVC convention (summary.cpp): arguments in x0..x7, results in x0..x17
const Regs kSvcWrites = Regs::X(0x3FFFFu);

bool IsSetjmp(const std::string& name) {
    return name == "setjmp" || name == "_setjmp" || name == "sigsetjmp" || name == "__sigsetjmp";
}

struct UnionFind {
    std::vector<int> parent;
    int Add() {
        parent.push_back(static_cast<int>(parent.size()));
        return parent.back();
    }
    int Find(int a) {
        while (parent[static_cast<size_t>(a)] != a) {
            parent[static_cast<size_t>(a)] = parent[static_cast<size_t>(parent[static_cast<size_t>(a)])];
            a = parent[static_cast<size_t>(a)];
        }
        return a;
    }
    void Join(int a, int b) { parent[static_cast<size_t>(Find(a))] = Find(b); }
};

}  // namespace

Regs ToRegs(const analysis::RegSet& r) {
    return {r.x, r.v, r.sp, r.nzcv};
}

Regs StdPass(const lift::Convention& conv) {
    return Regs::X(0xFu) | conv.Passable();
}

Plan MakePlan(const module::Image& image, const analysis::Discovery& d, const lift::Convention& conv, bool all_entries, unsigned threads) {
    Plan p;
    const auto& fns = d.functions;
    analysis::Pool pool{threads};
    // The register effects of every instruction: discovery's cache (one translation per distinct word), or one of the plan's own; prepared
    // for every function's instructions, so that the passes below only read it, on the pool
    std::unique_ptr<analysis::InsnCache> own;
    analysis::InsnCache* insns = d.insns.get();
    if (!insns) {
        own = std::make_unique<analysis::InsnCache>(image);
        insns = own.get();
    }
    {
        std::vector<const analysis::Function*> all;
        all.reserve(fns.size());
        for (const auto& [entry, f] : fns) all.push_back(&f);
        insns->Prepare(all, &pool);
    }
    const auto fxs = [&](uint64_t pc) { const auto& f = insns->Fx(pc); return std::make_pair(ToRegs(f.use), ToRegs(f.def)); };

    std::vector<uint64_t> callable;
    for (const auto& [entry, f] : fns) {
        FnFacts& ff = p.fns[entry];
        ff.entry = entry;
        ff.limit = f.limit;
        ff.size = f.end - f.entry;
        ff.name = f.name;
        ff.words = f.words;
        ff.reads = ToRegs(f.summary.reads);
        ff.writes = ToRegs(f.summary.writes);
        ff.returns = f.summary.returns;
        ff.host_callable = all_entries || f.address_taken || f.summary.exported || entry == image.text_off;
        ff.pads = std::map<uint64_t, uint64_t>(f.pads.begin(), f.pads.end());
        for (const auto& [br, jt] : f.tables) {
            ff.tables[br] = jt.targets;
        }
        for (const auto& [call, pad] : f.pads) {
            if (pad >= f.entry && pad < f.limit && std::find(ff.roots.begin(), ff.roots.end(), pad) == ff.roots.end()) {
                ff.roots.push_back(pad);
            }
        }
        for (const auto& [pc, target] : f.calls) {
            const auto t = target ? fns.find(target) : fns.end();
            if (t != fns.end() && IsSetjmp(t->second.name) && pc + 4 < f.limit) {
                ff.continuations.push_back(pc + 4);
            }
        }
        if (ff.host_callable) {
            callable.push_back(entry);
        }
    }

    // The functions a block's end hands control to besides its successors: a callee, tail-call targets, or every standard entry (indirect)
    struct EndFx {
        enum Kind { None, Call, Tail, Svc } kind = None;
        std::vector<uint64_t> targets;  // empty with indirect = every host-callable function
        bool indirect = false;
    };
    const auto end_of = [&](const analysis::Function& f, const analysis::Block& b) {
        EndFx e;
        switch (b.term) {
        case Term::Call:
            e.kind = EndFx::Call;
            e.targets.push_back(b.target);
            break;
        case Term::CallReg:
            e.kind = EndFx::Call;
            e.indirect = true;
            break;
        case Term::Svc:
            e.kind = EndFx::Svc;
            break;
        case Term::Tail:
        case Term::CondTail:
            e.kind = EndFx::Tail;
            e.targets.push_back(b.target);
            break;
        case Term::Indirect:
            e.kind = EndFx::Tail;
            e.indirect = true;
            break;
        case Term::Out:
            if (fns.count(b.end)) {
                e.kind = EndFx::Tail;
                e.targets.push_back(b.end);
            }
            break;
        case Term::Table:
            if (const auto t = f.tables.find(b.end - 4); t != f.tables.end()) {
                for (const uint64_t target : t->second.targets) {
                    if (fns.count(target) && !f.blocks.count(target)) {
                        e.kind = EndFx::Tail;  // a case that is another function's entry: a computed tail call
                        e.targets.push_back(target);
                    }
                }
            }
            break;
        default:
            break;
        }
        return e;
    };

    // The functions in address order, with their discovery view: the passes below run over them on the pool, each writing its own facts
    std::vector<FnFacts*> order;
    std::vector<const analysis::Function*> views;
    for (auto& [entry, ff] : p.fns) {
        order.push_back(&ff);
        views.push_back(&fns.at(entry));
    }
    const size_t n = order.size();
    std::vector<uint64_t> positions;  // the entries in address order: a function's position (node_of; -1 for an address that starts none)
    positions.reserve(n);
    for (const FnFacts* ff : order) positions.push_back(ff->entry);
    const auto node_of = [&](uint64_t entry) -> int64_t {
        const auto it = std::lower_bound(positions.begin(), positions.end(), entry);
        return it != positions.end() && *it == entry ? it - positions.begin() : -1;
    };

    // Every register a function or its callees write (raw): the v8..v15 among them have a new high half when it returns. A union over the
    // call graph from each function's own writes: rounds in which every function takes the union of its callees' sets of the last round
    // (Jacobi) reach the least fixed point above them, as rounds over the functions in address order seeing this round's sets did
    pool.For(n, [&](size_t k) {
        for (const auto& [start, b] : views[k]->blocks) {
            for (uint64_t pc = b.start; pc < b.end; pc += 4) {
                order[k]->raw = order[k]->raw | fxs(pc).second;
            }
        }
    }, 64);
    std::vector<Regs> next(n);
    for (bool changed = true; changed;) {
        changed = false;
        Regs ind;
        for (const uint64_t e : callable) {
            ind = ind | p.fns.at(e).raw;
        }
        pool.For(n, [&](size_t k) {
            const analysis::Function& f = *views[k];
            Regs r = order[k]->raw;
            for (const auto& [start, b] : f.blocks) {
                const EndFx e = end_of(f, b);
                if (e.kind == EndFx::Svc) {
                    r = r | kSvcWrites;
                } else if (e.indirect) {
                    r = r | (callable.empty() ? Regs::All() : ind);
                }
                for (const uint64_t t : e.targets) {
                    const auto it = p.fns.find(t);
                    r = r | (it == p.fns.end() ? Regs::All() : it->second.raw);
                }
            }
            next[k] = r;
        }, 64);
        for (size_t k = 0; k < n; ++k) {
            if (!(next[k] == order[k]->raw)) {
                order[k]->raw = next[k];
                changed = true;
            }
        }
    }
    for (auto& [entry, ff] : p.fns) {
        ff.writes = ff.writes | (ff.raw & Regs::V(kCalleeSavedV));
        ff.pass = Regs::X(0xFu) | (ff.reads & conv.Passable());
    }

    // What an indirect call may reach: reads, must-writes and writes over the standard entries, and for the writes a function outside the module
    // too (analysis::ForeignWrites: AAPCS64's result registers), which the module's own functions' union must not narrow: the call site reloads
    // these from the block after the call (or takes them from its return value)
    Regs ind_reads, ind_must = Regs::All(), ind_writes = ToRegs(analysis::ForeignWrites());
    for (const uint64_t e : callable) {
        const auto& f = fns.at(e);
        ind_reads = ind_reads | ToRegs(f.summary.reads);
        if (f.summary.returns) {
            ind_must = ind_must & ToRegs(f.summary.must);
            ind_writes = ind_writes | p.fns.at(e).writes;
        }
    }
    if (callable.empty()) {
        ind_reads = ind_writes = Regs::All();
        ind_must = {};
    }
    p.indirect_writes = ind_writes;
    // The standard entries' signature (section 2.3: "one fixed signature"; include/nxaot/aot_module.h, ABI 3): the same in every pack of a
    // convention, so that an indirect call may enter a standard entry of another pack directly (an NRO's function from main, main's from the
    // NRO): x0..x3 and every passable register in host registers, whatever the module's address-taken functions read
    p.indirect_pass = StdPass(conv);

    // LIVE_AFTER (section 2.3): the union over a function's call sites of what is read before being written after the call; a tail call
    // passes on its own function's; an indirect call site counts for every standard entry; the host reads x0, x1 and v0..v3
    for (auto& [entry, ff] : p.fns) {
        ff.live_after = ff.host_callable ? kHostReads : Regs{};
    }
    // What a function adds to its callees' LIVE_AFTER is a function of its own LIVE_AFTER (and of facts fixed here), and LIVE_AFTER only grows
    // by unions: a function whose LIVE_AFTER did not change since its last pass would add again what it added then, and is not passed again.
    // The functions of a round are passed on the pool, each into its own additions, which are joined (unions) after the round
    std::vector<Regs> passed_with(n);
    std::vector<uint8_t> passed(n);
    struct Adds {
        std::vector<std::pair<uint64_t, Regs>> to;  // (callee, what is live after its call sites or tail calls here)
        Regs ind;                                   // the same for every standard entry (indirect sites)
    };
    std::vector<Adds> adds(n);
    std::vector<Regs> add(n);  // a round's additions, by the callee's position
    for (bool changed = true; changed;) {
        changed = false;
        ++p.live_after_rounds;
        pool.For(n, [&](size_t k) {
            FnFacts& ff = *order[k];
            Adds& out = adds[k];
            out = {};
            if (passed[k] && passed_with[k] == ff.live_after) {
                return;
            }
            passed[k] = 1;
            passed_with[k] = ff.live_after;
            const analysis::Function& f = *views[k];
            std::map<uint64_t, Regs> live_in;
            for (bool again = true; again;) {
                again = false;
                for (auto it = f.blocks.rbegin(); it != f.blocks.rend(); ++it) {
                    const analysis::Block& b = it->second;
                    Regs live, after;
                    for (const auto& [s, kind] : b.succ) {
                        const auto li = live_in.find(s);
                        if (li == live_in.end()) {
                            continue;
                        }
                        if (kind == Edge::Branch || kind == Edge::Fall || kind == Edge::Table) {
                            live = live | li->second;
                        } else {
                            after = after | li->second;  // Return, Unwind, Escape: what is live once the call or the svc is done
                        }
                    }
                    const EndFx e = end_of(f, b);
                    const auto callee = [&](uint64_t t, Regs& reads, Regs& must) {
                        const auto c = fns.find(t);
                        if (c == fns.end()) {
                            reads = Regs::All();
                            must = {};
                        } else {
                            reads = ToRegs(c->second.summary.reads);
                            must = ToRegs(c->second.summary.must);
                        }
                    };
                    if (b.term == Term::Ret) {
                        live = live | ff.live_after;
                    } else if (e.kind == EndFx::Svc) {
                        live = live | after | kSvcReads;
                    } else if (e.kind == EndFx::Call || e.kind == EndFx::Tail) {
                        const Regs out_regs = e.kind == EndFx::Call ? after : ff.live_after;
                        if (e.indirect) {
                            live = live | (out_regs - ind_must) | ind_reads;
                            out.ind = out.ind | out_regs;
                        }
                        for (const uint64_t t : e.targets) {
                            Regs reads, must;
                            callee(t, reads, must);
                            live = live | (out_regs - must) | reads;
                            out.to.emplace_back(t, out_regs);
                        }
                    }
                    for (uint64_t pc = b.end; pc > b.start;) {
                        pc -= 4;
                        const auto [use, def] = fxs(pc);
                        live = (live - def) | use;
                    }
                    if (!live_in.count(b.start) || !(live_in[b.start] == live)) {
                        live_in[b.start] = live;
                        again = true;
                    }
                }
            }
        }, 16);
        std::fill(add.begin(), add.end(), Regs{});
        Regs add_ind;
        for (const Adds& a : adds) {
            for (const auto& [t, r] : a.to) {
                if (const int64_t d = node_of(t); d >= 0) add[static_cast<size_t>(d)] = add[static_cast<size_t>(d)] | r;  // others: no function
            }
            add_ind = add_ind | a.ind;
        }
        for (size_t k = 0; k < n; ++k) {
            FnFacts& ff = *order[k];
            if (!((ff.live_after | add[k]) == ff.live_after)) {
                ff.live_after = ff.live_after | add[k];
                changed = true;
            }
        }
        for (const uint64_t e : callable) {
            FnFacts& ff = p.fns.at(e);
            if (!((ff.live_after | add_ind) == ff.live_after)) {
                ff.live_after = ff.live_after | add_ind;
                changed = true;
            }
        }
        if (memory::On()) {
            const std::string at = "LIVE_AFTER round " + std::to_string(p.live_after_rounds);
            uint64_t passes = memory::Of(order) + memory::Of(views) + memory::Of(next) + memory::Of(passed_with) + memory::Of(passed) +
                              memory::Of(adds) + memory::Of(add) + memory::Of(callable) + memory::Of(positions);
            for (const Adds& a : adds) passes += memory::Of(a.to);
            memory::Note("plan: passes (orders, the writes, LIVE_AFTER's additions)", passes, at);
            memory::Note("plan: facts per function", BytesOf(p), at);
        }
    }

    // RV: a tail call returns its callee's return value, so the v0..v3 a function returns in host registers must be the same over every chain
    // of tail calls (a component); the standard entries share one, since an indirect call cannot tell them apart. A component returns the v0..v3
    // every returning member writes; other written v registers reach callers through the block
    // The union-find's nodes: the functions' positions in address order (node_of), then the standard entries' node
    UnionFind uf;
    for (size_t k = 0; k < n; ++k) uf.Add();
    const int ind_node = uf.Add();
    for (size_t k = 0; k < n; ++k) {
        const FnFacts& ff = *order[k];
        const analysis::Function& f = *views[k];
        if (ff.host_callable) {
            uf.Join(static_cast<int>(k), ind_node);
        }
        for (const auto& [start, b] : f.blocks) {
            const EndFx e = end_of(f, b);
            if (e.kind != EndFx::Tail) {
                continue;
            }
            if (e.indirect) {
                uf.Join(static_cast<int>(k), ind_node);
            }
            for (const uint64_t t : e.targets) {
                if (const int64_t d = node_of(t); d >= 0) {
                    uf.Join(static_cast<int>(k), static_cast<int>(d));
                }
            }
        }
    }
    std::vector<uint32_t> rv(n + 1);   // per component root
    std::vector<uint8_t> has_rv(n + 1);
    for (size_t k = 0; k < n; ++k) {
        const int c = uf.Find(static_cast<int>(k));
        if (!has_rv[static_cast<size_t>(c)]) {
            has_rv[static_cast<size_t>(c)] = 1;
            rv[static_cast<size_t>(c)] = 0xFu;
        }
        if (order[k]->returns) {
            rv[static_cast<size_t>(c)] &= order[k]->writes.v & 0xFu;
        }
    }
    // The standard entries' component returns the fixed signature's v registers in host registers (kStdRv: none), whatever its members write:
    // they store what they write of v0..v3 at their returns instead (it is in LIVE_AFTER for every standard entry: kHostReads), where a caller
    // in any pack reloads it
    rv[static_cast<size_t>(uf.Find(ind_node))] = kStdRv;
    for (size_t k = 0; k < n; ++k) {
        order[k]->component = uf.Find(static_cast<int>(k));
        order[k]->rv = rv[static_cast<size_t>(order[k]->component)];
    }
    p.indirect_rv = kStdRv;
    // The blocks a function's walk reached outside its own range: the aux region's (the frame step's stubs, design 1.1), lifted as its code
    for (auto& [entry, ff] : p.fns) {
        const analysis::Function& f = fns.at(entry);
        for (const auto& [start, b] : f.blocks) {
            if (start < f.entry || start >= f.limit) ff.extra.emplace_back(b.start, b.end);
        }
    }
    if (memory::On()) {
        memory::Note("plan: tail-call components (RV)", memory::Of(uf.parent) + memory::Of(rv) + memory::Of(has_rv), "the plan's RV");
        memory::Note("plan: facts per function", BytesOf(p), "the end of the plan");
        insns->NoteMemory("the end of the plan");
        memory::Rss("the end of the plan");
    }
    return p;
}

uint64_t BytesOf(const Plan& plan) {
    uint64_t b = memory::Of(plan.fns);
    for (const auto& [entry, ff] : plan.fns) {
        b += memory::Of(ff.name) + memory::Of(ff.continuations) + memory::Of(ff.pads) + memory::Of(ff.tables) + memory::Of(ff.roots) +
             memory::Of(ff.extra);
        for (const auto& [br, targets] : ff.tables) b += memory::Of(targets);
    }
    return b;
}

}  // namespace nxaot::build
