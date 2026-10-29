#include "build/pack.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <set>

#include <sys/resource.h>

#include <lld/Common/Driver.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/BinaryFormat/Dwarf.h>
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
#include <llvm/IR/Verifier.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
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
#include "build/plan.h"
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
    llvm::PassBuilder pb{tm};
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
    return llvm::StructType::get(c, {i32, i32, name, name, i64, i64, i64, i64, i32, i32, i32, i32, ptr, ptr, ptr, i64, ptr, ptr, ptr, ptr});
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

bool BuildObject(const module::Image& image, const BuildOptions& o, const std::string& object_path, BuildStats* stats, std::string* error) {
    BuildStats& s = *stats;
    const std::string prefix = Sanitize(image.name);
    s.module = image.name;

    // ---- Analysis: discovery, the census's FPSR answer (decision 5), the convention facts ----
    auto t = Clock::now();
    const analysis::Discovery d = analysis::Discover(image);
    bool fpsr = o.fpsr_flags == 1;
    if (o.fpsr_flags < 0) {
        const auto counts = analysis::Census(image.bytes.data() + image.text_off, image.text_size, image.text_off);
        const auto it = counts.find("answer.fpsr_read");
        fpsr = it != counts.end() && it->second != 0;
    }
    s.fpsr_flags = fpsr;
    s.t_discover = Since(t);
    for (const auto& n : d.notes) {
        s.notes.push_back(n);
    }
    lift::Convention conv;
    conv.cc = o.cc;
    t = Clock::now();
    const Plan plan = MakePlan(image, d, conv, o.all_entries);
    s.t_plan = Since(t);

    auto tm = MakeTarget(o.cpu, o.tier, &s.cpu, &s.features, error);
    if (!tm) {
        return false;
    }
    llvm::LLVMContext ctx;
    // The backend's remarks: a guest memory access folded into another instruction keeps no !pcsections entry ("loosing !pcsections
    // metadata"); those are counted (the line table then maps that native address through its function), anything else is kept as a note
    struct Diagnostics {
        BuildStats* s;
    } diag{&s};
    ctx.setDiagnosticHandlerCallBack(
        [](const llvm::DiagnosticInfo& di, void* context) {
            auto* d = static_cast<Diagnostics*>(context);
            std::string text;
            llvm::raw_string_ostream out{text};
            llvm::DiagnosticPrinterRawOStream printer{out};
            di.print(printer);
            if (out.str().find("pcsections") != std::string::npos) {
                ++d->s->lines_lost;
            } else if (di.getSeverity() == llvm::DS_Error || di.getSeverity() == llvm::DS_Warning) {
                d->s->notes.push_back(std::string(di.getSeverity() == llvm::DS_Error ? "[RED] " : "") + "nxaot build: llvm: " + out.str());
            }
        },
        &diag);
    auto m = std::make_unique<llvm::Module>(prefix, ctx);
    m->setDataLayout(tm->createDataLayout());
    m->setTargetTriple(tm->getTargetTriple().str());
    llvm::IRBuilder<> b{ctx};
    auto* i8 = b.getInt8Ty();
    auto* i32 = b.getInt32Ty();
    auto* i64 = b.getInt64Ty();
    auto* ptr = b.getPtrTy();
    auto* v128 = llvm::FixedVectorType::get(i64, 2);

    // ---- The module's base, the empty inline-cache descriptor, the run-time entry points ----
    auto* base = new llvm::GlobalVariable(*m, i64, false, llvm::GlobalValue::InternalLinkage, b.getInt64(0), "nxaot.base");
    base->setAlignment(llvm::Align(8));
    auto* ic_desc_ty = llvm::StructType::get(ctx, {i64, ptr});
    auto* ic_empty = new llvm::GlobalVariable(*m, ic_desc_ty, true, llvm::GlobalValue::InternalLinkage,
                                              llvm::ConstantStruct::get(ic_desc_ty, {b.getInt64(UINT64_MAX), llvm::ConstantPointerNull::get(ptr)}),
                                              "nxaot.ic_empty");
    ic_empty->setAlignment(llvm::Align(16));
    const auto extern_fn = [&](const char* name, llvm::FunctionType* ty) {
        auto* f = llvm::Function::Create(ty, llvm::GlobalValue::ExternalLinkage, name, *m);
        f->addFnAttr(llvm::Attribute::NoUnwind);
        return f;
    };
    auto* svc = extern_fn("dm_aot_svc", llvm::FunctionType::get(b.getVoidTy(), {ptr, i32}, false));
    auto* trap = extern_fn("nxaot_rt_trap", llvm::FunctionType::get(b.getVoidTy(), {ptr, i64, i32}, false));
    trap->setDoesNotReturn();
    trap->addFnAttr(llvm::Attribute::Cold);
    // Slow paths through preserve_all / preserve_most wrappers (section 2.3): the call sites keep their registers, the wrapper saves them
    const auto wrapper = [&](const char* name, llvm::Function* target, unsigned cc) {
        auto* w = llvm::Function::Create(target->getFunctionType(), llvm::GlobalValue::InternalLinkage, name, *m);
        w->setCallingConv(cc);
        w->addFnAttr(llvm::Attribute::NoUnwind);
        w->addFnAttr(llvm::Attribute::NoInline);
        w->addFnAttr(llvm::Attribute::Cold);
        llvm::IRBuilder<> wb{llvm::BasicBlock::Create(ctx, "entry", w)};
        std::vector<llvm::Value*> args;
        for (auto& a : w->args()) {
            args.push_back(&a);
        }
        auto* call = wb.CreateCall(target, args);
        wb.CreateRet(call);
        return w;
    };
    auto* dispatch_c = extern_fn("nxaot_rt_dispatch", llvm::FunctionType::get(ptr, {ptr, i64, ptr, i64}, false));
    auto* dispatch = wrapper("nxaot.dispatch", dispatch_c, llvm::CallingConv::PreserveAll);
    auto* counter_c = extern_fn("dm_aot_cntpct", llvm::FunctionType::get(i64, {ptr}, false));
    auto* counter = wrapper("nxaot.cntpct", counter_c, llvm::CallingConv::PreserveMost);
    counter->removeFnAttr(llvm::Attribute::Cold);

    // ---- Debug information: a synthetic file per module, line = guest offset (section 9.2) ----
    std::unique_ptr<llvm::DIBuilder> dib;
    llvm::DIFile* file = nullptr;
    llvm::DISubroutineType* sub_ty = nullptr;
    if (o.debug_info) {
        dib = std::make_unique<llvm::DIBuilder>(*m);
        file = dib->createFile(prefix + ".s", ".");
        dib->createCompileUnit(llvm::dwarf::DW_LANG_C, file, std::string("nxaot (LLVM ") + NXAOT_LOCK_LLVM + ")", o.tier != Tier::C, "", 0);
        sub_ty = dib->createSubroutineType(dib->getOrCreateTypeArray({}));
        m->addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
        m->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 5);
    }
    const auto subprogram = [&](llvm::Function* f, uint64_t line) -> llvm::DISubprogram* {
        if (!dib) {
            return nullptr;
        }
        auto* sp = dib->createFunction(file, f->getName(), f->getName(), file, static_cast<unsigned>(line), sub_ty, static_cast<unsigned>(line),
                                       llvm::DINode::FlagZero,
                                       llvm::DISubprogram::SPFlagDefinition | (o.tier != Tier::C ? llvm::DISubprogram::SPFlagOptimized
                                                                                                  : llvm::DISubprogram::SPFlagZero));
        f->setSubprogram(sp);
        return sp;
    };

    // ---- Declarations: a body per function, a standard entry per host-callable function ----
    const lift::Regs std_pass = plan.indirect_pass;  // the standard entries' one signature (section 2.3)
    llvm::FunctionType* std_ty = conv.Type(ctx, std_pass);
    std::map<uint64_t, lift::CalleeView> views;
    std::map<uint64_t, llvm::Function*> std_entries;
    for (const auto& [entry, ff] : plan.fns) {
        auto* f = llvm::Function::Create(conv.Type(ctx, ff.pass), llvm::GlobalValue::InternalLinkage, prefix + "+0x" + llvm::utohexstr(entry), *m);
        conv.Apply(f);
        if (o.no_inline) {
            f->addFnAttr(llvm::Attribute::NoInline);  // nxaot_bench: calls stay calls, to measure the convention
        }
        lift::CalleeView& v = views[entry];
        v.fn = f;
        v.pass = ff.pass;
        v.writes = ff.writes;
        v.rv = ff.rv;
        v.returns = ff.returns;
        s.words += ff.words;
    }
    s.functions = plan.fns.size();
    for (const auto& [entry, ff] : plan.fns) {
        if (!ff.host_callable) {
            continue;
        }
        llvm::Function* body = views[entry].fn;
        if (conv.cc == lift::CallConv::Ghc && ff.pass == std_pass) {
            std_entries[entry] = body;  // the uniform signature already takes everything
            continue;
        }
        // The standard entry (section 2.3): every passable register in, what the body does not take stored to the block, then a tail call
        auto* e = llvm::Function::Create(std_ty, llvm::GlobalValue::InternalLinkage, body->getName() + ".std", *m);
        conv.Apply(e);
        e->addFnAttr(llvm::Attribute::NoUnwind);
        e->addFnAttr("frame-pointer", "none");
        e->addFnAttr(llvm::Attribute::getWithUWTableKind(ctx, llvm::UWTableKind::Async));
        llvm::DISubprogram* sp = subprogram(e, entry);
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
        std_entries[entry] = e;
    }
    s.standard_entries = std_entries.size();

    // ---- Lifting every function (function mode) ----
    t = Clock::now();
    lift::CalleeView indirect;
    indirect.pass = std_pass;
    indirect.writes = plan.indirect_writes;
    indirect.rv = plan.indirect_rv;
    indirect.returns = true;
    indirect.exact = false;  // a union: the function that runs stores only its own writes
    lift::LiftOptions lo;
    lo.fpcr = 0;
    lo.fpsr_flags = fpsr;
    lo.target = TargetOf(*tm);
    const decode::CodeReader text = [&image](uint64_t a) -> std::optional<uint32_t> {
        if (!image.InText(a) || image.InData(a)) {
            return std::nullopt;
        }
        return image.Word(a);
    };
    for (const auto& [entry, ff] : plan.fns) {
        lift::FunctionPlan fp;
        fp.conv = conv;
        fp.fn = views[entry].fn;
        fp.self = views[entry];
        fp.live_after = ff.live_after;
        fp.entry = entry;
        fp.limit = ff.limit;
        fp.correctness = o.correctness;
        fp.base = base;
        fp.callee = [&views](uint64_t off) -> const lift::CalleeView* {
            const auto it = views.find(off);
            return it == views.end() ? nullptr : &it->second;
        };
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
        fp.rt_dispatch = dispatch;
        fp.rt_svc = svc;
        fp.rt_trap = trap;
        fp.rt_counter = counter;
        fp.ic_empty = ic_empty;
        fp.ic_prefix = "nxaot.ic." + llvm::utohexstr(entry) + ".";
        fp.debug = subprogram(fp.fn, entry);
        fp.line_section = o.line_table ? "nxaot_lines" : "";
        lift::Region region;
        region.entry = entry;
        region.lo = entry;
        region.hi = ff.limit;
        region.max_blocks = size_t{1} << 20;
        lo.plan = &fp;
        const uint64_t limit = ff.limit;
        const decode::CodeReader read = [&text, entry, limit](uint64_t a) -> std::optional<uint32_t> {
            if (a < entry || a >= limit) {
                return std::nullopt;  // a block runs to the function's limit: the next function's entry is reached by a tail call
            }
            return text(a);
        };
        const lift::Lifted l = lift::LiftRegion(*m, fp.fn->getName().str(), region, read, lo);
        s.blocks += l.blocks;
        s.calls += l.calls;
        s.indirect_calls += l.indirect_calls;
        s.tail_calls += l.tail_calls;
        s.indirect_tails += l.indirect_tails;
        s.returns += l.returns;
        s.svcs += l.svcs;
        s.traps += l.traps;
        s.tables += ff.tables.size();
        s.resume_sites += ff.continuations.size() + ff.pads.size();
        if (!l.function) {
            // Refused (an opcode the lowering does not cover, an internal error): a body that hands the function to the JIT fallback (section 11,
            // case 4), so that the rest of the module still runs
            ++s.refused;
            std::string why = l.error;
            for (const auto& op : l.unsupported) {
                why += (why.empty() ? "" : ", ") + op;
            }
            s.notes.push_back("[RED] nxaot build: " + fp.fn->getName().str() + " refused (" + why + "): it runs in the JIT fallback (phase 3)");
            llvm::Function* f = fp.fn;
            f->setLinkage(llvm::GlobalValue::InternalLinkage);
            llvm::IRBuilder<> fb{llvm::BasicBlock::Create(ctx, "entry", f)};
            if (fp.debug) {
                fb.SetCurrentDebugLocation(llvm::DILocation::get(ctx, static_cast<unsigned>(entry), 0, fp.debug));
            }
            auto* call = fb.CreateCall(trap, {f->getArg(0), fb.getInt64(entry), fb.getInt32(NXAOT_TRAP_JIT_REFUSED)});
            call->setDoesNotReturn();
            fb.CreateUnreachable();
        }
    }
    s.t_lift = Since(t);

    // ---- The tables: the lookup hash, the function map, the line table's bounds; the trampoline; the descriptor ----
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
    s.lookup_slots = slots;
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
            rows.push_back(llvm::ConstantStruct::get(entry_ty, {b.getInt64(keys[i]), targets[i] ? rel(targets[i], field) : b.getInt32(0), b.getInt32(0)}));
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
            rows.push_back(llvm::ConstantStruct::get(entry_ty, {b.getInt64(entry), rel(views[entry].fn, field), b.getInt32(size)}));
            ++i;
        }
        fnmap->setInitializer(llvm::ConstantArray::get(fns_ty, rows));
    }
    // The line table is the section !pcsections fills; the linker defines its bounds (weak: a module without entries has none)
    auto* lines_start = new llvm::GlobalVariable(*m, i8, true, llvm::GlobalValue::ExternalWeakLinkage, nullptr, "__start_nxaot_lines");
    auto* lines_stop = new llvm::GlobalVariable(*m, i8, true, llvm::GlobalValue::ExternalWeakLinkage, nullptr, "__stop_nxaot_lines");
    lines_start->setVisibility(llvm::GlobalValue::HiddenVisibility);
    lines_stop->setVisibility(llvm::GlobalValue::HiddenVisibility);

    // enter(st, standard entry): the C convention into the pack's convention, every passable register from st; x0, x1 and the standard entries'
    // RV back to st unless the guest left through a non-local transfer (st->pending: the block then holds the whole register file)
    auto* enter = llvm::Function::Create(llvm::FunctionType::get(b.getVoidTy(), {ptr, ptr}, false), llvm::GlobalValue::InternalLinkage, "nxaot.enter", *m);
    enter->addFnAttr(llvm::Attribute::NoUnwind);
    enter->addFnAttr(llvm::Attribute::NoInline);
    enter->addFnAttr(llvm::Attribute::getWithUWTableKind(ctx, llvm::UWTableKind::Async));
    {
        llvm::DISubprogram* sp = subprogram(enter, 0);
        llvm::IRBuilder<> eb{llvm::BasicBlock::Create(ctx, "entry", enter)};
        if (sp) {
            eb.SetCurrentDebugLocation(llvm::DILocation::get(ctx, 0, 0, sp));
        }
        llvm::Value* st = enter->getArg(0);
        const auto field = [&](size_t off) { return eb.CreateConstInBoundsGEP1_64(i8, st, off); };
        std::vector<llvm::Value*> args(std_ty->getNumParams());
        args[0] = st;
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
        auto* call = eb.CreateCall(std_ty, enter->getArg(1), args);
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
        layout->getElementOffset(19) != offsetof(nxaot_module_desc, tool)) {
        *error = "the descriptor's LLVM layout differs from include/nxaot/aot_module.h";
        return false;
    }
    std::string name_bytes = image.name.substr(0, 31);
    name_bytes.resize(32, '\0');
    const std::string tool = std::string("nxaot llvm ") + NXAOT_LOCK_LLVM + " dynarmic " + NXAOT_LOCK_DYNARMIC + " cc " + lift::CallConvName(o.cc) +
                             " tier " + TierName(o.tier) + " mode " + o.mode + (o.correctness ? " correctness" : "") +
                             (o.all_entries ? " all-entries" : "") + " cpu " + s.cpu;
    auto* tool_str = b.CreateGlobalString(tool, "nxaot.tool", 0, m.get());
    const uint32_t flags = (o.cc == lift::CallConv::Ghc ? NXAOT_FLAG_GHC : 0) | (o.correctness ? NXAOT_FLAG_CORRECTNESS : 0) |
                           (o.all_entries ? NXAOT_FLAG_ALL_ENTRIES : 0) | (fpsr ? NXAOT_FLAG_FPSR : 0);
    auto* desc = new llvm::GlobalVariable(
        *m, desc_ty, true, llvm::GlobalValue::ExternalLinkage,
        llvm::ConstantStruct::get(
            desc_ty, {b.getInt32(NXAOT_MODULE_ABI), b.getInt32(flags), llvm::ConstantDataArray::getString(ctx, name_bytes, false),
                      llvm::ConstantDataArray::get(ctx, llvm::ArrayRef<uint8_t>(image.sha256.data(), image.sha256.size())),
                      b.getInt64(image.bytes.size()), b.getInt64(image.TextEnd()), b.getInt64(image.bss_size), b.getInt64(image.text_off),
                      b.getInt32(0), b.getInt32(slots - 1), b.getInt32(plan.indirect_rv), b.getInt32(0), base, lookup, fnmap,
                      b.getInt64(plan.fns.size()), lines_start, lines_stop, enter, tool_str}),
        "nxaot_module");
    desc->setAlignment(llvm::Align(8));
    if (dib) {
        dib->finalize();
    }

    std::string verify;
    llvm::raw_string_ostream verify_out{verify};
    if (llvm::verifyModule(*m, &verify_out)) {
        *error = "invalid module: " + verify_out.str().substr(0, 4000);
        return false;
    }

    // ---- The tier's passes, then native code ----
    t = Clock::now();
    Optimise(*m, tm.get(), o.tier);
    s.t_opt = Since(t);
    if (o.count_spills || !o.asm_path.empty()) {
        // The register allocator's spills and reloads, from the comments of verbose assembly of the same code
        std::unique_ptr<llvm::Module> copy = llvm::CloneModule(*m);
        llvm::SmallString<0> text_asm;
        llvm::raw_svector_ostream out{text_asm};
        llvm::legacy::PassManager pm;
        tm->Options.MCOptions.AsmVerbose = true;
        if (!tm->addPassesToEmitFile(pm, out, nullptr, llvm::CodeGenFileType::AssemblyFile)) {
            pm.run(*copy);
            if (!o.asm_path.empty()) {
                std::error_code ec;
                llvm::raw_fd_ostream asm_out{o.asm_path, ec, llvm::sys::fs::OF_None};
                if (!ec) {
                    asm_out << text_asm.str();
                }
            }
            llvm::StringRef rest = text_asm.str();
            while (!rest.empty()) {
                auto [line, tail] = rest.split('\n');
                rest = tail;
                if (line.contains("Spill")) {
                    ++s.spills;
                } else if (line.contains("Reload")) {
                    ++s.reloads;
                }
            }
        }
    }
    t = Clock::now();
    {
        std::error_code ec;
        llvm::raw_fd_ostream out{object_path, ec, llvm::sys::fs::OF_None};
        if (ec) {
            *error = "cannot write " + object_path + ": " + ec.message();
            return false;
        }
        llvm::legacy::PassManager pm;
        if (tm->addPassesToEmitFile(pm, out, nullptr, llvm::CodeGenFileType::ObjectFile)) {
            *error = "the target machine cannot emit an object file";
            return false;
        }
        pm.run(*m);
    }
    s.t_codegen = Since(t);
    if (auto obj = llvm::object::ObjectFile::createObjectFile(object_path)) {
        s.object_bytes = obj->getBinary()->getData().size();
        for (const auto& sec : obj->getBinary()->sections()) {
            auto n = sec.getName();
            if (n && *n == ".text") {
                s.text_bytes += sec.getSize();
            }
        }
    } else {
        llvm::consumeError(obj.takeError());
    }
    struct rusage ru {};
    getrusage(RUSAGE_SELF, &ru);
    s.peak_rss_kib = static_cast<uint64_t>(ru.ru_maxrss);
    return true;
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

}  // namespace nxaot::build
