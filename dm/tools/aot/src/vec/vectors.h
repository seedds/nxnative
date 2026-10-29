// Test vectors of nxaot_vec (tests/vectors/*.tsv, written by tests/gen_vectors.py) and the machine state both executors start from.
//
// A vector line: id, class, text, code, seed, fpcr, regs, mem, separated by tabs.
//   code  the instruction words, hex, comma-separated; they run from kCodeBase
//   seed  64-bit hex: fills x0..x30, sp, nzcv, tpidr_el0, tpidrro_el0, the counter and the 64 KiB memory image, and (from a stream of its own) the
//         vector registers v0..v31 (SeedState)
//   fpcr  hex: the FPCR both executors start with; the lifter specialises the code for it (aot-recompiler.md section 7.1)
//   regs  name=hex overrides, comma-separated (x0..x30, sp, nzcv, fpsr, tpidr, tpidrro, cnt; v0..v31 with up to 32 hex digits, the high lane first),
//         and vk=<kind>, the lane kind the seed fills v0..v31 with (FillKind), or -
//   mem   offset:hexbytes overrides of the image (offset from kGuestBase, hex), comma-separated, or -
// The generator relies on these conventions; tests/gen_vectors.py repeats the constants.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace nxaot::vec {

constexpr uint64_t kGuestBase = 0x10000000;  // where the memory image is (one-to-one guest addresses: the lifted code uses it as the host address)
constexpr uint64_t kImageSize = 0x10000;     // 64 KiB
constexpr uint64_t kCodeOffset = 0x8000;     // the code words are also in the image, so pc-relative loads read them
constexpr uint64_t kCodeBase = kGuestBase + kCodeOffset;

using V128 = std::array<uint64_t, 2>;  // lane 0 (bits 63:0) first, as dynarmic's A64::Vector

// What the seed fills v0..v31 with (vk=): random bits, or lanes of one element kind drawn from that kind's special values (design section 12:
// zeros, infinities, signalling and quiet NaNs with payloads, denormals, extreme normals, rounding ties, integer boundaries) and random values
enum class FillKind { Random, F16, F32, F64, I8, I16, I32, I64 };

struct Vector {
    std::string id, cls, text;
    std::vector<uint32_t> code;
    uint64_t seed = 0;
    uint32_t fpcr = 0;
    std::vector<std::pair<std::string, uint64_t>> regs;
    std::vector<std::pair<unsigned, V128>> vregs;
    FillKind fill = FillKind::Random;
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> mem;
    std::string where;  // file:line
};

struct State {
    std::array<uint64_t, 31> x{};
    uint64_t sp = 0, pc = 0;
    uint32_t nzcv = 0, fpcr = 0, fpsr = 0;
    uint64_t tpidr = 0, tpidrro = 0, cnt = 0;
    std::array<V128, 32> v{};
};

// The end of a run, on either executor
struct Outcome {
    State s;
    std::vector<uint8_t> image;
    uint32_t reason = 0, arg = 0;  // enum dm_exit_reason (include/nxaot/guest_state.h)
    std::string failure;           // a run that did not end normally (a fault, an access outside the image, no exit), empty otherwise
    std::vector<int> issues;       // the lifted run: lift::DynarmicIssue values of the continuations it ran (Lifted::Run)
};

// Reads vectors from .tsv or .tsv.gz files, from every such file of a folder (sorted by name), or from standard input ("-"), one at a time, so that
// a sweep of millions of generated vectors (gen_vectors.py --count) streams through the harness
class VectorReader {
public:
    explicit VectorReader(std::vector<std::string> paths);
    ~VectorReader();
    // False at the end, or with *error set on a malformed line or an unreadable path
    bool Next(Vector& out, std::string* error);

private:
    bool OpenNext(std::string* error);
    std::vector<std::string> files;
    size_t next_file = 0;
    void* in = nullptr;  // a zlib gzFile
    std::string current;
    size_t line = 0;
};

// The initial state and image of a vector: the seed's values, the code words, then the overrides
void SeedState(const Vector& v, State& s, std::vector<uint8_t>& image);

bool InCode(const Vector& v, uint64_t pc);

}  // namespace nxaot::vec
