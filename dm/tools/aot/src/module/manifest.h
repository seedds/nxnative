// The manifest of a behaviour image (docs/design/aot-recompiler.md section 1.2): TSV, one row kind per line, beside <name>.image.bin. patch_nso.py
// --aot will write it for game modules (a later task); `nxaot image` writes one with layout and data rows for an NRO (src/module/image.h), so that
// the tool has one input form. Numbers are hex, written with 0x as the rewriter's tables are (patch_nso.py, sites.tsv), read with or without it;
// the observation number is decimal, as OBS_* in dm/host/dm.h.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nxaot::module {

struct Manifest {
    int version = 1;
    std::string module;    // "main", "sdk", "nro-<hash>", ...
    std::string build_id;  // hex, as fixups.tsv has it
    std::string sha256;    // of <name>.image.bin, 64 hex digits

    struct Layout {
        uint64_t text_end = 0, ro_off = 0, data_off = 0, bss = 0, image_size = 0;
    };
    std::optional<Layout> layout;

    struct Aux {  // the frame-step stub region and its state page (FRAME_STEP)
        uint64_t offset = 0, length = 0, fps_state = 0;
    };
    std::optional<Aux> aux;

    struct Range {
        uint64_t lo = 0, hi = 0;
    };
    std::vector<Range> data;  // data blocks in text, [lo, hi)

    struct Obs {  // an observation gate before the instruction at offset
        uint64_t offset = 0;
        uint32_t number = 0;
        uint64_t parameter = 0;
        uint32_t word = 0;  // the original instruction
    };
    std::vector<Obs> obs;

    struct Spin {  // a spin-wait region: its head and the back-edges that get the spin gate
        uint64_t head = 0;
        std::vector<uint64_t> back_edges;
    };
    std::vector<Spin> spin;

    struct Cut {  // for the log only: already applied to the image
        uint64_t offset = 0;
        std::string old_hex, new_hex, reason;
    };
    std::vector<Cut> cuts;

    struct OracleT {  // the rewriter's offset tables (sites.tsv T rows)
        uint64_t br = 0;
        std::vector<uint64_t> targets;
    };
    std::vector<OracleT> oracle_t;

    struct OracleJ {  // its byte and halfword tables (sites.tsv J rows)
        uint64_t br = 0, base = 0;
        uint32_t width = 0;
    };
    std::vector<OracleJ> oracle_j;

    std::vector<Range> oracle_w;  // its exclusive windows (sites.tsv W rows)
};

// Parses a manifest. nullopt with *error = "line N: ..." on the first malformed line
std::optional<Manifest> ParseManifest(const std::string& text, std::string* error);

// The canonical text of a manifest: the header, then layout, aux, data, obs, spin, cut, oracle-T, oracle-J, oracle-W rows in their order
std::string WriteManifest(const Manifest& m);

}  // namespace nxaot::module
