#include "analysis/summary.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

// ---- The preservation analysis' view of one instruction, compiled once per distinct word (InsnCache) ----
// Interpret above, on dynarmic's IR of the instruction, reads only the opcodes below; every other instruction's value is unknown. A program is
// those operations in order, each operand the result of an earlier one, an immediate, or unknown; operations whose values no effect reads are
// dropped. Running it on a state does what Interpret does on the same IR

enum class AK : uint8_t { GetX, GetW, SetX, SetW, GetSP, SetSP, GetV, SetV, ClearV, Elem64, Zext, Add, Sub, Read64, Read128, Write };

struct AOp {
    AK kind = AK::GetX;
    uint8_t reg = 0;         // GetX..ClearV: the register; Elem64: the lane; Write: the bytes
    int32_t a = -1, b = -1;  // operands: >= 0 the result of operation a (b); -1 unknown; <= -2 the immediate -2 - a (-2 - b)
};

enum class Rel : uint8_t {
    None,  // the value
    Pc,    // the pc plus value (adr, bl's return address, literal addresses)
    Page,  // the pc's 4 KiB page plus value (adrp)
};

struct AImm {
    uint64_t value = 0;
    Rel rel = Rel::None;
    uint64_t At(uint64_t pc) const { return rel == Rel::None ? value : rel == Rel::Pc ? pc + value : (pc & ~0xFFFull) + value; }
};

struct Program {
    std::vector<AOp> ops;
    std::vector<AImm> imms;
    // The same operations, operands and number of immediates (their values aside)
    bool SameShape(const Program& o) const {
        if (ops.size() != o.ops.size() || imms.size() != o.imms.size()) return false;
        for (size_t i = 0; i < ops.size(); ++i) {
            if (ops[i].kind != o.ops[i].kind || ops[i].reg != o.ops[i].reg || ops[i].a != o.ops[i].a || ops[i].b != o.ops[i].b) return false;
        }
        return true;
    }
};

bool HasValue(AK k) {
    return k != AK::SetX && k != AK::SetW && k != AK::SetSP && k != AK::SetV && k != AK::ClearV && k != AK::Write;
}

AOp Op(AK kind, unsigned reg = 0, int32_t a = -1, int32_t b = -1) {
    AOp op;
    op.kind = kind;
    op.reg = static_cast<uint8_t>(reg);
    op.a = a;
    op.b = b;
    return op;
}

Program Compile(const IR::Block& block) {
    Program p;
    std::unordered_map<const IR::Inst*, int32_t> index;  // the instructions whose value an operation gives
    const auto operand = [&](const IR::Value& v) -> int32_t {
        if (v.IsImmediate()) {
            const IR::Type t = v.GetType();
            if (t != IR::Type::U1 && t != IR::Type::U8 && t != IR::Type::U16 && t != IR::Type::U32 && t != IR::Type::U64) return -1;
            p.imms.push_back({v.GetImmediateAsU64(), Rel::None});
            return -2 - static_cast<int32_t>(p.imms.size() - 1);
        }
        const auto it = index.find(v.GetInst());
        return it == index.end() ? -1 : it->second;
    };
    const auto reg = [](const IR::Value& v) { return static_cast<unsigned>(v.GetA64RegRef()); };
    const auto vec = [](const IR::Value& v) { return static_cast<unsigned>(v.GetA64VecRef()); };
    for (const auto& inst : block) {
        AOp op;
        bool emit = true;
        switch (inst.GetOpcode()) {
        case IR::Opcode::A64GetX: op = Op(AK::GetX, reg(inst.GetArg(0))); break;
        case IR::Opcode::A64GetW: op = Op(AK::GetW, reg(inst.GetArg(0))); break;
        case IR::Opcode::A64SetX: op = Op(AK::SetX, reg(inst.GetArg(0)), operand(inst.GetArg(1))); break;
        case IR::Opcode::A64SetW: op = Op(AK::SetW, reg(inst.GetArg(0)), operand(inst.GetArg(1))); break;
        case IR::Opcode::A64GetSP: op = Op(AK::GetSP); break;
        case IR::Opcode::A64SetSP: op = Op(AK::SetSP, 0, operand(inst.GetArg(0))); break;
        case IR::Opcode::A64GetQ:
        case IR::Opcode::A64GetD: op = Op(AK::GetV, vec(inst.GetArg(0))); break;
        case IR::Opcode::A64SetQ:
        case IR::Opcode::A64SetD: op = Op(AK::SetV, vec(inst.GetArg(0)), operand(inst.GetArg(1))); break;
        case IR::Opcode::A64SetS: op = Op(AK::ClearV, vec(inst.GetArg(0))); break;
        case IR::Opcode::VectorGetElement64: op = Op(AK::Elem64, inst.GetArg(1).GetU8(), operand(inst.GetArg(0))); break;
        case IR::Opcode::ZeroExtendLongToQuad:
        case IR::Opcode::VectorZeroUpper: op = Op(AK::Zext, 0, operand(inst.GetArg(0))); break;
        case IR::Opcode::Add64:
        case IR::Opcode::Sub64: {
            const bool sub = inst.GetOpcode() == IR::Opcode::Sub64;
            const IR::Value carry = inst.GetArg(2);
            if (carry.IsImmediate() && carry.GetU1() == sub) {
                op = Op(sub ? AK::Sub : AK::Add, 0, operand(inst.GetArg(0)), operand(inst.GetArg(1)));
            } else {
                emit = false;  // not a plain add or subtract: its value is unknown, as Interpret leaves it
            }
            break;
        }
        case IR::Opcode::A64ReadMemory64: op = Op(AK::Read64, 0, operand(inst.GetArg(1))); break;
        case IR::Opcode::A64ReadMemory128: op = Op(AK::Read128, 0, operand(inst.GetArg(1))); break;
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
            const IR::Opcode o = inst.GetOpcode();
            const unsigned bytes = o == IR::Opcode::A64WriteMemory8 || o == IR::Opcode::A64ExclusiveWriteMemory8 ? 1
                                   : o == IR::Opcode::A64WriteMemory16 || o == IR::Opcode::A64ExclusiveWriteMemory16 ? 2
                                   : o == IR::Opcode::A64WriteMemory32 || o == IR::Opcode::A64ExclusiveWriteMemory32 ? 4
                                   : o == IR::Opcode::A64WriteMemory64 || o == IR::Opcode::A64ExclusiveWriteMemory64 ? 8
                                                                                                                  : 16;
            op = Op(AK::Write, bytes, operand(inst.GetArg(1)), operand(inst.GetArg(2)));
            break;
        }
        default:
            emit = false;
            break;
        }
        if (!emit) continue;
        if (HasValue(op.kind)) index[&inst] = static_cast<int32_t>(p.ops.size());
        p.ops.push_back(op);
    }
    // Operations whose values no effect reads are dropped, the others renumbered
    std::vector<bool> live(p.ops.size());
    for (size_t i = p.ops.size(); i-- > 0;) {
        if (!HasValue(p.ops[i].kind)) live[i] = true;
        if (!live[i]) continue;
        if (p.ops[i].a >= 0) live[static_cast<size_t>(p.ops[i].a)] = true;
        if (p.ops[i].b >= 0) live[static_cast<size_t>(p.ops[i].b)] = true;
    }
    std::vector<int32_t> renumber(p.ops.size(), -1);
    std::vector<AOp> kept;
    for (size_t i = 0; i < p.ops.size(); ++i) {
        if (!live[i]) continue;
        AOp op = p.ops[i];
        if (op.a >= 0) op.a = renumber[static_cast<size_t>(op.a)];
        if (op.b >= 0) op.b = renumber[static_cast<size_t>(op.b)];
        renumber[i] = static_cast<int32_t>(kept.size());
        kept.push_back(op);
    }
    p.ops = std::move(kept);
    return p;
}

// Runs a program on the abstract state, for the instruction at pc
void Run(State& s, const Program& p, uint64_t pc) {
    std::pair<Abs, Abs> small[24];
    std::vector<std::pair<Abs, Abs>> big;
    std::pair<Abs, Abs>* val = small;
    if (p.ops.size() > 24) {
        big.resize(p.ops.size());
        val = big.data();
    }
    const auto get = [&](int32_t r) -> std::pair<Abs, Abs> {
        if (r >= 0) return val[r];
        if (r == -1) return {};
        const AImm& imm = p.imms[static_cast<size_t>(-2 - r)];
        return {Abs::K(imm.At(pc)), Abs{}};
    };
    for (size_t i = 0; i < p.ops.size(); ++i) {
        const AOp& op = p.ops[i];
        std::pair<Abs, Abs> r{};
        switch (op.kind) {
        case AK::GetX: r.first = s.x[op.reg]; break;
        case AK::GetW: {
            const Abs a = s.x[op.reg];
            r.first = a.kind == Abs::Const ? Abs::K(static_cast<uint32_t>(a.off)) : Abs{};
            break;
        }
        case AK::SetX: s.x[op.reg] = get(op.a).first; break;
        case AK::SetW: {
            const Abs a = get(op.a).first;
            s.x[op.reg] = a.kind == Abs::Const ? Abs::K(static_cast<uint32_t>(a.off)) : Abs{};
            break;
        }
        case AK::GetSP: r.first = s.sp; break;
        case AK::SetSP: s.sp = get(op.a).first; break;
        case AK::GetV: r.first = s.vlo[op.reg]; break;
        case AK::SetV: s.vlo[op.reg] = get(op.a).first; break;
        case AK::ClearV: s.vlo[op.reg] = {}; break;
        case AK::Elem64: {
            const auto v = get(op.a);
            r.first = op.reg == 0 ? v.first : v.second;
            break;
        }
        case AK::Zext: r = {get(op.a).first, Abs::K(0)}; break;
        case AK::Add:
        case AK::Sub: r.first = Add(get(op.a).first, get(op.b).first, op.kind == AK::Sub); break;
        case AK::Read64: r.first = ReadSlot(s, get(op.a).first, 0); break;
        case AK::Read128: r = {ReadSlot(s, get(op.a).first, 0), ReadSlot(s, get(op.a).first, 8)}; break;
        case AK::Write: {
            const auto v = get(op.b);
            WriteSlot(s, get(op.a).first, op.reg, op.reg >= 8 ? v.first : Abs{}, v.second);
            break;
        }
        }
        val[i] = r;
    }
}

// The displacements of the second and third translations, as the lifter finds pc-relative immediates (lower_calls.cpp, FnRelative): neither is
// a multiple of 2^32, so that an immediate the pc reaches only in part (truncated) moves by another amount and is not taken as relative; the
// first keeps the pc's place in its page and the second does not, which tells the pc (adr) from its page (adrp)
constexpr uint64_t kProbeDelta = (1ull << 44) + 0x1000;
constexpr uint64_t kProbeDelta2 = (1ull << 45) + 0x2004;

}  // namespace

struct InsnCache::Impl {
    struct Entry {
        RegFx fx;
        Program program;
        bool relative_ok = true;  // the program holds at every pc (pc-relative immediates added); false: translated at every use
    };
    const module::Image& img;
    std::vector<uint32_t> ids;                       // per text word: 1 + its entry, 0 when not seen yet
    std::unordered_map<uint32_t, uint32_t> by_word;  // word -> entry
    std::vector<Entry> entries;
    std::unordered_map<uint64_t, RegFx> outside;     // words outside the text (not expected): InstructionFx of each
    uint64_t translations = 0;
    uint64_t checked = 0;  // NXAOT_CHECK_PROGRAMS: programs run beside Interpret

    explicit Impl(const module::Image& image) : img(image), ids(image.text_size / 4) {}
    ~Impl() {
        if (checked) std::fprintf(stderr, "nxaot: %llu instructions interpreted both ways, the same\n", static_cast<unsigned long long>(checked));
    }

    const Entry* At(uint64_t pc) {
        if (!img.InText(pc) || (pc & 3) || (pc - img.text_off) / 4 >= ids.size()) return nullptr;
        uint32_t& id = ids[(pc - img.text_off) / 4];
        if (!id) {
            const uint32_t w = *img.Word(pc);
            const auto [it, inserted] = by_word.try_emplace(w, static_cast<uint32_t>(entries.size()));
            if (inserted) entries.push_back(Make(pc, w));
            id = it->second + 1;
        }
        return &entries[id - 1];
    }

    // The word's facts from its translations at pc and at two other addresses: every immediate the same at all three is a constant, one that
    // moved with the pc is pc-relative, one that moved with the pc's page is page-relative; anything else (or another shape) is translated at
    // every use instead
    Entry Make(uint64_t pc, uint32_t w) {
        Entry e;
        e.fx = InstructionFx(pc, w);
        const Program a = Compile(Translate(pc, w));
        const uint64_t pc1 = pc + kProbeDelta, pc2 = pc + kProbeDelta2;
        const Program b = Compile(Translate(pc1, w));
        const Program c = Compile(Translate(pc2, w));
        translations += 4;
        e.program = a;
        if (!a.SameShape(b) || !a.SameShape(c)) {
            e.relative_ok = false;
            return e;
        }
        const uint64_t page = pc & ~0xFFFull;
        for (size_t i = 0; i < a.imms.size(); ++i) {
            const uint64_t va = a.imms[i].value, vb = b.imms[i].value, vc = c.imms[i].value;
            if (va == vb && va == vc) continue;
            if (vb - va == pc1 - pc && vc - va == pc2 - pc) {
                e.program.imms[i] = {va - pc, Rel::Pc};
            } else if (vb - va == (pc1 & ~0xFFFull) - page && vc - va == (pc2 & ~0xFFFull) - page) {
                e.program.imms[i] = {va - page, Rel::Page};
            } else {
                e.relative_ok = false;
                return e;
            }
        }
        return e;
    }

    const RegFx& Fx(uint64_t pc) {
        if (const Entry* e = At(pc)) return e->fx;
        const auto it = outside.find(pc);
        if (it != outside.end()) return it->second;
        const auto w = img.Word(pc);
        ++translations;
        return outside[pc] = w ? InstructionFx(pc, *w) : RegFx{};
    }

    // Interpret(s, pc, the word at pc), from the cached program. NXAOT_CHECK_PROGRAMS=1 runs Interpret on dynarmic's IR beside it and stops
    // at the first state that differs (the check of this cache's equivalence, README.md "Pack throughput")
    void Interpret(State& s, uint64_t pc) {
        static const bool check = std::getenv("NXAOT_CHECK_PROGRAMS") != nullptr;
        const Entry* e = At(pc);
        if (e && e->relative_ok) {
            if (check) {
                State ref = s;
                analysis::Interpret(ref, pc, *img.Word(pc));
                Run(s, e->program, pc);
                if (!(ref == s)) {
                    std::fprintf(stderr, "[RED] nxaot: the cached program of %#llx (word %08x) differs from its IR\n",
                                 static_cast<unsigned long long>(pc), *img.Word(pc));
                    std::abort();
                }
                ++checked;
                return;
            }
            Run(s, e->program, pc);
            return;
        }
        if (const auto w = img.Word(pc)) {
            ++translations;
            Run(s, Compile(Translate(pc, *w)), pc);
        }
    }
};

InsnCache::InsnCache(const module::Image& image) : impl(std::make_unique<Impl>(image)) {}
InsnCache::~InsnCache() = default;
const RegFx& InsnCache::Fx(uint64_t pc) {
    return impl->Fx(pc);
}
uint64_t InsnCache::Distinct() const {
    return impl->entries.size();
}
uint64_t InsnCache::Translations() const {
    return impl->translations;
}

namespace {

struct CalleeFx {
    RegSet reads, writes, must;
    bool preserving = false, returns = true, known = false;
};

class Summariser {
public:
    Summariser(const module::Image& image, std::map<uint64_t, Function>& fns, InsnCache& cache, SummaryMemo* memo)
            : img(image), fns(fns), cache(cache.Internal()), memo(memo) {
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

    // The fixed point, round by round over every function in address order, each round seeing the summaries the earlier functions of the
    // same round got (as it always did). A function is computed again only when a summary it reads changed since its last computation: a
    // callee's, or for blr and unbounded br any address-taken function's; Compute is a function of those and of the function's own code, which
    // does not change during a run, so a skipped computation would give the summary the function has. The rounds and their results are those
    // of computing every function in every round
    void Run(uint64_t* rounds, uint64_t* computes) {
        std::vector<Function*> order;
        std::vector<uint64_t> entries;
        for (auto& [entry, f] : fns) {
            order.push_back(&f);
            entries.push_back(entry);
        }
        const size_t n = order.size();
        const auto index = [&](uint64_t entry) -> int64_t {
            const auto it = std::lower_bound(entries.begin(), entries.end(), entry);
            return it != entries.end() && *it == entry ? it - entries.begin() : -1;
        };
        std::vector<std::vector<uint32_t>> deps(n);
        std::vector<bool> uses_indirect(n), is_taken(n);
        for (size_t i = 0; i < n; ++i) {
            const Function& f = *order[i];
            is_taken[i] = f.address_taken;
            for (const auto& [start, b] : f.blocks) {
                uint64_t target = 0;
                switch (b.term) {
                case Term::Call:
                case Term::Tail:
                case Term::CondTail: target = b.target; break;
                case Term::Out: target = b.end; break;
                case Term::CallReg:
                case Term::Indirect: uses_indirect[i] = true; continue;
                default: continue;
                }
                if (const int64_t d = index(target); d >= 0) deps[i].push_back(static_cast<uint32_t>(d));
            }
            std::sort(deps[i].begin(), deps[i].end());
            deps[i].erase(std::unique(deps[i].begin(), deps[i].end()), deps[i].end());
        }
        std::vector<uint64_t> computed_at(n, 0), changed_at(n, 0);
        for (unsigned round = 0; round < 64; ++round) {
            bool changed = false;
            if (rounds) ++*rounds;
            for (size_t i = 0; i < n; ++i) {
                bool need = computed_at[i] == 0 || (uses_indirect[i] && taken_changed > computed_at[i]);
                for (size_t k = 0; !need && k < deps[i].size(); ++k) need = changed_at[deps[i][k]] > computed_at[i];
                if (!need) continue;
                Function& f = *order[i];
                computed_at[i] = ++tick;
                Summary s;
                std::vector<uint64_t> inputs;
                const Summary* known = nullptr;
                if (memo) {
                    inputs = Inputs(f, deps[i], entries, uses_indirect[i]);
                    for (const auto& [in, out] : memo->by_function[f.entry]) {
                        if (in == inputs) known = &out;
                    }
                }
                if (known) {
                    s = *known;
                    ++memo->hits;
                } else {
                    if (computes) ++*computes;
                    s = Compute(f);
                    if (memo) {
                        auto& seen = memo->by_function[f.entry];
                        if (seen.size() >= kMemoPerFunction) seen.erase(seen.begin());
                        seen.emplace_back(std::move(inputs), s);
                    }
                }
                if (!(s.reads == f.summary.reads) || !(s.writes == f.summary.writes) || !(s.must == f.summary.must) ||
                    s.preserving != f.summary.preserving || s.returns != f.summary.returns || !(s.restored == f.summary.restored)) {
                    f.summary = s;
                    changed = true;
                    changed_at[i] = ++tick;
                    if (is_taken[i]) taken_changed = changed_at[i];
                }
            }
            if (!changed) return;
        }
    }

private:
    static constexpr size_t kMemoPerFunction = 8;  // the summaries kept per function (the oldest go first)

    static void Put(std::vector<uint64_t>& v, const RegSet& r) {
        v.push_back(uint64_t{r.x} | uint64_t{r.v} << 32);
        v.push_back(uint64_t{r.sp} | uint64_t{r.nzcv} << 1);
    }
    static void Put(std::vector<uint64_t>& v, const CalleeFx& c) {
        Put(v, c.reads);
        Put(v, c.writes);
        Put(v, c.must);
        v.push_back(uint64_t{c.preserving} | uint64_t{c.returns} << 1 | uint64_t{c.known} << 2);
    }
    // Everything Compute(f) reads besides the instructions: its code's state, its exported flag, its callees' summaries as Of() gives them
    // (deps: the callees that are functions; any other target is a constant) and the address-taken union when it has blr or unbounded br
    std::vector<uint64_t> Inputs(const Function& f, const std::vector<uint32_t>& deps, const std::vector<uint64_t>& entries, bool indirect_use) {
        std::vector<uint64_t> v{f.state, f.summary.exported};
        for (const uint32_t d : deps) Put(v, Of(entries[d]));
        if (indirect_use) Put(v, Indirect());
        return v;
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
    // blr, and a br discovery could not bound: any address-taken function, or a function outside the module (ForeignWrites). The union over
    // all of them is kept until one of their summaries changes (it was computed afresh for every such block of every function at every step,
    // a cost of the address-taken functions' count each)
    const CalleeFx& Indirect() {
        if (indirect_valid && indirect_at >= taken_changed) return indirect;
        CalleeFx c;
        if (taken.empty()) {
            c.reads = c.writes = RegSet::All();
        } else {
            c.must = RegSet::All();
            c.preserving = true;
            c.returns = false;
            c.known = true;
            c.writes = ForeignWrites();
            for (const Function* f : taken) {
                c.reads = c.reads | f->summary.reads;
                if (!f->summary.returns) continue;  // a callee that does not return leaves nothing to the code after the call
                c.writes = c.writes | f->summary.writes;
                c.must = c.must & f->summary.must;
                c.preserving = c.preserving && f->summary.preserving;
                c.returns = c.returns || f->summary.returns;
            }
        }
        indirect = c;
        indirect_valid = true;
        indirect_at = tick;
        return indirect;
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
    std::optional<CalleeFx> EndFx(const Block& b) {
        switch (b.term) {
        case Term::Call: return Of(b.target);
        case Term::CallReg: return Indirect();
        case Term::Svc: return Svc();
        case Term::Tail:
        case Term::CondTail: return Of(b.target);
        case Term::Indirect: return Indirect();
        case Term::Out: return fns.count(b.end) ? std::optional<CalleeFx>(Of(b.end)) : std::nullopt;
        default: return std::nullopt;
        }
    }
    static bool Exits(const Block& b) {
        return b.term == Term::Ret || b.term == Term::Tail || b.term == Term::CondTail || b.term == Term::Indirect || b.term == Term::Out;
    }

    // A block of the function being summarised, with what the analyses need of it: the register effects of its instructions composed (a
    // backward pass gives live_in = (live_out - def) | use; the forward ones add def), its successors as block numbers (-1: not a block of the
    // function) and its end's effect
    struct Node {
        const Block* b = nullptr;
        RegSet def, use;
        std::vector<std::pair<int32_t, Edge>> succ;
        std::optional<CalleeFx> e;
    };

    Summary Compute(const Function& f) {
        Summary s;
        s.exported = f.summary.exported;
        std::vector<Node> g;
        std::vector<uint64_t> starts;
        g.reserve(f.blocks.size());
        starts.reserve(f.blocks.size());
        for (const auto& [start, b] : f.blocks) {
            Node nd;
            nd.b = &b;
            for (uint64_t pc = b.end; pc > b.start;) {
                pc -= 4;
                const RegFx& fx = cache.Fx(pc);
                nd.use = (nd.use - fx.def) | fx.use;
                nd.def = nd.def | fx.def;
            }
            nd.e = EndFx(b);
            g.push_back(std::move(nd));
            starts.push_back(start);
        }
        const auto block = [&](uint64_t pc) -> int32_t {
            const auto it = std::lower_bound(starts.begin(), starts.end(), pc);
            return it != starts.end() && *it == pc ? static_cast<int32_t>(it - starts.begin()) : -1;
        };
        for (Node& nd : g) {
            for (const auto& [t, kind] : nd.b->succ) nd.succ.emplace_back(block(t), kind);
        }
        const int32_t entry = block(f.entry);

        // Reads before writes: backward liveness; a call's reads are uses, its must-writes kill
        std::vector<RegSet> live_in(g.size());
        std::vector<bool> has_live(g.size());
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t i = g.size(); i-- > 0;) {
                const Node& nd = g[i];
                const Block& b = *nd.b;
                RegSet live;
                for (const auto& [t, kind] : nd.succ) {
                    if (kind == Edge::Return || kind == Edge::Escape || kind == Edge::Unwind) continue;  // the call's effect stands between
                    if (t >= 0 && has_live[static_cast<size_t>(t)]) live = live | live_in[static_cast<size_t>(t)];
                }
                if (nd.e) {
                    RegSet after;
                    for (const auto& [t, kind] : nd.succ) {
                        if ((kind == Edge::Return || kind == Edge::Escape || kind == Edge::Unwind) && t >= 0 && has_live[static_cast<size_t>(t)]) {
                            after = after | live_in[static_cast<size_t>(t)];
                        }
                    }
                    if (b.term == Term::Call || b.term == Term::CallReg || b.term == Term::Svc) {
                        live = live | (after - nd.e->must) | nd.e->reads;
                    } else {
                        live = live | nd.e->reads;  // a tail call: the callee's reads are this function's
                    }
                }
                live = (live - nd.def) | nd.use;
                if (!has_live[i] || !(live_in[i] == live)) {
                    live_in[i] = live;
                    has_live[i] = true;
                    changed = true;
                }
            }
        }
        s.reads = entry >= 0 && has_live[static_cast<size_t>(entry)] ? live_in[static_cast<size_t>(entry)] : RegSet{};

        // Everything written, callees included
        RegSet writes;
        for (const Node& nd : g) {
            writes = writes | nd.def;
            if (nd.e) writes = writes | nd.e->writes;
        }

        // Must-writes: forward, intersection at joins, over every exit
        std::vector<RegSet> must_in(g.size());
        std::vector<bool> reached(g.size());
        if (entry >= 0) reached[static_cast<size_t>(entry)] = true;
        for (const uint64_t r : f.roots) {
            if (f.speculative.count(r) && !f.spec_live.count(r)) continue;
            if (std::none_of(f.pads.begin(), f.pads.end(), [r](const auto& p) { return p.second == r; })) {
                if (const int32_t k = block(r); k >= 0) {
                    must_in[static_cast<size_t>(k)] = {};
                    reached[static_cast<size_t>(k)] = true;
                }
            }
        }
        RegSet must = RegSet::All();
        bool any_exit = false;
        changed = true;
        while (changed) {
            changed = false;
            must = RegSet::All();
            any_exit = false;
            for (size_t i = 0; i < g.size(); ++i) {
                if (!reached[i]) continue;
                const Node& nd = g[i];
                const Block& b = *nd.b;
                RegSet m = must_in[i] | nd.def;
                if (nd.e && (b.term == Term::Call || b.term == Term::CallReg || b.term == Term::Svc)) m = m | nd.e->must;
                if (Exits(b)) {
                    RegSet at_exit = m;
                    if (nd.e && b.term != Term::Ret) at_exit = at_exit | nd.e->must;
                    must = must & at_exit;
                    any_exit = true;
                }
                for (const auto& [t, kind] : nd.succ) {
                    if (t < 0) continue;
                    const size_t k = static_cast<size_t>(t);
                    if (!reached[k]) {
                        reached[k] = true;
                        must_in[k] = m;
                        changed = true;
                    } else {
                        const RegSet next = must_in[k] & m;
                        if (!(next == must_in[k])) {
                            must_in[k] = next;
                            changed = true;
                        }
                    }
                }
            }
        }

        // Preservation: the abstract interpretation, every exit checked. A block whose entry state did not change since it was last
        // interpreted is not interpreted again: it would give the same state to its successors (joining a state twice changes nothing) and
        // the same verdict at its exit, which is kept from that time. The rounds and the states are those of interpreting every block of
        // every round; the verdict is the same because it only intersects the exits' restored sets (or clears them for a bad exit)
        std::vector<State> in(g.size());
        std::vector<uint64_t> version(g.size(), 0), done(g.size(), 0);
        struct Verdict {
            bool exit = false, path_returns = false, ok = false;
            RegSet restored;
        };
        std::vector<Verdict> verdict(g.size());
        if (entry >= 0) {
            in[static_cast<size_t>(entry)] = State::Entry();
            version[static_cast<size_t>(entry)] = 1;
        }
        std::set<uint64_t> pads;
        for (const auto& [call, pad] : f.pads) pads.insert(pad);
        for (const uint64_t r : f.roots) {
            if (pads.count(r)) continue;  // a landing pad: entered from its calls (Edge::Unwind), with what the unwinder restores
            if (f.speculative.count(r) && !f.spec_live.count(r)) continue;  // reached by the function's own edges now (a table's case)
            State st = State::Entry();    // a label: entered by a jump nothing in the function shows; nothing known
            for (auto& v : st.x) v = {};
            st.sp = {};
            for (auto& v : st.vlo) v = {};
            if (const int32_t k = block(r); k >= 0) {
                in[static_cast<size_t>(k)].Join(st);
                ++version[static_cast<size_t>(k)];
            }
        }
        changed = true;
        unsigned guard = 0;
        while (changed && guard++ < 256) {
            changed = false;
            for (size_t i = 0; i < g.size(); ++i) {
                if (!in[i].valid || done[i] == version[i]) continue;
                done[i] = version[i];
                const Node& nd = g[i];
                const Block& b = *nd.b;
                State st = in[i];
                for (uint64_t pc = b.start; pc < b.end; pc += 4) cache.Interpret(st, pc);
                const auto& e = nd.e;
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
                Verdict& vd = verdict[i];
                vd = {};
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
                    vd.exit = true;
                    vd.path_returns = b.term == Term::Ret || (e && e->returns);
                    vd.ok = ok;
                    vd.restored = st.Restored();
                }
                for (const auto& [t, kind] : nd.succ) {
                    if (t < 0) continue;
                    const size_t k = static_cast<size_t>(t);
                    State next = in[k];
                    State from = st;
                    if (kind == Edge::Unwind) from.x[0] = from.x[1] = {};  // the exception object and the selector the personality sets
                    next.Join(from);
                    if (!(next == in[k])) {
                        in[k] = std::move(next);
                        ++version[k];
                        changed = true;
                    }
                }
            }
        }
        RegSet restored = Callee();
        bool returns = false;
        for (size_t i = 0; i < g.size(); ++i) {
            const Verdict& vd = verdict[i];
            if (!in[i].valid || !done[i] || !vd.exit || !vd.path_returns) continue;
            returns = true;
            restored = vd.ok ? restored & vd.restored : RegSet{};
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
    InsnCache::Impl& cache;
    SummaryMemo* memo;
    std::vector<const Function*> taken;
    uint64_t tick = 0, taken_changed = 0;  // Run's clock: computations and changes are numbered in order
    CalleeFx indirect;                     // Indirect(), as of indirect_at
    bool indirect_valid = false;
    uint64_t indirect_at = 0;
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

void Summarise(const module::Image& image, std::map<uint64_t, Function>& functions, InsnCache* cache, SummaryMemo* memo, uint64_t* rounds,
               uint64_t* computes) {
    std::unique_ptr<InsnCache> own;
    if (!cache) {
        own = std::make_unique<InsnCache>(image);
        cache = own.get();
    }
    Summariser{image, functions, *cache, memo}.Run(rounds, computes);
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
