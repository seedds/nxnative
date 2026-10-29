// LLVM ORC JIT for lifted code inside nxaot_vec: modules get the target's data layout, the default O2 pipeline (its SROA / mem2reg promote the
// register allocas), and are compiled for the pack machine's own CPU by default (llvm::sys::getHostCPUName and its features), or for a named CPU
// level such as x86-64-v3 (aot-recompiler.md section 15, decision 8).
#pragma once

#include <memory>
#include <string>

namespace llvm {
class LLVMContext;
class Module;
}  // namespace llvm

namespace nxaot::lift {

class Jit {
public:
    // cpu: empty for the host's CPU, else an LLVM CPU name ("x86-64-v3"). Null with *error set on failure
    static std::unique_ptr<Jit> Create(const std::string& cpu, std::string* error);
    ~Jit();

    // A module to fill with lifted functions, with this JIT's triple and data layout
    std::unique_ptr<llvm::Module> NewModule(llvm::LLVMContext& context, const std::string& name) const;

    // Verifies, optimises and adds the module (its context goes with it). False with *error set when the module is invalid
    bool Add(std::unique_ptr<llvm::Module> module, std::unique_ptr<llvm::LLVMContext> context, std::string* error);

    // The address of a function of an added module, null when absent
    void* Lookup(const std::string& name, std::string* error);

    // The CPU and features code is compiled for
    std::string Cpu() const;
    std::string Features() const;

    // The optimised IR of the last module added (nxaot_vec --dump)
    const std::string& LastIr() const;
    void KeepIr(bool keep);

private:
    Jit();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace nxaot::lift
