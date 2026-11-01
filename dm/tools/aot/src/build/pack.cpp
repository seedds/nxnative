#include "build/pack.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <thread>

#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

#include <lld/Common/Driver.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticPrinter.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassTimingInfo.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Pass.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/StandardInstrumentations.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/SubtargetFeature.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/DCE.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include "analysis/census.h"
#include "analysis/discover.h"
#include "analysis/insn.h"
#include "build/plan.h"
#include "cache/cache.h"
#include "nxaot/aot_module.h"
#include "nxaot/guest_state.h"
#include "nxaot_lock.h"

LLD_HAS_DRIVER(elf)

namespace nxaot::build {

namespace {

using Clock = std::chrono::steady_clock;
double Since(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

// The memory a compile worker is given when --workers picks their number (AutoWorkers). Measured on this repository's development machine
// (README.md, "Pack throughput"): with 8 workers the whole process peaks at about 250 MiB on the game-shaped benchmark at tier W, 240 MiB on
// switch-gcc (one function of 44,059 instructions) and 584 MiB on the benchmark at H+ (shards of up to 64,000 instructions); room for larger
// functions and shards
constexpr uint64_t kWorkerMemory = 512ull << 20;

// The cache key's format: changed whenever what a key covers changes (UnitKey). 2: the gate sites and the aux region's code, the line marks'
// section, the standard entries' fixed signature (ABI 3)
constexpr const char* kUnitKeyFormat = "nxaot-unit 2";

// The section each unit's !pcsections marks go to (the line table, "The line table" below): read back after code generation and kept out of
// the link (SHF_EXCLUDE)
constexpr const char* kLineMarks = ".nxaot_pcs";

std::string Sanitize(std::string s) {
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".nro") == 0) {
        s.resize(s.size() - 4);
    }
    for (char& c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
            c = '_';
        }
    }
    return s.empty() ? "module" : s;
}

llvm::CodeGenOptLevel CodegenLevel(Tier t) {
    switch (t) {
    case Tier::C:
        return llvm::CodeGenOptLevel::None;
    case Tier::W:
        return llvm::CodeGenOptLevel::Less;
    case Tier::H:
        return llvm::CodeGenOptLevel::Default;
    default:
        return llvm::CodeGenOptLevel::Aggressive;
    }
}

std::unique_ptr<llvm::TargetMachine> MakeTarget(const std::string& cpu_name, Tier tier, std::string* cpu_out, std::string* features_out,
                                                std::string* error) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
    const std::string triple = llvm::sys::getProcessTriple();
    std::string err;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, err);
    if (!target) {
        *error = err;
        return nullptr;
    }
    llvm::SubtargetFeatures features;
    std::string cpu = cpu_name;
    if (cpu.empty()) {
        // The pack machine's own CPU and its features (section 9.3: the -march=native of the brief)
        cpu = llvm::sys::getHostCPUName().str();
        llvm::StringMap<bool> host;
        if (llvm::sys::getHostCPUFeatures(host)) {
            for (const auto& f : host) {
                features.AddFeature(f.getKey(), f.getValue());
            }
        }
    }
    llvm::TargetOptions to;
    auto* tm = target->createTargetMachine(triple, cpu, features.getString(), to, llvm::Reloc::PIC_, std::nullopt, CodegenLevel(tier));
    if (!tm) {
        *error = "cannot create a target machine for " + triple + " " + cpu;
        return nullptr;
    }
    *cpu_out = cpu;
    *features_out = features.getString();
    return std::unique_ptr<llvm::TargetMachine>(tm);
}

lift::Target TargetOf(const llvm::TargetMachine& tm) {
    const llvm::MCSubtargetInfo* sti = tm.getMCSubtargetInfo();
    lift::Target t;
    t.fma = sti->checkFeatures("+fma");
    t.f16c = sti->checkFeatures("+f16c");
    t.avx2 = sti->checkFeatures("+avx2");
    t.aes = sti->checkFeatures("+aes");
    t.sha = sti->checkFeatures("+sha");
    t.pclmul = sti->checkFeatures("+pclmul");
    return t;
}

void Optimise(llvm::Module& m, llvm::TargetMachine* tm, Tier tier) {
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    // --time-passes (llvm::TimePassesIsEnabled): the standard instrumentations time every pass of the new pass manager
    llvm::PassInstrumentationCallbacks pic;
    std::optional<llvm::StandardInstrumentations> si;
    if (llvm::TimePassesIsEnabled) {
        si.emplace(m.getContext(), false);
        si->registerCallbacks(pic, &mam);
    }
    llvm::PassBuilder pb{tm, llvm::PipelineTuningOptions(), std::nullopt, &pic};
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    llvm::ModulePassManager mpm;
    switch (tier) {
    case Tier::C: {
        // Section 9.3, tier C: SROA, instcombine, simplifycfg, dead-code elimination; FastISel and the fast register allocator
        llvm::FunctionPassManager fpm;
        fpm.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
        fpm.addPass(llvm::InstCombinePass());
        fpm.addPass(llvm::SimplifyCFGPass());
        fpm.addPass(llvm::DCEPass());
        mpm.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));
        break;
    }
    case Tier::W:
        mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O1);
        break;
    case Tier::H:
        mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
        break;
    case Tier::HPlus:
        mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
        break;
    }
    mpm.run(m, mam);
}

// The module descriptor's layout (include/nxaot/aot_module.h), field by field
llvm::StructType* DescType(llvm::LLVMContext& c) {
    auto* i32 = llvm::Type::getInt32Ty(c);
    auto* i64 = llvm::Type::getInt64Ty(c);
    auto* ptr = llvm::PointerType::get(c, 0);
    auto* name = llvm::ArrayType::get(llvm::Type::getInt8Ty(c), 32);
    return llvm::StructType::get(c, {i32, i32, name, name, i64, i64, i64, i64, i32, i32, i32, i32, ptr, ptr, ptr, i64, ptr, ptr, ptr, ptr, ptr, i64, ptr,
                                     i32, i32, ptr, i64});
}

// The gates of design 1.1 the module's manifest asks for (its obs and spin rows), include/nxaot/aot_module.h
struct Gates {
    std::map<uint64_t, uint64_t> obs;   // observed instruction -> the number | parameter << 8
    std::map<uint64_t, uint64_t> spin;  // back-edge branch -> the head
    bool Empty() const { return obs.empty() && spin.empty(); }
};

Gates GatesOf(const module::Image& image) {
    Gates g;
    if (!image.manifest) {
        return g;
    }
    for (const auto& o : image.manifest->obs) {
        g.obs[o.offset] = o.number | o.parameter << 8;
    }
    for (const auto& s : image.manifest->spin) {
        for (const uint64_t e : s.back_edges) {
            g.spin[e] = s.head;
        }
    }
    return g;
}

// The gate sites in a function's code (the blocks discovery's walk reached, the aux region's among them), sorted by offset: the gates its compiled
// code calls. An observation gate whose instruction a block holds; a spin wait's back-edge that ends a block (a branch ends its block)
std::vector<nxaot_gate_site> GateSitesOf(const analysis::Function& f, const Gates& g) {
    std::vector<nxaot_gate_site> out;
    for (const auto& [start, b] : f.blocks) {
        for (auto it = g.obs.lower_bound(b.start); it != g.obs.end() && it->first < b.end; ++it) {
            out.push_back({static_cast<uint32_t>(it->first), NXAOT_GATE_OBS, it->second});
        }
        if (b.end >= 4) {
            if (const auto it = g.spin.find(b.end - 4); it != g.spin.end()) {
                out.push_back({static_cast<uint32_t>(it->first), NXAOT_GATE_SPIN, it->second});
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const nxaot_gate_site& a, const nxaot_gate_site& b) { return a.offset < b.offset; });
    return out;
}

// ---- What every module of a pack shares ----

struct Common {
    const module::Image& image;
    const BuildOptions& o;
    const analysis::Discovery& d;
    const Plan& plan;
    lift::Convention conv;
    std::string prefix;  // the module's symbols are <prefix>+0x<offset>
    bool fpsr = false;
    std::string triple, cpu, features;
    lift::Target target;
    std::string tool;    // the toolchain's identity for the cache keys: nxaot.lock, LLVM, this executable's SHA-256
    Gates gates;                          // the manifest's gate rows
    std::vector<nxaot_gate_site> served;  // the gate sites compiled code calls (some function's code holds them), sorted: the descriptor's table

    std::vector<nxaot_gate_site> SitesOf(uint64_t entry) const { return GateSitesOf(d.functions.at(entry), gates); }
};

// The counts of one unit (or of the tables' object), which a pack sums and the cache keeps beside each unit's object
struct UnitStats {
    uint64_t blocks = 0, calls = 0, indirect_calls = 0, tail_calls = 0, indirect_tails = 0, returns = 0, svcs = 0, traps = 0, refused = 0;
    uint64_t lines_lost = 0, spills = 0, reloads = 0, text_bytes = 0, object_bytes = 0, gates = 0;
    double t_lift = 0, t_verify = 0, t_opt = 0, t_codegen = 0;
    std::vector<std::string> notes;

    cache::Facts ToFacts() const {
        cache::Facts f;
        f.counts = {{"blocks", blocks}, {"calls", calls}, {"indirect_calls", indirect_calls}, {"tail_calls", tail_calls},
                    {"indirect_tails", indirect_tails}, {"returns", returns}, {"svcs", svcs}, {"traps", traps}, {"refused", refused},
                    {"lines_lost", lines_lost}, {"text_bytes", text_bytes}, {"object_bytes", object_bytes}, {"gates", gates}};
        f.notes = notes;
        return f;
    }
    static UnitStats FromFacts(const cache::Facts& f) {
        UnitStats u;
        const auto get = [&](const char* k) {
            const auto it = f.counts.find(k);
            return it == f.counts.end() ? uint64_t{0} : it->second;
        };
        u.blocks = get("blocks");
        u.calls = get("calls");
        u.indirect_calls = get("indirect_calls");
        u.tail_calls = get("tail_calls");
        u.indirect_tails = get("indirect_tails");
        u.returns = get("returns");
        u.svcs = get("svcs");
        u.traps = get("traps");
        u.refused = get("refused");
        u.lines_lost = get("lines_lost");
        u.text_bytes = get("text_bytes");
        u.object_bytes = get("object_bytes");
        u.gates = get("gates");
        u.notes = f.notes;
        return u;
    }
};

// ---- The line table (design 3.5; include/nxaot/aot_module.h) ----
// Each unit marks its guest memory accesses with !pcsections, into the section kLineMarks of its object. For each metadata node LLVM writes the
// native address of every machine instruction that carries it (each a 32-bit pc-relative relocation to the code) and then the node's data once,
// here the guest offset (AsmPrinter::emitPCSections). Several machine instructions share a node whenever LLVM splits or duplicates an access (a
// guest ldp, dc zva's eight stores, a loop rotated or unrolled), so the section is no array of {native, offset} pairs, which the readers of ABI 2
// took it for: in the corpus 246 of fnptr-gcc's 415 entries and 8,812 of switch-gcc's 24,293 named an address outside the code or an offset
// outside the text. Its words are told apart by their relocations, which only the unit's object has: ReadLineMarks reads them after code
// generation (and from a cached object), attributes each address to the function whose symbol holds it, and MarkExcluded sets SHF_EXCLUDE on the
// section, which the link then drops. The pack's table is nxaot's own (lines.o: CompileLines), a run per function of {native - body, offset}
struct LineRow {
    uint64_t fn = 0;              // the module offset of the function whose body holds the native address
    uint32_t native = 0, offset = 0;  // bytes from the start of that body, and the guest instruction's module offset
};

// The function symbols of a unit's code that are bodies (<prefix>+0x<offset>) or standard entries (<body>.std): the module offset of the body
std::optional<uint64_t> BodyOf(llvm::StringRef name, const std::string& prefix, bool* std_entry) {
    const std::string head = prefix + "+0x";
    if (!name.starts_with(head)) {
        return std::nullopt;
    }
    name = name.drop_front(head.size());
    *std_entry = name.consume_back(".std");
    uint64_t v = 0;
    if (name.getAsInteger(16, v)) {
        return std::nullopt;
    }
    return v;
}

bool ReadLineMarks(llvm::StringRef object, const std::string& prefix, std::vector<LineRow>* rows, uint64_t* lost, std::string* error) {
    auto file = llvm::object::ObjectFile::createObjectFile(llvm::MemoryBufferRef(object, "unit"));
    if (!file) {
        *error = "the line marks: " + llvm::toString(file.takeError());
        return false;
    }
    const auto* elf = llvm::dyn_cast<llvm::object::ELF64LEObjectFile>(file->get());
    if (!elf) {
        *error = "the line marks: the unit's object is not ELF64";
        return false;
    }
    std::optional<llvm::object::SectionRef> marks;
    for (const llvm::object::SectionRef& s : elf->sections()) {
        auto name = s.getName();
        if (!name) {
            llvm::consumeError(name.takeError());
            continue;
        }
        if (*name == kLineMarks) {
            marks = s;
            break;
        }
    }
    if (!marks) {
        return true;  // no guest memory access in the unit
    }
    auto data = marks->getContents();
    if (!data) {
        *error = "the line marks: " + llvm::toString(data.takeError());
        return false;
    }
    // The section's relocations: the words that are native addresses, each the code-section offset it names (S + A of R_X86_64_PC32: the
    // difference to the word itself, as emitLabelDifference writes it)
    std::map<uint64_t, uint64_t> address_at;
    std::optional<llvm::object::SectionRef> code;
    for (const llvm::object::SectionRef& rs : elf->sections()) {
        auto target = rs.getRelocatedSection();
        if (!target) {
            llvm::consumeError(target.takeError());
            continue;
        }
        if (*target == elf->section_end() || **target != *marks) {
            continue;
        }
        for (const llvm::object::RelocationRef& r : rs.relocations()) {
            const llvm::object::ELFRelocationRef er{r};
            auto addend = er.getAddend();
            const llvm::object::symbol_iterator sym = r.getSymbol();
            if (!addend || sym == elf->symbol_end() || r.getType() != llvm::ELF::R_X86_64_PC32) {
                if (!addend) llvm::consumeError(addend.takeError());
                *error = "the line marks: a relocation that is no R_X86_64_PC32 to the code";
                return false;
            }
            auto value = sym->getValue();
            auto sec = sym->getSection();
            if (!value || !sec || *sec == elf->section_end()) {
                if (!value) llvm::consumeError(value.takeError());
                if (!sec) llvm::consumeError(sec.takeError());
                *error = "the line marks: a relocation to no section";
                return false;
            }
            if (code && **sec != *code) {
                *error = "the line marks name more than one code section";
                return false;
            }
            code = **sec;
            address_at[r.getOffset()] = *value + static_cast<uint64_t>(*addend);
        }
    }
    if (!code) {
        return true;
    }
    // The functions of that section by address: bodies (their rows) and standard entries (no guest access of their own)
    struct Fn {
        uint64_t lo = 0, hi = 0, body = 0;
        bool std_entry = false;
    };
    std::vector<Fn> fns;
    for (const llvm::object::ELFSymbolRef s : elf->symbols()) {
        if (s.getELFType() != llvm::ELF::STT_FUNC) continue;
        auto sec = s.getSection();
        auto name = s.getName();
        auto value = s.getValue();
        if (!sec || !name || !value) {
            if (!sec) llvm::consumeError(sec.takeError());
            if (!name) llvm::consumeError(name.takeError());
            if (!value) llvm::consumeError(value.takeError());
            continue;
        }
        if (*sec == elf->section_end() || **sec != *code) continue;
        Fn f;
        if (const auto body = BodyOf(*name, prefix, &f.std_entry)) {
            f.lo = *value;
            f.hi = *value + s.getSize();
            f.body = *body;
            fns.push_back(f);
        }
    }
    std::sort(fns.begin(), fns.end(), [](const Fn& a, const Fn& b) { return a.lo < b.lo; });
    const llvm::StringRef bytes = *data;
    std::vector<uint64_t> group;
    for (uint64_t p = 0; p + 4 <= bytes.size(); p += 4) {
        if (const auto it = address_at.find(p); it != address_at.end()) {
            group.push_back(it->second);
            continue;
        }
        uint32_t offset = 0;
        std::memcpy(&offset, bytes.data() + p, 4);
        if (group.empty()) {
            *error = "the line marks: a guest offset with no native address before it at +" + std::to_string(p);
            return false;
        }
        for (const uint64_t native : group) {
            auto f = std::upper_bound(fns.begin(), fns.end(), native, [](uint64_t a, const Fn& g) { return a < g.lo; });
            if (f == fns.begin() || native >= std::prev(f)->hi || std::prev(f)->std_entry) {
                ++*lost;  // outside every body (none expected)
                continue;
            }
            --f;
            rows->push_back({f->body, static_cast<uint32_t>(native - f->lo), offset});
        }
        group.clear();
    }
    if (!group.empty() || bytes.size() % 4) {
        *error = "the line marks end in native addresses with no guest offset after them";
        return false;
    }
    return true;
}

// Sets SHF_EXCLUDE on the line marks' section of an ELF64 object in place, so that the link drops it (lld discards SHF_EXCLUDE input sections,
// and the relocations that apply to them); nothing else of the object changes
bool MarkExcluded(std::string* object, std::string* error) {
    const auto u16 = [&](size_t at) { uint16_t v; std::memcpy(&v, object->data() + at, 2); return v; };
    const auto u32 = [&](size_t at) { uint32_t v; std::memcpy(&v, object->data() + at, 4); return v; };
    const auto u64 = [&](size_t at) { uint64_t v; std::memcpy(&v, object->data() + at, 8); return v; };
    if (object->size() < 64 || std::memcmp(object->data(), "\x7f" "ELF\x02\x01", 6) != 0) {
        *error = "the unit's object is not ELF64 little-endian";
        return false;
    }
    const uint64_t shoff = u64(0x28);
    const uint16_t shentsize = u16(0x3A), shnum = u16(0x3C), shstrndx = u16(0x3E);
    if (shentsize < 64 || shstrndx >= shnum || shoff + static_cast<uint64_t>(shnum) * shentsize > object->size()) {
        *error = "the unit's object has no section header table";
        return false;
    }
    const uint64_t strtab = u64(shoff + static_cast<uint64_t>(shstrndx) * shentsize + 0x18);
    for (uint16_t i = 0; i < shnum; ++i) {
        const uint64_t h = shoff + static_cast<uint64_t>(i) * shentsize;
        const uint64_t name = strtab + u32(h);
        if (name < object->size() && std::strncmp(object->data() + name, kLineMarks, object->size() - name) == 0) {
            const uint64_t flags = u64(h + 8) | llvm::ELF::SHF_EXCLUDE;
            std::memcpy(object->data() + h + 8, &flags, 8);
        }
    }
    return true;
}

// What a module (the tables' object or a unit) holds of the run time and of the code every unit shares. The tables' object defines the
// module's base, the empty inline-cache descriptor, the slow-path wrappers and jit_entry; a unit declares them. Every name is hidden: lld
// resolves them inside the pack's shared object, which exports only the descriptor
struct Kit {
    llvm::Module* m = nullptr;
    llvm::GlobalVariable* base = nullptr;
    llvm::GlobalVariable* ic_empty = nullptr;
    llvm::Function *svc = nullptr, *trap = nullptr, *dispatch = nullptr, *counter = nullptr, *rt_jit = nullptr, *safepoint = nullptr;
    llvm::Function* gate = nullptr;  // nxaot_rt_gate's preserve_all wrapper, in a module with gate sites only (design 1.1)
    llvm::Function* jit_entry = nullptr;
    llvm::FunctionType* std_ty = nullptr;  // the standard entries' one signature (section 2.3)
    // Debug information: a synthetic file per module, line = guest offset (section 9.2)
    std::unique_ptr<llvm::DIBuilder> dib;
    llvm::DIFile* file = nullptr;
    llvm::DISubroutineType* sub_ty = nullptr;
    bool optimized = true;

    llvm::DISubprogram* Subprogram(llvm::Function* f, uint64_t line) const {
        if (!dib) {
            return nullptr;
        }
        auto* sp = dib->createFunction(file, f->getName(), f->getName(), file, static_cast<unsigned>(line), sub_ty, static_cast<unsigned>(line),
                                       llvm::DINode::FlagZero,
                                       llvm::DISubprogram::SPFlagDefinition | (optimized ? llvm::DISubprogram::SPFlagOptimized
                                                                                         : llvm::DISubprogram::SPFlagZero));
        f->setSubprogram(sp);
        return sp;
    }
};

void Hide(llvm::GlobalValue* g) {
    g->setVisibility(llvm::GlobalValue::HiddenVisibility);
    g->setDSOLocal(true);
}

// The diagnostics of an LLVM context: a guest memory access folded into another instruction keeps no !pcsections entry ("loosing !pcsections
// metadata"); those are counted (the line table then maps that native address through its function), anything else is kept as a note
void Diagnose(llvm::LLVMContext& ctx, UnitStats* st) {
    ctx.setDiagnosticHandlerCallBack(
        [](const llvm::DiagnosticInfo& di, void* context) {
            auto* s = static_cast<UnitStats*>(context);
            std::string text;
            llvm::raw_string_ostream out{text};
            llvm::DiagnosticPrinterRawOStream printer{out};
            di.print(printer);
            if (out.str().find("pcsections") != std::string::npos) {
                ++s->lines_lost;
            } else if (di.getSeverity() == llvm::DS_Error || di.getSeverity() == llvm::DS_Warning) {
                s->notes.push_back(std::string(di.getSeverity() == llvm::DS_Error ? "[RED] " : "") + "nxaot build: llvm: " + out.str());
            }
        },
        st);
}

Kit MakeKit(llvm::Module& m, const Common& c, bool tables) {
    llvm::LLVMContext& ctx = m.getContext();
    llvm::IRBuilder<> b{ctx};
    auto* i32 = b.getInt32Ty();
    auto* i64 = b.getInt64Ty();
    auto* ptr = b.getPtrTy();
    auto* v128 = llvm::FixedVectorType::get(i64, 2);
    const BuildOptions& o = c.o;
    Kit k;
    k.m = &m;
    k.optimized = o.tier != Tier::C;

    // ---- Debug information ----
    if (o.debug_info) {
        k.dib = std::make_unique<llvm::DIBuilder>(m);
        k.file = k.dib->createFile(c.prefix + ".s", ".");
        k.dib->createCompileUnit(llvm::dwarf::DW_LANG_C, k.file, std::string("nxaot (LLVM ") + NXAOT_LOCK_LLVM + ")", k.optimized, "", 0);
        k.sub_ty = k.dib->createSubroutineType(k.dib->getOrCreateTypeArray({}));
        m.addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
        m.addModuleFlag(llvm::Module::Warning, "Dwarf Version", 5);
    }

    // ---- The module's base, the empty inline-cache descriptor ----
    k.base = new llvm::GlobalVariable(m, i64, false, llvm::GlobalValue::ExternalLinkage, tables ? b.getInt64(0) : nullptr, "nxaot.base");
    k.base->setAlignment(llvm::Align(8));
    Hide(k.base);
    auto* ic_desc_ty = llvm::StructType::get(ctx, {i64, ptr});
    k.ic_empty = new llvm::GlobalVariable(
        m, ic_desc_ty, true, llvm::GlobalValue::ExternalLinkage,
        tables ? llvm::ConstantStruct::get(ic_desc_ty, {b.getInt64(UINT64_MAX), llvm::ConstantPointerNull::get(ptr)}) : nullptr, "nxaot.ic_empty");
    k.ic_empty->setAlignment(llvm::Align(16));
    Hide(k.ic_empty);

    // ---- The run time's entry points, and the slow paths through preserve_all / preserve_most wrappers (section 2.3: the call sites keep
    // their registers, the wrapper saves them) ----
    const auto extern_fn = [&](const char* name, llvm::FunctionType* ty) {
        auto* f = llvm::Function::Create(ty, llvm::GlobalValue::ExternalLinkage, name, m);
        f->addFnAttr(llvm::Attribute::NoUnwind);
        return f;
    };
    k.svc = extern_fn("dm_aot_svc", llvm::FunctionType::get(b.getVoidTy(), {ptr, i32}, false));
    k.trap = extern_fn("nxaot_rt_trap", llvm::FunctionType::get(b.getVoidTy(), {ptr, i64, i32}, false));
    k.trap->setDoesNotReturn();
    k.trap->addFnAttr(llvm::Attribute::Cold);
    const auto wrapper = [&](const char* name, llvm::FunctionType* ty, llvm::Function* target, unsigned cc) {
        auto* w = llvm::Function::Create(ty, llvm::GlobalValue::ExternalLinkage, name, m);
        Hide(w);
        w->setCallingConv(cc);
        w->addFnAttr(llvm::Attribute::NoUnwind);
        w->addFnAttr(llvm::Attribute::NoInline);
        w->addFnAttr(llvm::Attribute::Cold);
        if (target) {
            llvm::IRBuilder<> wb{llvm::BasicBlock::Create(ctx, "entry", w)};
            std::vector<llvm::Value*> args;
            for (auto& a : w->args()) {
                args.push_back(&a);
            }
            auto* call = wb.CreateCall(target, args);
            if (call->getType()->isVoidTy()) {
                wb.CreateRetVoid();
            } else {
                wb.CreateRet(call);
            }
        }
        return w;
    };
    auto* dispatch_ty = llvm::FunctionType::get(ptr, {ptr, i64, ptr, i64}, false);
    auto* counter_ty = llvm::FunctionType::get(i64, {ptr}, false);
    auto* safepoint_ty = llvm::FunctionType::get(b.getVoidTy(), {ptr}, false);
    k.dispatch = wrapper("nxaot.dispatch", dispatch_ty, tables ? extern_fn("nxaot_rt_dispatch", dispatch_ty) : nullptr,
                         llvm::CallingConv::PreserveAll);
    k.counter = wrapper("nxaot.cntpct", counter_ty, tables ? extern_fn("dm_aot_cntpct", counter_ty) : nullptr, llvm::CallingConv::PreserveMost);
    k.counter->removeFnAttr(llvm::Attribute::Cold);
    // Section 11: the JIT fallback, a C call from the code that hands it a continuation (and from jit_entry, below); section 6.2: the
    // safepoint, through a preserve_all wrapper so that a poll's cold path saves nothing it does not store itself
    k.rt_jit = extern_fn("nxaot_rt_jit", llvm::FunctionType::get(b.getVoidTy(), {ptr, i64, i64}, false));
    k.rt_jit->addFnAttr(llvm::Attribute::Cold);
    k.safepoint = wrapper("nxaot.safepoint", safepoint_ty, tables ? extern_fn("nxaot_rt_safepoint", safepoint_ty) : nullptr,
                          llvm::CallingConv::PreserveAll);
    // Design 1.1: the gates (observation gates, spin waits' back-edges), through a preserve_all wrapper: the code stored every register the
    // block lacks before the call, and keeps the rest of its host registers across it. Only a module with gate sites imports nxaot_rt_gate.
    // Not cold: an observation gate runs at every pass of its instruction, a spin wait's at every pass of its loop
    if (!c.served.empty()) {
        auto* gate_ty = llvm::FunctionType::get(b.getVoidTy(), {ptr, i64}, false);
        k.gate = wrapper("nxaot.gate", gate_ty, tables ? extern_fn("nxaot_rt_gate", gate_ty) : nullptr, llvm::CallingConv::PreserveAll);
        k.gate->removeFnAttr(llvm::Attribute::Cold);
    }

    // ---- jit_entry (include/nxaot/aot_module.h): the JIT fallback with the standard entries' signature ----
    // What it is passed to the block (the caller stored the rest: the block then holds the whole register file), the JIT from st->pc until
    // the guest returns to lr, then x0, x1 and the standard entries' RV from the block (poison-equivalent when st->pending is set: the caller
    // propagates the transfer before it looks at them)
    const lift::Regs std_pass = c.plan.indirect_pass;
    const lift::Convention& conv = c.conv;
    k.std_ty = conv.Type(ctx, std_pass);
    k.jit_entry = llvm::Function::Create(k.std_ty, llvm::GlobalValue::ExternalLinkage, "nxaot.jit_entry", m);
    Hide(k.jit_entry);
    conv.Apply(k.jit_entry);
    k.jit_entry->addFnAttr(llvm::Attribute::NoUnwind);
    k.jit_entry->addFnAttr(llvm::Attribute::NoInline);
    k.jit_entry->addFnAttr(llvm::Attribute::Cold);
    k.jit_entry->addFnAttr("frame-pointer", "none");
    k.jit_entry->addFnAttr(llvm::Attribute::getWithUWTableKind(ctx, llvm::UWTableKind::Async));
    if (tables) {
        llvm::Function* je = k.jit_entry;
        llvm::DISubprogram* sp = k.Subprogram(je, 0);
        llvm::IRBuilder<> jb{llvm::BasicBlock::Create(ctx, "entry", je)};
        if (sp) {
            jb.SetCurrentDebugLocation(llvm::DILocation::get(ctx, 0, 0, sp));
        }
        llvm::Value* st = je->getArg(0);
        const auto field = [&](size_t off) { return jb.CreateConstInBoundsGEP1_64(b.getInt8Ty(), st, off); };
        for (unsigned i = 0; i < 31; ++i) {
            const int a = conv.ArgX(std_pass, i);
            if (a >= 0 && (std_pass.x >> i & 1)) {
                jb.CreateAlignedStore(je->getArg(static_cast<unsigned>(a)), field(offsetof(dm_guest_state, x) + 8 * i), llvm::Align(8));
            }
        }
        for (unsigned i = 0; i < 32; ++i) {
            const int a = conv.ArgV(std_pass, i);
            if (a >= 0 && (std_pass.v >> i & 1)) {
                jb.CreateAlignedStore(je->getArg(static_cast<unsigned>(a)), field(offsetof(dm_guest_state, v) + 16 * i), llvm::Align(16));
            }
        }
        jb.CreateAlignedStore(je->getArg(1), field(offsetof(dm_guest_state, sp)), llvm::Align(8));
        jb.CreateAlignedStore(je->getArg(2), field(offsetof(dm_guest_state, x) + 8 * 30), llvm::Align(8));
        jb.CreateCall(k.rt_jit, {st, je->getArg(2), je->getArg(1)});
        llvm::Value* r = llvm::PoisonValue::get(conv.RetType(ctx));
        r = jb.CreateInsertValue(r, jb.CreateAlignedLoad(i64, field(offsetof(dm_guest_state, x)), llvm::Align(8)), 0);
        r = jb.CreateInsertValue(r, jb.CreateAlignedLoad(i64, field(offsetof(dm_guest_state, x) + 8), llvm::Align(8)), 1);
        for (unsigned i = 0; i < 4; ++i) {
            if (c.plan.indirect_rv >> i & 1) {
                r = jb.CreateInsertValue(r, jb.CreateAlignedLoad(v128, field(offsetof(dm_guest_state, v) + 16 * i), llvm::Align(16)), 2 + i);
            }
        }
        jb.CreateRet(r);
    }
    return k;
}

std::string BodyName(const Common& c, uint64_t entry) {
    return c.prefix + "+0x" + llvm::utohexstr(entry);
}

// The body of a guest function in a module: declared (hidden) where another unit defines it, or the definition-to-be of the unit's own
llvm::Function* Body(llvm::Module& m, const Common& c, uint64_t entry, const FnFacts& ff) {
    const std::string name = BodyName(c, entry);
    if (llvm::Function* f = m.getFunction(name)) {
        return f;
    }
    auto* f = llvm::Function::Create(c.conv.Type(m.getContext(), ff.pass), llvm::GlobalValue::ExternalLinkage, name, m);
    Hide(f);
    c.conv.Apply(f);
    if (c.o.no_inline) {
        f->addFnAttr(llvm::Attribute::NoInline);  // nxaot_bench: calls stay calls, to measure the convention
    }
    return f;
}

// Whether a function has a standard entry of its own (section 2.3); with ghccc a body that takes every passable register and returns the fixed
// signature's v registers is its own
bool HasStdEntry(const Common& c, const FnFacts& ff) {
    return ff.host_callable && !(c.conv.cc == lift::CallConv::Ghc && ff.pass == c.plan.indirect_pass && ff.rv == c.plan.indirect_rv);
}

// The standard entry of a host-callable function: declared, or defined (every passable register in, what the body does not take stored to the
// block, then a tail call of the body)
llvm::Function* StdEntry(const Kit& k, const Common& c, uint64_t entry, const FnFacts& ff, llvm::Function* body, bool define) {
    llvm::Module& m = *k.m;
    llvm::LLVMContext& ctx = m.getContext();
    if (!HasStdEntry(c, ff)) {
        return body;
    }
    auto* e = llvm::Function::Create(k.std_ty, llvm::GlobalValue::ExternalLinkage, body->getName() + ".std", m);
    Hide(e);
    c.conv.Apply(e);
    e->addFnAttr(llvm::Attribute::NoUnwind);
    e->addFnAttr("frame-pointer", "none");
    e->addFnAttr(llvm::Attribute::getWithUWTableKind(ctx, llvm::UWTableKind::Async));
    if (!define) {
        return e;
    }
    const lift::Regs std_pass = c.plan.indirect_pass;
    const lift::Convention& conv = c.conv;
    auto* i8 = llvm::Type::getInt8Ty(ctx);
    auto* i64 = llvm::Type::getInt64Ty(ctx);
    auto* v128 = llvm::FixedVectorType::get(i64, 2);
    llvm::DISubprogram* sp = k.Subprogram(e, entry);
    llvm::IRBuilder<> eb{llvm::BasicBlock::Create(ctx, "entry", e)};
    if (sp) {
        eb.SetCurrentDebugLocation(llvm::DILocation::get(ctx, static_cast<unsigned>(entry), 0, sp));
    }
    llvm::Value* st = e->getArg(0);
    for (unsigned i = 0; i < 31; ++i) {
        const int a = conv.ArgX(std_pass, i);
        if (a >= 0 && (std_pass.x >> i & 1) && !(ff.pass.x >> i & 1)) {
            eb.CreateAlignedStore(e->getArg(static_cast<unsigned>(a)), eb.CreateConstInBoundsGEP1_64(i8, st, offsetof(dm_guest_state, x) + 8 * i),
                                  llvm::Align(8));
        }
    }
    for (unsigned i = 0; i < 32; ++i) {
        const int a = conv.ArgV(std_pass, i);
        if (a >= 0 && (std_pass.v >> i & 1) && !(ff.pass.v >> i & 1)) {
            eb.CreateAlignedStore(e->getArg(static_cast<unsigned>(a)), eb.CreateConstInBoundsGEP1_64(i8, st, offsetof(dm_guest_state, v) + 16 * i),
                                  llvm::Align(16));
        }
    }
    std::vector<llvm::Value*> args{st, e->getArg(1), e->getArg(2)};
    llvm::FunctionType* bt = body->getFunctionType();
    args.resize(bt->getNumParams());
    for (unsigned i = 0; i < 31; ++i) {
        const int a = conv.ArgX(ff.pass, i);
        if (a >= 0) {
            args[static_cast<size_t>(a)] = (ff.pass.x >> i & 1) ? static_cast<llvm::Value*>(e->getArg(static_cast<unsigned>(conv.ArgX(std_pass, i))))
                                                                : llvm::PoisonValue::get(i64);
        }
    }
    for (unsigned i = 0; i < 32; ++i) {
        const int a = conv.ArgV(ff.pass, i);
        if (a >= 0) {
            args[static_cast<size_t>(a)] = (ff.pass.v >> i & 1) ? static_cast<llvm::Value*>(e->getArg(static_cast<unsigned>(conv.ArgV(std_pass, i))))
                                                                : llvm::PoisonValue::get(v128);
        }
    }
    auto* call = eb.CreateCall(bt, body, args);
    conv.Apply(call);
    call->setTailCallKind(llvm::CallInst::TCK_MustTail);
    call->setIsNoInline();
    eb.CreateRet(call);
    return e;
}

// The module's last steps: the verifier, the tier's passes, native code (and assembly for --spills / --asm) into *object
bool Finish(llvm::Module& m, const Common& c, llvm::TargetMachine& tm, std::string* object, UnitStats* st, std::string* asm_text,
            std::string* error) {
    auto t = Clock::now();
    std::string verify;
    llvm::raw_string_ostream verify_out{verify};
    if (llvm::verifyModule(m, &verify_out)) {
        *error = "invalid module " + m.getName().str() + ": " + verify_out.str().substr(0, 4000);
        return false;
    }
    st->t_verify += Since(t);
    t = Clock::now();
    Optimise(m, &tm, c.o.tier);
    st->t_opt += Since(t);
    if (c.o.count_spills || !c.o.asm_path.empty()) {
        // The register allocator's spills and reloads, from the comments of verbose assembly of the same code
        std::unique_ptr<llvm::Module> copy = llvm::CloneModule(m);
        llvm::SmallString<0> text_asm;
        llvm::raw_svector_ostream out{text_asm};
        llvm::legacy::PassManager pm;
        tm.Options.MCOptions.AsmVerbose = true;
        if (!tm.addPassesToEmitFile(pm, out, nullptr, llvm::CodeGenFileType::AssemblyFile)) {
            pm.run(*copy);
            llvm::StringRef rest = text_asm.str();
            while (!rest.empty()) {
                auto [line, tail] = rest.split('\n');
                rest = tail;
                if (line.contains("Spill")) {
                    ++st->spills;
                } else if (line.contains("Reload")) {
                    ++st->reloads;
                }
            }
            if (asm_text) {
                *asm_text = text_asm.str().str();
            }
        }
    }
    t = Clock::now();
    llvm::SmallVector<char, 0> buf;
    {
        llvm::raw_svector_ostream out{buf};
        llvm::legacy::PassManager pm;
        if (tm.addPassesToEmitFile(pm, out, nullptr, llvm::CodeGenFileType::ObjectFile)) {
            *error = "the target machine cannot emit an object file";
            return false;
        }
        pm.run(m);
    }
    st->t_codegen += Since(t);
    object->assign(buf.data(), buf.size());
    st->object_bytes = object->size();
    if (auto obj = llvm::object::ObjectFile::createObjectFile(llvm::MemoryBufferRef(llvm::StringRef(*object), m.getName()))) {
        for (const auto& sec : (*obj)->sections()) {
            auto n = sec.getName();
            if (n && *n == ".text") {
                st->text_bytes += sec.getSize();
            }
        }
    } else {
        llvm::consumeError(obj.takeError());
    }
    return true;
}

// ---- One compile unit: guest functions (one at tiers C and W, a shard at H and H+) and their standard entries in a module of its own ----

bool CompileUnit(const Common& c, llvm::TargetMachine& tm, const std::vector<uint64_t>& entries, std::string* object, UnitStats* st,
                 std::string* ir_text, std::string* asm_text, std::string* error) {
    const BuildOptions& o = c.o;
    const auto t0 = Clock::now();
    llvm::LLVMContext ctx;
    Diagnose(ctx, st);
    llvm::Module m{BodyName(c, entries.front()), ctx};
    m.setDataLayout(tm.createDataLayout());
    m.setTargetTriple(tm.getTargetTriple().str());
    Kit k = MakeKit(m, c, false);
    auto* i8 = llvm::Type::getInt8Ty(ctx);
    auto* i64 = llvm::Type::getInt64Ty(ctx);
    auto* v128 = llvm::FixedVectorType::get(i64, 2);

    for (const uint64_t entry : entries) {
        const FnFacts& ff = c.plan.fns.at(entry);
        StdEntry(k, c, entry, ff, Body(m, c, entry, ff), true);
    }

    // The callees as this unit sees them: declared when the lifter first asks for one
    std::map<uint64_t, lift::CalleeView> views;
    const auto view_of = [&](uint64_t off) -> const lift::CalleeView* {
        if (const auto it = views.find(off); it != views.end()) {
            return &it->second;
        }
        const auto pf = c.plan.fns.find(off);
        if (pf == c.plan.fns.end()) {
            return nullptr;
        }
        lift::CalleeView v;
        v.fn = Body(m, c, off, pf->second);
        v.pass = pf->second.pass;
        v.writes = pf->second.writes;
        v.rv = pf->second.rv;
        v.returns = pf->second.returns;
        return &views.emplace(off, v).first->second;
    };
    lift::CalleeView indirect;
    indirect.pass = c.plan.indirect_pass;
    indirect.writes = c.plan.indirect_writes;
    indirect.rv = c.plan.indirect_rv;
    indirect.returns = true;
    indirect.exact = false;  // a union: the function that runs stores only its own writes
    lift::LiftOptions lo;
    lo.fpcr = 0;
    lo.fpsr_flags = c.fpsr;
    lo.target = c.target;
    for (const uint64_t entry : entries) {
        const FnFacts& ff = c.plan.fns.at(entry);
        llvm::Function* body = Body(m, c, entry, ff);
        lift::FunctionPlan fp;
        fp.conv = c.conv;
        fp.fn = body;
        fp.self = *view_of(entry);
        fp.live_after = ff.live_after;
        fp.entry = entry;
        fp.limit = ff.limit;
        fp.correctness = o.correctness;
        fp.base = k.base;
        fp.callee = view_of;
        fp.indirect = indirect;
        const FnFacts* facts = &ff;
        fp.resume = [facts](uint64_t call_pc) {
            std::vector<uint64_t> r = facts->continuations;
            if (const auto p = facts->pads.find(call_pc); p != facts->pads.end()) {
                r.push_back(p->second);
            }
            return r;
        };
        fp.tables = [facts](uint64_t br) -> const std::vector<uint64_t>* {
            const auto it = facts->tables.find(br);
            return it == facts->tables.end() ? nullptr : &it->second;
        };
        fp.roots = ff.roots;
        fp.rt_dispatch = k.dispatch;
        fp.rt_svc = k.svc;
        fp.rt_trap = k.trap;
        fp.rt_counter = k.counter;
        fp.rt_jit = k.rt_jit;
        fp.rt_safepoint = k.safepoint;
        fp.jit_entry = k.jit_entry;
        fp.safepoints = o.safepoints;
        fp.ic_empty = k.ic_empty;
        fp.ic_prefix = "nxaot.ic." + llvm::utohexstr(entry) + ".";
        fp.debug = k.Subprogram(body, entry);
        fp.line_section = o.line_table ? kLineMarks : "";
        // The gates its code holds (design 1.1), which its compiled code must call: checked against what the lifter emitted below
        const std::vector<nxaot_gate_site> sites = c.SitesOf(entry);
        for (const auto& g : sites) {
            if (g.kind == NXAOT_GATE_OBS) fp.obs_gates.insert(g.offset);
            else fp.spin_gates[g.offset] = g.arg;
        }
        fp.rt_gate = k.gate;
        lift::Region region;
        region.entry = entry;
        region.lo = entry;
        region.hi = ff.limit;
        if (c.image.aux_size) {
            region.extra.emplace_back(c.image.aux_off, c.image.aux_off + c.image.aux_size);  // the frame step's stubs (design 1.1)
        }
        region.max_blocks = size_t{1} << 20;
        lo.plan = &fp;
        const module::Image& image = c.image;
        const uint64_t limit = ff.limit;
        const decode::CodeReader read = [&image, entry, limit](uint64_t a) -> std::optional<uint32_t> {
            if ((a < entry || a >= limit) && !image.InAux(a)) {
                return std::nullopt;  // a block runs to the function's limit: the next function's entry is reached by a tail call
            }
            if (!image.InCode(a)) {
                return std::nullopt;
            }
            return image.Word(a);
        };
        const lift::Lifted l = lift::LiftRegion(m, body->getName().str(), region, read, lo);
        if (l.function) {
            std::vector<uint64_t> want;
            for (const auto& g : sites) want.push_back(g.offset);
            if (want != l.gates) {
                std::string list;
                for (const uint64_t g : want) list += " " + llvm::utohexstr(g);
                *error = "[RED] nxaot build: " + body->getName().str() + " holds the gate sites" + list + " but its code calls " +
                         std::to_string(l.gates.size()) + " of them (design 1.1: every obs and spin row a function's code holds must be served)";
                return false;
            }
            st->gates += l.gates.size();
        } else if (!sites.empty()) {
            *error = "[RED] nxaot build: " + body->getName().str() + " holds " + std::to_string(sites.size()) +
                     " gate sites and the lifter refused it: the JIT fallback that would run it calls no gate (design 1.1)";
            return false;
        }
        st->blocks += l.blocks;
        st->calls += l.calls;
        st->indirect_calls += l.indirect_calls;
        st->tail_calls += l.tail_calls;
        st->indirect_tails += l.indirect_tails;
        st->returns += l.returns;
        st->svcs += l.svcs;
        st->traps += l.traps;
        if (!l.function) {
            // Refused (an opcode the lowering does not cover, an internal error): a body that hands the function to the JIT fallback (section 11,
            // case 4), so that the rest of the module still runs: what it is passed to the block, the JIT from its entry until it returns to lr,
            // then the native return of what the JIT left in the block
            ++st->refused;
            std::string why = l.error;
            for (const auto& op : l.unsupported) {
                why += (why.empty() ? "" : ", ") + op;
            }
            st->notes.push_back("[RED] nxaot build: " + body->getName().str() + " refused (" + why + "): it runs in the JIT fallback");
            llvm::Function* f = body;
            llvm::IRBuilder<> fb{llvm::BasicBlock::Create(ctx, "entry", f)};
            if (fp.debug) {
                fb.SetCurrentDebugLocation(llvm::DILocation::get(ctx, static_cast<unsigned>(entry), 0, fp.debug));
            }
            llvm::Value* stp = f->getArg(0);
            const auto field = [&](size_t off) { return fb.CreateConstInBoundsGEP1_64(i8, stp, off); };
            for (unsigned i = 0; i < 31; ++i) {
                const int a = c.conv.ArgX(ff.pass, i);
                if (a >= 0 && (ff.pass.x >> i & 1)) {
                    fb.CreateAlignedStore(f->getArg(static_cast<unsigned>(a)), field(offsetof(dm_guest_state, x) + 8 * i), llvm::Align(8));
                }
            }
            for (unsigned i = 0; i < 32; ++i) {
                const int a = c.conv.ArgV(ff.pass, i);
                if (a >= 0 && (ff.pass.v >> i & 1)) {
                    fb.CreateAlignedStore(f->getArg(static_cast<unsigned>(a)), field(offsetof(dm_guest_state, v) + 16 * i), llvm::Align(16));
                }
            }
            fb.CreateAlignedStore(f->getArg(1), field(offsetof(dm_guest_state, sp)), llvm::Align(8));
            fb.CreateAlignedStore(f->getArg(2), field(offsetof(dm_guest_state, x) + 8 * 30), llvm::Align(8));
            llvm::Value* at = fb.CreateAdd(fb.CreateAlignedLoad(i64, k.base, llvm::Align(8)), fb.getInt64(entry));
            fb.CreateAlignedStore(at, field(offsetof(dm_guest_state, pc)), llvm::Align(8));
            fb.CreateAlignedStore(fb.getInt32(NXAOT_TRAP_JIT_REFUSED), field(offsetof(dm_guest_state, exit_reason)), llvm::Align(4));
            fb.CreateCall(k.rt_jit, {stp, f->getArg(2), f->getArg(1)});
            llvm::Value* r = llvm::PoisonValue::get(c.conv.RetType(ctx));
            r = fb.CreateInsertValue(r, fb.CreateAlignedLoad(i64, field(offsetof(dm_guest_state, x)), llvm::Align(8)), 0);
            r = fb.CreateInsertValue(r, fb.CreateAlignedLoad(i64, field(offsetof(dm_guest_state, x) + 8), llvm::Align(8)), 1);
            for (unsigned i = 0; i < 4; ++i) {
                if (ff.rv >> i & 1) {
                    r = fb.CreateInsertValue(r, fb.CreateAlignedLoad(v128, field(offsetof(dm_guest_state, v) + 16 * i), llvm::Align(16)), 2 + i);
                }
            }
            fb.CreateRet(r);
        }
    }
    if (k.dib) {
        k.dib->finalize();
    }
    st->t_lift += Since(t0);
    if (ir_text) {
        llvm::raw_string_ostream out{*ir_text};
        m.print(out, nullptr);
    }
    return Finish(m, c, tm, object, st, asm_text, error);
}

// ---- The tables' object: the lookup hash, the function map, the returns, the trampoline, the descriptor, and the shared code ----

bool CompileTables(const Common& c, llvm::TargetMachine& tm, std::string* object, UnitStats* st, BuildStats* s, std::string* ir_text,
                   std::string* asm_text, std::string* error) {
    const BuildOptions& o = c.o;
    const Plan& plan = c.plan;
    const auto t0 = Clock::now();
    llvm::LLVMContext ctx;
    Diagnose(ctx, st);
    llvm::Module mod{c.prefix, ctx};
    llvm::Module* m = &mod;
    m->setDataLayout(tm.createDataLayout());
    m->setTargetTriple(tm.getTargetTriple().str());
    Kit k = MakeKit(*m, c, true);
    llvm::IRBuilder<> b{ctx};
    auto* i8 = b.getInt8Ty();
    auto* i32 = b.getInt32Ty();
    auto* i64 = b.getInt64Ty();
    auto* ptr = b.getPtrTy();
    auto* v128 = llvm::FixedVectorType::get(i64, 2);
    const lift::Convention& conv = c.conv;
    const lift::Regs std_pass = plan.indirect_pass;

    // Every body and standard entry, declared: the units define them
    std::map<uint64_t, llvm::Function*> bodies, std_entries;
    for (const auto& [entry, ff] : plan.fns) {
        bodies[entry] = Body(*m, c, entry, ff);
        s->words += ff.words;
        ++s->tier_functions[static_cast<size_t>(o.tier)];
    }
    s->functions = plan.fns.size();
    for (const auto& [entry, ff] : plan.fns) {
        if (ff.host_callable) {
            std_entries[entry] = StdEntry(k, c, entry, ff, bodies[entry], false);
        }
    }
    s->standard_entries = std_entries.size();
    for (const auto& [entry, ff] : plan.fns) {
        s->tables += ff.tables.size();
        s->resume_sites += ff.continuations.size() + ff.pads.size();
    }

    const auto rel = [&](llvm::Constant* target, llvm::Constant* field) {
        llvm::Constant* diff = llvm::ConstantExpr::getSub(llvm::ConstantExpr::getPtrToInt(target, i64), llvm::ConstantExpr::getPtrToInt(field, i64));
        return llvm::ConstantExpr::getTrunc(diff, i32);
    };
    // The address of field 1 (the self-relative native address) of row i of a table
    const auto field_of = [&](llvm::ArrayType* ty, llvm::GlobalVariable* table, uint64_t i) {
        return llvm::ConstantExpr::getInBoundsGetElementPtr(ty, table, llvm::ArrayRef<llvm::Constant*>{b.getInt64(0), b.getInt64(i), b.getInt32(1)});
    };
    uint32_t slots = 2;
    while (slots < 2 * std_entries.size()) {
        slots *= 2;
    }
    s->lookup_slots = slots;
    auto* entry_ty = llvm::StructType::get(ctx, {i64, i32, i32});
    auto* lookup_ty = llvm::ArrayType::get(entry_ty, slots);
    auto* lookup = new llvm::GlobalVariable(*m, lookup_ty, true, llvm::GlobalValue::InternalLinkage, nullptr, "nxaot.lookup");
    lookup->setSection(".nxaot.lookup");
    lookup->setAlignment(llvm::Align(16));
    {
        std::vector<uint64_t> keys(slots, NXAOT_LOOKUP_EMPTY);
        std::vector<llvm::Function*> targets(slots, nullptr);
        for (const auto& [entry, fn] : std_entries) {
            uint32_t h = nxaot_lookup_hash(entry, slots - 1);
            while (keys[h] != NXAOT_LOOKUP_EMPTY) {
                h = (h + 1) & (slots - 1);
            }
            keys[h] = entry;
            targets[h] = fn;
        }
        std::vector<llvm::Constant*> rows;
        for (uint32_t i = 0; i < slots; ++i) {
            llvm::Constant* field = field_of(lookup_ty, lookup, i);
            const auto ff = keys[i] == NXAOT_LOOKUP_EMPTY ? plan.fns.end() : plan.fns.find(keys[i]);
            const uint32_t flags = ff != plan.fns.end() && !ff->second.writes.sp ? NXAOT_LOOKUP_SP_KEPT : 0u;
            rows.push_back(llvm::ConstantStruct::get(entry_ty, {b.getInt64(keys[i]), targets[i] ? rel(targets[i], field) : b.getInt32(0), b.getInt32(flags)}));
        }
        lookup->setInitializer(llvm::ConstantArray::get(lookup_ty, rows));
    }
    auto* fns_ty = llvm::ArrayType::get(entry_ty, plan.fns.size());
    auto* fnmap = new llvm::GlobalVariable(*m, fns_ty, true, llvm::GlobalValue::InternalLinkage, nullptr, "nxaot.fns");
    fnmap->setSection(".nxaot.fnmap");
    fnmap->setAlignment(llvm::Align(16));
    {
        std::vector<llvm::Constant*> rows;
        uint64_t i = 0;
        for (const auto& [entry, ff] : plan.fns) {
            llvm::Constant* field = field_of(fns_ty, fnmap, i);
            const uint32_t size = static_cast<uint32_t>(ff.size);
            rows.push_back(llvm::ConstantStruct::get(entry_ty, {b.getInt64(entry), rel(bodies[entry], field), b.getInt32(size)}));
            ++i;
        }
        fnmap->setInitializer(llvm::ConstantArray::get(fns_ty, rows));
    }
    // The return addresses of every call site (bl, blr) of compiled code, sorted: where a native frame may wait for its callee, which the JIT
    // fallback stops at besides the compiled entries (src/rt/jit.cpp)
    std::vector<uint32_t> returns;
    for (const auto& [entry, f] : c.d.functions) {
        for (const auto& [pc, target] : f.calls) {
            returns.push_back(static_cast<uint32_t>(pc + 4));
        }
    }
    std::sort(returns.begin(), returns.end());
    returns.erase(std::unique(returns.begin(), returns.end()), returns.end());
    auto* returns_ty = llvm::ArrayType::get(i32, returns.size());
    auto* returns_table = new llvm::GlobalVariable(*m, returns_ty, true, llvm::GlobalValue::InternalLinkage,
                                                   llvm::ConstantDataArray::get(ctx, llvm::ArrayRef<uint32_t>(returns)), "nxaot.returns");
    returns_table->setSection(".nxaot.returns");
    returns_table->setAlignment(llvm::Align(16));
    // The line table: lines.o defines it once every unit is compiled (CompileLines, "The line table" above); none with --no-line-table
    llvm::Constant* fn_lines = llvm::ConstantPointerNull::get(ptr);
    llvm::Constant* lines = llvm::ConstantPointerNull::get(ptr);
    if (o.line_table) {
        auto* fl = new llvm::GlobalVariable(*m, i8, true, llvm::GlobalValue::ExternalLinkage, nullptr, "nxaot.fn_lines");
        auto* ln = new llvm::GlobalVariable(*m, i8, true, llvm::GlobalValue::ExternalLinkage, nullptr, "nxaot.lines");
        Hide(fl);
        Hide(ln);
        fn_lines = fl;
        lines = ln;
    }
    // The gate sites compiled code calls (design 1.1), sorted by offset: what the host checks the manifest's obs and spin rows against
    llvm::Constant* gates = llvm::ConstantPointerNull::get(ptr);
    if (!c.served.empty()) {
        auto* site_ty = llvm::StructType::get(ctx, {i32, i32, i64});
        std::vector<llvm::Constant*> rows;
        for (const auto& g : c.served) {
            rows.push_back(llvm::ConstantStruct::get(site_ty, {b.getInt32(g.offset), b.getInt32(g.kind), b.getInt64(g.arg)}));
        }
        auto* gates_ty = llvm::ArrayType::get(site_ty, rows.size());
        auto* table = new llvm::GlobalVariable(*m, gates_ty, true, llvm::GlobalValue::InternalLinkage, llvm::ConstantArray::get(gates_ty, rows),
                                               "nxaot.gates");
        table->setSection(".nxaot.gates");
        table->setAlignment(llvm::Align(16));
        gates = table;
    }

    // enter(st, standard entry): the C convention into the pack's convention, every passable register from st; x0, x1 and the standard entries'
    // RV back to st unless the guest left through a non-local transfer (st->pending: the block then holds the whole register file)
    auto* enter = llvm::Function::Create(llvm::FunctionType::get(b.getVoidTy(), {ptr, ptr}, false), llvm::GlobalValue::InternalLinkage, "nxaot.enter", *m);
    enter->addFnAttr(llvm::Attribute::NoUnwind);
    enter->addFnAttr(llvm::Attribute::NoInline);
    enter->addFnAttr(llvm::Attribute::getWithUWTableKind(ctx, llvm::UWTableKind::Async));
    {
        llvm::DISubprogram* sp = k.Subprogram(enter, 0);
        llvm::IRBuilder<> eb{llvm::BasicBlock::Create(ctx, "entry", enter)};
        if (sp) {
            eb.SetCurrentDebugLocation(llvm::DILocation::get(ctx, 0, 0, sp));
        }
        llvm::Value* st0 = enter->getArg(0);
        const auto field = [&](size_t off) { return eb.CreateConstInBoundsGEP1_64(i8, st0, off); };
        std::vector<llvm::Value*> args(k.std_ty->getNumParams());
        args[0] = st0;
        args[1] = eb.CreateAlignedLoad(i64, field(offsetof(dm_guest_state, sp)), llvm::Align(8));
        args[2] = eb.CreateAlignedLoad(i64, field(offsetof(dm_guest_state, x) + 8 * 30), llvm::Align(8));
        for (unsigned i = 0; i < 31; ++i) {
            if (const int a = conv.ArgX(std_pass, i); a >= 0) {
                args[static_cast<size_t>(a)] = eb.CreateAlignedLoad(i64, field(offsetof(dm_guest_state, x) + 8 * i), llvm::Align(8));
            }
        }
        for (unsigned i = 0; i < 32; ++i) {
            if (const int a = conv.ArgV(std_pass, i); a >= 0) {
                args[static_cast<size_t>(a)] = eb.CreateAlignedLoad(v128, field(offsetof(dm_guest_state, v) + 16 * i), llvm::Align(16));
            }
        }
        auto* call = eb.CreateCall(k.std_ty, enter->getArg(1), args);
        conv.Apply(call);
        llvm::Value* pending = eb.CreateAlignedLoad(i64, field(offsetof(dm_guest_state, pending)), llvm::Align(8));
        auto* store_bb = llvm::BasicBlock::Create(ctx, "results", enter);
        auto* done_bb = llvm::BasicBlock::Create(ctx, "done", enter);
        eb.CreateCondBr(eb.CreateICmpEQ(pending, eb.getInt64(0)), store_bb, done_bb);
        eb.SetInsertPoint(store_bb);
        eb.CreateAlignedStore(eb.CreateExtractValue(call, 0), field(offsetof(dm_guest_state, x)), llvm::Align(8));
        eb.CreateAlignedStore(eb.CreateExtractValue(call, 1), field(offsetof(dm_guest_state, x) + 8), llvm::Align(8));
        for (unsigned i = 0; i < 4; ++i) {
            if (plan.indirect_rv >> i & 1) {
                eb.CreateAlignedStore(eb.CreateExtractValue(call, 2 + i), field(offsetof(dm_guest_state, v) + 16 * i), llvm::Align(16));
            }
        }
        eb.CreateBr(done_bb);
        eb.SetInsertPoint(done_bb);
        eb.CreateRetVoid();
    }

    auto* desc_ty = DescType(ctx);
    const llvm::StructLayout* layout = m->getDataLayout().getStructLayout(desc_ty);
    if (layout->getSizeInBytes() != sizeof(nxaot_module_desc) || layout->getElementOffset(12) != offsetof(nxaot_module_desc, base) ||
        layout->getElementOffset(16) != offsetof(nxaot_module_desc, fn_lines) || layout->getElementOffset(17) != offsetof(nxaot_module_desc, lines) ||
        layout->getElementOffset(19) != offsetof(nxaot_module_desc, tool) || layout->getElementOffset(20) != offsetof(nxaot_module_desc, returns) ||
        layout->getElementOffset(22) != offsetof(nxaot_module_desc, jit_entry) || layout->getElementOffset(23) != offsetof(nxaot_module_desc, std_x) ||
        layout->getElementOffset(25) != offsetof(nxaot_module_desc, gates) || layout->getElementOffset(26) != offsetof(nxaot_module_desc, gate_count)) {
        *error = "the descriptor's LLVM layout differs from include/nxaot/aot_module.h";
        return false;
    }
    const module::Image& image = c.image;
    std::string name_bytes = image.name.substr(0, 31);
    name_bytes.resize(32, '\0');
    const std::string tool = std::string("nxaot llvm ") + NXAOT_LOCK_LLVM + " dynarmic " + NXAOT_LOCK_DYNARMIC + " cc " + lift::CallConvName(o.cc) +
                             " tier " + TierName(o.tier) + " mode " + o.mode + (o.correctness ? " correctness" : "") +
                             (o.all_entries ? " all-entries" : "") + (o.safepoints ? "" : " no-safepoints") + " cpu " + c.cpu;
    auto* tool_str = b.CreateGlobalString(tool, "nxaot.tool", 0, m);
    const uint32_t flags = (o.cc == lift::CallConv::Ghc ? NXAOT_FLAG_GHC : 0) | (o.correctness ? NXAOT_FLAG_CORRECTNESS : 0) |
                           (o.all_entries ? NXAOT_FLAG_ALL_ENTRIES : 0) | (c.fpsr ? NXAOT_FLAG_FPSR : 0) |
                           (o.safepoints ? NXAOT_FLAG_SAFEPOINTS : 0);
    auto* desc = new llvm::GlobalVariable(
        *m, desc_ty, true, llvm::GlobalValue::ExternalLinkage,
        llvm::ConstantStruct::get(
            desc_ty, {b.getInt32(NXAOT_MODULE_ABI), b.getInt32(flags), llvm::ConstantDataArray::getString(ctx, name_bytes, false),
                      llvm::ConstantDataArray::get(ctx, llvm::ArrayRef<uint8_t>(image.sha256.data(), image.sha256.size())),
                      b.getInt64(image.bytes.size()), b.getInt64(image.TextEnd()), b.getInt64(image.bss_size), b.getInt64(image.text_off),
                      b.getInt32(0), b.getInt32(slots - 1), b.getInt32(plan.indirect_rv), b.getInt32(0), k.base, lookup, fnmap,
                      b.getInt64(plan.fns.size()), fn_lines, lines, enter, tool_str, returns_table, b.getInt64(returns.size()),
                      k.jit_entry, b.getInt32(plan.indirect_pass.x), b.getInt32(plan.indirect_pass.v), gates, b.getInt64(c.served.size())}),
        "nxaot_module");
    desc->setAlignment(llvm::Align(8));
    if (k.dib) {
        k.dib->finalize();
    }
    st->t_lift += Since(t0);
    if (ir_text) {
        llvm::raw_string_ostream out{*ir_text};
        m->print(out, nullptr);
    }
    return Finish(*m, c, tm, object, st, asm_text, error);
}

// ---- lines.o: the line table, from every unit's line marks (ReadLineMarks), once all are compiled ----
// nxaot.fn_lines: a {first, count} run per function in the function map's order; nxaot.lines: {native - body, offset} per guest memory access,
// each run sorted by native address (include/nxaot/aot_module.h)
bool CompileLines(const Common& c, llvm::TargetMachine& tm, const std::vector<LineRow>& all, std::string* object, UnitStats* st, uint64_t* count,
                  std::string* error) {
    llvm::LLVMContext ctx;
    Diagnose(ctx, st);
    llvm::Module m{c.prefix + ".lines", ctx};
    m.setDataLayout(tm.createDataLayout());
    m.setTargetTriple(tm.getTargetTriple().str());
    std::map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>> by_fn;
    for (const LineRow& r : all) {
        by_fn[r.fn].emplace_back(r.native, r.offset);
    }
    std::vector<uint32_t> index, rows;
    for (const auto& [entry, ff] : c.plan.fns) {
        auto it = by_fn.find(entry);
        std::vector<std::pair<uint32_t, uint32_t>> run;
        if (it != by_fn.end()) {
            run = std::move(it->second);
            std::sort(run.begin(), run.end());
            run.erase(std::unique(run.begin(), run.end()), run.end());
        }
        index.push_back(static_cast<uint32_t>(rows.size() / 2));
        index.push_back(static_cast<uint32_t>(run.size()));
        for (const auto& [native, offset] : run) {
            rows.push_back(native);
            rows.push_back(offset);
        }
    }
    *count = rows.size() / 2;
    const auto table = [&](const char* name, const char* section, const std::vector<uint32_t>& words) {
        auto* init = llvm::ConstantDataArray::get(ctx, llvm::ArrayRef<uint32_t>(words));
        auto* g = new llvm::GlobalVariable(m, init->getType(), true, llvm::GlobalValue::ExternalLinkage, init, name);
        Hide(g);
        g->setSection(section);
        g->setAlignment(llvm::Align(8));
    };
    table("nxaot.fn_lines", ".nxaot.fnlines", index);
    table("nxaot.lines", ".nxaot.lines", rows);
    return Finish(m, c, tm, object, st, nullptr, error);
}

// ---- The cache key of a unit (design section 13) ----

void AddRegs(cache::KeyBuilder& k, std::string_view name, const lift::Regs& r) {
    k.Add(name, std::to_string(r.x) + "," + std::to_string(r.v) + "," + std::to_string(r.sp) + "," + std::to_string(r.nzcv));
}

// The SHA-256 of everything a unit's object depends on: the toolchain (nxaot.lock, LLVM, this executable), the target machine (triple, CPU and
// its features), the tier and every option that changes code (the convention, the correctness mode, the debug information and line table,
// the safepoints, FPCR and the FPSR modelling), the module's symbol prefix, the standard entries' signature and the indirect view; then the
// function: its bounds and convention facts (PASS, WRITES, RV, returns, LIVE_AFTER, a standard entry, resume points, landing pads, jump tables,
// roots), its instruction words from its entry to its limit with the pc-relative fields (b, bl, b.cond, cbz, tbz, adr, adrp, literal loads)
// replaced by reference numbers as nso_match.py normalises relocations, the referenced addresses in order, and the convention facts of every
// function it references (its callees' classes). The lowering writes module offsets into the code (the symbol of the body, return addresses,
// pcs, the line table), so the referenced addresses are module offsets and the entry is part of the key: a function that moved in a game
// update misses until the lowering reaches other code through symbols only (phase 5b)
std::string UnitKey(const Common& c, uint64_t entry) {
    const BuildOptions& o = c.o;
    const FnFacts& ff = c.plan.fns.at(entry);
    cache::KeyBuilder k;
    k.Add("format", kUnitKeyFormat).Add("tool", c.tool).Add("target", c.triple + " " + c.cpu + " " + c.features);
    k.Add("tier", TierName(o.tier)).Add("cc", lift::CallConvName(o.cc)).Add("correctness", o.correctness).Add("debug_info", o.debug_info);
    k.Add("line_table", o.line_table).Add("safepoints", o.safepoints).Add("no_inline", o.no_inline).Add("fpsr", c.fpsr).Add("fpcr", uint64_t{0});
    k.Add("prefix", c.prefix);
    AddRegs(k, "indirect_pass", c.plan.indirect_pass);
    AddRegs(k, "indirect_writes", c.plan.indirect_writes);
    k.Add("indirect_rv", c.plan.indirect_rv);
    k.Add("entry", entry).Add("limit", ff.limit);
    AddRegs(k, "pass", ff.pass);
    AddRegs(k, "writes", ff.writes);
    AddRegs(k, "live_after", ff.live_after);
    k.Add("rv", ff.rv).Add("returns", ff.returns).Add("std_entry", HasStdEntry(c, ff));
    std::string list;
    for (const uint64_t r : ff.continuations) list += std::to_string(r) + ",";
    k.Add("continuations", list);
    list.clear();
    for (const auto& [call, pad] : ff.pads) list += std::to_string(call) + ":" + std::to_string(pad) + ",";
    k.Add("pads", list);
    list.clear();
    for (const auto& [br, targets] : ff.tables) {
        list += std::to_string(br) + ":";
        for (const uint64_t t : targets) list += std::to_string(t) + ",";
        list += ";";
    }
    k.Add("tables", list);
    list.clear();
    for (const uint64_t r : ff.roots) list += std::to_string(r) + ",";
    k.Add("roots", list);
    // The code: per word of [entry, limit) whether the lifter may read it, and the word with its pc-relative field cleared and a reference
    // number in its place
    const module::Image& img = c.image;
    std::string code;
    std::vector<uint64_t> refs;
    std::map<uint64_t, uint32_t> ref_of;
    const auto ref = [&](uint64_t target) {
        const auto [it, added] = ref_of.try_emplace(target, static_cast<uint32_t>(refs.size()));
        if (added) refs.push_back(target);
        return it->second;
    };
    // The words of [entry, limit), then of its blocks in the aux region (the frame step's stubs it reaches, design 1.1)
    std::vector<uint64_t> pcs;
    for (uint64_t pc = entry; pc < ff.limit; pc += 4) pcs.push_back(pc);
    for (const auto& [lo, hi] : ff.extra) {
        for (uint64_t pc = lo; pc < hi; pc += 4) pcs.push_back(pc);
    }
    for (const uint64_t pc : pcs) {
        const auto w = img.InCode(pc) ? img.Word(pc) : std::nullopt;
        uint32_t word = w.value_or(0), number = 0;
        bool relative = false;
        if (w) {
            const analysis::Insn i = analysis::ClassifyWord(word, pc);
            if (i.kind == analysis::Kind::B || i.kind == analysis::Kind::BL) {
                word &= 0xFC000000u;
                relative = true;
            } else if (i.kind == analysis::Kind::BCond) {
                word &= (word & 0x7E000000u) == 0x36000000u ? 0xFFF8001Fu : 0xFF00001Fu;  // tbz / tbnz: imm14; b.cond, cbz / cbnz: imm19
                relative = true;
            } else if (i.adr || i.adrp) {
                word &= 0x9F00001Fu;
                relative = true;
            } else if ((word & 0x3B000000u) == 0x18000000u) {  // ldr / ldrsw / prfm (literal), scalar and vector
                const uint64_t target = pc + static_cast<uint64_t>(analysis::SignExtend((word >> 5) & 0x7FFFFu, 19) * 4);
                word &= 0xFF00001Fu;
                number = ref(target);
                code += 'L';
            }
            if (relative) {
                number = ref(i.target);
                code += 'R';
            }
        }
        code += w ? 'W' : '-';
        code.append(reinterpret_cast<const char*>(&word), 4);
        code.append(reinterpret_cast<const char*>(&number), 4);
    }
    k.Add("code", code);
    list.clear();
    for (const uint64_t r : refs) list += std::to_string(r) + ",";
    k.Add("refs", list);
    list.clear();
    for (const auto& [lo, hi] : ff.extra) list += std::to_string(lo) + "-" + std::to_string(hi) + ",";
    k.Add("extra", list);
    // The gates its code calls (design 1.1): their sites, kinds and arguments
    list.clear();
    for (const auto& g : c.SitesOf(entry)) list += std::to_string(g.offset) + ":" + std::to_string(g.kind) + ":" + std::to_string(g.arg) + ",";
    k.Add("gates", list);
    // The convention facts of every function the code references: its direct branch targets, the next function (a block that runs to the
    // limit tail-calls it), its tables' targets and resume points
    std::set<uint64_t> referenced(refs.begin(), refs.end());
    referenced.insert(ff.limit);
    for (const auto& [br, targets] : ff.tables) referenced.insert(targets.begin(), targets.end());
    referenced.insert(ff.continuations.begin(), ff.continuations.end());
    for (const auto& [call, pad] : ff.pads) referenced.insert(pad);
    list.clear();
    for (const uint64_t t : referenced) {
        const auto it = c.plan.fns.find(t);
        if (it == c.plan.fns.end()) continue;
        const FnFacts& cf = it->second;
        list += std::to_string(t) + ":" + std::to_string(cf.pass.x) + "," + std::to_string(cf.pass.v) + "," + std::to_string(cf.pass.sp) + "/" +
                std::to_string(cf.writes.x) + "," + std::to_string(cf.writes.v) + "," + std::to_string(cf.writes.sp) + "," +
                std::to_string(cf.writes.nzcv) + "/" + std::to_string(cf.rv) + "/" + std::to_string(cf.returns) + ";";
    }
    k.Add("callees", list);
    return k.Hex();
}

// The key of a unit: its one function's, or for a shard the keys of its functions in order
std::string UnitKey(const Common& c, const std::vector<uint64_t>& entries) {
    if (entries.size() == 1) {
        return UnitKey(c, entries.front());
    }
    cache::KeyBuilder k;
    k.Add("format", kUnitKeyFormat).Add("shard", entries.size());
    for (const uint64_t e : entries) {
        k.Add("fn", UnitKey(c, e));
    }
    return k.Hex();
}

// The compile units of a module (design 9.3). At tiers C and W a function each: their pipelines optimise one function at a time, and a unit of one
// function is what the per-function cache keys. At H and H+, whose pipelines inline, shards of neighbouring functions in address order (the
// callees a function calls most are mostly near it): a shard ends after a function whose code's SHA-256 starts with a byte that is a multiple of
// kShardCut once it holds kShardMin instructions, and at kShardMax, so that a change in one function moves the shards' ends near it only
std::vector<std::vector<uint64_t>> Units(const module::Image& image, const Plan& plan, Tier tier) {
    std::vector<std::vector<uint64_t>> units;
    if (tier == Tier::C || tier == Tier::W) {
        for (const auto& [entry, ff] : plan.fns) {
            units.push_back({entry});
        }
        return units;
    }
    constexpr uint64_t kShardMin = 16'000, kShardMax = 64'000;
    constexpr unsigned kShardCut = 8;
    std::vector<uint64_t> shard;
    uint64_t words = 0;
    for (const auto& [entry, ff] : plan.fns) {
        shard.push_back(entry);
        words += ff.words;
        const uint64_t end = std::min<uint64_t>(ff.limit, image.bytes.size());
        const auto code = llvm::ArrayRef<uint8_t>(image.bytes.data() + std::min<uint64_t>(entry, end), image.bytes.data() + end);
        const bool cut = words >= kShardMax || (words >= kShardMin && llvm::SHA256::hash(code)[0] % kShardCut == 0);
        if (cut) {
            units.push_back(std::move(shard));
            shard.clear();
            words = 0;
        }
    }
    if (!shard.empty()) {
        units.push_back(std::move(shard));
    }
    return units;
}

std::string ToolIdentity() {
    std::string self;
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = '\0';
        self = cache::FileSha256(buf);
    }
    return std::string("nxaot.lock llvm ") + NXAOT_LOCK_LLVM + " dynarmic " + NXAOT_LOCK_DYNARMIC + "; LLVM " + LLVM_VERSION_STRING +
           "; executable " + (self.empty() ? "unknown" : self);
}

bool WriteFile(const std::string& path, const std::string& data, std::string* error) {
    std::ofstream out{path, std::ios::binary};
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out) {
        *error = "cannot write " + path;
        return false;
    }
    return true;
}

unsigned CpusAvailable() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        const int n = CPU_COUNT(&set);
        if (n > 0) return static_cast<unsigned>(n);
    }
    const unsigned h = std::thread::hardware_concurrency();
    return h ? h : 1;
}

uint64_t MemoryAvailable() {
    std::ifstream in{"/proc/meminfo"};
    std::string key;
    uint64_t kib = 0;
    std::string unit;
    while (in >> key >> kib >> unit) {
        if (key == "MemAvailable:") return kib << 10;
    }
    return 0;  // unknown
}

}  // namespace

const char* TierName(Tier t) {
    switch (t) {
    case Tier::C:
        return "C";
    case Tier::W:
        return "W";
    case Tier::H:
        return "H";
    default:
        return "H+";
    }
}

unsigned AutoWorkers() {
    unsigned n = CpusAvailable();
    if (const uint64_t free = MemoryAvailable()) {
        n = std::min<uint64_t>(n, std::max<uint64_t>(1, free / kWorkerMemory));
    }
    return std::max(1u, n);
}

bool BuildObjects(const module::Image& image, const BuildOptions& o, const std::string& work_dir, std::vector<std::string>* objects,
                  BuildStats* stats, std::string* error) {
    BuildStats& s = *stats;
    s.module = image.name;

    // The workers: the units' compile and discovery's jump tables run on them (neither result depends on how many there are)
    unsigned workers = o.workers ? o.workers : AutoWorkers();
    if (llvm::TimePassesIsEnabled) {
        workers = 1;  // LLVM's pass timers are not thread-safe
    }

    // ---- Analysis: discovery, the census's FPSR answer (decision 5), the convention facts ----
    auto t = Clock::now();
    analysis::DiscoverOptions dopts;
    dopts.threads = workers;
    const analysis::Discovery d = analysis::Discover(image, dopts);
    s.discover_times = d.times;
    const auto tc = Clock::now();
    bool fpsr = o.fpsr_flags == 1;
    if (o.fpsr_flags < 0) {
        fpsr = analysis::ReadsFpsr(image.bytes.data() + image.text_off, image.text_size);  // the census's answer.fpsr_read
    }
    s.fpsr_flags = fpsr;
    s.t_census = Since(tc);
    s.t_discover = Since(t);
    for (const auto& n : d.notes) {
        s.notes.push_back(n);
    }
    lift::Convention conv;
    conv.cc = o.cc;
    t = Clock::now();
    const Plan plan = MakePlan(image, d, conv, o.all_entries);
    s.t_plan = Since(t);

    // ---- The units and the workers; a target machine per worker, made here (LLVM's target registry is set up once, on this thread) ----
    const std::vector<std::vector<uint64_t>> units = Units(image, plan, o.tier);
    s.units = units.size();
    workers = static_cast<unsigned>(std::max<size_t>(1, std::min<size_t>(workers, units.size() + 1)));
    s.workers = workers;
    std::vector<std::unique_ptr<llvm::TargetMachine>> tms;
    for (unsigned i = 0; i < workers; ++i) {
        auto tm = MakeTarget(o.cpu, o.tier, &s.cpu, &s.features, error);
        if (!tm) {
            return false;
        }
        tms.push_back(std::move(tm));
    }
    // The gates (design 1.1): the manifest's obs and spin rows the functions' code holds are served (the units call them, CompileUnit checks
    // it); a row no discovered code holds is not, and the host, which checks every row against the descriptor, says so as well
    Common c{image, o, d, plan, conv, Sanitize(image.name), fpsr, tms[0]->getTargetTriple().str(), s.cpu, s.features, TargetOf(*tms[0]), "",
             GatesOf(image), {}};
    {
        std::map<uint64_t, nxaot_gate_site> served;
        for (const auto& [entry, f] : d.functions) {
            for (const auto& g : GateSitesOf(f, c.gates)) served[g.offset] = g;
        }
        for (const auto& [off, g] : served) c.served.push_back(g);
        for (const auto& [off, arg] : c.gates.obs) {
            if (!served.count(off)) {
                s.notes.push_back("[RED] nxaot build: the observation gate at " + c.prefix + "+0x" + llvm::utohexstr(off) +
                                  " is in no discovered function's code: the pack does not call it (its observer will not run)");
            }
        }
        for (const auto& [off, head] : c.gates.spin) {
            if (!served.count(off)) {
                s.notes.push_back("[RED] nxaot build: the spin wait back-edge at " + c.prefix + "+0x" + llvm::utohexstr(off) +
                                  " is in no discovered function's code: the pack does not call its gate");
            }
        }
        s.gate_sites = c.served.size();
    }

    std::optional<cache::Cache> cache;
    // --asm, --spills and --ir want every unit compiled (the cache keeps objects, not their assembly or IR)
    const bool use_cache = !o.cache_dir.empty() && !o.count_spills && o.asm_path.empty() && o.ir_path.empty();
    if (use_cache) {
        cache.emplace(o.cache_dir);
        if (!cache->Open(error)) {
            return false;
        }
        c.tool = ToolIdentity();
    }
    if (const std::error_code ec = llvm::sys::fs::create_directories(work_dir)) {
        *error = "cannot create " + work_dir + ": " + ec.message();
        return false;
    }

    // Jobs: the tables' object first (job 0), then the units, the largest first so that the last to finish are small ones; each unit's object
    // depends only on the unit, so the order and the number of workers change nothing in the pack
    std::vector<size_t> order(units.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::vector<uint64_t> unit_words(units.size());
    for (size_t i = 0; i < units.size(); ++i) {
        for (const uint64_t e : units[i]) unit_words[i] += plan.fns.at(e).words;
    }
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return unit_words[a] > unit_words[b]; });
    struct Result {
        std::string path, error, ir, asm_text;
        UnitStats st;
        bool hit = false;
        uint64_t read = 0, written = 0;
        double t_key = 0;
        std::vector<LineRow> lines;  // the unit's line marks (ReadLineMarks)
        uint64_t lines_outside = 0;  // marks in no body
    };
    // A unit's object: its line marks read back and its marks' section kept out of the link (in the cache too: a hit is read the same way)
    const auto line_marks = [&](std::string* object, Result& res, bool fresh) {
        if (!o.line_table) return true;
        if (fresh && !MarkExcluded(object, &res.error)) return false;
        return ReadLineMarks(*object, c.prefix, &res.lines, &res.lines_outside, &res.error);
    };
    std::vector<Result> results(units.size() + 1);
    std::atomic<size_t> next{0};
    static const bool trace = std::getenv("NXAOT_TRACE") != nullptr;  // each unit's entry on stderr before it is compiled
    const auto work = [&](llvm::TargetMachine* tm) {
        for (size_t j; (j = next.fetch_add(1)) < results.size();) {
            const size_t r = j == 0 ? 0 : order[j - 1] + 1;
            Result& res = results[r];
            std::string object;
            if (r == 0) {
                if (CompileTables(c, *tm, &object, &res.st, &s, o.ir_path.empty() ? nullptr : &res.ir, o.asm_path.empty() ? nullptr : &res.asm_text,
                                  &res.error)) {
                    res.path = work_dir + "/tables.o";
                    WriteFile(res.path, object, &res.error);
                }
                continue;
            }
            const std::vector<uint64_t>& entries = units[r - 1];
            const uint64_t entry = entries.front();
            std::string key;
            if (cache) {
                const auto tk = Clock::now();
                key = UnitKey(c, entries);
                res.t_key = Since(tk);
                cache::Facts facts;
                uint64_t bytes = 0;
                if (cache->Lookup(key, &facts, &bytes)) {
                    res.hit = true;
                    res.read = bytes;
                    res.st = UnitStats::FromFacts(facts);
                    res.path = cache->ObjectPath(key);
                    if (o.line_table) {
                        auto buf = llvm::MemoryBuffer::getFile(res.path);
                        if (!buf) {
                            res.error = "cannot read " + res.path + ": " + buf.getError().message();
                            continue;
                        }
                        std::string cached = (*buf)->getBuffer().str();
                        line_marks(&cached, res, false);
                    }
                    continue;
                }
            }
            if (trace) {
                std::fprintf(stderr, "nxaot: unit %#llx\n", static_cast<unsigned long long>(entry));
            }
            if (!CompileUnit(c, *tm, entries, &object, &res.st, o.ir_path.empty() ? nullptr : &res.ir, o.asm_path.empty() ? nullptr : &res.asm_text,
                             &res.error)) {
                continue;
            }
            if (!line_marks(&object, res, true)) {
                continue;
            }
            if (cache) {
                if (cache->Store(key, object, res.st.ToFacts(), &res.error)) {
                    res.path = cache->ObjectPath(key);
                    res.written = object.size();
                }
            } else {
                res.path = work_dir + "/f" + llvm::utohexstr(entry) + ".o";
                WriteFile(res.path, object, &res.error);
            }
        }
    };
    t = Clock::now();
    if (workers == 1) {
        work(tms[0].get());
    } else {
        std::vector<std::thread> pool;
        for (unsigned i = 0; i < workers; ++i) {
            pool.emplace_back(work, tms[i].get());
        }
        for (auto& th : pool) {
            th.join();
        }
    }
    s.t_compile_wall = Since(t);

    // ---- The results, in link order: the tables' object, then the units by entry, then the line table's ----
    std::string ir, asm_text;
    std::vector<LineRow> lines;
    for (size_t r = 0; r < results.size(); ++r) {
        Result& res = results[r];
        if (!res.error.empty()) {
            *error = res.error;
            return false;
        }
        objects->push_back(res.path);
        lines.insert(lines.end(), res.lines.begin(), res.lines.end());
        res.lines.clear();
        res.lines.shrink_to_fit();
        s.lines_lost += res.lines_outside;
        const UnitStats& u = res.st;
        s.blocks += u.blocks;
        s.calls += u.calls;
        s.indirect_calls += u.indirect_calls;
        s.tail_calls += u.tail_calls;
        s.indirect_tails += u.indirect_tails;
        s.returns += u.returns;
        s.svcs += u.svcs;
        s.traps += u.traps;
        s.refused += u.refused;
        s.lines_lost += u.lines_lost;
        s.spills += u.spills;
        s.reloads += u.reloads;
        s.text_bytes += u.text_bytes;
        s.object_bytes += u.object_bytes;
        s.t_lift += u.t_lift;
        s.t_verify += u.t_verify;
        s.t_opt += u.t_opt;
        s.t_codegen += u.t_codegen;
        s.t_key += res.t_key;
        for (const auto& n : u.notes) {
            s.notes.push_back(n);
        }
        if (r) {
            s.cache_hits += res.hit;
            s.cache_misses += cache && !res.hit;
            s.cache_bytes_read += res.read;
            s.cache_bytes_written += res.written;
        }
        ir += res.ir;
        asm_text += res.asm_text;
    }
    if (o.line_table) {
        const auto tl = Clock::now();
        UnitStats ls;
        std::string object;
        if (!CompileLines(c, *tms[0], lines, &object, &ls, &s.line_entries, error)) {
            return false;
        }
        const std::string path = work_dir + "/lines.o";
        if (!WriteFile(path, object, error)) {
            return false;
        }
        objects->push_back(path);
        s.object_bytes += ls.object_bytes;
        s.t_compile_wall += Since(tl);
    }
    if (!o.ir_path.empty() && !WriteFile(o.ir_path, ir, error)) {
        return false;
    }
    if (!o.asm_path.empty() && !WriteFile(o.asm_path, asm_text, error)) {
        return false;
    }
    struct rusage ru {};
    getrusage(RUSAGE_SELF, &ru);
    s.peak_rss_kib = static_cast<uint64_t>(ru.ru_maxrss);
    if (llvm::TimePassesIsEnabled) {
        llvm::reportAndResetTimings(&llvm::errs());
    }
    return true;
}

bool BuildObject(const module::Image& image, const BuildOptions& o, const std::string& object_path, BuildStats* stats, std::string* error) {
    const std::string work = object_path + ".parts";
    std::vector<std::string> objects;
    bool ok = BuildObjects(image, o, work, &objects, stats, error);
    if (ok) {
        const auto t = Clock::now();
        ok = LinkRelocatable(objects, object_path, error);
        stats->t_link += Since(t);
    }
    llvm::sys::fs::remove_directories(work);
    return ok;
}

bool LinkShared(const std::vector<std::string>& objects, const std::string& output, const std::string& soname, std::string* error) {
    std::vector<const char*> args{"ld.lld", "-shared", "--hash-style=gnu", "-z", "now", "-z", "relro", "-z", "noexecstack", "--eh-frame-hdr",
                                  "-soname", soname.c_str(), "-o", output.c_str()};
    for (const auto& o : objects) {
        args.push_back(o.c_str());
    }
    std::string out, err;
    llvm::raw_string_ostream out_s{out}, err_s{err};
    const lld::Result r = lld::lldMain(args, out_s, err_s, {{lld::Gnu, &lld::elf::link}});
    if (r.retCode != 0) {
        *error = "lld: " + err_s.str();
        return false;
    }
    return true;
}

bool LinkRelocatable(const std::vector<std::string>& objects, const std::string& output, std::string* error) {
    std::vector<const char*> args{"ld.lld", "-r", "-o", output.c_str()};
    for (const auto& o : objects) {
        args.push_back(o.c_str());
    }
    std::string out, err;
    llvm::raw_string_ostream out_s{out}, err_s{err};
    const lld::Result r = lld::lldMain(args, out_s, err_s, {{lld::Gnu, &lld::elf::link}});
    if (r.retCode != 0) {
        *error = "lld -r: " + err_s.str();
        return false;
    }
    return true;
}

}  // namespace nxaot::build
