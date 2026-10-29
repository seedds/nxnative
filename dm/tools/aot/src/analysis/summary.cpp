#include "analysis/summary.h"

#include <algorithm>
#include <array>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "analysis/insn.h"
#include "decode/translate.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/cond.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/terminal.h"

namespace nxaot::analysis {

using namespace Dynarmic;

namespace {

constexpr uint32_t kCalleeSavedX = 0x3FF80000u;  // x19..x29
constexpr uint32_t kCalleeSavedV = 0x0000FF00u;  // v8..v15 (their low halves)

RegSet Callee() {
    RegSet r;
    r.x = kCalleeSavedX;
    r.v = kCalleeSavedV;
    r.sp = true;
    return r;
}

// The SVC convention the host answers with (dm/host/hle_svc.c, svc_dispatch(num, regs)): arguments in x0..x7, results in x0..x7; x8..x17 are
// taken as clobbered too, everything from x19 kept, as a call that preserves
RegSet SvcReads() { RegSet r; r.x = 0xFFu; return r; }
RegSet SvcWrites() { RegSet r; r.x = 0x3FFFFu; return r; }

IR::Block Translate(uint64_t pc, uint32_t word) {
    IR::Block block{A64::LocationDescriptor{pc, FP::FPCR{decode::kDefaultFpcr}}};
    decode::TranslateOne(block, pc, word);
    return block;
}

}  // namespace

RegFx InstructionFx(uint64_t pc, uint32_t word) {
    RegFx fx;
    const IR::Block block = Translate(pc, word);
    auto use_x = [&](unsigned r) { if (!(fx.def.x >> r & 1)) fx.use.x |= 1u << r; };
    auto use_v = [&](unsigned v) { if (!(fx.def.v >> v & 1)) fx.use.v |= 1u << v; };
    for (const auto& inst : block) {
        for (size_t i = 0; i < inst.NumArgs(); ++i) {
            const IR::Value a = inst.GetArg(i);
            if (a.GetType() == IR::Type::Cond && a.GetCond() != IR::Cond::AL && a.GetCond() != IR::Cond::NV && !fx.def.nzcv) {
                fx.use.nzcv = true;
            }
        }
        switch (inst.GetOpcode()) {
        case IR::Opcode::A64GetW:
        case IR::Opcode::A64GetX:
            use_x(static_cast<unsigned>(inst.GetArg(0).GetA64RegRef()));
            break;
        case IR::Opcode::A64SetW:
        case IR::Opcode::A64SetX:
            fx.def.x |= 1u << static_cast<unsigned>(inst.GetArg(0).GetA64RegRef());
            break;
        case IR::Opcode::A64GetSP:
            if (!fx.def.sp) fx.use.sp = true;
            break;
        case IR::Opcode::A64SetSP:
            fx.def.sp = true;
            break;
        case IR::Opcode::A64GetS:
        case IR::Opcode::A64GetD:
        case IR::Opcode::A64GetQ:
            use_v(static_cast<unsigned>(inst.GetArg(0).GetA64VecRef()));
            break;
        case IR::Opcode::A64SetS:
        case IR::Opcode::A64SetD:
        case IR::Opcode::A64SetQ:
            fx.def.v |= 1u << static_cast<unsigned>(inst.GetArg(0).GetA64VecRef());
            break;
        case IR::Opcode::A64GetCFlag:
        case IR::Opcode::A64GetNZCVRaw:
            if (!fx.def.nzcv) fx.use.nzcv = true;
            break;
        case IR::Opcode::A64SetNZCV:
        case IR::Opcode::A64SetNZCVRaw:
            fx.def.nzcv = true;
            break;
        default:
            break;
        }
    }
    const IR::Terminal term = block.GetTerminal();
    if (const auto* t = boost::get<IR::Term::If>(&term); t && t->if_ != IR::Cond::AL && t->if_ != IR::Cond::NV && !fx.def.nzcv) {
        fx.use.nzcv = true;
    }
    return fx;
}

namespace {

// ---- The abstract values of the preservation analysis ----

struct Abs {
    enum Kind : uint8_t { Unknown, Orig, Sp, Const } kind = Unknown;
    uint8_t reg = 0;   // Orig: x0..x30 as 0..30, the low half of v0..v31 as 32..63
    int64_t off = 0;   // Orig and Sp: an offset added to the entry value; Const: the value
    bool operator==(const Abs&) const = default;
    static Abs Of(uint8_t r) { return {Orig, r, 0}; }
    static Abs SpAt(int64_t o) { return {Sp, 0, o}; }
    static Abs K(uint64_t v) { return {Const, 0, static_cast<int64_t>(v)}; }
};

struct State {
    std::array<Abs, 31> x{};
    Abs sp;
    std::array<Abs, 32> vlo{};
    std::map<int64_t, Abs> slots;  // 8-byte stack slots by their offset from the entry SP
    bool valid = false;
    bool operator==(const State&) const = default;

    static State Entry() {
        State s;
        for (uint8_t r = 0; r < 31; ++r) s.x[r] = Abs::Of(r);
        s.sp = Abs::SpAt(0);
        for (uint8_t v = 0; v < 32; ++v) s.vlo[v] = Abs::Of(static_cast<uint8_t>(32 + v));
        s.valid = true;
        return s;
    }
    void Join(const State& o) {
        if (!o.valid) return;
        if (!valid) {
            *this = o;
            return;
        }
        for (size_t i = 0; i < 31; ++i) if (!(x[i] == o.x[i])) x[i] = {};
        if (!(sp == o.sp)) sp = {};
        for (size_t i = 0; i < 32; ++i) if (!(vlo[i] == o.vlo[i])) vlo[i] = {};
        for (auto it = slots.begin(); it != slots.end();) {
            const auto other = o.slots.find(it->first);
            if (other == o.slots.end() || !(other->second == it->second)) it = slots.erase(it);
            else ++it;
        }
    }
    // Which of x19..x29, SP and v8..v15 hold their entry values
    RegSet Restored() const {
        RegSet r;
        for (unsigned i = 19; i <= 29; ++i) if (x[i] == Abs::Of(static_cast<uint8_t>(i))) r.x |= 1u << i;
        r.sp = sp == Abs::SpAt(0);
        for (unsigned v = 8; v <= 15; ++v) if (vlo[v] == Abs::Of(static_cast<uint8_t>(32 + v))) r.v |= 1u << v;
        return r;
    }
};

Abs Add(const Abs& a, const Abs& b, bool subtract) {
    const int64_t sign = subtract ? -1 : 1;
    if (b.kind == Abs::Const && (a.kind == Abs::Sp || a.kind == Abs::Orig || a.kind == Abs::Const)) {
        Abs r = a;
        r.off = static_cast<int64_t>(static_cast<uint64_t>(a.off) + static_cast<uint64_t>(sign * b.off));
        return r;
    }
    if (!subtract && a.kind == Abs::Const && (b.kind == Abs::Sp || b.kind == Abs::Orig)) {
        Abs r = b;
        r.off = static_cast<int64_t>(static_cast<uint64_t>(b.off) + static_cast<uint64_t>(a.off));
        return r;
    }
    return {};
}

void WriteSlot(State& s, const Abs& addr, unsigned bytes, const Abs& lo, const Abs& hi) {
    if (addr.kind != Abs::Sp) {
        return;  // a store not addressed from SP: taken not to reach the save slots (the design's "no other store to those slots" is not
                 // proven for stores through other pointers)
    }
    for (auto it = s.slots.begin(); it != s.slots.end();) {
        if (it->first + 8 > addr.off && it->first < addr.off + static_cast<int64_t>(bytes)) it = s.slots.erase(it);
        else ++it;
    }
    if (bytes >= 8 && !(addr.off & 7)) {
        s.slots[addr.off] = lo;
        if (bytes == 16) s.slots[addr.off + 8] = hi;
    }
}

Abs ReadSlot(const State& s, const Abs& addr, int64_t plus) {
    if (addr.kind != Abs::Sp) return {};
    const auto it = s.slots.find(addr.off + plus);
    return it == s.slots.end() ? Abs{} : it->second;
}

// One instruction's dynarmic IR on the abstract state
void Interpret(State& s, uint64_t pc, uint32_t word) {
    const IR::Block block = Translate(pc, word);
    std::unordered_map<const IR::Inst*, std::pair<Abs, Abs>> val;
    auto get = [&](const IR::Value& v) -> std::pair<Abs, Abs> {
        if (v.IsImmediate()) {
            return {v.GetType() == IR::Type::U1 || v.GetType() == IR::Type::U8 || v.GetType() == IR::Type::U16 || v.GetType() == IR::Type::U32 ||
                            v.GetType() == IR::Type::U64
                        ? Abs::K(v.GetImmediateAsU64())
                        : Abs{},
                    Abs{}};
        }
        const auto it = val.find(v.GetInst());
        return it == val.end() ? std::pair<Abs, Abs>{} : it->second;
    };
    for (const auto& inst : block) {
        std::pair<Abs, Abs> r{};
        switch (inst.GetOpcode()) {
        case IR::Opcode::A64GetX:
            r.first = s.x[static_cast<size_t>(inst.GetArg(0).GetA64RegRef())];
            break;
        case IR::Opcode::A64GetW: {
            const Abs a = s.x[static_cast<size_t>(inst.GetArg(0).GetA64RegRef())];
            r.first = a.kind == Abs::Const ? Abs::K(static_cast<uint32_t>(a.off)) : Abs{};
            break;
        }
        case IR::Opcode::A64SetX:
            s.x[static_cast<size_t>(inst.GetArg(0).GetA64RegRef())] = get(inst.GetArg(1)).first;
            break;
        case IR::Opcode::A64SetW: {
            const Abs a = get(inst.GetArg(1)).first;
            s.x[static_cast<size_t>(inst.GetArg(0).GetA64RegRef())] = a.kind == Abs::Const ? Abs::K(static_cast<uint32_t>(a.off)) : Abs{};
            break;
        }
        case IR::Opcode::A64GetSP:
            r.first = s.sp;
            break;
        case IR::Opcode::A64SetSP:
            s.sp = get(inst.GetArg(0)).first;
            break;
        case IR::Opcode::A64GetQ:
        case IR::Opcode::A64GetD:
            r.first = s.vlo[static_cast<size_t>(inst.GetArg(0).GetA64VecRef())];
            break;
        case IR::Opcode::A64SetQ:
        case IR::Opcode::A64SetD:
            s.vlo[static_cast<size_t>(inst.GetArg(0).GetA64VecRef())] = get(inst.GetArg(1)).first;
            break;
        case IR::Opcode::A64SetS:
            s.vlo[static_cast<size_t>(inst.GetArg(0).GetA64VecRef())] = {};
            break;
        case IR::Opcode::VectorGetElement64: {
            const auto v = get(inst.GetArg(0));
            r.first = inst.GetArg(1).GetU8() == 0 ? v.first : v.second;
            break;
        }
        case IR::Opcode::ZeroExtendLongToQuad:
            r = {get(inst.GetArg(0)).first, Abs::K(0)};
            break;
        case IR::Opcode::VectorZeroUpper:  // the frontend's 64-bit register writes (V(64, ...): SetD of VectorZeroUpper)
            r = {get(inst.GetArg(0)).first, Abs::K(0)};
            break;
        case IR::Opcode::Add64:
        case IR::Opcode::Sub64: {
            const bool sub = inst.GetOpcode() == IR::Opcode::Sub64;
            const IR::Value carry = inst.GetArg(2);
            if (carry.IsImmediate() && carry.GetU1() == sub) {  // a plain add (carry 0) or subtract (carry 1)
                r.first = Add(get(inst.GetArg(0)).first, get(inst.GetArg(1)).first, sub);
            }
            break;
        }
        case IR::Opcode::A64ReadMemory64:
            r.first = ReadSlot(s, get(inst.GetArg(1)).first, 0);
            break;
        case IR::Opcode::A64ReadMemory128:
            r = {ReadSlot(s, get(inst.GetArg(1)).first, 0), ReadSlot(s, get(inst.GetArg(1)).first, 8)};
            break;
        case IR::Opcode::A64WriteMemory8:
        case IR::Opcode::A64WriteMemory16:
        case IR::Opcode::A64WriteMemory32:
        case IR::Opcode::A64WriteMemory64:
        case IR::Opcode::A64WriteMemory128:
        case IR::Opcode::A64ExclusiveWriteMemory8:
        case IR::Opcode::A64ExclusiveWriteMemory16:
        case IR::Opcode::A64ExclusiveWriteMemory32:
        case IR::Opcode::A64ExclusiveWriteMemory64:
        case IR::Opcode::A64ExclusiveWriteMemory128: {
            const IR::Opcode op = inst.GetOpcode();
            const unsigned bytes = op == IR::Opcode::A64WriteMemory8 || op == IR::Opcode::A64ExclusiveWriteMemory8 ? 1
                                   : op == IR::Opcode::A64WriteMemory16 || op == IR::Opcode::A64ExclusiveWriteMemory16 ? 2
                                   : op == IR::Opcode::A64WriteMemory32 || op == IR::Opcode::A64ExclusiveWriteMemory32 ? 4
                                   : op == IR::Opcode::A64WriteMemory64 || op == IR::Opcode::A64ExclusiveWriteMemory64 ? 8
                                                                                                                      : 16;
            const auto v = get(inst.GetArg(2));
            WriteSlot(s, get(inst.GetArg(1)).first, bytes, bytes >= 8 ? v.first : Abs{}, v.second);
            break;
        }
        default:
            break;
        }
        val[&inst] = r;
    }
}

struct CalleeFx {
    RegSet reads, writes, must;
    bool preserving = false, returns = true, known = false;
};

class Summariser {
public:
    Summariser(const module::Image& image, std::map<uint64_t, Function>& fns) : img(image), fns(fns) {
        for (auto& [entry, f] : fns) {
            f.summary.reads = {};
            f.summary.writes = {};
            f.summary.must = RegSet::All();
            f.summary.preserving = true;
            f.summary.returns = true;
            f.summary.restored = Callee();
            if (f.address_taken) taken.push_back(&f);
        }
    }

    void Run() {
        for (unsigned round = 0; round < 64; ++round) {
            bool changed = false;
            for (auto& [entry, f] : fns) {
                const Summary s = Compute(f);
                if (!(s.reads == f.summary.reads) || !(s.writes == f.summary.writes) || !(s.must == f.summary.must) ||
                    s.preserving != f.summary.preserving || s.returns != f.summary.returns || !(s.restored == f.summary.restored)) {
                    f.summary = s;
                    changed = true;
                }
            }
            if (!changed) return;
        }
    }

private:
    const RegFx& Fx(uint64_t pc) {
        const auto it = fx_cache.find(pc);
        if (it != fx_cache.end()) return it->second;
        const auto w = img.Word(pc);
        return fx_cache[pc] = w ? InstructionFx(pc, *w) : RegFx{};
    }

    CalleeFx Of(uint64_t target) const {
        CalleeFx c;
        const auto it = fns.find(target);
        if (it == fns.end()) {
            c.reads = c.writes = RegSet::All();
            return c;
        }
        const Summary& s = it->second.summary;
        return {s.reads, s.writes, s.must, s.preserving, s.returns, true};
    }
    // blr, and a br discovery could not bound: any address-taken function
    CalleeFx Indirect() const {
        CalleeFx c;
        if (taken.empty()) {
            c.reads = c.writes = RegSet::All();
            return c;
        }
        c.must = RegSet::All();
        c.preserving = true;
        c.returns = false;
        c.known = true;
        for (const Function* f : taken) {
            c.reads = c.reads | f->summary.reads;
            if (!f->summary.returns) continue;  // a callee that does not return leaves nothing to the code after the call
            c.writes = c.writes | f->summary.writes;
            c.must = c.must & f->summary.must;
            c.preserving = c.preserving && f->summary.preserving;
            c.returns = c.returns || f->summary.returns;
        }
        return c;
    }
    CalleeFx Svc() const {
        CalleeFx c;
        c.reads = SvcReads();
        c.writes = SvcWrites();
        c.preserving = true;
        c.known = true;
        return c;
    }
    // What the block's end does besides its instructions: a call, a tail call or an indirect jump
    std::optional<CalleeFx> EndFx(const Function& f, const Block& b) const {
        switch (b.term) {
        case Term::Call: return Of(b.target);
        case Term::CallReg: return Indirect();
        case Term::Svc: return Svc();
        case Term::Tail:
        case Term::CondTail: return Of(b.target);
        case Term::Indirect: return Indirect();
        case Term::Out: return fns.count(b.end) ? std::optional<CalleeFx>(Of(b.end)) : std::nullopt;
        default: (void)f; return std::nullopt;
        }
    }
    static bool Exits(const Block& b) {
        return b.term == Term::Ret || b.term == Term::Tail || b.term == Term::CondTail || b.term == Term::Indirect || b.term == Term::Out;
    }

    Summary Compute(const Function& f) {
        Summary s;
        s.exported = f.summary.exported;
        // Reads before writes: backward liveness; a call's reads are uses, its must-writes kill
        std::map<uint64_t, RegSet> live_in;
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto it = f.blocks.rbegin(); it != f.blocks.rend(); ++it) {
                const Block& b = it->second;
                RegSet live;
                for (const auto& [t, kind] : b.succ) {
                    if (kind == Edge::Return || kind == Edge::Escape || kind == Edge::Unwind) continue;  // the call's effect stands between
                    if (const auto li = live_in.find(t); li != live_in.end()) live = live | li->second;
                }
                if (const auto e = EndFx(f, b)) {
                    RegSet after;
                    for (const auto& [t, kind] : b.succ) {
                        if ((kind == Edge::Return || kind == Edge::Escape || kind == Edge::Unwind) && live_in.count(t)) after = after | live_in.at(t);
                    }
                    if (b.term == Term::Call || b.term == Term::CallReg || b.term == Term::Svc) {
                        live = live | (after - e->must) | e->reads;
                    } else {
                        live = live | e->reads;  // a tail call: the callee's reads are this function's
                    }
                }
                for (uint64_t pc = b.end; pc > b.start;) {
                    pc -= 4;
                    const RegFx& fx = Fx(pc);
                    live = (live - fx.def) | fx.use;
                }
                if (!live_in.count(b.start) || !(live_in[b.start] == live)) {
                    live_in[b.start] = live;
                    changed = true;
                }
            }
        }
        s.reads = live_in.count(f.entry) ? live_in.at(f.entry) : RegSet{};

        // Everything written, callees included
        RegSet writes;
        for (const auto& [start, b] : f.blocks) {
            for (uint64_t pc = b.start; pc < b.end; pc += 4) writes = writes | Fx(pc).def;
            if (const auto e = EndFx(f, b)) writes = writes | e->writes;
        }

        // Must-writes: forward, intersection at joins, over every exit
        std::map<uint64_t, RegSet> must_in;
        std::map<uint64_t, bool> reached;
        must_in[f.entry] = {};
        reached[f.entry] = true;
        for (const uint64_t r : f.roots) {
            if (f.speculative.count(r) && !f.spec_live.count(r)) continue;
            if (std::none_of(f.pads.begin(), f.pads.end(), [r](const auto& p) { return p.second == r; })) {
                must_in[r] = {};
                reached[r] = true;
            }
        }
        RegSet must = RegSet::All();
        bool any_exit = false;
        changed = true;
        while (changed) {
            changed = false;
            must = RegSet::All();
            any_exit = false;
            for (const auto& [start, b] : f.blocks) {
                if (!reached[start]) continue;
                RegSet m = must_in[start];
                for (uint64_t pc = b.start; pc < b.end; pc += 4) m = m | Fx(pc).def;
                const auto e = EndFx(f, b);
                if (e && (b.term == Term::Call || b.term == Term::CallReg || b.term == Term::Svc)) m = m | e->must;
                if (Exits(b)) {
                    RegSet at_exit = m;
                    if (e && b.term != Term::Ret) at_exit = at_exit | e->must;
                    must = must & at_exit;
                    any_exit = true;
                }
                for (const auto& [t, kind] : b.succ) {
                    if (!f.blocks.count(t)) continue;
                    if (!reached[t]) {
                        reached[t] = true;
                        must_in[t] = m;
                        changed = true;
                    } else {
                        const RegSet next = must_in[t] & m;
                        if (!(next == must_in[t])) {
                            must_in[t] = next;
                            changed = true;
                        }
                    }
                }
            }
        }

        // Preservation: the abstract interpretation, every exit checked
        std::map<uint64_t, State> in;
        in[f.entry] = State::Entry();
        std::set<uint64_t> pads;
        for (const auto& [call, pad] : f.pads) pads.insert(pad);
        for (const uint64_t r : f.roots) {
            if (pads.count(r)) continue;  // a landing pad: entered from its calls (Edge::Unwind), with what the unwinder restores
            if (f.speculative.count(r) && !f.spec_live.count(r)) continue;  // reached by the function's own edges now (a table's case)
            State st = State::Entry();    // a label: entered by a jump nothing in the function shows; nothing known
            for (auto& v : st.x) v = {};
            st.sp = {};
            for (auto& v : st.vlo) v = {};
            in[r].Join(st);
        }
        RegSet restored = Callee();
        bool returns = false;
        changed = true;
        unsigned guard = 0;
        while (changed && guard++ < 256) {
            changed = false;
            restored = Callee();
            returns = false;
            for (const auto& [start, b] : f.blocks) {
                if (!in[start].valid) continue;
                State st = in[start];
                for (uint64_t pc = b.start; pc < b.end; pc += 4) {
                    if (const auto w = img.Word(pc)) Interpret(st, pc, *w);
                }
                const auto e = EndFx(f, b);
                if (e && (b.term == Term::Call || b.term == Term::CallReg || b.term == Term::Svc)) {
                    for (unsigned r = 0; r < 31; ++r) {
                        const bool saved = (kCalleeSavedX >> r & 1) && e->preserving;
                        if ((e->writes.x >> r & 1) && !saved) st.x[r] = {};
                    }
                    for (unsigned v = 0; v < 32; ++v) {
                        const bool saved = (kCalleeSavedV >> v & 1) && e->preserving;
                        if ((e->writes.v >> v & 1) && !saved) st.vlo[v] = {};
                    }
                    if (e->writes.sp && !e->preserving) st.sp = {};
                }
                if (Exits(b)) {
                    // ret / br x30 returns to the entry's x30 only when x30 holds it; anything else (longjmp's br x30) is a non-local transfer
                    bool ok = true;
                    if (b.term == Term::Ret) {
                        const auto w = img.Word(b.end - 4);
                        const unsigned rn = w ? (*w >> 5) & 31 : 30;
                        ok = st.x[rn] == Abs::Of(static_cast<uint8_t>(30));
                    } else if (e && !e->preserving) {
                        ok = false;
                    }
                    // A return path: a ret, or a tail call to a callee that returns (to this function's caller). Restored only where the
                    // path keeps the callee-saved state and the tail callee preserves it; a ret elsewhere (longjmp's br x30) restores nothing
                    const bool path_returns = b.term == Term::Ret || (e && e->returns);
                    if (path_returns) {
                        returns = true;
                        restored = ok ? restored & st.Restored() : RegSet{};
                    }
                }
                for (const auto& [t, kind] : b.succ) {
                    if (!f.blocks.count(t)) continue;
                    State next = in[t];
                    State from = st;
                    if (kind == Edge::Unwind) from.x[0] = from.x[1] = {};  // the exception object and the selector the personality sets
                    next.Join(from);
                    if (!(next == in[t])) {
                        in[t] = next;
                        changed = true;
                    }
                }
            }
        }
        s.restored = returns ? restored : RegSet{};
        s.returns = returns;
        s.preserving = returns && s.restored == Callee();
        s.writes = writes - s.restored;
        s.must = any_exit ? must - s.restored : RegSet{};
        return s;
    }

    const module::Image& img;
    std::map<uint64_t, Function>& fns;
    std::vector<const Function*> taken;
    std::unordered_map<uint64_t, RegFx> fx_cache;
};

// ---- Exclusive windows ----

constexpr unsigned kWindowLimit = 32;

bool IsRead(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::A64ReadMemory8: case IR::Opcode::A64ReadMemory16: case IR::Opcode::A64ReadMemory32: case IR::Opcode::A64ReadMemory64:
    case IR::Opcode::A64ReadMemory128: case IR::Opcode::A64ExclusiveReadMemory8: case IR::Opcode::A64ExclusiveReadMemory16:
    case IR::Opcode::A64ExclusiveReadMemory32: case IR::Opcode::A64ExclusiveReadMemory64: case IR::Opcode::A64ExclusiveReadMemory128:
        return true;
    default:
        return false;
    }
}

// Taint through one instruction (as the census follows windows, census.cpp): x registers derived from the exclusive load's result
void Taint(uint64_t pc, uint32_t w, uint32_t& tainted, bool seed, bool& load_through, bool& other_load) {
    const IR::Block block = Translate(pc, w);
    std::unordered_set<const IR::Inst*> t;
    auto is = [&](const IR::Value& v) { return !v.IsImmediate() && t.count(v.GetInst()); };
    for (const auto& inst : block) {
        bool any = false;
        for (size_t i = 0; i < inst.NumArgs(); ++i) any = any || is(inst.GetArg(i));
        switch (inst.GetOpcode()) {
        case IR::Opcode::A64GetW:
        case IR::Opcode::A64GetX:
            if (tainted >> static_cast<unsigned>(inst.GetArg(0).GetA64RegRef()) & 1) t.insert(&inst);
            break;
        case IR::Opcode::A64SetW:
        case IR::Opcode::A64SetX: {
            const uint32_t bit = 1u << static_cast<unsigned>(inst.GetArg(0).GetA64RegRef());
            tainted = is(inst.GetArg(1)) ? tainted | bit : tainted & ~bit;
            break;
        }
        default:
            if (IsRead(inst.GetOpcode())) {
                const bool exclusive = inst.GetOpcode() >= IR::Opcode::A64ExclusiveReadMemory8 && inst.GetOpcode() <= IR::Opcode::A64ExclusiveReadMemory128;
                if (seed && exclusive) {
                    t.insert(&inst);
                } else if (is(inst.GetArg(1))) {
                    load_through = true;
                    t.insert(&inst);
                } else {
                    other_load = true;
                }
            } else if (any) {
                t.insert(&inst);
            }
            break;
        }
    }
}

}  // namespace

void Summarise(const module::Image& image, std::map<uint64_t, Function>& functions) {
    Summariser{image, functions}.Run();
}

void FindWindows(const module::Image& image, Function& fn) {
    fn.windows.clear();
    fn.unpaired_exclusive = 0;
    std::set<uint64_t> covered;
    for (const auto& [start, b] : fn.blocks) {
        for (uint64_t pc = b.start; pc < b.end; pc += 4) covered.insert(pc);
    }
    for (const uint64_t pc : covered) {
        const auto w = image.Word(pc);
        if (!w) continue;
        const Insn i = ClassifyWord(*w, pc);
        if (!i.excl_load) continue;
        uint32_t tainted = 0;
        bool through = false, other = false, closed = false;
        Taint(pc, *w, tainted, true, through, other);
        uint64_t end = pc;
        for (uint64_t q = pc + 4; q <= pc + 4 * kWindowLimit && covered.count(q); q += 4) {
            const auto qw = image.Word(q);
            if (!qw) break;
            const Insn qi = ClassifyWord(*qw, q);
            if (qi.excl_load) break;  // a window never spans a second exclusive load (rewriter.md, Exclusive windows)
            if (qi.excl_store) {
                closed = true;
                end = q + 4;
                break;
            }
            if (qi.kind != Kind::Normal && qi.kind != Kind::BCond) break;  // conditional branches fall through, as the rewriter follows windows
            Taint(q, *qw, tainted, false, through, other);
        }
        if (!closed) {
            ++fn.unpaired_exclusive;
            continue;
        }
        fn.windows.push_back({pc, end, through ? "aba" : other ? "load" : "cas"});
    }
}

}  // namespace nxaot::analysis
