// The reference executor of nxaot_vec: dynarmic's own A64 JIT (its x86-64 backend) over the vector's memory image, configured as the recompiled
// code sees the Switch (counter frequency, cache type, dc zva size, thread registers). The run stops when the guest leaves the vector's code: dynarmic
// then fetches outside it and raises NoExecuteFault, which ends the run with pc at the first address outside. New code runs twice and the
// second run is the outcome (Run: its final FPSR then holds the guest code's flags only).
#pragma once

#include <memory>

#include "vec/vectors.h"

namespace nxaot::vec {

class Reference {
public:
    Reference();
    ~Reference();
    Outcome Run(const Vector& v, const State& init, const std::vector<uint8_t>& image);

private:
    Outcome RunOnce(const Vector& v, const State& init, const std::vector<uint8_t>& image, bool clear_cache);
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace nxaot::vec
