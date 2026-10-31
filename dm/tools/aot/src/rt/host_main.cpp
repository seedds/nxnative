// nxaot_host: runs a module recompiled by `nxaot build` natively (src/rt/host.h): the image mapped at its one-to-one address in the arena, the
// pack's shared object loaded with dlopen, _start run until svcExitProcess, the corpus's output lines printed as nxaot_run prints them.
//   nxaot_host <library.so> <module.nro | name.aot.tsv> [--relocate] [--set <symbol>=<value | @offset>]... [--quiet] [--repeat N] [--lookup]
//              [--jit-all | --jit-none] [--jit-one-block] [--no-jit-log] [--pause-test N] [--pause-seed S] [--pause-interval-us U]
// --relocate applies the module's relocations in the host (as dm/host's loader does) before the run; --set writes a 64-bit value at a symbol
// (@offset: the module's base plus offset) after that; --quiet counts the lines instead of printing them; --repeat runs N times (a fresh image
// each time, the lines of the first run only) and reports the fastest; --lookup prints the module offsets of the lookup table and exits.
// The JIT fallback (design section 11): by default compiled entries run natively and the rest in the JIT, each JIT entry logged once per address
// as a [RED] line on standard error (--no-jit-log: counted only); --jit-all runs everything in the JIT (no compiled entry is taken); --jit-none
// refuses every case the JIT would take (the run stops with status 3); --jit-one-block makes the JIT return after every block and the host check
// the next pc (a measurement of design 11's alternative). --pause-test N (design section 6.2): another host thread pauses every guest thread N
// times at random intervals (seed S, up to U microseconds, 100 by default) while more than one runs, checks their state blocks and resumes them.
// Exit status: the run's (rt::Status: 0 svcExitProcess, 1 trap, 2 usage or a library that does not match the image, 3 the JIT fallback is
// needed and refused, 4 fault, 5 an SVC it does not answer, 6 returned where it must not, 9 the pause test failed). The [RED] line of a failed
// run goes to standard output after the lines; a summary (status, time, lines, misses, the JIT's counts, the pause test's) to standard error.
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
        } else if (a == "--jit-all") {
            options.jit = nxaot::rt::JitMode::All;
        } else if (a == "--jit-none") {
            options.jit = nxaot::rt::JitMode::None;
        } else if (a == "--jit-one-block") {
            options.jit_one_block = true;
        } else if (a == "--no-jit-log") {
            options.jit_log = false;
        } else if (a == "--pause-test" && i + 1 < argc) {
            options.pause_test = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 0));
        } else if (a == "--pause-seed" && i + 1 < argc) {
            options.pause_seed = std::strtoull(argv[++i], nullptr, 0);
        } else if (a == "--pause-interval-us" && i + 1 < argc) {
            options.pause_interval_us = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 0));
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
                             "[--repeat N] [--lookup]\n"
                             "                  [--jit-all | --jit-none] [--jit-one-block] [--no-jit-log] [--pause-test N] [--pause-seed S]\n"
                             "                  [--pause-interval-us U]\n");
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
            options.jit_log = false;
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
    const char* mode = options.jit == nxaot::rt::JitMode::All ? "jit-all" : options.jit == nxaot::rt::JitMode::None ? "jit-none" : "mixed";
    std::fprintf(stderr, "nxaot_host: %s: %s after %.6f s (fastest of %u: %.6f s), %llu lines, %llu dispatcher misses, %zu line-table entries, %llu threads; %s\n",
                 path.c_str(), nxaot::rt::StatusName(first.status), first.seconds, repeat, best.seconds,
                 static_cast<unsigned long long>(first.lines), static_cast<unsigned long long>(first.misses), host->LineEntries(),
                 static_cast<unsigned long long>(first.threads), host->Desc().tool);
    std::fprintf(stderr, "nxaot_host: jit (%s): %llu runs from %llu addresses, %llu Run() calls, %llu stepped stops, %llu native calls, %.6f s\n", mode,
                 static_cast<unsigned long long>(first.jit_runs), static_cast<unsigned long long>(first.jit_addresses),
                 static_cast<unsigned long long>(first.jit_run_calls), static_cast<unsigned long long>(first.jit_steps),
                 static_cast<unsigned long long>(first.jit_native_calls), first.jit_seconds);
    if (options.pause_test) {
        std::fprintf(stderr, "nxaot_host: pause test: %llu pauses (%llu with more than one thread running), thread stops: %llu at polls, %llu in SVCs, "
                             "%llu in the JIT; %llu register-invariant checks, %llu inconsistent; longest stop %.1f us\n",
                     static_cast<unsigned long long>(first.pauses), static_cast<unsigned long long>(first.pauses_threaded),
                     static_cast<unsigned long long>(first.stops_poll), static_cast<unsigned long long>(first.stops_svc),
                     static_cast<unsigned long long>(first.stops_jit), static_cast<unsigned long long>(first.invariant_checks),
                     static_cast<unsigned long long>(first.inconsistent), first.pause_max_us);
    }
    return first.status;
}
