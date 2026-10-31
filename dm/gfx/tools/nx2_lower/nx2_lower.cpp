// Switch 2 (SM86) shader code -> Maxwell code, through Eglantine's lowering (lower_to_maxwell.h, program_capture.h, GPL-3.0-or-later, taken unchanged: dm/gfx/tools/nx2_lower/README.md).
// Usage: nx2_lower <in> <out>
//   in:  records of [u32 id][u32 header bytes][u32 code bytes][code]
//   out: records of [u32 id][u32 lowered bytes, 0 = rejected][u32 reason bytes][u32 constant buffer of the texture handles, ~0 = none named][u32 map entries][u32 jump tables][u32 computed branches]
//        [lowered][reason][map: u32 lowered instruction of each native one][jump tables: u32 c1 byte offset]
// The capture and lowering are what Eden's GenericEnvironment::InitializeNativeCode does on the program it reads from GPU memory
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include "shader_recompiler/frontend/ampere/program_capture.h"

int main(int argc, char **argv) {
    using namespace Shader::Ampere;
    if (argc != 3) { fprintf(stderr, "usage: nx2_lower <in> <out>\n"); return 2; }
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    if (!in || !out) { perror("open"); return 2; }
    u32 h[3];
    while (fread(h, 4, 3, in) == 3) {
        std::vector<u8> d(h[2]);
        if (h[2] && fread(d.data(), 1, h[2], in) != h[2]) { fprintf(stderr, "short record\n"); return 2; }
        std::vector<Instruction> code;
        ProgramCaptureState capture;
        std::string_view why;
        for (u32 i = 0; h[1] + (u64{i} + 1) * sizeof(Instruction) <= h[2] && i < ProgramCaptureState::MaximumInstructions; i++) {
            Instruction ins;
            memcpy(&ins, d.data() + h[1] + i * sizeof(Instruction), sizeof ins);
            code.push_back(ins);
            auto r = capture.Observe(ins, i);
            if (r == ProgramCaptureState::Result::InvalidTarget) { why = "branch outside the capture"; break; }
            if (r == ProgramCaptureState::Result::Complete) break;
        }
        std::vector<u64> lowered;
        u32 texture_buffer = ~0U, branches = 0;
        std::vector<u32> map, tables;
        if (why.empty()) {
            auto r = LowerToMaxwell(code);
            if (r) { lowered = std::move(r.instructions); texture_buffer = r.required_texture_buffer; map = std::move(r.source_instruction_offsets); }
            else why = r.error;
        }
        // A computed branch reads its target from c1: the LDC writing its register (Maxwell's encoding, c1, no address mode) says where that table starts
        for (size_t i = 0; i < lowered.size(); i++) {
            if (lowered[i] >> 52 != 0xe25) continue;
            branches++;
            const u64 reg = lowered[i] >> 8 & 0xff;
            for (size_t j = i; j-- > 0;) {
                if ((lowered[j] & 0xff) != reg) continue;
                if (lowered[j] >> 51 == 0x1df2 && (lowered[j] >> 36 & 0x1f) == 1 && !(lowered[j] >> 44 & 3)) tables.push_back(static_cast<u32>(lowered[j] >> 20 & 0xffff));
                break;
            }
        }
        u32 o[7] = {h[0], static_cast<u32>(lowered.size() * 8), static_cast<u32>(why.size()), texture_buffer, static_cast<u32>(map.size()), static_cast<u32>(tables.size()), branches};
        fwrite(o, 4, 7, out);
        fwrite(lowered.data(), 8, lowered.size(), out);
        fwrite(why.data(), 1, why.size(), out);
        fwrite(map.data(), 4, map.size(), out);
        fwrite(tables.data(), 4, tables.size(), out);
    }
    fclose(out);
    return 0;
}
