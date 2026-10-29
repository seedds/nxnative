#include "analysis/census.h"

#include <cstdio>
#include <cstring>
#include <optional>
#include <unordered_set>
#include <vector>

#include "decode/translate.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/frontend/A64/decoder/a64.h"
#include "dynarmic/frontend/A64/translate/impl/impl.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace nxaot::analysis {

namespace {

using namespace Dynarmic;

// Keys printed even when their count is 0, so that the phase 0 questions get an explicit answer
const char* const kAlwaysKeys[] = {
    "words", "decoded", "undecoded", "unallocated",
    "class.reserved", "class.unallocated", "class.sve", "class.dp_imm", "class.branch_sys", "class.loadstore", "class.dp_reg", "class.fp_simd",
    "fp_simd.dp", "fp_simd.loadstore", "crypto.aes", "crypto.sha1", "crypto.sha256", "crypto.pmull64", "crypto.v82", "crc32", "crc32c",
    "lse", "lse.cas", "lse.casp", "lse.ldop", "lse.swp", "rcpc.ldapr",
    "mrs.nzcv", "mrs.fpcr", "mrs.fpsr", "mrs.tpidr_el0", "mrs.tpidrro_el0", "mrs.cntfrq_el0", "mrs.cntpct_el0", "mrs.cntvct_el0", "mrs.ctr_el0",
    "mrs.dczid_el0", "msr.nzcv", "msr.fpcr", "msr.fpsr", "msr.tpidr_el0", "msr.pstate_imm",
    "dc.zva", "dc.other", "ic", "hint.nop", "hint.yield", "hint.wfe", "hint.wfi", "hint.sev", "hint.other",
    "barrier.dmb", "barrier.dsb", "barrier.isb", "clrex",
    "branch.b", "branch.bl", "branch.cond", "branch.cbz", "branch.tbz", "branch.br", "branch.blr", "branch.ret",
    "svc", "brk", "udf", "hvc_smc",
    "excl.load", "excl.store", "ordered.ldar", "ordered.stlr", "ordered.lor",
    "excl.windows", "excl.windows.aba_sensitive", "excl.windows.other_load", "excl.unpaired",
    "sp.from_memory",
    "answer.lse_present", "answer.fpsr_read", "answer.sp_from_memory", "answer.aba_sensitive_windows", "answer.cntfrq_read",
};

constexpr size_t kWindowLimit = 32;  // instructions after an exclusive load that the scan follows for its store

const char* TopClass(uint32_t w) {
    // The Arm ARM's top-level encoding table, op0 = bits 28:25
    const uint32_t op0 = (w >> 25) & 0xF;
    if (op0 == 0b0000) {
        return "reserved";
    }
    if (op0 == 0b0001 || op0 == 0b0011) {
        return "unallocated";
    }
    if (op0 == 0b0010) {
        return "sve";
    }
    if ((op0 & 0b1110) == 0b1000) {
        return "dp_imm";
    }
    if ((op0 & 0b1110) == 0b1010) {
        return "branch_sys";
    }
    if ((op0 & 0b0101) == 0b0100) {
        return "loadstore";
    }
    if ((op0 & 0b0111) == 0b0101) {
        return "dp_reg";
    }
    return "fp_simd";
}

std::optional<std::string> DecoderName(uint32_t w) {
    if (auto d = A64::Decode<A64::TranslatorVisitor>(w)) {
        return std::string(d->get().GetName());
    }
    return std::nullopt;
}

std::string SysregName(uint32_t w) {
    const uint32_t op0 = 2 + ((w >> 19) & 1), op1 = (w >> 16) & 7, crn = (w >> 12) & 15, crm = (w >> 8) & 15, op2 = (w >> 5) & 7;
    struct Known {
        uint32_t op0, op1, crn, crm, op2;
        const char* name;
    };
    static const Known known[] = {
        {3, 3, 4, 2, 0, "nzcv"},        {3, 3, 4, 4, 0, "fpcr"},          {3, 3, 4, 4, 1, "fpsr"},          {3, 3, 13, 0, 2, "tpidr_el0"},
        {3, 3, 13, 0, 3, "tpidrro_el0"}, {3, 3, 14, 0, 0, "cntfrq_el0"}, {3, 3, 14, 0, 1, "cntpct_el0"}, {3, 3, 14, 0, 2, "cntvct_el0"},
        {3, 3, 0, 0, 1, "ctr_el0"},      {3, 3, 0, 0, 7, "dczid_el0"},   {3, 3, 4, 2, 1, "daif"},         {3, 0, 0, 0, 0, "midr_el1"},
        {3, 0, 0, 0, 5, "mpidr_el1"},
    };
    for (const auto& k : known) {
        if (k.op0 == op0 && k.op1 == op1 && k.crn == crn && k.crm == crm && k.op2 == op2) {
            return k.name;
        }
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "s%u_%u_c%u_c%u_%u", op0, op1, crn, crm, op2);
    return buf;
}

// What the scans need to know about a word
struct Info {
    const char* top = "";
    std::optional<std::string> name;
    bool excl_load = false, excl_store = false;
    bool ends_block = false;  // a branch, or an exception-generating instruction
    bool ends_window = false;  // control leaves the straight line for good: unconditional branches, returns, exceptions, undecoded words
    bool integer = false;      // an integer class whose IR the data-flow scans can follow
};

Info Classify(uint32_t w, Counts* counts) {
    Info info;
    info.top = TopClass(w);
    info.name = DecoderName(w);
    auto add = [&](const std::string& key) {
        if (counts) {
            ++(*counts)[key];
        }
    };
    add(std::string("class.") + info.top);
    const std::string top = info.top;
    const bool v_bit = (w >> 26) & 1;
    info.integer = top == "dp_imm" || top == "dp_reg" || (top == "loadstore" && !v_bit);
    if (top == "fp_simd") {
        add("fp_simd.dp");
    } else if (top == "loadstore" && v_bit) {
        add("fp_simd.loadstore");
    }

    // Encodings dynarmic does not decode (Arm ARM masks)
    if ((w & 0x3FA07C00u) == 0x08A07C00u) {
        add("lse");
        add("lse.cas");
    } else if ((w & 0xBFA07C00u) == 0x08207C00u) {
        add("lse");
        add("lse.casp");
    } else if ((w & 0x3F200C00u) == 0x38200000u) {
        const uint32_t o3opc = (w >> 12) & 0xF;
        if (o3opc == 0b1100) {
            add("rcpc.ldapr");
        } else if (o3opc == 0b1000) {
            add("lse");
            add("lse.swp");
        } else if (o3opc < 0b1000) {
            add("lse");
            add("lse.ldop");
        }
    }
    if ((w & 0xFFF8F01Fu) == 0xD500401Fu) {
        add("msr.pstate_imm");
    }
    if ((w & 0xFFE0001Fu) == 0xD4000002u || (w & 0xFFE0001Fu) == 0xD4000003u) {
        add("hvc_smc");
        info.ends_block = info.ends_window = true;
    }
    if ((w >> 16) == 0) {
        add("udf");
        info.ends_block = info.ends_window = true;
    }

    if (!info.name) {
        add("undecoded");
        info.ends_window = true;
        return info;
    }
    add("decoded");
    const std::string& n = *info.name;
    if (n.empty()) {
        add("unallocated");
        info.ends_window = true;
        return info;
    }
    if (n == "AESE" || n == "AESD" || n == "AESMC" || n == "AESIMC") {
        add("crypto.aes");
    } else if (n.rfind("SHA1", 0) == 0) {
        add("crypto.sha1");
    } else if (n.rfind("SHA256", 0) == 0) {
        add("crypto.sha256");
    } else if (n.rfind("SHA512", 0) == 0 || n.rfind("SM3", 0) == 0 || n.rfind("SM4", 0) == 0 || n == "EOR3" || n == "BCAX" || n == "RAX1" || n == "XAR") {
        add("crypto.v82");
    } else if (n == "PMULL, PMULL2" && ((w >> 22) & 3) == 3) {
        add("crypto.pmull64");
    } else if (n == "CRC32B, CRC32H, CRC32W, CRC32X") {
        add("crc32");
    } else if (n == "CRC32CB, CRC32CH, CRC32CW, CRC32CX") {
        add("crc32c");
    } else if (n == "MRS") {
        add("mrs." + SysregName(w));
    } else if (n == "MSR (register)") {
        add("msr." + SysregName(w));
    } else if (n == "DC ZVA") {
        add("dc.zva");
    } else if (n.rfind("DC ", 0) == 0) {
        add("dc.other");
    } else if (n.rfind("IC ", 0) == 0) {
        add("ic");
    } else if (n == "NOP") {
        add("hint.nop");
    } else if (n == "YIELD") {
        add("hint.yield");
    } else if (n == "WFE") {
        add("hint.wfe");
    } else if (n == "WFI") {
        add("hint.wfi");
    } else if (n == "SEV" || n == "SEVL") {
        add("hint.sev");
    } else if (n == "HINT") {
        add("hint.other");
    } else if (n == "DMB") {
        add("barrier.dmb");
    } else if (n == "DSB") {
        add("barrier.dsb");
    } else if (n == "ISB") {
        add("barrier.isb");
    } else if (n == "CLREX") {
        add("clrex");
    } else if (n == "B") {
        add("branch.b");
        info.ends_block = info.ends_window = true;
    } else if (n == "BL") {
        add("branch.bl");
        info.ends_block = info.ends_window = true;
    } else if (n == "B.cond") {
        add("branch.cond");
        info.ends_block = true;
    } else if (n == "CBZ" || n == "CBNZ") {
        add("branch.cbz");
        info.ends_block = true;
    } else if (n == "TBZ" || n == "TBNZ") {
        add("branch.tbz");
        info.ends_block = true;
    } else if (n == "BR") {
        add("branch.br");
        info.ends_block = info.ends_window = true;
    } else if (n == "BLR") {
        add("branch.blr");
        info.ends_block = info.ends_window = true;
    } else if (n == "RET") {
        add("branch.ret");
        info.ends_block = info.ends_window = true;
    } else if (n == "SVC") {
        add("svc");
        info.ends_block = info.ends_window = true;
    } else if (n == "BRK") {
        add("brk");
        info.ends_block = info.ends_window = true;
    } else if (n == "LDXRB, LDXRH, LDXR" || n == "LDAXRB, LDAXRH, LDAXR" || n == "LDXP" || n == "LDAXP") {
        add("excl.load");
        info.excl_load = true;
    } else if (n == "STXRB, STXRH, STXR" || n == "STLXRB, STLXRH, STLXR" || n == "STXP" || n == "STLXP") {
        add("excl.store");
        info.excl_store = true;
    } else if (n == "LDARB, LDARH, LDAR") {
        add("ordered.ldar");
    } else if (n == "STLRB, STLRH, STLR") {
        add("ordered.stlr");
    } else if (n == "LDLARB, LDLARH, LDLAR" || n == "STLLRB, STLLRH, STLLR") {
        add("ordered.lor");
    }
    return info;
}

// Taint through one instruction's dynarmic IR. tainted: general registers (bit i = x_i) whose value derives from the tracked source.
// seed_exclusive: the result of an exclusive load is tainted (the window scan); loads_taint: every load result is (the SP scan)
struct Flow {
    bool load_through = false;  // a memory read whose address is tainted
    bool load = false;          // any memory read
    bool sp_from_taint = false; // SP written with a tainted value
};

bool IsRead(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::A64ReadMemory8:
    case IR::Opcode::A64ReadMemory16:
    case IR::Opcode::A64ReadMemory32:
    case IR::Opcode::A64ReadMemory64:
    case IR::Opcode::A64ReadMemory128:
    case IR::Opcode::A64ExclusiveReadMemory8:
    case IR::Opcode::A64ExclusiveReadMemory16:
    case IR::Opcode::A64ExclusiveReadMemory32:
    case IR::Opcode::A64ExclusiveReadMemory64:
    case IR::Opcode::A64ExclusiveReadMemory128:
        return true;
    default:
        return false;
    }
}

Flow Propagate(uint64_t pc, uint32_t w, uint32_t& tainted, bool seed_exclusive, bool loads_taint) {
    Flow flow;
    IR::Block block{A64::LocationDescriptor{pc, FP::FPCR{decode::kDefaultFpcr}}};
    decode::TranslateOne(block, pc, w);
    std::unordered_set<const IR::Inst*> t;
    auto is_tainted = [&](const IR::Value& v) { return !v.IsImmediate() && t.count(v.GetInst()); };
    for (const auto& inst : block) {
        const IR::Opcode op = inst.GetOpcode();
        bool any = false;
        for (size_t i = 0; i < inst.NumArgs(); ++i) {
            any = any || is_tainted(inst.GetArg(i));
        }
        switch (op) {
        case IR::Opcode::A64GetW:
        case IR::Opcode::A64GetX:
            if (tainted & (1u << static_cast<unsigned>(inst.GetArg(0).GetA64RegRef()))) {
                t.insert(&inst);
            }
            break;
        case IR::Opcode::A64SetW:
        case IR::Opcode::A64SetX: {
            const uint32_t bit = 1u << static_cast<unsigned>(inst.GetArg(0).GetA64RegRef());
            tainted = is_tainted(inst.GetArg(1)) ? tainted | bit : tainted & ~bit;
            break;
        }
        case IR::Opcode::A64SetSP:
            flow.sp_from_taint = flow.sp_from_taint || is_tainted(inst.GetArg(0));
            break;
        default:
            if (IsRead(op)) {
                flow.load = true;
                const bool through = is_tainted(inst.GetArg(1));
                flow.load_through = flow.load_through || through;
                const bool exclusive = op >= IR::Opcode::A64ExclusiveReadMemory8 && op <= IR::Opcode::A64ExclusiveReadMemory128;
                if (through || loads_taint || (seed_exclusive && exclusive)) {
                    t.insert(&inst);
                }
            } else if (any) {
                t.insert(&inst);
            }
            break;
        }
    }
    return flow;
}

}  // namespace

Counts Census(const uint8_t* data, size_t size, uint64_t base) {
    Counts c;
    for (const char* k : kAlwaysKeys) {
        c[k] = 0;
    }
    const size_t n = size / 4;
    std::vector<uint32_t> words(n);
    std::memcpy(words.data(), data, n * 4);
    std::vector<Info> info(n);
    c["words"] = n;
    for (size_t i = 0; i < n; ++i) {
        info[i] = Classify(words[i], &c);
    }

    // Exclusive windows: from each exclusive load, the straight line (conditional branches fall through) to its exclusive store, as the rewriter
    // finds them (rewriter.md, Exclusive windows); ABA-sensitive when a load inside reads through the loaded value (section 6.5)
    for (size_t i = 0; i < n; ++i) {
        if (!info[i].excl_load) {
            continue;
        }
        uint32_t tainted = 0;
        Propagate(base + 4 * i, words[i], tainted, true, false);
        bool closed = false, through = false, other_load = false;
        for (size_t j = i + 1; j < n && j <= i + kWindowLimit; ++j) {
            if (info[j].excl_load || info[j].ends_window) {
                break;
            }
            if (info[j].excl_store) {
                closed = true;
                break;
            }
            if (!info[j].integer) {
                continue;
            }
            const Flow f = Propagate(base + 4 * j, words[j], tainted, false, false);
            through = through || f.load_through;
            other_load = other_load || (f.load && !f.load_through);
        }
        if (!closed) {
            ++c["excl.unpaired"];
            continue;
        }
        ++c["excl.windows"];
        if (through) {
            ++c["excl.windows.aba_sensitive"];
        } else if (other_load) {
            ++c["excl.windows.other_load"];
        }
    }

    // SP written from memory (fiber switches, longjmp, an unwinder's register restore): within a basic block, SP set from a value a load produced
    uint32_t loaded = 0;
    for (size_t i = 0; i < n; ++i) {
        if (info[i].ends_block) {
            loaded = 0;
            continue;
        }
        if (!info[i].integer || !info[i].name || info[i].name->empty()) {
            continue;
        }
        if (Propagate(base + 4 * i, words[i], loaded, false, true).sp_from_taint) {
            ++c["sp.from_memory"];
        }
    }

    c["answer.lse_present"] = c["lse"] > 0;
    c["answer.fpsr_read"] = c["mrs.fpsr"] > 0;
    c["answer.sp_from_memory"] = c["sp.from_memory"] > 0;
    c["answer.aba_sensitive_windows"] = c["excl.windows.aba_sensitive"] > 0;
    c["answer.cntfrq_read"] = c["mrs.cntfrq_el0"] > 0;
    return c;
}

}  // namespace nxaot::analysis
