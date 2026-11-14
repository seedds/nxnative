// The corpus's run time (corpus/rt/rt.c) for a native x86-64 build of a corpus program or of gen_bench.py's benchmark, with a timing harness:
// the same C compiled for the host, the time nxaot's recompiled code is measured against (tests/native_ratio.py; README.md, "Against native").
// The program is compiled with -Dmain=nxaot_corpus_main and linked with this file (-rdynamic, -Wl,-z,now):
//   <program> [--repeat N] [--quiet] [--set <symbol>=<value>]...
// It runs the program's main N times in one process, as nxaot_host --repeat does: the first run prints the program's lines, every later run
// counts them only and starts from the image's writable data as it was before the first (the executable's writable segment outside RELRO,
// restored from a copy, as nxaot_host maps a fresh image for each run); --set writes a 64-bit value at an exported symbol before each run
// (int_ops_rounds). Standard error gets the line measure_packs.py and native_ratio.py read:
//   native: <program>: exited <status> after <s> s (fastest of <N>: <s> s), <lines> lines
// The time of a run is main's, on CLOCK_MONOTONIC; the recompiled run's (nxaot_host) is _start's to svcExitProcess, which adds the corpus's
// crt0 (a relocation check, the TLS pointer, the init and fini arrays: nothing measurable but on the shortest programs).
#define _GNU_SOURCE

// rt.c's DT_INIT and DT_FINI functions are _init and _fini, which the host's crti.o defines: renamed here, and run as the first constructor and
// the last destructor, as the corpus's crt0 runs DT_INIT before the init array and DT_FINI after the fini array. rt.c comes before the system
// headers: elf.h (link.h) defines its DT_ and R_AARCH64_ names as macros
#define _init nxaot_native_dt_init
#define _fini nxaot_native_dt_fini
#include "../corpus/rt/rt.c"
#undef _init
#undef _fini

#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

__attribute__((constructor(101))) static void native_dt_init(void) {
    nxaot_native_dt_init();
}

__attribute__((destructor(101))) static void native_dt_fini(void) {
    nxaot_native_dt_fini();
}

int nxaot_corpus_main(void);

// The harness's own state is thread-local: the main thread's TLS block lies outside the executable's writable segment, which every run after
// the first restores
static __thread int native_quiet;
static __thread unsigned long long native_lines;

void rt_svc_output_debug_string(const char *s, size_t n) {
    ++native_lines;
    if (!native_quiet) {
        fwrite(s, 1, n, stdout);
        fputc('\n', stdout);
    }
}

void rt_svc_exit_process(void) {
    fflush(stdout);
    exit(0);
}

// The executable's writable data: its PF_W load segment, less the part PT_GNU_RELRO makes read-only after relocation
static __thread uintptr_t native_rw_start, native_rw_end;

static int native_find_rw(struct dl_phdr_info *info, size_t size, void *arg) {
    (void)size;
    (void)arg;
    uintptr_t start = 0, end = 0, relro_end = 0;
    for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type == PT_LOAD && (p->p_flags & PF_W)) {
            start = info->dlpi_addr + p->p_vaddr;
            end = start + p->p_memsz;
        } else if (p->p_type == PT_GNU_RELRO) {
            relro_end = info->dlpi_addr + p->p_vaddr + p->p_memsz;
        }
    }
    if (relro_end > start && relro_end <= end) {
        start = relro_end;
    }
    native_rw_start = start;
    native_rw_end = end;
    return 1;  // the first object is the executable
}

static double native_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
    unsigned repeat = 1;
    int quiet = 0;
    const char *sets[64];
    unsigned nsets = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--repeat") && i + 1 < argc) {
            repeat = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--quiet")) {
            quiet = 1;
        } else if (!strcmp(argv[i], "--set") && i + 1 < argc && nsets < 64) {
            sets[nsets++] = argv[++i];
        } else {
            fprintf(stderr, "usage: %s [--repeat N] [--quiet] [--set <symbol>=<value>]...\n", argv[0]);
            return 2;
        }
    }
    if (!repeat) {
        return 2;
    }
    dl_iterate_phdr(native_find_rw, NULL);
    const size_t rw_size = native_rw_end - native_rw_start;
    unsigned char *copy = rw_size ? malloc(rw_size) : NULL;
    if (rw_size && !copy) {
        return 2;
    }
    if (copy) {
        memcpy(copy, (const void *)native_rw_start, rw_size);
    }
    int first_status = 0;
    unsigned long long first_lines = 0;
    double first = 0, best = 0;
    for (unsigned r = 0; r < repeat; ++r) {
        if (r) {
            memcpy((void *)native_rw_start, copy, rw_size);
        }
        for (unsigned k = 0; k < nsets; ++k) {
            const char *eq = strchr(sets[k], '=');
            if (!eq || (size_t)(eq - sets[k]) >= 128) {
                fprintf(stderr, "native: --set takes <symbol>=<value>\n");
                return 2;
            }
            char name[128];
            memcpy(name, sets[k], (size_t)(eq - sets[k]));
            name[eq - sets[k]] = 0;
            uint64_t *p = dlsym(RTLD_DEFAULT, name);
            if (!p) {
                fprintf(stderr, "native: no exported symbol %s\n", name);
                return 2;
            }
            *p = strtoull(eq + 1, NULL, 0);
        }
        native_quiet = quiet || r > 0;
        native_lines = 0;
        const double t0 = native_now();
        const int status = nxaot_corpus_main();
        const double t = native_now() - t0;
        if (!r) {
            first_status = status;
            first_lines = native_lines;
            first = best = t;
            fflush(stdout);
        } else if (t < best) {
            best = t;
        }
    }
    fflush(stdout);
    fprintf(stderr, "native: %s: exited %d after %.6f s (fastest of %u: %.9f s), %llu lines\n", argv[0], first_status, first, repeat, best,
            first_lines);
    return first_status;
}
