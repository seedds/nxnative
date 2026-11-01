#include "build/coff.h"

#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include <llvm/ADT/StringExtras.h>
#include <llvm/BinaryFormat/COFF.h>
#include <llvm/BinaryFormat/ELF.h>
#include <llvm/MC/MCAsmInfo.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCDisassembler/MCDisassembler.h>
#include <llvm/MC/MCInst.h>
#include <llvm/MC/MCInstrInfo.h>
#include <llvm/MC/MCRegisterInfo.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/MCTargetOptions.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/ELF.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MathExtras.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Win64EH.h>
#include <llvm/Support/raw_ostream.h>

namespace nxaot::build {

namespace {

using ELFT = llvm::object::ELF64LE;
using Shdr = ELFT::Shdr;

enum class Kind { Drop, Text, Rdata, Data, Bss, Debug };

struct Reloc {
    uint32_t offset = 0, symbol = 0;
    uint16_t type = 0;
};

struct Section {
    std::string name;
    Kind kind = Kind::Drop;
    uint32_t flags = 0;
    uint64_t size = 0;
    std::string data;  // the ELF section's bytes, with the addends written in (empty for .bss)
    std::vector<Reloc> relocs;
    uint32_t number = 0;  // its COFF section number (1-based)
    uint32_t symbol = 0;  // the index of its section symbol
};

struct Symbol {
    std::string name;
    uint32_t value = 0;
    int32_t section = 0;  // COFF section number, 0 undefined
    uint16_t type = 0;
    uint8_t storage = 0;
    bool section_def = false;  // a section symbol: one auxiliary record (its section definition) follows
    const Section* sec = nullptr;
};

void Put16(std::string* s, uint16_t v) { s->append(reinterpret_cast<const char*>(&v), 2); }
void Put32(std::string* s, uint32_t v) { s->append(reinterpret_cast<const char*>(&v), 4); }

uint32_t AlignFlag(uint64_t align, std::string* error) {
    if (align <= 1) return llvm::COFF::IMAGE_SCN_ALIGN_1BYTES;
    if (!llvm::isPowerOf2_64(align) || align > 8192) {
        *error = "a section aligned to " + std::to_string(align) + " bytes, which COFF cannot express";
        return 0;
    }
    return static_cast<uint32_t>(llvm::Log2_64(align) + 1) << 20;
}

// ---- Unwind information: .eh_frame's CFI as x64 exception handling's .pdata and .xdata ----
// Microsoft's "x64 exception handling": a RUNTIME_FUNCTION {BeginAddress, EndAddress, UnwindInfoAddress} per function in .pdata (RVAs:
// IMAGE_REL_AMD64_ADDR32NB), its UNWIND_INFO in .xdata {Version 1 | Flags << 3, SizeOfProlog, CountOfCodes, FrameRegister | FrameOffset << 4,
// UNWIND_CODE[CountOfCodes] (an even count), no handler}, each UNWIND_CODE {CodeOffset: the end of the prologue instruction, UnwindOp | OpInfo << 4}
// plus its operation's slots, in reverse order of the prologue. RtlVirtualUnwind undoes the operations whose CodeOffset lies at or before the pc
// (all of them past the prologue), from the frame's rsp, or from FrameRegister - 16 * FrameOffset once UWOP_SET_FPREG has run; inside an epilogue
// it does not use them: it recognises the epilogue by its bytes (add rsp, imm / lea rsp, [frame + d]; pops; ret or a jmp out of the function or an
// indirect jmp with REX.W) and runs it forward.
// A function's unwind information is built from its instructions (LLVM's x86 disassembler) and its CFI (Translate below has the rules). The SysV
// code compiled here has shapes Win64's compilers never emit, which one UNWIND_INFO per function cannot express: a frame set up after the function's
// start (shrink-wrapping), arguments pushed for a call in the body, callees that pop their arguments (swifttailcc), epilogues RtlVirtualUnwind does
// not recognise (vzeroupper after the pops, swifttailcc's add of its tail-call area after them, a store before a tail call, an indirect tail jump
// without REX.W). Such a function gets one RUNTIME_FUNCTION per stretch of instructions with one frame, each UNWIND_INFO describing its stretch's
// frame exactly (Windows takes the entry that holds the pc, and no entry needs another's: no chained information). What has no description is
// left out and counted by reason (CoffStats::unwind): a CFA on another register than rsp or rbp, a CFI rule other than "saved at CFA + n", a frame
// pointer set before the frame's last push (SysV's push %rbp; mov %rsp, %rbp; Win64 sets it last), a function whose CFI starts inside a frame.

constexpr int64_t kUnsaved = std::numeric_limits<int64_t>::min();
constexpr int kRegs = 33;  // DWARF registers of x86-64: 0..15 the general registers, 16 the return address, 17..32 xmm0..xmm15
constexpr int kRa = 16, kRsp = 7, kRbp = 6;

struct CfiState {
    int cfa_reg = kRsp;
    int64_t cfa_off = 8;
    std::array<int64_t, kRegs> saved;  // the slot, CFA-relative; kUnsaved when the register keeps its value
    CfiState() { saved.fill(kUnsaved); }
};
using CfiRows = std::vector<std::pair<uint64_t, CfiState>>;  // the state from this offset in the function on, in order

const CfiState& StateAt(const CfiRows& rows, uint64_t x) {  // the last row at or before x
    size_t lo = 0, hi = rows.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (rows[mid].first <= x) lo = mid + 1; else hi = mid;
    }
    return rows[lo ? lo - 1 : 0].second;
}
const CfiState& StateFrom(const CfiRows& rows, uint64_t x) {  // the first row at or after x, else the last
    for (const auto& r : rows) {
        if (r.first >= x) return r.second;
    }
    return rows.back().second;
}

// Where the CFI saves register reg from x on: its slot in the first row at or after x that has one (LLVM records a prologue's saves at its end)
int64_t SlotFrom(const CfiRows& rows, uint64_t x, int reg) {
    for (const auto& r : rows) {
        if (r.first >= x && r.second.saved[reg] != kUnsaved) return r.second.saved[reg];
    }
    return kUnsaved;
}

struct Reader {
    llvm::StringRef d;
    size_t p = 0;
    bool bad = false;
    uint64_t Fixed(unsigned n) {
        if (p + n > d.size()) {
            bad = true;
            return 0;
        }
        uint64_t v = 0;
        for (unsigned i = 0; i < n; ++i) v |= static_cast<uint64_t>(static_cast<uint8_t>(d[p + i])) << (8 * i);
        p += n;
        return v;
    }
    uint8_t U8() { return static_cast<uint8_t>(Fixed(1)); }
    uint64_t Uleb() {
        uint64_t v = 0;
        for (unsigned s = 0;; s += 7) {
            const uint8_t b = U8();
            if (bad) return 0;
            if (s < 64) v |= static_cast<uint64_t>(b & 0x7f) << s;
            if (!(b & 0x80)) return v;
        }
    }
    int64_t Sleb() {
        uint64_t v = 0;
        unsigned s = 0;
        uint8_t b = 0;
        do {
            b = U8();
            if (bad) return 0;
            if (s < 64) v |= static_cast<uint64_t>(b & 0x7f) << s;
            s += 7;
        } while (b & 0x80);
        if (s < 64 && (b & 0x40)) v |= ~static_cast<uint64_t>(0) << s;
        return static_cast<int64_t>(v);
    }
};

int EncodedSize(uint8_t enc) {  // DW_EH_PE_* value formats
    switch (enc & 0x0f) {
    case 0x00: case 0x04: case 0x0c: return 8;
    case 0x02: case 0x0a: return 2;
    case 0x03: case 0x0b: return 4;
    default: return -1;
    }
}

struct Cie {
    uint64_t code_align = 1;
    int64_t data_align = -8;
    uint8_t fde_enc = 0;
    bool z = false;
    CfiState initial;
    std::string bad;
};

const char* CfaOpName(uint8_t op) {
    switch (op) {
    case 0x01: return "DW_CFA_set_loc";
    case 0x09: return "DW_CFA_register";
    case 0x0f: return "DW_CFA_def_cfa_expression";
    case 0x10: return "DW_CFA_expression";
    case 0x14: return "DW_CFA_val_offset";
    case 0x15: return "DW_CFA_val_offset_sf";
    case 0x16: return "DW_CFA_val_expression";
    case 0x2f: return "DW_CFA_GNU_negative_offset_extended";
    default: return nullptr;
    }
}

// Runs a CFI program from *st at *loc, recording a row at every change; false with *bad set at a rule the codes cannot express
bool RunCfi(llvm::StringRef ops, const Cie& cie, const CfiState& initial, CfiState* st, uint64_t* loc, CfiRows* rows, std::string* bad) {
    Reader r{ops};
    std::vector<CfiState> remembered;
    const auto changed = [&] {
        if (!rows) return;
        if (!rows->empty() && rows->back().first == *loc) rows->back().second = *st;
        else rows->push_back({*loc, *st});
    };
    const auto save = [&](uint64_t reg, int64_t off) {
        if (reg >= kRegs) {
            *bad = "a CFI rule for DWARF register " + std::to_string(reg) + ", which has no unwind code";
            return false;
        }
        st->saved[reg] = off;
        changed();
        return true;
    };
    const auto unsave = [&](uint64_t reg, bool restore) {
        if (reg >= kRegs) return true;  // a register the codes do not know keeps its value
        st->saved[reg] = restore ? initial.saved[reg] : kUnsaved;
        changed();
        return true;
    };
    const auto cfa = [&](int64_t reg, int64_t off) {
        if (reg != kRsp && reg != kRbp) {
            *bad = "a CFA on DWARF register " + std::to_string(reg);
            return false;
        }
        st->cfa_reg = static_cast<int>(reg);
        st->cfa_off = off;
        changed();
        return true;
    };
    while (r.p < r.d.size()) {
        const uint8_t op = r.U8();
        const uint8_t low = op & 0x3f;
        bool ok = true;
        switch (op & 0xc0) {
        case 0x40: *loc += low * cie.code_align; continue;  // DW_CFA_advance_loc
        case 0x80: ok = save(low, static_cast<int64_t>(r.Uleb()) * cie.data_align); break;  // DW_CFA_offset
        case 0xc0: ok = unsave(low, true); break;  // DW_CFA_restore
        default:
            switch (op) {
            case 0x00: break;  // DW_CFA_nop
            case 0x02: *loc += r.Fixed(1) * cie.code_align; break;
            case 0x03: *loc += r.Fixed(2) * cie.code_align; break;
            case 0x04: *loc += r.Fixed(4) * cie.code_align; break;
            case 0x05: { const uint64_t reg = r.Uleb(); ok = save(reg, static_cast<int64_t>(r.Uleb()) * cie.data_align); break; }
            case 0x06: ok = unsave(r.Uleb(), true); break;
            case 0x07: {  // DW_CFA_undefined
                const uint64_t reg = r.Uleb();
                if (reg == kRa) {
                    *bad = "the return address undefined (the outermost frame)";
                    return false;
                }
                ok = unsave(reg, false);
                break;
            }
            case 0x08: ok = unsave(r.Uleb(), false); break;  // DW_CFA_same_value
            case 0x0a: remembered.push_back(*st); break;
            case 0x0b:
                if (remembered.empty()) {
                    *bad = "DW_CFA_restore_state without DW_CFA_remember_state";
                    return false;
                }
                *st = remembered.back();
                remembered.pop_back();
                changed();
                break;
            case 0x0c: { const int64_t reg = static_cast<int64_t>(r.Uleb()); ok = cfa(reg, static_cast<int64_t>(r.Uleb())); break; }
            case 0x0d: ok = cfa(static_cast<int64_t>(r.Uleb()), st->cfa_off); break;
            case 0x0e: ok = cfa(st->cfa_reg, static_cast<int64_t>(r.Uleb())); break;
            case 0x11: { const uint64_t reg = r.Uleb(); ok = save(reg, r.Sleb() * cie.data_align); break; }
            case 0x12: { const int64_t reg = static_cast<int64_t>(r.Uleb()); ok = cfa(reg, r.Sleb() * cie.data_align); break; }
            case 0x13: ok = cfa(st->cfa_reg, r.Sleb() * cie.data_align); break;
            case 0x2e: r.Uleb(); break;  // DW_CFA_GNU_args_size: for a personality routine; the CFA stays
            default: {
                const char* name = CfaOpName(op);
                *bad = std::string("a CFI rule the codes cannot express (") + (name ? name : "DW_CFA 0x" + llvm::utohexstr(op)) + ")";
                return false;
            }
            }
        }
        if (!ok) return false;
        if (r.bad) {
            *bad = "a truncated CFI program";
            return false;
        }
    }
    return true;
}

// A function of the object as its FDE gives it: its section (ELF index), offset and size there, and its CFI rows
struct CfiFunction {
    uint32_t section = 0;
    uint64_t begin = 0, size = 0;
    CfiRows rows;
    std::string bad;
};

// The FDEs of .eh_frame (eh: its bytes; at: the target, as (ELF section, offset), of the relocation at each offset of it)
bool ReadEhFrame(llvm::StringRef eh, const std::map<uint64_t, std::pair<uint32_t, uint64_t>>& at, std::vector<CfiFunction>* out, std::string* error) {
    std::map<uint64_t, Cie> cies;
    for (uint64_t pos = 0; pos + 4 <= eh.size();) {
        Reader r{eh, static_cast<size_t>(pos)};
        const uint64_t length = r.Fixed(4);
        if (length == 0) break;  // the terminator
        if (length == 0xffffffff) {
            *error = ".eh_frame: a 64-bit record at +0x" + llvm::utohexstr(pos);
            return false;
        }
        const uint64_t end = pos + 4 + length;
        if (end > eh.size()) {
            *error = ".eh_frame: a record past the section's end at +0x" + llvm::utohexstr(pos);
            return false;
        }
        r.d = eh.substr(0, end);
        const uint64_t id_at = r.p;
        const uint64_t id = r.Fixed(4);
        if (id == 0) {  // a CIE
            Cie c;
            const uint8_t version = r.U8();
            std::string aug;
            for (char ch; r.p < r.d.size() && (ch = r.d[r.p++]) != 0;) aug += ch;
            c.code_align = r.Uleb();
            c.data_align = r.Sleb();
            const uint64_t ra = version == 1 ? r.U8() : r.Uleb();
            if (ra != kRa) c.bad = "a CIE whose return address is not DWARF register 16";
            size_t ops_at = r.p;
            if (!aug.empty() && aug[0] == 'z') {
                c.z = true;
                const uint64_t n = r.Uleb();
                const size_t data_end = r.p + n;
                for (size_t k = 1; k < aug.size() && !r.bad; ++k) {
                    if (aug[k] == 'R') {
                        c.fde_enc = r.U8();
                    } else if (aug[k] == 'L') {
                        r.U8();
                    } else if (aug[k] == 'P') {
                        const int sz = EncodedSize(r.U8());
                        if (sz < 0) c.bad = "a CIE with a personality pointer of unknown size";
                        else r.Fixed(static_cast<unsigned>(sz));
                    } else if (aug[k] != 'S' && aug[k] != 'B') {
                        break;  // unknown: the length skips it
                    }
                }
                ops_at = data_end;
            } else if (!aug.empty()) {
                c.bad = "a CIE with augmentation '" + aug + "'";
            }
            if (r.bad || ops_at > r.d.size()) {
                *error = ".eh_frame: a malformed CIE at +0x" + llvm::utohexstr(pos);
                return false;
            }
            if (c.bad.empty()) {
                CfiState st;
                uint64_t loc = 0;
                if (!RunCfi(r.d.substr(ops_at), c, st, &st, &loc, nullptr, &c.bad)) {
                    // c.bad set: every FDE of this CIE is left out with it
                }
                c.initial = st;
            }
            cies[pos] = c;
        } else {  // an FDE: id is the distance back to its CIE
            const auto cie = cies.find(id_at - id);
            if (cie == cies.end()) {
                *error = ".eh_frame: an FDE at +0x" + llvm::utohexstr(pos) + " without its CIE";
                return false;
            }
            const Cie& c = cie->second;
            const int sz = EncodedSize(c.fde_enc);
            if (sz < 0) {
                *error = ".eh_frame: an FDE pointer encoding 0x" + llvm::utohexstr(c.fde_enc);
                return false;
            }
            const uint64_t begin_at = r.p;
            r.Fixed(static_cast<unsigned>(sz));
            const uint64_t range = r.Fixed(static_cast<unsigned>(sz));
            if (c.z) r.p += r.Uleb();
            if (r.bad || r.p > r.d.size()) {
                *error = ".eh_frame: a malformed FDE at +0x" + llvm::utohexstr(pos);
                return false;
            }
            const auto target = at.find(begin_at);
            if (target != at.end()) {  // else an FDE of no kept code
                CfiFunction f;
                f.section = target->second.first;
                f.begin = target->second.second;
                f.size = range;
                f.bad = c.bad;
                if (f.bad.empty()) {
                    CfiState st = c.initial;
                    uint64_t loc = 0;
                    f.rows.push_back({0, st});
                    RunCfi(r.d.substr(r.p), c, c.initial, &st, &loc, &f.rows, &f.bad);
                }
                out->push_back(std::move(f));
            }
        }
        pos = end;
    }
    return true;
}

// The x86 disassembler, for the frame-setup instructions and the instructions' bounds
struct X86Decoder {
    std::unique_ptr<llvm::MCRegisterInfo> mri;
    std::unique_ptr<llvm::MCAsmInfo> mai;
    std::unique_ptr<llvm::MCSubtargetInfo> sti;
    std::unique_ptr<llvm::MCInstrInfo> mii;
    std::unique_ptr<llvm::MCContext> ctx;
    std::unique_ptr<llvm::MCDisassembler> dis;
    unsigned rsp = 0;  // the register's number in the MC layer
    bool Init(std::string* error) {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetDisassembler();
        const std::string triple = "x86_64-unknown-linux-gnu";
        std::string err;
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, err);
        if (!target) {
            *error = "no x86-64 target: " + err;
            return false;
        }
        mri.reset(target->createMCRegInfo(triple));
        llvm::MCTargetOptions mopts;
        mai.reset(target->createMCAsmInfo(*mri, triple, mopts));
        sti.reset(target->createMCSubtargetInfo(triple, "", ""));
        mii.reset(target->createMCInstrInfo());
        if (!mri || !mai || !sti || !mii) {
            *error = "the x86-64 target's MC layer";
            return false;
        }
        ctx = std::make_unique<llvm::MCContext>(llvm::Triple(triple), mai.get(), mri.get(), sti.get());
        dis.reset(target->createMCDisassembler(*sti, *ctx));
        if (!dis) {
            *error = "no x86 disassembler in this LLVM";
            return false;
        }
        for (unsigned r = 1; r < mri->getNumRegs() && !rsp; ++r) {
            if (llvm::StringRef(mri->getName(r)) == "RSP") rsp = r;
        }
        if (!rsp) {
            *error = "no rsp in the x86 target's registers";
            return false;
        }
        return true;
    }
};

struct RegId {
    int dwarf = -1;  // DWARF number (17 + n for xmm n and ymm n)
    int win = -1;    // the unwind codes' number: the general register's (rax 0, rcx 1, rdx 2, rbx 3, rsp 4, rbp 5, rsi 6, rdi 7, r8..r15) or n of xmm n
    bool xmm = false;
};

RegId RegOf(const X86Decoder& dec, const llvm::MCOperand& op) {
    RegId id;
    if (!op.isReg() || !op.getReg()) return id;
    const llvm::StringRef name = dec.mri->getName(op.getReg());
    static const struct { const char* name; int dwarf, win; } gprs[] = {
        {"RAX", 0, 0}, {"RDX", 1, 2}, {"RCX", 2, 1}, {"RBX", 3, 3}, {"RSI", 4, 6}, {"RDI", 5, 7}, {"RBP", 6, 5}, {"RSP", 7, 4},
        {"R8", 8, 8}, {"R9", 9, 9}, {"R10", 10, 10}, {"R11", 11, 11}, {"R12", 12, 12}, {"R13", 13, 13}, {"R14", 14, 14}, {"R15", 15, 15}};
    for (const auto& g : gprs) {
        if (name == g.name) {
            id.dwarf = g.dwarf;
            id.win = g.win;
            return id;
        }
    }
    unsigned n = 0;
    if ((name.starts_with("XMM") || name.starts_with("YMM")) && !name.substr(3).getAsInteger(10, n) && n < 16) {
        id.dwarf = 17 + static_cast<int>(n);
        id.win = static_cast<int>(n);
        id.xmm = true;
    }
    return id;
}

// A function's unwind information: one RUNTIME_FUNCTION per run of instructions that one UNWIND_INFO describes, or why it has none
struct UnwindEntry {
    uint64_t begin, end;  // function-relative
    std::string info;     // the UNWIND_INFO's bytes
};
struct UnwindFn {
    std::vector<UnwindEntry> entries;
    std::string why;             // not describable: why (no entries)
    std::set<std::string> more;  // why it took more than one entry
};

// The epilogue's instructions RtlVirtualUnwind recognises by their bytes
bool IsPop(const uint8_t* b, uint64_t n) { return (n == 1 && b[0] >= 0x58 && b[0] <= 0x5f) || (n == 2 && (b[0] & 0xf0) == 0x40 && b[1] >= 0x58 && b[1] <= 0x5f); }
bool IsAddRsp(const uint8_t* b, uint64_t n) { return n >= 4 && b[0] == 0x48 && (b[1] == 0x83 || b[1] == 0x81) && b[2] == 0xc4 && n == (b[1] == 0x83 ? 4u : 7u); }
bool IsLeaRsp(const uint8_t* b, uint64_t n) {  // lea d(%frame), %rsp: no SIB, an 8- or 32-bit displacement
    return n >= 4 && (b[0] & 0xfe) == 0x48 && b[1] == 0x8d && ((b[2] >> 3) & 7) == 4 && (b[2] & 7) != 4 && ((b[2] >> 6) == 1 || (b[2] >> 6) == 2);
}

int WinOfDwarf(int dwarf) {  // a general register's number in the unwind codes from its DWARF number
    static const int map[16] = {0, 2, 1, 3, 6, 7, 5, 4, 8, 9, 10, 11, 12, 13, 14, 15};
    return dwarf >= 0 && dwarf < 16 ? map[dwarf] : -1;
}

// An operation of a prologue, at the end of its instruction (function-relative)
struct UnwindOp {
    uint64_t end;
    uint8_t op, reg;
    int64_t value;  // an allocation's size; a save's offset from the frame's base
};

// UNWIND_INFO from operations in prologue order (the codes are written in reverse); begin: the entry's start, CodeOffset = end - begin (0 when
// zero_offsets: every operation applies wherever the pc is in the entry)
bool EncodeInfo(const std::vector<UnwindOp>& ops, uint64_t begin, uint64_t prologue_end, uint8_t frame, bool zero_offsets, std::string* info, std::string* why) {
    namespace W = llvm::Win64EH;
    std::vector<uint16_t> slots;
    for (auto it = ops.rbegin(); it != ops.rend(); ++it) {
        const UnwindOp& o = *it;
        const uint64_t at = zero_offsets ? 0 : o.end - begin;
        if (at > 255) {
            *why = "a prologue of more than 255 bytes";
            return false;
        }
        const auto code = [&](uint8_t op, uint8_t opinfo) { slots.push_back(static_cast<uint16_t>(at | static_cast<unsigned>(op | opinfo << 4) << 8)); };
        switch (o.op) {
        case W::UOP_PushNonVol:
            code(W::UOP_PushNonVol, o.reg);
            break;
        case W::UOP_AllocSmall:
        case W::UOP_AllocLarge:
            if (o.value <= 0 || o.value % 8) {
                *why = "an allocation of " + std::to_string(o.value) + " bytes";
                return false;
            }
            if (o.value <= 128) {
                code(W::UOP_AllocSmall, static_cast<uint8_t>(o.value / 8 - 1));
            } else if (o.value <= 0x7fff8) {
                code(W::UOP_AllocLarge, 0);
                slots.push_back(static_cast<uint16_t>(o.value / 8));
            } else if (o.value <= 0xfffffff8) {
                code(W::UOP_AllocLarge, 1);
                slots.push_back(static_cast<uint16_t>(o.value));
                slots.push_back(static_cast<uint16_t>(static_cast<uint64_t>(o.value) >> 16));
            } else {
                *why = "an allocation over 4 GiB";
                return false;
            }
            break;
        case W::UOP_SetFPReg:
            code(W::UOP_SetFPReg, 0);
            break;
        case W::UOP_SaveNonVol:
        case W::UOP_SaveXMM128: {
            const int64_t scale = o.op == W::UOP_SaveXMM128 ? 16 : 8;
            if (o.value < 0 || o.value > 0xffffffff) {
                *why = "a register saved outside the frame the codes reach";
                return false;
            }
            if (o.value % scale == 0 && o.value / scale <= 0xffff) {
                code(o.op, o.reg);
                slots.push_back(static_cast<uint16_t>(o.value / scale));
            } else {
                code(static_cast<uint8_t>(o.op + 1), o.reg);  // UWOP_SAVE_NONVOL_FAR, UWOP_SAVE_XMM128_FAR: 32 bits, unscaled
                slots.push_back(static_cast<uint16_t>(o.value));
                slots.push_back(static_cast<uint16_t>(static_cast<uint64_t>(o.value) >> 16));
            }
            break;
        }
        default:
            *why = "an unwind operation " + std::to_string(o.op);
            return false;
        }
    }
    if (slots.size() > 255) {
        *why = "more than 255 unwind code slots";
        return false;
    }
    const uint64_t size_of_prolog = zero_offsets ? 0 : prologue_end - begin;
    if (size_of_prolog > 255) {
        *why = "a prologue of more than 255 bytes";
        return false;
    }
    const size_t count = slots.size();
    if (slots.size() % 2) slots.push_back(0);
    info->clear();
    *info += static_cast<char>(1);  // Version 1, no flags (no handler, no chained information)
    *info += static_cast<char>(size_of_prolog);
    *info += static_cast<char>(count);
    *info += static_cast<char>(frame);
    for (const uint16_t s : slots) Put16(info, s);
    return true;
}

// The operations that describe a CFI state at once (CFA = rsp + cfa_off, each saved register at its slot): a save of each from where it is, then
// the allocation down to the return address. UWOP_SAVE_* read from the frame's base, which without a frame register is the rsp the unwinding
// starts from, so the order of the saves and the allocation does not matter
bool StateOps(const CfiState& s, std::vector<UnwindOp>* ops, std::string* why) {
    namespace W = llvm::Win64EH;
    if (s.cfa_reg != kRsp) {
        *why = "a frame on rbp outside its prologue's body";
        return false;
    }
    if (s.saved[kRa] != -8 || s.cfa_off < 8 || s.cfa_off % 8) {
        *why = "a return address the codes cannot reach (CFA rsp + " + std::to_string(s.cfa_off) + ")";
        return false;
    }
    ops->clear();
    for (int r = 0; r < kRegs; ++r) {
        // A slot below rsp is free memory: its register was restored before rsp passed it (the wrappers reload the vector registers before their
        // add, and the CFI keeps their rules), so it keeps its value
        if (r == kRa || s.saved[r] == kUnsaved || s.cfa_off + s.saved[r] < 0) continue;
        const bool xmm = r >= 17;
        ops->push_back({0, static_cast<uint8_t>(xmm ? W::UOP_SaveXMM128 : W::UOP_SaveNonVol), static_cast<uint8_t>(xmm ? r - 17 : WinOfDwarf(r)),
                        s.cfa_off + s.saved[r]});
    }
    if (s.cfa_off > 8) ops->push_back({0, W::UOP_AllocLarge, 0, s.cfa_off - 8});
    return true;
}

bool SameCfa(const CfiState& a, const CfiState& b) { return a.cfa_reg == b.cfa_reg && a.cfa_off == b.cfa_off; }
bool SameState(const CfiState& a, const CfiState& b) { return SameCfa(a, b) && a.saved == b.saved; }
bool Frameless(const CfiState& s) {
    if (s.cfa_reg != kRsp || s.cfa_off != 8) return false;
    for (int r = 0; r < kRegs; ++r) {
        if (r != kRa && s.saved[r] != kUnsaved) return false;
    }
    return true;
}

// A function's unwind information. The instructions are decoded (LLVM's x86 disassembler) and given a state each:
//   the prologue   from the first instruction that sets up a frame (the function's start, or later when LLVM shrink-wrapped it), decoded: each
//                  frame-setup instruction gives its operation at its end, up to the first other instruction; the frame so built must be the
//                  CFI's at that point (the CFA's register and offset, every saved register's slot). It and the body after it, while the CFA
//                  stays the body's, are the primary entry, with the prologue's codes
//   an epilogue    from the instruction where the CFI leaves the body's frame to the return or jmp that ends it, followed instruction by instruction
//                  (pop, add / sub / lea of rsp): swifttailcc's epilogue frees the tail-call area after its pops, which LLVM's CFI does not say
//   the rest       the CFI's row (stack arguments pushed for a call, a frameless head before a shrink-wrapped prologue, the body after an epilogue)
// An epilogue RtlVirtualUnwind recognises by its bytes stays in the entry it starts in (Windows runs it forward); every other run of instructions
// with one state gets an entry of its own whose codes describe that state at once (StateOps; for the body on a frame pointer, the prologue's codes
// at offset 0). Left out: a CFI the codes cannot express, a function whose CFI starts inside a frame (a continuation: none here, split parts have
// prologues of their own), a frame pointer set before the frame's last push or allocation (SysV's push %rbp; mov %rsp, %rbp; Win64 sets it last),
// a state on rbp outside the body, an epilogue instruction that moves rsp in a way not followed
template <typename Target>  // int64_t(uint64_t field, int64_t in_place): a rel32 jmp's target, function-relative, -1 out of the section
UnwindFn Translate(const CfiFunction& f, llvm::ArrayRef<uint8_t> code, const X86Decoder& dec, const Target& jump_target) {
    namespace W = llvm::Win64EH;
    UnwindFn u;
    if (!f.bad.empty()) {
        u.why = f.bad;
        return u;
    }
    const CfiState& entry = StateAt(f.rows, 0);
    if (entry.cfa_reg != kRsp || entry.cfa_off != 8) {
        u.why = "its CFI starts inside a frame (a continuation, which would take chained information)";
        return u;
    }
    if (entry.saved[kRa] != -8) {
        u.why = "a return address the codes cannot reach";
        return u;
    }
    struct Insn {
        uint64_t off, len;
        llvm::MCInst mc;
    };
    std::vector<Insn> insns;
    for (uint64_t pc = 0; pc < code.size();) {
        Insn in{pc, 0, {}};
        if (dec.dis->getInstruction(in.mc, in.len, code.slice(pc), pc, llvm::nulls()) != llvm::MCDisassembler::Success || !in.len) {
            u.why = "code the x86 disassembler cannot read";
            return u;
        }
        insns.push_back(in);
        pc += in.len;
    }
    const size_t n = insns.size();
    const auto name_of = [&](size_t i) { return dec.mii->getName(insns[i].mc.getOpcode()); };
    const auto before = [&](size_t i) -> const CfiState& { return StateAt(f.rows, insns[i].off); };
    const auto after = [&](size_t i) -> const CfiState& { return StateAt(f.rows, insns[i].off + insns[i].len); };
    const auto reg = [&](size_t i, unsigned k) { return k < insns[i].mc.getNumOperands() ? RegOf(dec, insns[i].mc.getOperand(k)) : RegId{}; };
    const auto rsp_imm = [&](size_t i, const char* prefix) -> int64_t {  // add / sub $imm, %rsp: imm, else 0
        const llvm::MCInst& mc = insns[i].mc;
        return name_of(i).starts_with(prefix) && mc.getNumOperands() == 3 && reg(i, 0).dwarf == kRsp && mc.getOperand(2).isImm() ? mc.getOperand(2).getImm() : 0;
    };

    // ---- The prologue: where the frame is set up, its operations ----
    size_t s = n;  // its first instruction
    for (size_t i = 0; i < n; ++i) {
        if (Frameless(before(i)) && !Frameless(after(i))) {
            s = i;
            while (s > 0 && (name_of(s - 1) == "PUSH64r" || rsp_imm(s - 1, "SUB64ri") > 0) && Frameless(before(s - 1))) --s;  // setup the CFI does not record
            break;
        }
        if (!Frameless(before(i))) break;  // a frame the instructions before did not set up: the CFI's rows only
    }
    std::vector<UnwindOp> ops;
    std::array<int64_t, kRegs> slot;
    slot.fill(kUnsaved);
    int64_t depth = 8;  // CFA - rsp
    bool fp = false, moved_after_fp = false, saved = false, saved_before_alloc = false;
    int64_t fp_depth = 0, fp_k = 0;
    size_t p = s;  // the first instruction after the prologue
    for (; p < n; ++p) {
        const Insn& in = insns[p];
        const uint64_t end = in.off + in.len;
        const llvm::StringRef name = name_of(p);
        const unsigned nops = in.mc.getNumOperands();
        const auto slot_from = [&](int r) { return SlotFrom(f.rows, end, r); };
        const auto alloc = [&](int64_t size) {
            depth += size;
            moved_after_fp |= fp;
            saved_before_alloc |= saved;
            ops.push_back({end, W::UOP_AllocLarge, 0, size});
        };
        const CfiState& later = StateFrom(f.rows, end);
        if (p == s && name == "ENDBR64") {
            continue;  // CET's landing pad
        } else if (name == "PUSH64r") {
            const RegId r = reg(p, 0);
            if (r.dwarf >= 0 && slot_from(r.dwarf) == -(depth + 8)) {
                depth += 8;
                moved_after_fp |= fp;
                saved_before_alloc |= saved;
                ops.push_back({end, W::UOP_PushNonVol, static_cast<uint8_t>(r.win), 0});
                slot[r.dwarf] = -depth;
            } else {
                alloc(8);  // a push the CFI records no save for: LLVM's push %rax that allocates 8 bytes
            }
        } else if (const int64_t size = rsp_imm(p, "SUB64ri"); size > 0) {
            alloc(size);
        } else if ((name == "MOV64rr" || name == "MOV64rr_REV") && !fp && reg(p, 0).dwarf == kRbp && reg(p, 1).dwarf == kRsp && later.cfa_reg == kRbp) {
            fp = true;
            fp_depth = depth;
            ops.push_back({end, W::UOP_SetFPReg, 0, 0});
        } else if (name == "LEA64r" && !fp && nops == 6 && reg(p, 0).dwarf == kRbp && reg(p, 1).dwarf == kRsp && !in.mc.getOperand(3).getReg() &&
                   in.mc.getOperand(4).isImm() && !in.mc.getOperand(5).getReg() && later.cfa_reg == kRbp) {
            const int64_t d = in.mc.getOperand(4).getImm();
            if (d < 0 || d % 16 || d > 240) break;  // FrameOffset cannot express it: the CFI's rows describe the rest
            fp = true;
            fp_depth = depth;
            fp_k = d / 16;
            ops.push_back({end, W::UOP_SetFPReg, 0, 0});
        } else if (name.ends_with("mr") && nops == 6 && reg(p, 0).dwarf == kRsp && in.mc.getOperand(1).isImm() && in.mc.getOperand(1).getImm() == 1 &&
                   !in.mc.getOperand(2).getReg() && in.mc.getOperand(3).isImm() && !in.mc.getOperand(4).getReg() && reg(p, 5).dwarf >= 0) {
            const RegId r = reg(p, 5);
            const int64_t at = in.mc.getOperand(3).getImm() - depth;  // rsp is CFA - depth
            if (slot_from(r.dwarf) != at) break;  // a store the CFI does not call a save: the body's
            saved = true;
            slot[r.dwarf] = at;
            ops.push_back({end, static_cast<uint8_t>(r.xmm ? W::UOP_SaveXMM128 : W::UOP_SaveNonVol), static_cast<uint8_t>(r.win), at});
        } else {
            break;
        }
    }
    // The frame so built against the CFI where the prologue ends
    const uint64_t prologue_begin = s < n ? insns[s].off : 0, prologue_end = p < n ? insns[p].off : f.size;
    const CfiState body = s < n ? StateAt(f.rows, prologue_end) : CfiState();
    bool primary = s < n && !ops.empty() && !moved_after_fp && !saved_before_alloc;
    if (primary) {
        primary = body.cfa_reg == (fp ? kRbp : kRsp) && body.cfa_off == (fp ? fp_depth - 16 * fp_k : depth) && body.saved[kRa] == -8;
        for (int r = 0; r < kRegs && primary; ++r) {
            if (r != kRa && body.saved[r] != kUnsaved && body.saved[r] != slot[r]) primary = false;
        }
    }
    std::string primary_info;
    const uint8_t frame = fp ? static_cast<uint8_t>(5 | fp_k << 4) : 0;  // FrameRegister rbp
    if (primary) {
        const int64_t base = fp ? fp_depth : depth;  // CFA - the frame's base (rsp after the prologue, or when the frame pointer was set)
        for (UnwindOp& o : ops) {
            if (o.op == W::UOP_SaveNonVol || o.op == W::UOP_SaveXMM128) o.value += base;
        }
        std::string why;
        if (!EncodeInfo(ops, prologue_begin, prologue_end, frame, false, &primary_info, &why)) primary = false;  // over 255 bytes: the CFI's rows
    }
    if (!primary && fp) {
        u.why = moved_after_fp ? "a frame pointer set before the frame's last push or allocation" : "a frame on rbp whose prologue does not match its CFI";
        return u;
    }
    if (s < n && !primary) u.more.insert("a prologue described by its CFI's rows");
    if (s > 0 && s < n) u.more.insert("a frame set up after the function's start (shrink-wrapping)");

    // ---- Each instruction's description ----
    //   kind 0  the primary entry (the prologue and the body after it)
    //   kind 1  the body's frame elsewhere, rsp lower by extra (stack arguments pushed for a call): the prologue's codes at once, after an allocation
    //   kind 2  a state of its own (an epilogue's, a frameless stretch, any state when the prologue could not be decoded): StateOps
    struct Desc {
        int kind = 2;
        int64_t extra = 0;
        CfiState state;
    };
    std::vector<Desc> desc(n);
    const auto shallower = [&](const CfiState& st) { return body.cfa_reg == kRsp ? st.cfa_reg == kRsp && st.cfa_off < body.cfa_off : st.cfa_reg == kRsp; };
    const auto body_desc = [&](size_t i, int64_t extra) {  // the body's frame at instruction i
        Desc d;
        d.kind = extra == 0 && i > 0 && desc[i - 1].kind == 0 ? 0 : 1;
        d.extra = extra;
        return d;
    };
    for (size_t i = 0; i < n; ++i) {
        if (primary && i >= s && i < p) {
            desc[i].kind = 0;
            continue;
        }
        const CfiState& b = before(i);
        if (!primary || i < p) {
            desc[i].state = b;
            continue;
        }
        const llvm::StringRef nm_i = name_of(i);
        const bool frees = nm_i == "POP64r" || rsp_imm(i, "ADD64ri") > 0 || (nm_i == "LEA64r" && reg(i, 0).dwarf == kRsp);
        if (SameCfa(b, body) && shallower(after(i)) && frees) {
            // An epilogue: its states followed from the body's by the instructions, up to the return or jmp that ends it. (Where the CFI leaves the
            // body's frame at an instruction that frees nothing, a jump, the row is the next block's: its instructions take their own rows.)
            const auto bytes = [&](size_t j) { return code.data() + insns[j].off; };
            size_t k = i;
            if (IsAddRsp(bytes(k), insns[k].len) || (fp && IsLeaRsp(bytes(k), insns[k].len))) ++k;
            while (k < n && IsPop(bytes(k), insns[k].len)) ++k;
            std::string odd;  // why RtlVirtualUnwind does not recognise it; empty when it does
            if (k >= n) {
                odd = "an epilogue that runs off the function's end";
            } else {
                const uint8_t* q = bytes(k);
                const uint64_t len = insns[k].len;
                const bool ret = (len == 1 && q[0] == 0xc3) || (len == 3 && q[0] == 0xc2) || (len == 2 && q[0] == 0xf3 && q[1] == 0xc3);
                if (ret) {
                } else if ((q[0] == 0xe9 && len == 5) || (q[0] == 0xeb && len == 2)) {
                    const int64_t to = q[0] == 0xe9 ? jump_target(insns[k].off + 1, static_cast<int32_t>(q[1] | q[2] << 8 | q[3] << 16 | static_cast<uint32_t>(q[4]) << 24))
                                                    : static_cast<int64_t>(insns[k].off + len) + static_cast<int8_t>(q[1]);
                    if (to >= 0 && static_cast<uint64_t>(to) < f.size) odd = "a jmp within the function";
                } else {
                    const size_t x = len > 1 && (q[0] & 0xf0) == 0x40 ? 1 : 0;
                    const bool rex_w = x && (q[0] & 0x08);
                    if (x + 1 < len && q[x] == 0xff && ((q[x + 1] >> 3) & 7) == 4) {
                        const unsigned mod = q[x + 1] >> 6;
                        if (!rex_w) odd = "an indirect tail jump without REX.W";
                        else if (mod != 0 && mod != 3) odd = "an indirect tail jump through a displaced address";
                    } else {
                        odd = dec.mii->getName(insns[k].mc.getOpcode()).lower() + " after the frame's release";
                    }
                }
            }
            if (!odd.empty()) u.more.insert("an epilogue Windows does not recognise (" + odd + ")");
            const Desc first = body_desc(i, 0);
            CfiState st = body;  // on rsp: followed from the body's; on a frame pointer: the CFI's once rsp holds the frame again
            for (size_t j = i; j < n; ++j) {
                if (odd.empty() || j == i) {
                    desc[j] = first;  // RtlVirtualUnwind runs a recognised epilogue forward from any of its instructions
                } else {
                    desc[j].kind = 2;
                    desc[j].state = st;
                }
                const llvm::MCInstrDesc& d = dec.mii->get(insns[j].mc.getOpcode());
                const llvm::StringRef nm = name_of(j);
                const bool last = d.isReturn() || (d.isBranch() && d.isBarrier());
                if (j == i && body.cfa_reg == kRbp) {
                    st = after(i);
                } else if (nm == "POP64r") {
                    const RegId r = reg(j, 0);
                    if (r.dwarf >= 0 && st.saved[r.dwarf] == -st.cfa_off) st.saved[r.dwarf] = kUnsaved;
                    st.cfa_off -= 8;
                } else if (nm == "PUSH64r") {
                    st.cfa_off += 8;
                } else if (const int64_t add = rsp_imm(j, "ADD64ri")) {
                    st.cfa_off -= add;
                } else if (const int64_t sub = rsp_imm(j, "SUB64ri")) {
                    st.cfa_off += sub;
                } else if (nm == "LEA64r" && insns[j].mc.getNumOperands() == 6 && reg(j, 0).dwarf == kRsp && reg(j, 1).dwarf == kRsp &&
                           !insns[j].mc.getOperand(3).getReg() && insns[j].mc.getOperand(4).isImm()) {
                    st.cfa_off -= insns[j].mc.getOperand(4).getImm();
                } else if (!last && !d.isCall() && d.hasDefOfPhysReg(insns[j].mc, dec.rsp, *dec.mri)) {
                    u.why = "an epilogue instruction that moves rsp in a way not followed (" + nm.lower() + ")";
                    return u;
                }
                if (st.cfa_off < 8) {
                    // Past the return address: a swifttailcc tail call that pops the function's own stack arguments copies the return address up
                    // first (mov N(%rsp), %reg; mov %reg, N + size(%rsp)) and frees down to it, as the jmp after it requires: it is at rsp now
                    bool restored = true;
                    for (int r = 0; r < kRegs; ++r) restored &= r == kRa || st.saved[r] == kUnsaved || st.cfa_off + st.saved[r] < 0;
                    if (!restored) {
                        u.why = "an epilogue that frees the return address before its registers";
                        return u;
                    }
                    st = CfiState();
                    st.saved[kRa] = -8;
                }
                i = j;
                if (last) break;
            }
            continue;
        }
        if (b.cfa_reg == body.cfa_reg && b.cfa_off >= body.cfa_off) {
            desc[i] = body_desc(i, b.cfa_off - body.cfa_off);  // the body, or below it (rsp-based: stack arguments of a call)
            if (desc[i].extra) u.more.insert("rsp moved in the body (stack arguments)");
            continue;
        }
        desc[i].state = b;  // after an epilogue's end, before the CFI says the frame again
    }

    // A call whose callee pops its stack arguments (swifttailcc): the CFI's row at the return address is the state after the pop, but RtlVirtualUnwind
    // reaches that address from the callee, its arguments still on the stack: the instruction there takes the call's description (a sample taken
    // while it runs is the one place this is off, by the arguments' size)
    for (size_t i = 0; i + 1 < n; ++i) {
        const CfiState &at_call = before(i), &at_return = before(i + 1);
        if (dec.mii->get(insns[i].mc.getOpcode()).isCall() && at_call.cfa_reg == kRsp && at_return.cfa_reg == kRsp && at_return.cfa_off < at_call.cfa_off) {
            desc[i + 1] = desc[i];
            u.more.insert("a call whose callee pops its arguments");
        }
    }

    // ---- The entries: a run of instructions with one description each ----
    const auto same = [](const Desc& x, const Desc& y) {
        return x.kind == y.kind && (x.kind == 0 || (x.kind == 1 && x.extra == y.extra) || (x.kind == 2 && SameState(x.state, y.state)));
    };
    for (size_t i = 0; i < n;) {
        size_t j = i + 1;
        while (j < n && same(desc[j], desc[i])) ++j;
        UnwindEntry e{insns[i].off, j < n ? insns[j].off : f.size, {}};
        std::string why;
        bool ok = true;
        if (desc[i].kind == 0) {
            e.info = primary_info;
        } else if (desc[i].kind == 1) {
            // The prologue's operations at offset 0, the saves read from where they are now (extra bytes further from rsp), after an allocation of extra
            std::vector<UnwindOp> b_ops = ops;
            for (UnwindOp& o : b_ops) {
                if (o.op == W::UOP_SaveNonVol || o.op == W::UOP_SaveXMM128) o.value += desc[i].extra;
            }
            if (desc[i].extra) b_ops.push_back({0, W::UOP_AllocLarge, 0, desc[i].extra});
            ok = EncodeInfo(b_ops, 0, 0, frame, true, &e.info, &why);
        } else {
            std::vector<UnwindOp> st_ops;
            ok = StateOps(desc[i].state, &st_ops, &why) && EncodeInfo(st_ops, 0, 0, 0, true, &e.info, &why);
        }
        if (!ok) {
            u.why = why;
            u.entries.clear();
            return u;
        }
        u.entries.push_back(std::move(e));
        i = j;
    }
    return u;
}

// The unwind information of the whole object: .xdata (the UNWIND_INFOs), .pdata (a RUNTIME_FUNCTION per described function, its three fields
// relocated: ADDR32NB against its code section's symbol or .xdata's, the offsets written in place), the counts for the log
struct Unwind {
    std::string xdata, pdata;
    struct Fix {
        uint32_t offset;   // in .pdata
        uint32_t section;  // the ELF index of the code section, or UINT32_MAX for .xdata
    };
    std::vector<Fix> fixes;
    CoffStats::UnwindCounts counts;
};

std::string Reasons(const std::map<std::string, uint64_t>& m) {
    std::string s;
    for (const auto& [why, n] : m) s += (s.empty() ? "" : ", ") + why + " (" + std::to_string(n) + ")";
    return s;
}

// The unwind information of the object's code sections (secs: the classified sections, their bytes still the ELF's; eh: .eh_frame's ELF index,
// SIZE_MAX when it has none)
bool BuildUnwind(const llvm::object::ELFFile<ELFT>& ef, llvm::ArrayRef<Shdr> shdrs, const std::vector<Section>& secs, size_t eh, const Shdr* symtab,
                 Unwind* out, std::string* error) {
    auto esyms = ef.symbols(symtab);
    if (!esyms) {
        *error = llvm::toString(esyms.takeError());
        return false;
    }
    auto strtab = ef.getStringTableForSymtab(*symtab);
    if (!strtab) {
        *error = llvm::toString(strtab.takeError());
        return false;
    }
    const auto is_code = [&](uint64_t shndx) { return shndx < secs.size() && secs[shndx].kind == Kind::Text; };
    // The function symbols of the code, for the functions without an FDE
    std::map<std::pair<uint64_t, uint64_t>, std::string> no_fde;
    for (const auto& es : *esyms) {
        if (es.getType() == llvm::ELF::STT_FUNC && es.st_size && is_code(es.st_shndx)) {
            auto name = es.getName(*strtab);
            no_fde.emplace(std::make_pair(static_cast<uint64_t>(es.st_shndx), static_cast<uint64_t>(es.st_value)), name ? name->str() : "?");
        }
    }
    // The relocations: .eh_frame's (each FDE's function) and the code's pc-relative ones (where a jmp of an epilogue goes)
    std::map<uint64_t, std::pair<uint32_t, uint64_t>> fde_at;
    std::map<uint64_t, std::map<uint64_t, std::pair<int64_t, int64_t>>> code_rel;  // code section -> field offset -> (target section or -1, S + A)
    for (const Shdr& rs : shdrs) {
        if (rs.sh_type != llvm::ELF::SHT_RELA || (rs.sh_info != eh && !is_code(rs.sh_info))) continue;
        auto relas = ef.relas(rs);
        if (!relas) {
            *error = llvm::toString(relas.takeError());
            return false;
        }
        for (const auto& r : *relas) {
            const uint32_t type = r.getType(false), sym = r.getSymbol(false);
            if (sym == 0 || sym >= esyms->size()) continue;
            const auto& es = (*esyms)[sym];
            const bool defined = es.st_shndx != llvm::ELF::SHN_UNDEF && es.st_shndx < llvm::ELF::SHN_LORESERVE;
            if (rs.sh_info == eh) {
                if (type != llvm::ELF::R_X86_64_PC32 && type != llvm::ELF::R_X86_64_64) {
                    *error = ".eh_frame: a relocation of type " + std::to_string(type) + " at +0x" + llvm::utohexstr(r.r_offset);
                    return false;
                }
                if (defined && is_code(es.st_shndx)) fde_at[r.r_offset] = {es.st_shndx, es.st_value + static_cast<uint64_t>(r.r_addend)};
            } else if (type == llvm::ELF::R_X86_64_PC32 || type == llvm::ELF::R_X86_64_PLT32) {
                code_rel[rs.sh_info][r.r_offset] = {defined ? static_cast<int64_t>(es.st_shndx) : -1, static_cast<int64_t>(es.st_value) + r.r_addend};
            }
        }
    }
    std::vector<CfiFunction> fns;
    if (eh != SIZE_MAX) {
        auto bytes = ef.getSectionContents(shdrs[eh]);
        if (!bytes) {
            *error = llvm::toString(bytes.takeError());
            return false;
        }
        if (!ReadEhFrame(llvm::StringRef(reinterpret_cast<const char*>(bytes->data()), bytes->size()), fde_at, &fns, error)) return false;
    }
    X86Decoder dec;
    if (!fns.empty() && !dec.Init(error)) return false;
    std::map<std::string, uint64_t> why, more;
    std::map<std::string, uint32_t> info_at;  // UNWIND_INFO -> its offset in .xdata
    CoffStats::UnwindCounts& c = out->counts;
    static const std::map<uint64_t, std::pair<int64_t, int64_t>> kNoRel;
    for (CfiFunction& f : fns) {
        const Section& s = secs[f.section];
        if (f.begin + f.size > s.data.size()) {
            *error = "an FDE past the end of " + s.name + " (+0x" + llvm::utohexstr(f.begin) + ", 0x" + llvm::utohexstr(f.size) + " bytes)";
            return false;
        }
        ++c.functions;
        no_fde.erase({f.section, f.begin});
        const auto rel = code_rel.find(f.section);
        const auto& rels = rel == code_rel.end() ? kNoRel : rel->second;
        // A jmp's target, function-relative, from its rel32 field at function offset field (the relocation's when the field has one)
        const auto target = [&](uint64_t field, int64_t in_place) -> int64_t {
            const auto r = rels.find(f.begin + field);
            if (r == rels.end()) return static_cast<int64_t>(field) + 4 + in_place;
            if (r->second.first != static_cast<int64_t>(f.section)) return -1;
            return r->second.second + 4 - static_cast<int64_t>(f.begin);
        };
        const llvm::ArrayRef<uint8_t> code(reinterpret_cast<const uint8_t*>(s.data.data()) + f.begin, f.size);
        const UnwindFn u = Translate(f, code, dec, target);
        if (!u.why.empty()) {
            ++c.not_described;
            ++why[u.why];
            continue;
        }
        ++c.described;
        c.entries += u.entries.size();
        if (u.entries.size() > 1) ++c.split;
        for (const std::string& m : u.more) ++more[m];
        for (const UnwindEntry& e : u.entries) {
            auto [it, fresh] = info_at.emplace(e.info, static_cast<uint32_t>(out->xdata.size()));  // one copy of each UNWIND_INFO
            if (fresh) out->xdata += e.info;  // 4-aligned: every UNWIND_INFO is 4 + 4k bytes
            const uint32_t po = static_cast<uint32_t>(out->pdata.size());
            Put32(&out->pdata, static_cast<uint32_t>(f.begin + e.begin));  // BeginAddress
            Put32(&out->pdata, static_cast<uint32_t>(f.begin + e.end));    // EndAddress
            Put32(&out->pdata, it->second);                                // UnwindInfoAddress
            out->fixes.push_back({po, f.section});
            out->fixes.push_back({po + 4, f.section});
            out->fixes.push_back({po + 8, UINT32_MAX});
        }
    }
    c.without_cfi = no_fde.size();
    c.functions += no_fde.size();
    for (const auto& [at, name] : no_fde) {
        if (c.without_cfi_names.size() > 200) break;
        c.without_cfi_names += (c.without_cfi_names.empty() ? "" : " ") + name;
    }
    c.why_not = Reasons(why);
    c.split_why = Reasons(more);
    return true;
}

}  // namespace

bool ElfToCoff(llvm::StringRef elf, bool keep_debug, std::string* coff, CoffStats* stats, std::string* error) {
    const auto fail = [&](const std::string& what) {
        *error = "ELF to COFF: " + what;
        return false;
    };
    auto file = llvm::object::ELFFile<ELFT>::create(elf);
    if (!file) return fail(llvm::toString(file.takeError()));
    const llvm::object::ELFFile<ELFT>& ef = *file;
    if (ef.getHeader().e_type != llvm::ELF::ET_REL || ef.getHeader().e_machine != llvm::ELF::EM_X86_64) {
        return fail("not an x86-64 relocatable ELF object");
    }
    auto shdrs = ef.sections();
    if (!shdrs) return fail(llvm::toString(shdrs.takeError()));
    auto shstrtab = ef.getSectionStringTable(*shdrs);
    if (!shstrtab) return fail(llvm::toString(shstrtab.takeError()));
    const size_t n = shdrs->size();

    // ---- The sections: which are kept, and as what ----
    std::vector<Section> secs(n);
    const Shdr* symtab = nullptr;
    size_t eh = SIZE_MAX;
    for (size_t i = 0; i < n; ++i) {
        const Shdr& s = (*shdrs)[i];
        auto name_or = ef.getSectionName(s, *shstrtab);
        if (!name_or) return fail(llvm::toString(name_or.takeError()));
        const llvm::StringRef name = *name_or;
        Section& out = secs[i];
        switch (s.sh_type) {
        case llvm::ELF::SHT_SYMTAB:
            symtab = &s;
            continue;
        case llvm::ELF::SHT_NULL:
        case llvm::ELF::SHT_STRTAB:
        case llvm::ELF::SHT_RELA:
        case llvm::ELF::SHT_GROUP:
        case llvm::ELF::SHT_LLVM_ADDRSIG:
        case llvm::ELF::SHT_SYMTAB_SHNDX:
            continue;
        case llvm::ELF::SHT_REL:
            return fail("a REL section (" + name.str() + "): x86-64 objects have RELA");
        default:
            break;
        }
        if (s.sh_flags & llvm::ELF::SHF_EXCLUDE) {
            ++stats->dropped_sections;  // the line marks (pack.cpp, "The line table"), read from the units before
            continue;
        }
        if (!(s.sh_flags & llvm::ELF::SHF_ALLOC)) {
            if (keep_debug && name.starts_with(".debug_")) {
                out.kind = Kind::Debug;
            } else {
                ++stats->dropped_sections;  // .comment, .note.GNU-stack, DWARF without debug information
                continue;
            }
        } else if (s.sh_type == llvm::ELF::SHT_X86_64_UNWIND || name == ".eh_frame") {
            ++stats->dropped_sections;  // ELF's unwind information: translated below
            if (eh != SIZE_MAX) return fail("two .eh_frame sections");
            eh = i;
            continue;
        } else if (s.sh_flags & llvm::ELF::SHF_TLS) {
            return fail("a thread-local section (" + name.str() + "), which a pack does not have");
        } else if (s.sh_type == llvm::ELF::SHT_INIT_ARRAY || s.sh_type == llvm::ELF::SHT_FINI_ARRAY || s.sh_type == llvm::ELF::SHT_PREINIT_ARRAY) {
            return fail("an initializer array (" + name.str() + "), which a pack does not have (its DLL has no entry point)");
        } else if (s.sh_type != llvm::ELF::SHT_PROGBITS && s.sh_type != llvm::ELF::SHT_NOBITS) {
            return fail("section " + name.str() + " of ELF type " + llvm::utohexstr(s.sh_type) + ", which has no COFF form here");
        } else if (s.sh_flags & llvm::ELF::SHF_EXECINSTR) {
            out.kind = Kind::Text;
        } else if (s.sh_type == llvm::ELF::SHT_NOBITS) {
            out.kind = Kind::Bss;
        } else if ((s.sh_flags & llvm::ELF::SHF_WRITE) && !name.starts_with(".data.rel.ro")) {
            out.kind = Kind::Data;
        } else {
            out.kind = Kind::Rdata;  // read-only data, and .data.rel.ro: written by relocation only (the loader applies base relocations to it)
        }
        namespace C = llvm::COFF;
        const uint32_t align = AlignFlag(s.sh_addralign, error);
        if (!align) return fail(name.str() + ": " + *error);
        switch (out.kind) {
        case Kind::Text:
            out.name = ".text";
            out.flags = C::IMAGE_SCN_CNT_CODE | C::IMAGE_SCN_MEM_EXECUTE | C::IMAGE_SCN_MEM_READ | align;
            break;
        case Kind::Rdata:
            out.name = ".rdata";
            out.flags = C::IMAGE_SCN_CNT_INITIALIZED_DATA | C::IMAGE_SCN_MEM_READ | align;
            break;
        case Kind::Data:
            out.name = ".data";
            out.flags = C::IMAGE_SCN_CNT_INITIALIZED_DATA | C::IMAGE_SCN_MEM_READ | C::IMAGE_SCN_MEM_WRITE | align;
            break;
        case Kind::Bss:
            out.name = ".bss";
            out.flags = C::IMAGE_SCN_CNT_UNINITIALIZED_DATA | C::IMAGE_SCN_MEM_READ | C::IMAGE_SCN_MEM_WRITE | align;
            break;
        case Kind::Debug:
            out.name = name.str();
            out.flags = C::IMAGE_SCN_CNT_INITIALIZED_DATA | C::IMAGE_SCN_MEM_DISCARDABLE | C::IMAGE_SCN_MEM_READ | C::IMAGE_SCN_ALIGN_1BYTES;
            break;
        case Kind::Drop:
            break;
        }
        out.size = s.sh_size;
        if (out.kind != Kind::Bss) {
            auto bytes = ef.getSectionContents(s);
            if (!bytes) return fail(llvm::toString(bytes.takeError()));
            out.data.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        }
        if (out.size > std::numeric_limits<uint32_t>::max()) return fail(name.str() + " is larger than 4 GiB");
    }
    if (!symtab) return fail("no symbol table");

    // ---- The unwind information, as two more sections (before the symbols: they get section symbols too) ----
    Unwind uw;
    if (!BuildUnwind(ef, *shdrs, secs, eh, symtab, &uw, error)) return fail("unwind information: " + *error);
    stats->unwind = uw.counts;
    const size_t xdata = secs.size(), pdata = xdata + 1;
    if (!uw.pdata.empty()) {
        namespace C = llvm::COFF;
        for (auto [name, bytes] : {std::make_pair(".xdata", &uw.xdata), std::make_pair(".pdata", &uw.pdata)}) {
            Section x;
            x.name = name;
            x.kind = Kind::Rdata;
            x.flags = C::IMAGE_SCN_CNT_INITIALIZED_DATA | C::IMAGE_SCN_MEM_READ | C::IMAGE_SCN_ALIGN_4BYTES;
            x.size = bytes->size();
            x.data = *bytes;
            secs.push_back(std::move(x));
        }
    }

    // ---- The symbols: the kept sections' first (each with its section definition), then the ELF symbols in their order ----
    std::vector<Symbol> syms;
    uint32_t records = 0;
    uint32_t number = 0;
    for (Section& s : secs) {
        if (s.kind == Kind::Drop) continue;
        s.number = ++number;
        s.symbol = records;
        Symbol sym;
        sym.name = s.name;
        sym.section = static_cast<int32_t>(s.number);
        sym.storage = llvm::COFF::IMAGE_SYM_CLASS_STATIC;
        sym.section_def = true;
        sym.sec = &s;
        syms.push_back(sym);
        records += 2;
    }
    if (number > 0xFEFF) return fail(std::to_string(number) + " sections, more than a COFF object holds");
    auto esyms = ef.symbols(symtab);
    if (!esyms) return fail(llvm::toString(esyms.takeError()));
    auto strtab = ef.getStringTableForSymtab(*symtab);
    if (!strtab) return fail(llvm::toString(strtab.takeError()));
    constexpr uint32_t kNone = UINT32_MAX;
    std::vector<uint32_t> index_of(esyms->size(), kNone);  // ELF symbol -> COFF symbol record
    std::vector<std::string> names(esyms->size());
    for (size_t j = 1; j < esyms->size(); ++j) {
        const auto& es = (*esyms)[j];
        auto name_or = es.getName(*strtab);
        if (!name_or) return fail(llvm::toString(name_or.takeError()));
        names[j] = name_or->str();
        const uint8_t type = es.getType(), bind = es.getBinding();
        const uint16_t shndx = es.st_shndx;
        if (type == llvm::ELF::STT_FILE) continue;
        if (shndx == llvm::ELF::SHN_XINDEX) return fail("symbol " + names[j] + " has an extended section index");
        if (type == llvm::ELF::STT_SECTION) {
            if (shndx < n && secs[shndx].kind != Kind::Drop) index_of[j] = secs[shndx].symbol;
            continue;
        }
        if (shndx == llvm::ELF::SHN_ABS || shndx == llvm::ELF::SHN_COMMON) continue;  // an error only when a relocation names it
        Symbol sym;
        sym.name = names[j].empty() ? "$nxaot.sym." + std::to_string(j) : names[j];
        sym.storage = bind == llvm::ELF::STB_LOCAL ? llvm::COFF::IMAGE_SYM_CLASS_STATIC : llvm::COFF::IMAGE_SYM_CLASS_EXTERNAL;
        if (shndx == llvm::ELF::SHN_UNDEF) {
            if (bind == llvm::ELF::STB_WEAK) return fail("the weak undefined symbol " + names[j] + ", which COFF has no form for here");
            if (bind == llvm::ELF::STB_LOCAL) continue;
        } else {
            if (shndx >= n || secs[shndx].kind == Kind::Drop) continue;  // in a dropped section (.eh_frame's labels)
            if (es.st_value > std::numeric_limits<uint32_t>::max()) return fail("symbol " + names[j] + " lies beyond 4 GiB in its section");
            sym.section = static_cast<int32_t>(secs[shndx].number);
            sym.value = static_cast<uint32_t>(es.st_value);
            if (type == llvm::ELF::STT_FUNC) sym.type = llvm::COFF::IMAGE_SYM_DTYPE_FUNCTION << llvm::COFF::SCT_COMPLEX_TYPE_SHIFT;
        }
        index_of[j] = records;
        syms.push_back(sym);
        records += 1;
    }

    // ---- The relocations, the addends written into the sections' bytes ----
    for (size_t i = 0; i < n; ++i) {
        const Shdr& rs = (*shdrs)[i];
        if (rs.sh_type != llvm::ELF::SHT_RELA) continue;
        if (rs.sh_info >= n) return fail("a relocation section for no section");
        Section& out = secs[rs.sh_info];
        if (out.kind == Kind::Drop) continue;
        auto relas = ef.relas(rs);
        if (!relas) return fail(llvm::toString(relas.takeError()));
        for (const auto& r : *relas) {
            const uint32_t type = r.getType(false), sym = r.getSymbol(false);
            const uint64_t at = r.r_offset;
            const int64_t addend = r.r_addend;
            const auto site = [&] {
                return "a relocation of type " + std::to_string(type) + " at +0x" + llvm::utohexstr(at) + " of section " +
                       std::to_string(rs.sh_info) + " (" + out.name + ") against " + (sym < names.size() ? "'" + names[sym] + "'" : "?");
            };
            if (sym == 0 || sym >= index_of.size() || index_of[sym] == kNone) {
                return fail(site() + ": its symbol has no COFF form (an absolute or common symbol, or one in a dropped section)");
            }
            Reloc cr;
            cr.offset = static_cast<uint32_t>(at);
            cr.symbol = index_of[sym];
            const auto put = [&](uint64_t value, unsigned bytes) {
                if (out.kind == Kind::Bss || at + bytes > out.data.size()) return false;
                std::memcpy(out.data.data() + at, &value, bytes);
                return true;
            };
            bool ok = false;
            switch (type) {
            case llvm::ELF::R_X86_64_PC32:
            case llvm::ELF::R_X86_64_PLT32: {
                // S + A - P in ELF; S + addend - (P + 4) in COFF
                const int64_t v = addend + 4;
                if (v < INT32_MIN || v > INT32_MAX) return fail(site() + ": its addend does not fit 32 bits");
                ok = put(static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(v))), 4);
                cr.type = llvm::COFF::IMAGE_REL_AMD64_REL32;
                break;
            }
            case llvm::ELF::R_X86_64_64:
                ok = put(static_cast<uint64_t>(addend), 8);
                cr.type = llvm::COFF::IMAGE_REL_AMD64_ADDR64;
                break;
            case llvm::ELF::R_X86_64_32:
            case llvm::ELF::R_X86_64_32S: {
                // In a DWARF section: an offset into another DWARF section (its section symbol plus the addend), COFF's SECREL
                const auto& es = (*esyms)[sym];
                const bool to_debug = es.st_shndx < n && secs[es.st_shndx].kind == Kind::Debug;
                if (out.kind != Kind::Debug || !to_debug) {
                    return fail(site() + ": a 32-bit absolute address, which a DLL above 4 GiB cannot hold");
                }
                if (addend < 0 || addend > UINT32_MAX) return fail(site() + ": its addend does not fit 32 bits");
                ok = put(static_cast<uint64_t>(addend) + es.st_value, 4);
                cr.symbol = secs[es.st_shndx].symbol;  // the section's symbol: SECREL counts from the start of its section
                cr.type = llvm::COFF::IMAGE_REL_AMD64_SECREL;
                break;
            }
            default:
                return fail(site() + ": no COFF form (a GOT, TLS or 64-bit pc-relative relocation, which a pack's code does not use)");
            }
            if (!ok) return fail(site() + ": outside its section's bytes");
            out.relocs.push_back(cr);
        }
    }

    // .pdata's relocations: each RUNTIME_FUNCTION's addresses as RVAs (ADDR32NB) from its code section's symbol and .xdata's, the offsets in place
    if (!uw.pdata.empty()) {
        for (const Unwind::Fix& fx : uw.fixes) {
            secs[pdata].relocs.push_back({fx.offset, fx.section == UINT32_MAX ? secs[xdata].symbol : secs[fx.section].symbol,
                                          llvm::COFF::IMAGE_REL_AMD64_ADDR32NB});
        }
    }

    // ---- The file: header, section headers, each section's bytes and relocations, the symbol table, the string table ----
    std::string strings;
    const auto string_offset = [&](const std::string& s) {
        const uint32_t off = static_cast<uint32_t>(4 + strings.size());
        strings += s;
        strings += '\0';
        return off;
    };
    std::vector<const Section*> kept;
    for (const Section& s : secs) {
        if (s.kind != Kind::Drop) kept.push_back(&s);
    }
    uint64_t at = llvm::COFF::Header16Size + kept.size() * llvm::COFF::SectionSize;
    std::string headers, body;
    uint64_t relocations = 0;
    for (const Section* s : kept) {
        const uint64_t raw = s->kind == Kind::Bss ? 0 : s->data.size();
        const bool ovfl = s->relocs.size() >= 0xFFFF;
        const uint64_t nrel = s->relocs.size() + (ovfl ? 1 : 0);
        const uint64_t raw_at = raw ? at : 0;
        at += raw;
        const uint64_t rel_at = nrel ? at : 0;
        at += nrel * llvm::COFF::RelocationSize;
        if (at > std::numeric_limits<uint32_t>::max()) return fail("the object would be larger than 4 GiB");
        // Name: up to 8 bytes in place, else "/<offset>" into the string table
        char name[8] = {};
        if (s->name.size() <= 8) {
            std::memcpy(name, s->name.data(), s->name.size());
        } else {
            const uint32_t off = string_offset(s->name);
            if (off > 9'999'999) return fail("the string table is too large for a section's long name");
            const std::string ref = "/" + std::to_string(off);
            std::memcpy(name, ref.data(), ref.size());
        }
        headers.append(name, 8);
        Put32(&headers, 0);                                   // VirtualSize
        Put32(&headers, 0);                                   // VirtualAddress
        Put32(&headers, static_cast<uint32_t>(s->size));      // SizeOfRawData (a .bss's size)
        Put32(&headers, static_cast<uint32_t>(raw_at));       // PointerToRawData
        Put32(&headers, static_cast<uint32_t>(rel_at));       // PointerToRelocations
        Put32(&headers, 0);                                   // PointerToLinenumbers
        Put16(&headers, static_cast<uint16_t>(ovfl ? 0xFFFF : nrel));
        Put16(&headers, 0);                                   // NumberOfLinenumbers
        Put32(&headers, s->flags | (ovfl ? llvm::COFF::IMAGE_SCN_LNK_NRELOC_OVFL : 0));
        if (raw) body += s->data;
        if (ovfl) {  // the first relocation holds the count, itself included
            Put32(&body, static_cast<uint32_t>(nrel));
            Put32(&body, 0);
            Put16(&body, 0);
        }
        for (const Reloc& r : s->relocs) {
            Put32(&body, r.offset);
            Put32(&body, r.symbol);
            Put16(&body, r.type);
        }
        relocations += s->relocs.size();
    }
    std::string table;
    for (const Symbol& sym : syms) {
        if (sym.name.size() <= 8) {
            char name[8] = {};
            std::memcpy(name, sym.name.data(), sym.name.size());
            table.append(name, 8);
        } else {
            Put32(&table, 0);
            Put32(&table, string_offset(sym.name));
        }
        Put32(&table, sym.value);
        Put16(&table, static_cast<uint16_t>(sym.section));
        Put16(&table, sym.type);
        table += static_cast<char>(sym.storage);
        table += static_cast<char>(sym.section_def ? 1 : 0);
        if (sym.section_def) {  // the section definition: length, relocations, line numbers, checksum, number, selection
            const uint64_t nrel = sym.sec->relocs.size();
            Put32(&table, static_cast<uint32_t>(sym.sec->size));
            Put16(&table, static_cast<uint16_t>(nrel >= 0xFFFF ? 0xFFFF : nrel));
            Put16(&table, 0);
            Put32(&table, 0);
            Put16(&table, 0);
            table += '\0';
            table.append(3, '\0');
        }
    }
    std::string& o = *coff;
    o.clear();
    Put16(&o, llvm::COFF::IMAGE_FILE_MACHINE_AMD64);
    Put16(&o, static_cast<uint16_t>(kept.size()));
    Put32(&o, 0);  // TimeDateStamp: none, for a deterministic object
    Put32(&o, static_cast<uint32_t>(at));  // PointerToSymbolTable
    Put32(&o, records);
    Put16(&o, 0);  // SizeOfOptionalHeader
    Put16(&o, 0);  // Characteristics
    o += headers;
    o += body;
    o += table;
    Put32(&o, static_cast<uint32_t>(4 + strings.size()));
    o += strings;
    stats->sections = kept.size();
    stats->symbols = records;
    stats->relocations = relocations;
    return true;
}

}  // namespace nxaot::build
