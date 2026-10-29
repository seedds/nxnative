#include "lift/jit.h"

#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/SubtargetFeature.h>

namespace nxaot::lift {

struct Jit::Impl {
    std::unique_ptr<llvm::orc::LLJIT> jit;
    std::unique_ptr<llvm::TargetMachine> tm;
    std::string cpu, features;
    bool keep_ir = false;
    std::string last_ir;
};

Jit::Jit() : impl(std::make_unique<Impl>()) {}
Jit::~Jit() = default;

std::unique_ptr<Jit> Jit::Create(const std::string& cpu, std::string* error) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();  // getHostCPUName() and getHostCPUFeatures()
    if (!jtmb) {
        *error = llvm::toString(jtmb.takeError());
        return nullptr;
    }
    if (!cpu.empty()) {
        // A named level: its own features only, not the build machine's
        jtmb->setCPU(cpu);
        jtmb->getFeatures() = llvm::SubtargetFeatures();
    }
    jtmb->setCodeGenOptLevel(llvm::CodeGenOptLevel::Default);

    std::unique_ptr<Jit> self{new Jit()};
    self->impl->cpu = jtmb->getCPU();
    self->impl->features = jtmb->getFeatures().getString();
    auto tm = jtmb->createTargetMachine();
    if (!tm) {
        *error = llvm::toString(tm.takeError());
        return nullptr;
    }
    self->impl->tm = std::move(*tm);
    auto jit = llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(*jtmb)).create();
    if (!jit) {
        *error = llvm::toString(jit.takeError());
        return nullptr;
    }
    self->impl->jit = std::move(*jit);
    // Lifted code calls nothing outside itself today; a helper it may need later (a libcall LLVM emits) is looked up in the process
    auto generator = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(self->impl->jit->getDataLayout().getGlobalPrefix());
    if (!generator) {
        *error = llvm::toString(generator.takeError());
        return nullptr;
    }
    self->impl->jit->getMainJITDylib().addGenerator(std::move(*generator));
    return self;
}

std::unique_ptr<llvm::Module> Jit::NewModule(llvm::LLVMContext& context, const std::string& name) const {
    auto module = std::make_unique<llvm::Module>(name, context);
    module->setDataLayout(impl->jit->getDataLayout());
    module->setTargetTriple(impl->jit->getTargetTriple().str());
    return module;
}

bool Jit::Add(std::unique_ptr<llvm::Module> module, std::unique_ptr<llvm::LLVMContext> context, std::string* error) {
    std::string verify;
    llvm::raw_string_ostream verify_out{verify};
    if (llvm::verifyModule(*module, &verify_out)) {
        *error = "invalid module: " + verify_out.str();
        return false;
    }

    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    llvm::PassBuilder pb{impl->tm.get()};
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
    mpm.run(*module, mam);

    if (impl->keep_ir) {
        impl->last_ir.clear();
        llvm::raw_string_ostream out{impl->last_ir};
        module->print(out, nullptr);
    }
    if (auto err = impl->jit->addIRModule(llvm::orc::ThreadSafeModule(std::move(module), llvm::orc::ThreadSafeContext(std::move(context))))) {
        *error = llvm::toString(std::move(err));
        return false;
    }
    return true;
}

void* Jit::Lookup(const std::string& name, std::string* error) {
    auto sym = impl->jit->lookup(name);
    if (!sym) {
        *error = llvm::toString(sym.takeError());
        return nullptr;
    }
    return sym->toPtr<void*>();
}

std::string Jit::Cpu() const {
    return impl->cpu;
}

std::string Jit::Features() const {
    return impl->features;
}

const std::string& Jit::LastIr() const {
    return impl->last_ir;
}

void Jit::KeepIr(bool keep) {
    impl->keep_ir = keep;
}

}  // namespace nxaot::lift
