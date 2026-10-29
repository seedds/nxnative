// The executor under test: each vector's code lifted (src/lift) into a function of an LLVM module, compiled with ORC for this machine's CPU, and run on
// a dm_guest_state. The memory image is mapped at kGuestBase in this process, since recompiled code uses guest addresses as host addresses
// (aot-recompiler.md section 5.1), with inaccessible guard pages around it; a fault in lifted code ends that vector's run, not the harness.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "dynarmic/ir/opcodes.h"
#include "nxaot/guest_state.h"
#include "vec/vectors.h"

namespace nxaot::vec {

struct Prepared {
    void (*fn)(dm_guest_state*) = nullptr;
    std::vector<std::string> unsupported;  // dynarmic IR opcodes the lifter refused the code for
    std::string error;                     // a lifter or JIT error
    std::vector<Dynarmic::IR::Opcode> opcodes;  // the IR opcodes of the lifted region
};

class Lifted {
public:
    static std::unique_ptr<Lifted> Create(const std::string& cpu, std::string* error);
    ~Lifted();

    // Lifts and compiles a batch of vectors as one module. With dump, *ir receives the optimised module
    std::vector<Prepared> Prepare(const std::vector<const Vector*>& batch, bool dump, std::string* ir);
    Outcome Run(const Prepared& p, const State& init, const std::vector<uint8_t>& image);
    std::string Cpu() const;

private:
    Lifted();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace nxaot::vec
