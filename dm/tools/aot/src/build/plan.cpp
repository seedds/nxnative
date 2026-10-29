#include "build/plan.h"

#include <algorithm>
#include <numeric>
#include <unordered_map>

#include "analysis/summary.h"

namespace nxaot::build {

using analysis::Edge;
using analysis::Term;
using lift::Regs;

namespace {

constexpr uint32_t kCalleeSavedV = 0x0000FF00u;  // v8..v15: AAPCS64 keeps only their low halves; a d8 restore zeroes the high half
const Regs kHostReads{0x3u, 0xFu, false, false};  // what the host reads after a guest function it called returns: x0, x1, v0..v3
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

Plan MakePlan(const module::Image& image, const analysis::Discovery& d, const lift::Convention& conv, bool all_entries) {
    Plan p;
    const auto& fns = d.functions;
    std::unordered_map<uint64_t, analysis::RegFx> fx_cache;
    const auto fx = [&](uint64_t pc) -> const analysis::RegFx& {
        const auto it = fx_cache.find(pc);
        if (it != fx_cache.end()) {
            return it->second;
        }
        const auto w = image.Word(pc);
        return fx_cache[pc] = w ? analysis::InstructionFx(pc, *w) : analysis::RegFx{};
    };
    const auto fxs = [&](uint64_t pc) { const auto& f = fx(pc); return std::make_pair(ToRegs(f.use), ToRegs(f.def)); };

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
        ff.pads = f.pads;
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

    // Every register a function or its callees write (raw): the v8..v15 among them have a new high half when it returns
    for (auto& [entry, ff] : p.fns) {
        const analysis::Function& f = fns.at(entry);
        for (const auto& [start, b] : f.blocks) {
            for (uint64_t pc = b.start; pc < b.end; pc += 4) {
                ff.raw = ff.raw | fxs(pc).second;
            }
        }
    }
    for (bool changed = true; changed;) {
        changed = false;
        Regs ind;
        for (const uint64_t e : callable) {
            ind = ind | p.fns.at(e).raw;
        }
        for (auto& [entry, ff] : p.fns) {
            const analysis::Function& f = fns.at(entry);
            Regs r = ff.raw;
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
            if (!(r == ff.raw)) {
                ff.raw = r;
                changed = true;
            }
        }
    }
    for (auto& [entry, ff] : p.fns) {
        ff.writes = ff.writes | (ff.raw & Regs::V(kCalleeSavedV));
        ff.pass = Regs::X(0xFu) | (ff.reads & conv.Passable());
    }

    // What an indirect call may reach: reads, must-writes and writes over the standard entries
    Regs ind_reads, ind_must = Regs::All(), ind_writes;
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
    p.indirect_pass = Regs::X(0xFu) | (ind_reads & conv.Passable());

    // LIVE_AFTER (section 2.3): the union over a function's call sites of what is read before being written after the call; a tail call
    // passes on its own function's; an indirect call site counts for every standard entry; the host reads x0, x1 and v0..v3
    for (auto& [entry, ff] : p.fns) {
        ff.live_after = ff.host_callable ? kHostReads : Regs{};
    }
    for (bool changed = true; changed;) {
        changed = false;
        ++p.live_after_rounds;
        std::map<uint64_t, Regs> add;
        Regs add_ind;
        for (auto& [entry, ff] : p.fns) {
            const analysis::Function& f = fns.at(entry);
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
                        const Regs out = e.kind == EndFx::Call ? after : ff.live_after;
                        if (e.indirect) {
                            live = live | (out - ind_must) | ind_reads;
                            add_ind = add_ind | out;
                        }
                        for (const uint64_t t : e.targets) {
                            Regs reads, must;
                            callee(t, reads, must);
                            live = live | (out - must) | reads;
                            add[t] = add[t] | out;
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
        }
        for (auto& [t, r] : add) {
            const auto it = p.fns.find(t);
            if (it != p.fns.end() && !((it->second.live_after | r) == it->second.live_after)) {
                it->second.live_after = it->second.live_after | r;
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
    }

    // RV: a tail call returns its callee's return value, so the v0..v3 a function returns in host registers must be the same over every chain
    // of tail calls (a component); the standard entries share one, since an indirect call cannot tell them apart. A component returns the v0..v3
    // every returning member writes; other written v registers reach callers through the block
    UnionFind uf;
    std::map<uint64_t, int> node;
    for (const auto& [entry, ff] : p.fns) {
        node[entry] = uf.Add();
    }
    const int ind_node = uf.Add();
    for (const auto& [entry, ff] : p.fns) {
        const analysis::Function& f = fns.at(entry);
        if (ff.host_callable) {
            uf.Join(node[entry], ind_node);
        }
        for (const auto& [start, b] : f.blocks) {
            const EndFx e = end_of(f, b);
            if (e.kind != EndFx::Tail) {
                continue;
            }
            if (e.indirect) {
                uf.Join(node[entry], ind_node);
            }
            for (const uint64_t t : e.targets) {
                if (node.count(t)) {
                    uf.Join(node[entry], node[t]);
                }
            }
        }
    }
    std::map<int, uint32_t> rv;
    for (const auto& [entry, ff] : p.fns) {
        const int c = uf.Find(node[entry]);
        if (!rv.count(c)) {
            rv[c] = 0xFu;
        }
        if (ff.returns) {
            rv[c] &= ff.writes.v & 0xFu;
        }
    }
    for (auto& [entry, ff] : p.fns) {
        ff.component = uf.Find(node[entry]);
        ff.rv = rv[ff.component];
    }
    p.indirect_rv = rv.count(uf.Find(ind_node)) ? rv[uf.Find(ind_node)] : 0;
    return p;
}

}  // namespace nxaot::build
