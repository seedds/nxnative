// Test vectors of nxaot_vec (tests/vectors/*.tsv, written by tests/gen_vectors.py) and the machine state both executors start from.
//
// A vector line: id, class, text, code, seed, regs, mem, separated by tabs.
//   code  the instruction words, hex, comma-separated; they run from kCodeBase
//   seed  64-bit hex: fills x0..x30, sp, nzcv, tpidr_el0, tpidrro_el0, the counter and the 64 KiB memory image (SeedState)
//   regs  name=hex overrides, comma-separated (x0..x30, sp, nzcv, fpcr, fpsr, tpidr, tpidrro, cnt), or -
//   mem   offset:hexbytes overrides of the image (offset from kGuestBase, hex), comma-separated, or -
// The generator relies on these conventions; tests/gen_vectors.py repeats the constants.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nxaot::vec {

constexpr uint64_t kGuestBase = 0x10000000;  // where the memory image is (one-to-one guest addresses: the lifted code uses it as the host address)
constexpr uint64_t kImageSize = 0x10000;     // 64 KiB
constexpr uint64_t kCodeOffset = 0x8000;     // the code words are also in the image, so pc-relative loads read them
constexpr uint64_t kCodeBase = kGuestBase + kCodeOffset;

struct Vector {
    std::string id, cls, text;
    std::vector<uint32_t> code;
    uint64_t seed = 0;
    std::vector<std::pair<std::string, uint64_t>> regs;
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> mem;
    std::string where;  // file:line
};

struct State {
    std::array<uint64_t, 31> x{};
    uint64_t sp = 0, pc = 0;
    uint32_t nzcv = 0, fpcr = 0, fpsr = 0;
    uint64_t tpidr = 0, tpidrro = 0, cnt = 0;
};

// The end of a run, on either executor
struct Outcome {
    State s;
    std::vector<uint8_t> image;
    uint32_t reason = 0, arg = 0;  // enum dm_exit_reason (include/nxaot/guest_state.h)
    std::string failure;           // a run that did not end normally (a fault, an access outside the image, no exit), empty otherwise
};

// Reads one .tsv file or every .tsv file of a folder (sorted by name). False with *error set on a malformed line
bool LoadVectors(const std::string& path, std::vector<Vector>& out, std::string* error);

// The initial state and image of a vector: the seed's values, the code words, then the overrides
void SeedState(const Vector& v, State& s, std::vector<uint8_t>& image);

bool InCode(const Vector& v, uint64_t pc);

}  // namespace nxaot::vec
