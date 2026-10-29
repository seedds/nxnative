// nxaot_host: runs a module recompiled by `nxaot build` natively (src/rt/host.h): the image mapped at its one-to-one address in the arena, the
// pack's shared object loaded with dlopen, _start run until svcExitProcess, the corpus's output lines printed as nxaot_run prints them.
//   nxaot_host <library.so> <module.nro | name.aot.tsv> [--relocate] [--set <symbol>=<value | @offset>]... [--quiet] [--repeat N] [--lookup]
// --relocate applies the module's relocations in the host (as dm/host's loader does) before the run; --set writes a 64-bit value at a symbol
// (@offset: the module's base plus offset) after that; --quiet counts the lines instead of printing them; --repeat runs N times (a fresh image
// each time, the lines of the first run only) and reports the fastest; --lookup prints the module offsets of the lookup table and exits.
// Exit status: the run's (rt::Status: 0 svcExitProcess, 1 trap, 2 usage or a library that does not match the image, 3 the JIT fallback is
// needed, 4 fault, 5 an SVC it does not answer, 6 returned where it must not). The [RED] line of a failed run goes to standard output after the
// lines, a summary to standard error.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "rt/host.h"

int main(int argc, char** argv) {
    std::string library, path;
    nxaot::rt::Options options;
    unsigned repeat = 1;
    bool lookup = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--relocate") {
            options.relocate = true;
        } else if (a == "--quiet") {
            options.quiet = true;
        } else if (a == "--lookup") {
            lookup = true;
        } else if (a == "--repeat" && i + 1 < argc) {
            repeat = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 0));
        } else if (a == "--set" && i + 1 < argc) {
            const std::string kv = argv[++i];
            const auto eq = kv.find('=');
            if (eq == std::string::npos) {
                std::fprintf(stderr, "nxaot_host: --set takes <symbol>=<value>\n");
                return 2;
            }
            options.sets.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
        } else if (library.empty() && !a.empty() && a[0] != '-') {
            library = a;
        } else if (path.empty() && !a.empty() && a[0] != '-') {
            path = a;
        } else {
            library.clear();
            break;
        }
    }
    if (library.empty() || path.empty() || repeat == 0) {
        std::fprintf(stderr, "usage: nxaot_host <library.so> <module.nro | name.aot.tsv> [--relocate] [--set <symbol>=<value | @offset>]... [--quiet] "
                             "[--repeat N] [--lookup]\n");
        return 2;
    }
    std::string error;
    const auto image = nxaot::module::LoadModule(path, &error);
    if (!image) {
        std::fprintf(stderr, "nxaot_host: %s\n", error.c_str());
        return 2;
    }
    auto host = nxaot::rt::Host::Create(library, *image, options, &error);
    if (!host) {
        std::printf("[RED] nxaot_host: %s\n", error.c_str());
        return 2;
    }
    if (lookup) {
        for (const uint64_t off : host->LookupOffsets()) {
            std::printf("%#llx\n", static_cast<unsigned long long>(off));
        }
        return 0;
    }
    nxaot::rt::Result first, best;
    for (unsigned i = 0; i < repeat; ++i) {
        if (i == 1) {
            options.quiet = true;
            host = nullptr;  // the arena is one reservation per process: a new host for the quiet runs
            host = nxaot::rt::Host::Create(library, *image, options, &error);
            if (!host) {
                std::printf("[RED] nxaot_host: %s\n", error.c_str());
                return 2;
            }
        }
        const nxaot::rt::Result r = host->RunStart();
        if (i == 0) {
            first = r;
            best = r;
        } else if (r.seconds < best.seconds) {
            best = r;
        }
        if (r.status != nxaot::rt::kExited) {
            break;
        }
    }
    std::fflush(stdout);
    if (first.status != nxaot::rt::kExited) {
        std::printf("%s\n", first.message.c_str());
    }
    std::fprintf(stderr, "nxaot_host: %s: %s after %.6f s (fastest of %u: %.6f s), %llu lines, %llu dispatcher misses, %zu line-table entries; %s\n",
                 path.c_str(), nxaot::rt::StatusName(first.status), first.seconds, repeat, best.seconds,
                 static_cast<unsigned long long>(first.lines), static_cast<unsigned long long>(first.misses), host->LineEntries(),
                 host->Desc().tool);
    return first.status;
}
