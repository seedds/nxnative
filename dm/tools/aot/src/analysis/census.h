// The census of phase 0 (docs/design/aot-recompiler.md section 14) over a raw blob of ARM64 code: instruction classes, the FP / SIMD mix, LSE,
// cryptographic and CRC instructions, system registers read and written, dc zva, hints, exclusive windows and whether each is ABA-sensitive by the
// rule of section 6.5, indirect branches, SP written from memory, brk and udf. Words are classified by dynarmic's A64 decoder (its matcher names),
// by the Arm ARM's top-level encoding table, and by masks of the Arm ARM for the encodings dynarmic does not decode (LSE atomics, msr immediate,
// hvc / smc). Data flow (windows, SP) runs on dynarmic's IR of each instruction.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace nxaot::analysis {

using Counts = std::map<std::string, uint64_t>;

// base: the guest address of the first byte (pc-relative forms and the reports use it). size need not be a multiple of 4; a tail is ignored
Counts Census(const uint8_t* data, size_t size, uint64_t base);

}  // namespace nxaot::analysis
