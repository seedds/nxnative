# Boost, the subset dynarmic needs

The headers of Boost 1.83.0 that dynarmic ([../dynarmic](../dynarmic/VENDOR.md)) and the JIT glue include (`boost/icl/interval_map.hpp`,
`boost/icl/interval_set.hpp`, `boost/variant.hpp`, `boost/variant/get.hpp`, headers only) and every Boost header those include, with all of
`boost/config/`. Unchanged copies of the release's files, at the same paths under `boost/`.

| | |
|---|---|
| Upstream | Boost 1.83.0, <https://archives.boost.io/release/1.83.0/source/boost_1_83_0.tar.bz2> |
| Archive SHA-256 | `6478edfe2f3305127cffe8caf73ea0176c53769f4bf1585be237eb30798c3b8e` (the one boost.org publishes) |
| Version | 1.83.0, the version of Ubuntu 24.04's `libboost-dev`, which the Linux images use |
| Files | 869 headers, 4,527,070 bytes, and `LICENSE_1_0.txt` |
| Digest | `33b58543463373a3ad7e855d5ef8199c6090559f80d0bded797dc7fc979d53b1`: `find boost -type f \| LC_ALL=C sort \| xargs sha256sum \| sha256sum` in this folder |
| Licence | Boost Software License 1.0 ([LICENSE_1_0.txt](LICENSE_1_0.txt)): no notice is needed in object code |
| Used by | [dm/host/CMakeLists.txt](../../host/CMakeLists.txt) (`NXHOST_JIT`), where CMake finds no Boost of its own: the Windows cross-build (the llvm-mingw image has none) |

Where CMake finds Boost (the Linux images' `libboost-dev`), the host takes that one, and `nxaot` ([dm/tools/aot](../../tools/aot/README.md#boost)) always does.

## Why a subset, and why this one

dynarmic includes four Boost headers; the whole release is 123 MB to download and 178 MB of headers. The other ways measured on 2026-11-14:

| | Download at each configure | In the repository | Notes |
|---|---|---|---|
| This subset | none | 869 files, 4.5 MB (0.56 MB gzip-compressed) | offline; the same bytes everywhere |
| The release archive, `FetchContent` with its hash | 123 MB (`.tar.bz2`; 47 MB for GitHub's `-b2-nodocs.tar.xz`), 95 s here | none | 178 MB of headers in the build folder, which is in memory (tmpfs) on the development machine |
| The 21 library repositories at tag `boost-1.83.0` | 8.1 MB in 21 archives, 9.5 s here | none | 21 hashes to keep; tests and documentation come with each |
| `bcp`'s subset | none | 21 MB | follows every `#include` of every configuration (preprocessor 9.6 MB, mpl 4.7 MB) |

The headers a compiler opens for those four are the same for clang 18 with libstdc++ on Linux (795 files) and clang 23 with libc++ for
`x86_64-w64-mingw32` (794) but for `boost/config`'s platform and standard library files; the whole of `boost/config/` (89 files, 0.4 MB) is here, so
another compiler or C++ library finds its configuration. The list is the one the host build opens: the same as `ninja -t deps` after building
dynarmic and the JIT glue in both images.

By library: preprocessor 1.6 MB (110 files), icl 0.58 MB (97), type_traits 0.50 MB (158), mpl 0.43 MB (224), config 0.43 MB (90), variant 0.19 MB (32),
move 0.15 MB, mp11 0.14 MB, range 0.11 MB, iterator 0.09 MB, the rest under 0.06 MB each.

## The command that made the copy

Run from the repository root (needs network access, Docker and the images `nxnative-build` and `mstorsjo/llvm-mingw:20261020` of
[docker/README.md](../../../docker/README.md)); it replaces everything in this folder except this file. If dynarmic includes other Boost headers after
an update, add them to the `printf` line:

```sh
set -eu
up=$(mktemp -d)
curl -sL https://archives.boost.io/release/1.83.0/source/boost_1_83_0.tar.bz2 -o "$up/boost.tar.bz2"
echo "6478edfe2f3305127cffe8caf73ea0176c53769f4bf1585be237eb30798c3b8e  $up/boost.tar.bz2" | sha256sum -c -
tar xjf "$up/boost.tar.bz2" -C "$up" boost_1_83_0/boost boost_1_83_0/LICENSE_1_0.txt
printf '#include <boost/%s>\n' icl/interval_map.hpp icl/interval_set.hpp variant.hpp variant/get.hpp > "$up/tu.cpp"
for run in "nxnative-build c++" "mstorsjo/llvm-mingw:20261020 x86_64-w64-mingw32-clang++"; do
    set -- $run
    docker run --rm -v "$up:/up:ro" "$1" "$2" -std=c++20 -M -isystem /up/boost_1_83_0 /up/tu.cpp
done | tr ' \\' '\n\n' | sed -n 's|^/up/boost_1_83_0/||p' > "$up/list"
(cd "$up/boost_1_83_0" && find boost/config -type f) >> "$up/list"
dst=dm/third_party/boost
find "$dst" -mindepth 1 -maxdepth 1 ! -name VENDOR.md -exec rm -rf {} +
LC_ALL=C sort -u "$up/list" | tar cf - -C "$up/boost_1_83_0" -T - | tar xf - -C "$dst"
cp "$up/boost_1_83_0/LICENSE_1_0.txt" "$dst/"
rm -rf "$up"
```

Then check the digest above (another one means other files: record it) and build the Windows cross-build ([docker/README.md](../../../docker/README.md)),
whose configure step names the subset (`NXHOST_JIT on: the JIT fallback (dynarmic) with Boost 1.83.0 (the subset in dm/third_party/boost)`).
