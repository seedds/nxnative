#include "analysis/summary.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <new>
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
    // Join(o) with o's x0 and x1 unknown when `unwind` (an unwind edge: the personality sets them), in place: whether this state changed.
    // The same as copying both states, joining and comparing (an unknown value is always Abs{}), without the copies
    bool JoinFrom(const State& o, bool unwind) {
        if (!o.valid) return false;
        if (!valid) {
            *this = o;
            if (unwind) x[0] = x[1] = {};
            return true;
        }
        bool changed = false;
        const auto meet = [&](Abs& a, const Abs& b) {
            if (!(a == b) && !(a == Abs{})) {
                a = {};
                changed = true;
            }
        };
        for (size_t i = 0; i < 31; ++i) meet(x[i], unwind && i < 2 ? Abs{} : o.x[i]);
        meet(sp, o.sp);
        for (size_t i = 0; i < 32; ++i) meet(vlo[i], o.vlo[i]);
        for (auto it = slots.begin(); it != slots.end();) {
            const auto other = o.slots.find(it->first);
            if (other == o.slots.end() || !(other->second == it->second)) {
                it = slots.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
        return changed;
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
    // The operations' values, each written by its operation before a later one reads it: storage left uninitialised (the values are
    // constructed in place below), since filling 24 pairs at every instruction the analysis interprets was a measurable part of it
    alignas(std::pair<Abs, Abs>) unsigned char small[24 * sizeof(std::pair<Abs, Abs>)];
    std::vector<std::pair<Abs, Abs>> big;
    std::pair<Abs, Abs>* val = reinterpret_cast<std::pair<Abs, Abs>*>(small);
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
        new (&val[i]) std::pair<Abs, Abs>(r);
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
    std::vector<std::unordered_map<uint32_t, uint32_t>> by_word;  // word -> entry, in shards (Shard)
    std::vector<Entry> entries;
    std::unordered_map<uint64_t, RegFx> outside;     // words outside the text (the aux region's): InstructionFx of each
    std::unordered_map<uint64_t, uint64_t> prepared; // function entry -> the walk state Prepare last made its instructions' facts for
    std::atomic<uint64_t> translations{0};
    std::atomic<uint64_t> checked{0};  // NXAOT_CHECK_PROGRAMS: programs run beside Interpret

    explicit Impl(const module::Image& image) : img(image), ids(image.text_size / 4), by_word(kShards) {}
    ~Impl() {
        if (checked) {
            std::fprintf(stderr, "nxaot: %llu instructions interpreted both ways, the same\n", static_cast<unsigned long long>(checked.load()));
        }
    }

    // The text word index of pc, or -1 for an address outside the text (the aux region's: Fx keeps them in outside)
    int64_t Index(uint64_t pc) const {
        if (!img.InText(pc) || (pc & 3) || (pc - img.text_off) / 4 >= ids.size()) return -1;
        return static_cast<int64_t>((pc - img.text_off) / 4);
    }

    // The word map in shards by the word (a word's shard is fixed, so that Prepare's order is the same on any number of threads)
    static constexpr size_t kShards = 16;
    static size_t Shard(uint32_t w) { return (w ^ (w >> 7) ^ (w >> 17)) % kShards; }
    const uint32_t* Known(uint32_t w) const {
        const auto& m = by_word[Shard(w)];
        const auto it = m.find(w);
        return it == m.end() ? nullptr : &it->second;
    }

    // InsnCache::Prepare. The functions whose code changed since they were last prepared are scanned on the pool for instructions without
    // facts (reading the cache only); the distinct new words are found per shard of words, each shard's in function order with the first pc
    // it was seen at (as At would make it at its first use: the facts are the same at any pc, the immediates that follow the pc being kept
    // relative), made on the pool (dynarmic's translations: Make reads the image only) and entered shard after shard. Afterwards Fx and
    // Interpret of those instructions only read the cache
    void Prepare(const std::vector<const Function*>& fns, Pool* pool) {
        const auto each = [&](size_t n, const std::function<void(size_t)>& body) {
            if (pool) {
                pool->For(n, body, 16);
            } else {
                for (size_t i = 0; i < n; ++i) body(i);
            }
        };
        std::vector<const Function*> todo;
        for (const Function* f : fns) {
            const auto [it, inserted] = prepared.try_emplace(f->entry, f->state);
            if (!inserted && f->state != 0 && it->second == f->state) continue;
            it->second = f->state;
            todo.push_back(f);
        }
        // Each function's instructions without facts: those whose word has an entry (known), those whose word has none yet (fresh), and
        // those outside the text
        struct Missing {
            std::vector<std::pair<uint32_t, uint32_t>> known, fresh;  // (text word index, entry), (text word index, word)
            std::vector<uint64_t> outside;
        };
        std::vector<Missing> missing(todo.size());
        each(todo.size(), [&](size_t k) {
            Missing& m = missing[k];
            for (const auto& [start, b] : todo[k]->blocks) {
                for (uint64_t pc = b.start; pc < b.end; pc += 4) {
                    const int64_t i = Index(pc);
                    if (i < 0) {
                        if (!outside.count(pc)) m.outside.push_back(pc);
                        continue;
                    }
                    if (ids[static_cast<size_t>(i)]) continue;
                    const uint32_t w = *img.Word(pc);
                    if (const uint32_t* e = Known(w)) m.known.emplace_back(static_cast<uint32_t>(i), *e);
                    else m.fresh.emplace_back(static_cast<uint32_t>(i), w);
                }
            }
        });
        // The new words, per shard: the first sighting of each in function order
        std::vector<std::vector<std::pair<uint32_t, uint64_t>>> first(kShards);  // (word, pc)
        std::vector<std::unordered_map<uint32_t, uint32_t>> place(kShards);       // word -> its place in first
        each(kShards, [&](size_t sh) {
            for (const Missing& m : missing) {
                for (const auto& [i, w] : m.fresh) {
                    if (Shard(w) != sh) continue;
                    if (place[sh].try_emplace(w, static_cast<uint32_t>(first[sh].size())).second) {
                        first[sh].emplace_back(w, img.text_off + 4 * uint64_t{i});
                    }
                }
            }
        });
        const uint32_t base = static_cast<uint32_t>(entries.size());
        std::vector<uint32_t> offset(kShards + 1, base);
        for (size_t sh = 0; sh < kShards; ++sh) offset[sh + 1] = offset[sh] + static_cast<uint32_t>(first[sh].size());
        entries.resize(offset[kShards]);
        each(kShards, [&](size_t sh) {
            for (size_t k = 0; k < first[sh].size(); ++k) by_word[sh].emplace(first[sh][k].first, offset[sh] + static_cast<uint32_t>(k));
        });
        std::vector<std::pair<size_t, size_t>> made;  // (shard, place) of every new entry
        made.reserve(offset[kShards] - base);
        for (size_t sh = 0; sh < kShards; ++sh) {
            for (size_t k = 0; k < first[sh].size(); ++k) made.emplace_back(sh, k);
        }
        each(made.size(), [&](size_t k) {
            const auto& [w, pc] = first[made[k].first][made[k].second];
            entries[base + k] = Make(pc, w);
        });
        each(missing.size(), [&](size_t k) {
            for (const auto& [i, e] : missing[k].known) ids[i] = e + 1;
            for (const auto& [i, w] : missing[k].fresh) ids[i] = offset[Shard(w)] + place[Shard(w)].at(w) + 1;
        });
        std::vector<uint64_t> fresh_outside;
        std::unordered_set<uint64_t> seen_outside;
        for (const Missing& m : missing) {
            for (const uint64_t pc : m.outside) {
                if (seen_outside.insert(pc).second) fresh_outside.push_back(pc);
            }
        }
        std::vector<RegFx> fx(fresh_outside.size());
        each(fresh_outside.size(), [&](size_t k) {
            const auto w = img.Word(fresh_outside[k]);
            fx[k] = w ? InstructionFx(fresh_outside[k], *w) : RegFx{};
        });
        translations += fresh_outside.size();
        for (size_t k = 0; k < fresh_outside.size(); ++k) outside.emplace(fresh_outside[k], fx[k]);
    }

    const Entry* At(uint64_t pc) {
        if (!img.InText(pc) || (pc & 3) || (pc - img.text_off) / 4 >= ids.size()) return nullptr;
        uint32_t& id = ids[(pc - img.text_off) / 4];
        if (!id) {
            const uint32_t w = *img.Word(pc);
            const auto [it, inserted] = by_word[Shard(w)].try_emplace(w, static_cast<uint32_t>(entries.size()));
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
    return impl->translations.load();
}
void InsnCache::Prepare(const std::vector<const Function*>& functions, Pool* pool) {
    impl->Prepare(functions, pool);
}

namespace {

struct CalleeFx {
    RegSet reads, writes, must;
    bool preserving = false, returns = true, known = false;
};

// The union the summaries give a blr and an unbounded br (Summariser::IndirectOf) over a set of the address-taken functions: what any of them may
// read, and over those that return what any may write, what all must write, whether all preserve, whether any returns. Unions,
// intersections, and and or: the same over the same functions combined in any order or grouping
struct TakenAgg {
    RegSet reads, writes, must = RegSet::All();
    bool preserving = true, returns = false;
    void Add(const Summary& s) {
        reads = reads | s.reads;
        if (!s.returns) {
            return;  // a callee that does not return leaves nothing to the code after the call
        }
        writes = writes | s.writes;
        must = must & s.must;
        returns = true;
        preserving = preserving && s.preserving;
    }
    void Add(const TakenAgg& o) {
        reads = reads | o.reads;
        writes = writes | o.writes;
        must = must & o.must;
        preserving = preserving && o.preserving;
        returns = returns || o.returns;
    }
};

bool SameSummary(const Summary& a, const Summary& b) {
    return a.reads == b.reads && a.writes == b.writes && a.must == b.must && a.preserving == b.preserving && a.returns == b.returns &&
           a.restored == b.restored;
}

class Summariser {
public:
    Summariser(const module::Image& image, std::map<uint64_t, Function>& fns, InsnCache& cache, SummaryMemo* memo, Pool* pool)
            : img(image), fns(fns), cache(cache.Internal()), memo(memo), pool(pool) {}

    // The fixed point, round by round over every function in address order, each round seeing the summaries the earlier functions of the
    // same round got (as it always did: Gauss-Seidel), from the same start: no register read, written or restored but the callee-saved ones,
    // every function preserving and returning. A round r is the map S(r) = G(S(r-1)): function i's summary in round r is a function of its
    // own code and exported flag, of the round-r summaries of the functions before it in address order that it reads (its callees; for blr
    // and unbounded br the address-taken functions' union) and the round-(r-1) summaries of the others (itself included). That makes a round
    // a triangular system with one solution, which is the round of the address-order loop whatever order its equations are solved in. So a
    // round is solved on the pool: the functions in levels (a function's level is past those of the callees before it in address order), a
    // level's functions at once, the union over the address-taken functions before each one taken from the summaries known when its level
    // starts; sweeps over the levels until a sweep finds every function's inputs those of its last computation (only the unions can still be
    // stale after a sweep). Each sweep's result is the same on any number of threads, the round's result that of the address-order loop, and
    // the rounds and the stop (a round that changes nothing, or 64 rounds) are the loop's. A function whose inputs are those of its last
    // computation keeps its summary; one whose inputs were seen in an earlier run takes that run's summary from the memo (Compute is a
    // function of its inputs)
    void Run(uint64_t* rounds, uint64_t* computes, uint64_t* sweeps) {
        for (auto& [entry, f] : fns) {
            order.push_back(&f);
            entries.push_back(entry);
        }
        const size_t n = order.size();
        deps.assign(n, {});
        uses_indirect.assign(n, 0);
        std::vector<uint8_t> is_taken(n);
        for (size_t i = 0; i < n; ++i) {
            is_taken[i] = order[i]->address_taken;
            if (is_taken[i]) taken.push_back(static_cast<uint32_t>(i));
        }
        // Each function's callees (its own blocks' ends): on the pool
        const auto callees = [&](size_t i) {
            const Function& f = *order[i];
            for (const auto& [start, b] : f.blocks) {
                uint64_t target = 0;
                switch (b.term) {
                case Term::Call:
                case Term::Tail:
                case Term::CondTail: target = b.target; break;
                case Term::Out: target = b.end; break;
                case Term::CallReg:
                case Term::Indirect: uses_indirect[i] = 1; continue;
                default: continue;
                }
                if (const int64_t d = Index(target); d >= 0) deps[i].push_back(static_cast<uint32_t>(d));
            }
            std::sort(deps[i].begin(), deps[i].end());
            deps[i].erase(std::unique(deps[i].begin(), deps[i].end()), deps[i].end());
        };
        if (pool) {
            pool->For(n, callees, 256);
        } else {
            for (size_t i = 0; i < n; ++i) callees(i);
        }
        taken_below.assign(n, 0);
        for (size_t i = 0, k = 0; i < n; ++i) {
            while (k < taken.size() && taken[k] < i) ++k;
            taken_below[i] = static_cast<uint32_t>(k);
        }
        // Levels: past every callee before the function in address order (those are solved first in each sweep), and a function with blr or
        // an unbounded br past the address-taken functions before it that have neither (so that the union it reads has their summaries of
        // this sweep: those of the address-taken functions with blr or unbounded br before it may be the last sweep's, or the round's start)
        std::vector<uint32_t> level(n, 0);
        uint32_t levels = 0, taken_plain = 0;  // the highest level + 1 of the address-taken functions without blr or unbounded br so far
        for (size_t i = 0; i < n; ++i) {
            for (const uint32_t d : deps[i]) {
                if (d < i) level[i] = std::max(level[i], level[d] + 1);
            }
            if (uses_indirect[i]) level[i] = std::max(level[i], taken_plain);
            if (is_taken[i] && !uses_indirect[i]) taken_plain = std::max(taken_plain, level[i] + 1);
            levels = std::max(levels, level[i] + 1);
        }
        std::vector<std::vector<uint32_t>> by_level(levels);
        std::vector<uint8_t> level_indirect(levels);
        for (size_t i = 0; i < n; ++i) {
            by_level[level[i]].push_back(static_cast<uint32_t>(i));
            level_indirect[level[i]] |= uses_indirect[i];
        }
        Summary start;
        start.must = RegSet::All();
        start.preserving = true;
        start.returns = true;
        start.restored = Callee();
        cur.assign(n, start);
        for (size_t i = 0; i < n; ++i) cur[i].exported = order[i]->summary.exported;
        last.assign(n, {});
        evaluated.assign(n, 0);
        outcome.assign(n, 0);
        memo_of.assign(n, nullptr);
        if (memo) {
            for (size_t i = 0; i < n; ++i) memo_of[i] = &memo->by_function[entries[i]];
        }
        // Change epochs: a function is looked at again only when something its inputs read changed at or after the epoch of its last look
        // (Stale): a callee before it in this round's summaries (changed), a callee from it on in the last round's (moved, at the round's
        // start), an address-taken function either way for the unions. Epochs number the rounds' starts and the levels' steps in order, so
        // they are the same on any number of threads
        uint64_t epoch = 0;
        changed_at.assign(n, 0);
        moved_at.assign(n, 0);
        looked_at.assign(n, 0);
        uint64_t computed = 0, hits = 0;
        const bool debug = std::getenv("NXAOT_DEBUG_SUMMARIES") != nullptr;
        for (unsigned round = 0; round < 64; ++round) {
            if (rounds) ++*rounds;
            const uint64_t round_epoch = ++epoch;
            if (round) {
                for (size_t i = 0; i < n; ++i) {
                    if (!SameSummary(cur[i], prev[i])) moved_at[i] = round_epoch;
                }
            }
            prev = cur;
            // above[k]: the union over the address-taken functions from the k-th on, as the last round left them; above_moved[k], when one of
            // them last moved
            above.assign(taken.size() + 1, TakenAgg{});
            above_moved.assign(taken.size() + 1, 0);
            for (size_t k = taken.size(); k-- > 0;) {
                above[k] = above[k + 1];
                above[k].Add(prev[taken[k]]);
                above_moved[k] = std::max(above_moved[k + 1], moved_at[taken[k]]);
            }
            uint64_t round_sweeps = 0, looked = 0;
            const uint64_t computed0 = computed;
            for (;;) {
                if (sweeps) ++*sweeps;
                ++round_sweeps;
                bool any = false, taken_moved = true;
                for (uint32_t l = 0; l < levels; ++l) {
                    const uint64_t now = ++epoch;
                    if (level_indirect[l] && taken_moved) {
                        taken_moved = false;
                        // below[k]: the union over the first k address-taken functions as this round knows them now; below_changed[k],
                        // when one of them last changed
                        below.assign(taken.size() + 1, TakenAgg{});
                        below_changed.assign(taken.size() + 1, 0);
                        for (size_t k = 0; k < taken.size(); ++k) {
                            below[k + 1] = below[k];
                            below[k + 1].Add(cur[taken[k]]);
                            below_changed[k + 1] = std::max(below_changed[k], changed_at[taken[k]]);
                        }
                    }
                    const std::vector<uint32_t>& members = by_level[l];
                    // The members something they read may have changed for (most, after the first sweep of a run, have none: they are
                    // found here, so that a step with little to do does not wake the pool), then those on the pool. What became of each
                    // (kLooked, kEvaluated, ...) is counted after the step, so that the threads share no counter
                    todo.clear();
                    for (const uint32_t i : members) {
                        if (!evaluated[i] || Stale(i)) todo.push_back(i);
                    }
                    const auto one = [&](size_t m) {
                        const size_t i = todo[m];
                        looked_at[i] = now;
                        outcome[i] = kLooked;
                        const Function& f = *order[i];
                        if (evaluated[i] && SameInputs(i, last[i])) return;
                        std::vector<uint64_t> inputs = Inputs(i);
                        outcome[i] |= kEvaluated;
                        Summary s;
                        const Summary* known = nullptr;
                        if (memo_of[i]) {
                            for (const auto& [in, out] : *memo_of[i]) {
                                if (in == inputs) known = &out;
                            }
                        }
                        if (known) {
                            s = *known;
                            outcome[i] |= kHit;
                        } else {
                            outcome[i] |= kComputed;
                            s = Compute(i);
                            if (memo_of[i]) {
                                auto& seen = *memo_of[i];
                                if (seen.size() >= kMemoPerFunction) seen.erase(seen.begin());
                                seen.emplace_back(inputs, s);
                            }
                        }
                        s.exported = f.summary.exported;
                        if (!SameSummary(s, cur[i])) {
                            cur[i] = s;
                            changed_at[i] = now;
                            if (is_taken[i]) outcome[i] |= kTakenMoved;
                        }
                        last[i] = std::move(inputs);
                        evaluated[i] = 1;
                    };
                    if (pool) {
                        pool->For(todo.size(), one, 32);
                    } else {
                        for (size_t m = 0; m < todo.size(); ++m) one(m);
                    }
                    for (const uint32_t i : todo) {
                        const uint8_t o = outcome[i];
                        looked += (o & kLooked) != 0;
                        any = any || (o & kEvaluated);
                        computed += (o & kComputed) != 0;
                        hits += (o & kHit) != 0;
                        taken_moved = taken_moved || (o & kTakenMoved);
                    }
                }
                if (!any) break;
            }
            if (debug) {
                std::fprintf(stderr, "nxaot: summaries round %u: %llu sweeps, %llu looked at, %llu computed\n", round,
                             static_cast<unsigned long long>(round_sweeps), static_cast<unsigned long long>(looked),
                             static_cast<unsigned long long>(computed - computed0));
            }
            bool changed = false;
            for (size_t i = 0; i < n && !changed; ++i) changed = !SameSummary(cur[i], prev[i]);
            if (!changed) break;
        }
        for (size_t i = 0; i < n; ++i) order[i]->summary = cur[i];
        if (computes) *computes += computed;
        if (memo) memo->hits += hits;
    }

private:
    static constexpr size_t kMemoPerFunction = 8;  // the summaries kept per function (the oldest go first)
    // A member's outcome in a level's step (Run)
    static constexpr uint8_t kLooked = 1, kEvaluated = 2, kComputed = 4, kHit = 8, kTakenMoved = 16;

    int64_t Index(uint64_t entry) const {
        const auto it = std::lower_bound(entries.begin(), entries.end(), entry);
        return it != entries.end() && *it == entry ? it - entries.begin() : -1;
    }
    // Function d's summary as function i's computation in this round reads it: this round's for a function before i, the last round's for
    // i itself and the functions after it
    const Summary& Value(size_t d, size_t i) const { return d < i ? cur[d] : prev[d]; }
    // Whether something Inputs(i) reads may have changed since i was last looked at: a callee's summary as Value gives it, or a part of the
    // union IndirectOf gives it (a change in the same level's step as the look counts: that step's below was taken before it)
    bool Stale(size_t i) const {
        const uint64_t at = looked_at[i];
        for (const uint32_t d : deps[i]) {
            if ((d < i ? changed_at[d] : moved_at[d]) >= at) return true;
        }
        if (uses_indirect[i]) {
            const uint32_t k = taken_below[i];
            if (below_changed[k] >= at || above_moved[k] >= at) return true;
        }
        return false;
    }
    static CalleeFx AsCallee(const Summary& s) { return {s.reads, s.writes, s.must, s.preserving, s.returns, true}; }

    template <class Out>
    static void Put(Out& out, const RegSet& r) {
        out(uint64_t{r.x} | uint64_t{r.v} << 32);
        out(uint64_t{r.sp} | uint64_t{r.nzcv} << 1);
    }
    template <class Out>
    static void Put(Out& out, const CalleeFx& c) {
        Put(out, c.reads);
        Put(out, c.writes);
        Put(out, c.must);
        out(uint64_t{c.preserving} | uint64_t{c.returns} << 1 | uint64_t{c.known} << 2);
    }
    // Everything Compute(i) reads besides the instructions: its code's state, its exported flag, its callees' summaries as Of() gives them
    // (deps: the callees that are functions; any other target is a constant) and the address-taken union when it has blr or unbounded br
    template <class Out>
    void EmitInputs(size_t i, Out& out) const {
        const Function& f = *order[i];
        out(f.state);
        out(uint64_t{f.summary.exported});
        for (const uint32_t d : deps[i]) Put(out, AsCallee(Value(d, i)));
        if (uses_indirect[i]) Put(out, IndirectOf(i));
    }
    std::vector<uint64_t> Inputs(size_t i) const {
        std::vector<uint64_t> v;
        v.reserve(2 + 7 * (deps[i].size() + 1));
        auto out = [&](uint64_t w) { v.push_back(w); };
        EmitInputs(i, out);
        return v;
    }
    // Inputs(i) == v, without building Inputs(i)
    bool SameInputs(size_t i, const std::vector<uint64_t>& v) const {
        size_t at = 0;
        bool same = true;
        auto out = [&](uint64_t w) {
            same = same && at < v.size() && v[at] == w;
            ++at;
        };
        EmitInputs(i, out);
        return same && at == v.size();
    }

    CalleeFx Of(uint64_t target, size_t i) const {
        const int64_t d = Index(target);
        if (d < 0) {
            CalleeFx c;
            c.reads = c.writes = RegSet::All();
            return c;
        }
        return AsCallee(Value(static_cast<size_t>(d), i));
    }
    // blr, and a br discovery could not bound: any address-taken function, or a function outside the module (ForeignWrites). For function i
    // the union over the address-taken functions before it as this round knows them (below) and the others as the last round left them
    // (above)
    CalleeFx IndirectOf(size_t i) const {
        CalleeFx c;
        if (taken.empty()) {
            c.reads = c.writes = RegSet::All();
            return c;
        }
        TakenAgg a = below[taken_below[i]];
        a.Add(above[taken_below[i]]);
        c.reads = a.reads;
        c.writes = ForeignWrites() | a.writes;
        c.must = a.must;
        c.preserving = a.preserving;
        c.returns = a.returns;
        c.known = true;
        return c;
    }
    static CalleeFx Svc() {
        CalleeFx c;
        c.reads = SvcReads();
        c.writes = SvcWrites();
        c.preserving = true;
        c.known = true;
        return c;
    }
    // What the block's end does besides its instructions: a call, a tail call or an indirect jump (indirect: IndirectOf(i))
    std::optional<CalleeFx> EndFx(const Block& b, size_t i, const CalleeFx& indirect) const {
        switch (b.term) {
        case Term::Call: return Of(b.target, i);
        case Term::CallReg: return indirect;
        case Term::Svc: return Svc();
        case Term::Tail:
        case Term::CondTail: return Of(b.target, i);
        case Term::Indirect: return indirect;
        case Term::Out: return fns.count(b.end) ? std::optional<CalleeFx>(Of(b.end, i)) : std::nullopt;
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

    Summary Compute(size_t i) {
        const Function& f = *order[i];
        const CalleeFx indirect = uses_indirect[i] ? IndirectOf(i) : CalleeFx{};
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
            nd.e = EndFx(b, i, indirect);
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
                    // An unwind edge: the exception object and the selector the personality sets
                    if (in[k].JoinFrom(st, kind == Edge::Unwind)) {
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
    Pool* pool;
    std::vector<Function*> order;              // the functions in address order
    std::vector<uint64_t> entries;             // their entries
    std::vector<std::vector<uint32_t>> deps;   // per function: the functions its blocks' ends call or jump to, ascending
    std::vector<uint8_t> uses_indirect;        // per function: has blr or an unbounded br
    std::vector<uint32_t> taken;               // the address-taken functions, ascending
    std::vector<uint32_t> taken_below;         // per function: how many address-taken functions precede it
    std::vector<Summary> prev, cur;            // the last round's summaries and this round's
    std::vector<TakenAgg> below, above;        // IndirectOf's parts
    std::vector<uint64_t> below_changed, above_moved;  // per part: the last epoch one of its functions changed (moved)
    std::vector<uint64_t> changed_at;          // per function: the epoch its summary last changed in this round
    std::vector<uint64_t> moved_at;            // per function: the round's epoch its last round's summary last changed
    std::vector<uint64_t> looked_at;           // per function: the epoch it was last looked at
    std::vector<std::vector<uint64_t>> last;   // per function: the inputs of its last computation in this run
    std::vector<uint8_t> evaluated;            // per function: computed in this run
    std::vector<uint8_t> outcome;              // per function: what its last look did (kLooked, ...)
    std::vector<uint32_t> todo;                // a level step's members to look at
    std::vector<std::vector<std::pair<std::vector<uint64_t>, Summary>>*> memo_of;  // per function: its memo
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
               uint64_t* computes, Pool* pool, uint64_t* sweeps) {
    std::unique_ptr<InsnCache> own;
    if (!cache) {
        own = std::make_unique<InsnCache>(image);
        cache = own.get();
    }
    // Every instruction's facts first (on the pool): the summaries then only read the cache, on any thread
    std::vector<const Function*> all;
    all.reserve(functions.size());
    for (const auto& [entry, f] : functions) all.push_back(&f);
    cache->Prepare(all, pool);
    Summariser{image, functions, *cache, memo, pool}.Run(rounds, computes, sweeps);
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
