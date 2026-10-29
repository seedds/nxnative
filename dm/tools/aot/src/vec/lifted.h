// The executor under test: each vector's code lifted (src/lift) into a function of an LLVM module, compiled with ORC for this machine's CPU, and run on
// a dm_guest_state. The memory image is mapped at kGuestBase in this process, since recompiled code uses guest addresses as host addresses
// (aot-recompiler.md section 5.1), with inaccessible guard pages around it; a fault in lifted code ends that vector's run, not the harness.
//
// The run does what the host does around guest code (section 7.1): MXCSR is loaded with the vector's FPCR (lift::GuestMxcsr) before the call and
// its exception flags are added to st->fpsr after it (lift::FpsrFromMxcsr). A function that leaves normally at a pc inside the vector's code (an
// msr fpcr that changed the mode bits, a computed branch back into the code) continues in a function lifted for that pc and the current FPCR, as
// the dispatcher of a pack will.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "dynarmic/ir/opcodes.h"
#include "lift/lifter.h"
#include "nxaot/guest_state.h"
#include "vec/vectors.h"

namespace nxaot::vec {

struct Prepared {
    void (*fn)(dm_guest_state*) = nullptr;
    std::vector<std::string> unsupported;  // dynarmic IR opcodes the lifter refused the code for
    std::string error;                     // a lifter or JIT error
    std::vector<Dynarmic::IR::Opcode> opcodes;  // the IR opcodes of the lifted region
    std::vector<int> dynarmic_issues;           // lift::DynarmicIssue values the region meets
};

class Lifted {
public:
    static std::unique_ptr<Lifted> Create(const std::string& cpu, lift::NaNMode nan_mode, bool fpsr_flags, std::string* error);
    ~Lifted();

    // Lifts and compiles a batch of vectors as one module (vectors with the same code and FPCR share one function). With dump, *ir receives
    // the optimised module
    std::vector<Prepared> Prepare(const std::vector<const Vector*>& batch, bool dump, std::string* ir);
    Outcome Run(const Vector& v, const Prepared& p, const State& init, const std::vector<uint8_t>& image);
    std::string Cpu() const;
    lift::Target Target() const;
    size_t Continuations() const;  // functions lifted for a continuation (Run)

private:
    Lifted();
    void* Continuation(const Vector& v, uint64_t pc, uint32_t fpcr, std::vector<int>* issues, std::string* error);
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace nxaot::vec
