# dynarmic, vendored

A plain copy (no git submodule) of the part of dynarmic that `nxaot` builds: the A64 frontend and the IR (the source of the recompiler's instruction semantics,
[aot-recompiler.md](../../../docs/design/aot-recompiler.md) section 9.1) and the x86-64 backend (the reference executor of the vector harness, section 12, and
the future JIT fallback, section 11), with the externals its CMake build asks for on an x86-64 host with its tests off. Nothing in the copy is changed.

| | |
|---|---|
| Upstream | <https://github.com/azahar-emu/dynarmic> (the maintained fork; the original repository returned 404 on 2026-10-28) |
| Commit | `a46601580d5512d324104f985b5f0209dc980ddc` ("Delete redundant add_library alias") |
| Commit date | 2026-09-26 |
| Licence | 0BSD ([LICENSE.txt](LICENSE.txt)) |
| Used by | [dm/tools/aot/CMakeLists.txt](../../tools/aot/CMakeLists.txt) (`add_subdirectory`, tests and docs off) |

## What is copied

| Path | From | Licence | Why |
|---|---|---|---|
| `LICENSE.txt`, `CMakeLists.txt`, `CMakeModules/`, `src/` | dynarmic itself | 0BSD | the library (frontends, IR, backends) and its build |
| `externals/CMakeLists.txt` | dynarmic | 0BSD | adds the externals below |
| `externals/fmt/` (`CMakeLists.txt`, `LICENSE`, `README.md`, `ChangeLog.md` (its CMake lists both as sources), `include/`, `src/`, `support/cmake/`) | fmtlib/fmt `7b273fbb541c7c9058800bc613f4291274b2d26e` (11.1.4-12) | MIT | formatting, used by dynarmic and mcl |
| `externals/mcl/` (`CMakeLists.txt`, `LICENSE`, `CMakeModules/`, `include/`, `src/`) | azahar-emu/mcl `5fc4beaf331037649b10625736b41365defb4f50` (0.1.14) | MIT | bit utilities, assertions |
| `externals/robin-map/` (`CMakeLists.txt`, `LICENSE`, `cmake/`, `include/`, `tsl-robin-map.natvis`) | Tessil/robin-map `054ec5ad67440fcd65e0497e5a27ef31f53fcc7f` (1.3.0-6) | MIT | hash maps |
| `externals/xbyak/` (`CMakeLists.txt`, `COPYRIGHT`, `cmake/`, `xbyak/`) | herumi/xbyak `8c0098b69f95d02dea47efcdd42ab5aa2d03c72d` (3.71-1656) | BSD-3-Clause | the x86-64 assembler of the x86-64 backend |
| `externals/zycore/` (`CMakeLists.txt`, `LICENSE`, `cmake/`, `include/`, `resources/`, `src/`) | zyantific/zycore-c `0b2432ced0884fd152b471d97ecf0258ff4d859f` (1.5.2) | MIT | Zydis's support library |
| `externals/zydis/` (`CMakeLists.txt`, `LICENSE`, `cmake/`, `include/`, `resources/`, `src/`) | zyantific/zydis `bffbb610cfea643b98e87658b9058382f7522807` (4.0.0-64) | MIT | the x86-64 disassembler of the backend's debugging output |

Not copied: dynarmic's `tests/`, `docs/` and `README.md`; the externals `catch` (Catch2, tests only), `oaknut` (ARM64 hosts and tests only) and `biscuit` (RISC-V
hosts only); and every test suite, example, documentation and tool folder of the externals above. Boost, which dynarmic also needs (`boost/icl`, `boost/variant`,
headers only), is not part of this copy: [dm/tools/aot/README.md](../../tools/aot/README.md) says where it comes from.

## The command that made the copy

Run from the repository root (needs git and network access); it replaces everything in this folder except this file:

```sh
set -eu
up=$(mktemp -d)
git clone https://github.com/azahar-emu/dynarmic.git "$up/dynarmic"
git -C "$up/dynarmic" checkout a46601580d5512d324104f985b5f0209dc980ddc
git -C "$up/dynarmic" submodule update --init
dst=dm/third_party/dynarmic
find "$dst" -mindepth 1 -maxdepth 1 ! -name VENDOR.md -exec rm -rf {} +
take() { for p in "$@"; do mkdir -p "$dst/$(dirname "$p")"; cp -R "$up/dynarmic/$p" "$dst/$p"; done; }
take LICENSE.txt CMakeLists.txt CMakeModules src externals/CMakeLists.txt
take externals/fmt/CMakeLists.txt externals/fmt/LICENSE externals/fmt/README.md externals/fmt/ChangeLog.md externals/fmt/include externals/fmt/src externals/fmt/support/cmake
take externals/mcl/CMakeLists.txt externals/mcl/LICENSE externals/mcl/CMakeModules externals/mcl/include externals/mcl/src
take externals/robin-map/CMakeLists.txt externals/robin-map/LICENSE externals/robin-map/cmake externals/robin-map/include externals/robin-map/tsl-robin-map.natvis
take externals/xbyak/CMakeLists.txt externals/xbyak/COPYRIGHT externals/xbyak/cmake externals/xbyak/xbyak
take externals/zycore/CMakeLists.txt externals/zycore/LICENSE externals/zycore/cmake externals/zycore/include externals/zycore/resources externals/zycore/src
take externals/zydis/CMakeLists.txt externals/zydis/LICENSE externals/zydis/cmake externals/zydis/include externals/zydis/resources externals/zydis/src
rm -rf "$up"
```

To move to another upstream commit, change the commit in the command and in the table above, run it, and check the vector harness and the census test
([dm/tools/aot/README.md](../../tools/aot/README.md)): the IR opcodes the lifter lowers come from `src/dynarmic/ir/opcodes.inc`, and a renamed or added opcode
shows up there first.
