// The hot indirect targets of design 12 (docs/design/aot-recompiler.md section 12; aot.h, "The hot indirect targets"; dm/tools/aot/README.md, "Hot indirect targets"): with
// DM_AOT_PROFILE=<folder>, what the inline caches of the packs' indirect calls and branches were bound to, written at exit beside the sample profile as
// <folder>/<module>.targets.tsv, which `nxaot build --profile <folder>/<module>.tsv` reads beside the profile: a site at tier H whose target dominates gets a guarded direct call.
// What a cache holds: one slot per indirect call or branch site of a compiled function (an internal global of its unit, dm/tools/aot/src/lift/lower_calls.cpp FnDispatch), a
// pointer to an immutable {guest, native} descriptor (aot_run.c desc_for), and nothing else: no count of hits. A hit is two loads and a compare in the pack's own code, which never
// reaches the host. What the host sees is the miss: nxaot_rt_dispatch(st, target, slot, site), with the site's module offset, each time the slot's descriptor is not the target's.
// So what is recorded is, per pack, site and target, how many times the dispatcher bound the site's cache to that target ("binds"): once per slot for a site that only ever
// reaches one target (the slot then hits for the rest of the run; two threads that miss at once both bind), and once more after each miss to another target for a site with
// several (the targets A, B, A, A give A 2, B 1). The counts are switches between targets, not calls: a site called a million times with A and once with B in the middle has A 2,
// B 1. A count of calls needs a counter on the hit path, an increment at every indirect call of every pack, so there is none (README.md, "Hot indirect targets", measured
// nothing for it). The misses that bind nothing of the caller's module are counted, not written: a target in another pack (bound, but its offset is another module's), one handed
// to another pack of another signature, a host function (the host-function page, the stub page), an address with no compiled entry (the JIT fallback: the misses file has it).
// Recording: dm_aot_target_note from nxaot_rt_dispatch, only with DM_AOT_PROFILE, into a small cache of the thread's own (TGT_CACHE rows, direct-mapped): a note that finds its
// row there adds one with no lock and no atomic read-modify-write; one that does not takes the lock once, to move the row it replaces into an open-addressing table of
// (pack, site, target, kind) under one lock, grown by doubling, and to take the row. Recording a profile is the launcher's default: one lock per miss was 6 ms of a level frame
// on Mario vs. Donkey Kong's Thread_0 (the game note, "where a frame goes"). The miss path runs on a guest thread, never in a signal handler (the sampler's handler does not
// touch this table); without DM_AOT_PROFILE a note returns after one relaxed load.
// The file, per module with rows this session: '#' lines naming the module, its build ID, its image's SHA-256 and its pack (as the profile and the misses file), then
// `0x<site> 0x<target> <binds>` per site and target, by site then target (module offsets). Merged across sessions of the same image, as the misses file is: a pack whose guard calls
// a hot target directly never binds that target again (the guard's hits skip the cache), so a file rewritten per session would lose the targets a guarded pack was built from, and
// the pack after it its guards; another image's file is replaced. Written at exit (atexit, registered at the first note), or earlier by dm_aot_targets_write (hle_lifecycle.c's
// graceful exit, beside dm_aot_profile_write, since _exit runs no atexit handler); once
#include "aot.h"
#include <errno.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <windows.h>
#endif

typedef struct { const dm_aot_pack *pack; uint64_t site, target, n; uint32_t kind; } tgt_slot;   // pack NULL: an empty slot
#define TGT_MAX_SLOTS ((size_t)1 << 24)   // 16 M slots of 40 bytes: far beyond a game's (site, target) pairs; past it, notes are dropped and counted
static tgt_slot *g_slots;
static size_t g_cap, g_used;              // slots (0 or a power of two), slots in use
static uint64_t g_dropped;                // notes no slot was found for (no memory, or the table at its limit)
static int g_registered;                  // the atexit writer
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_on = -1;             // DM_AOT_PROFILE is set and the sampler runs (dm_aot_profile_on): -1 until read
static _Atomic int g_written;

static int targets_on(void) {
    int v = atomic_load_explicit(&g_on, memory_order_relaxed);
    if (v < 0) {
        v = dm_aot_profile_on() ? 1 : 0;
        atomic_store_explicit(&g_on, v, memory_order_relaxed);
    }
    return v;
}

static size_t slot_hash(const dm_aot_pack *p, uint64_t site, uint64_t target, uint32_t kind, size_t mask) {
    uint64_t h = (uint64_t)(uintptr_t)p * 0x9E3779B97F4A7C15ull;
    h ^= site * 0xC2B2AE3D27D4EB4Full;
    h = (h ^ (h >> 29)) * 0x165667B19E3779F9ull;
    h ^= (target >> 2) * 0x9E3779B97F4A7C15ull ^ kind;
    h ^= h >> 32;
    return (size_t)h & mask;
}

static int grow(void) {   // under g_lock: twice the slots, the rows rehashed; -1 when it cannot
    const size_t cap = g_cap ? 2 * g_cap : 4096;
    if (cap > TGT_MAX_SLOTS) return -1;
    tgt_slot *s = calloc(cap, sizeof *s);
    if (!s) return -1;
    for (size_t i = 0; i < g_cap; i++) {
        const tgt_slot *o = &g_slots[i];
        if (!o->pack) continue;
        size_t h = slot_hash(o->pack, o->site, o->target, o->kind, cap - 1);
        while (s[h].pack) h = (h + 1) & (cap - 1);
        s[h] = *o;
    }
    free(g_slots);
    g_slots = s;
    g_cap = cap;
    return 0;
}

static void write_at_exit(void) { dm_aot_targets_write(); }

// count notes of (p, site, target, kind) into the table, under g_lock. At most half full; a table that cannot grow takes rows until one slot is left
static void table_add(const dm_aot_pack *p, uint64_t site, uint64_t target, uint32_t kind, uint64_t count) {
    if ((g_used + 1) * 2 > g_cap && grow() && g_used + 1 >= g_cap) {
        g_dropped += count;
        return;
    }
    for (size_t h = slot_hash(p, site, target, kind, g_cap - 1);; h = (h + 1) & (g_cap - 1)) {
        tgt_slot *s = &g_slots[h];
        if (!s->pack) { *s = (tgt_slot){p, site, target, count, kind}; g_used++; return; }
        if (s->pack == p && s->site == site && s->target == target && s->kind == kind) { s->n += count; return; }
    }
}

// A thread's cache (above): its rows change only under g_lock, so the writer, which holds it, reads whole rows; their counts the thread alone adds to, and the writer reads
// them relaxed. Every thread's cache lives to the process's end, on g_caches: the writer adds the exited threads' counts too
#define TGT_CACHE 256
typedef struct { const dm_aot_pack *pack; uint64_t site, target; uint32_t kind; _Atomic uint64_t n; } tgt_row;
typedef struct tgt_cache { struct tgt_cache *next; tgt_row row[TGT_CACHE]; } tgt_cache;
static _Thread_local tgt_cache *t_cache;
static tgt_cache *g_caches;

void dm_aot_target_note(const dm_aot_pack *caller, uint64_t site, uint64_t target, int kind) {
    if (!caller || !targets_on() || atomic_load_explicit(&g_written, memory_order_relaxed)) return;
    tgt_cache *c = t_cache;
    if (!c) {
        c = calloc(1, sizeof *c);
        pthread_mutex_lock(&g_lock);
        if (!g_registered) { g_registered = 1; atexit(write_at_exit); }
        if (c) { c->next = g_caches; g_caches = c; }
        else table_add(caller, site, target, (uint32_t)kind, 1);   // no memory for a cache: this note straight into the table, the next tries again
        pthread_mutex_unlock(&g_lock);
        if (!c) return;
        t_cache = c;
    }
    tgt_row *e = &c->row[slot_hash(caller, site, target, (uint32_t)kind, TGT_CACHE - 1)];
    if (e->pack == caller && e->site == site && e->target == target && e->kind == (uint32_t)kind) {
        atomic_store_explicit(&e->n, atomic_load_explicit(&e->n, memory_order_relaxed) + 1, memory_order_relaxed);   // this thread alone adds to it
        return;
    }
    pthread_mutex_lock(&g_lock);
    const uint64_t had = atomic_load_explicit(&e->n, memory_order_relaxed);
    if (e->pack && had) table_add(e->pack, e->site, e->target, e->kind, had);
    e->pack = caller; e->site = site; e->target = target; e->kind = (uint32_t)kind;
    atomic_store_explicit(&e->n, 1, memory_order_relaxed);
    pthread_mutex_unlock(&g_lock);
}

// ---------- The writer ----------
typedef struct { uint64_t site, target, n; int now; } trow;
static int trow_cmp(const void *a, const void *b) {
    const trow *x = a, *y = b;
    if (x->site != y->site) return x->site < y->site ? -1 : 1;
    return x->target < y->target ? -1 : x->target > y->target;
}
static int trow_add(trow **rows, size_t *n, size_t *cap, uint64_t site, uint64_t target, uint64_t count, int now) {
    if (*n == *cap) {
        const size_t c = *cap ? 2 * *cap : 64;
        trow *r = realloc(*rows, c * sizeof *r);
        if (!r) return -1;
        *rows = r; *cap = c;
    }
    (*rows)[(*n)++] = (trow){site, target, count, now};
    return 0;
}

// The same helpers as aot_profile.c's writer (its own are static there)
static int is_sep(char c) {
#if defined(_WIN32)
    if (c == '\\') return 1;   // a Windows path may use either separator
#endif
    return c == '/';
}
static void mkdirs(char *path) {   // mkdir -p
    for (char *s = path + 1; *s; s++) if (is_sep(*s)) { const char c = *s; *s = 0; mkdir(path, 0755); *s = c; }
    mkdir(path, 0755);
}
static void file_name(const module_t *m, char *out, size_t n) {   // a module's name as a file name
    snprintf(out, n, "%s", m->name);
    for (char *s = out; *s; s++) if (*s == '/' || *s == '\\') *s = '_';
}
static void sha_hex(const dm_aot_pack *p, char out[65]) {   // the image the pack was compiled from (its descriptor's SHA-256: the image's, as the loader checked it)
    const uint8_t *h = dm_aot_pack_desc(p)->sha256;
    for (int b = 0; b < 32; b++) snprintf(out + 2 * b, 3, "%02x", h[b]);
}
#if defined(_WIN32)
static int replace_file(const char *from, const char *to) { return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1; }   // rename does not replace a file there
#else
static int replace_file(const char *from, const char *to) { return rename(from, to); }
#endif

// The rows of the targets file at path when its header names image sha (an earlier session of this image wrote it), appended to rows: their number. *other: the file names another
// image (its rows are not taken: the file is replaced)
static size_t earlier(const char *path, const char *sha, trow **rows, size_t *n, size_t *cap, int *other) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char line[2048], mod[64], bid[80], hex[80];   // longer than any line the writer makes (the pack's path is under 1100 bytes)
    int same = 0;
    const size_t first = *n;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') {
            if (sscanf(line, "# module %63[^,], build ID %79[^,], image SHA-256 %79s", mod, bid, hex) == 3) { same = !strcmp(hex, sha); *other = !same; }
            continue;
        }
        unsigned long long site, target, cnt;
        if (same && sscanf(line, "%llx %llx %llu", &site, &target, &cnt) == 3 && trow_add(rows, n, cap, site, target, cnt, 0)) break;
    }
    fclose(f);
    return *n - first;
}

int dm_aot_targets_write(void) {
    if (!targets_on() || atomic_exchange(&g_written, 1)) return -1;
    const char *env = getenv("DM_AOT_PROFILE");
    char dir[1024];
    if (!env || !*env || strlen(env) >= sizeof dir - 96) return -1;   // aot_profile.c said so already
    snprintf(dir, sizeof dir, "%s", env);
    // A copy of the table, every thread's cache added into it first: a note after this finds g_written set and records nothing (one already past that test adds to a
    // cache row whose count was read here: it is lost, a bind at exit)
    pthread_mutex_lock(&g_lock);
    for (tgt_cache *c = g_caches; c; c = c->next) {
        for (unsigned i = 0; i < TGT_CACHE; i++) {
            tgt_row *e = &c->row[i];
            const uint64_t had = atomic_exchange_explicit(&e->n, 0, memory_order_relaxed);
            if (e->pack && had) table_add(e->pack, e->site, e->target, e->kind, had);
        }
    }
    const size_t used = g_used;
    const uint64_t dropped = g_dropped;
    tgt_slot *all = used ? malloc(used * sizeof *all) : NULL;
    size_t n = 0;
    for (size_t i = 0; all && i < g_cap; i++) if (g_slots[i].pack) all[n++] = g_slots[i];
    pthread_mutex_unlock(&g_lock);
    if (used && !all) { LOG("[RED] aot: targets: no memory for %zu rows: none written", used); return -1; }
    // What is not written: misses per kind (their dispatcher calls) and the sites they came from
    uint64_t miss[5] = {0}, bound = 0;
    size_t pairs = 0;
    for (size_t i = 0; i < n; i++) {
        if (all[i].kind < 5) miss[all[i].kind] += all[i].n;
        if (all[i].kind == DM_AOT_TARGET_BOUND) { bound += all[i].n; pairs++; }
    }
    if (!pairs) {
        LOG("aot: targets: no inline cache was bound to a target of its own module this session: no targets file written to %s (%llu misses to another pack's code, %llu to host "
            "functions, %llu to addresses with no compiled entry)", dir, (unsigned long long)(miss[DM_AOT_TARGET_OTHER_PACK] + miss[DM_AOT_TARGET_HANDOVER]),
            (unsigned long long)miss[DM_AOT_TARGET_HOST], (unsigned long long)miss[DM_AOT_TARGET_NOCODE]);
        free(all);
        return 0;
    }
    mkdirs(dir);
    int files = 0, bad = 0;
    uint64_t outside = 0;   // bound rows whose target lies outside the caller's module (never: the caller's own lookup table bound them)
    const int npacks = dm_aot_pack_count();
    for (int i = 0; i < npacks; i++) {
        const dm_aot_pack *p = dm_aot_pack_at(i);
        const module_t *m = dm_aot_pack_module(p);
        const uint64_t base = (uint64_t)(uintptr_t)m->base;
        trow *rows = NULL;
        size_t k = 0, cap = 0, now = 0;
        for (size_t j = 0; j < n; j++) {
            if (all[j].pack != p || all[j].kind != DM_AOT_TARGET_BOUND) continue;
            if (all[j].target - base >= m->size) { outside++; continue; }
            if (trow_add(&rows, &k, &cap, all[j].site, all[j].target - base, all[j].n, 1)) { k = 0; break; }
            now++;
        }
        if (!now || !k) {
            if (now) { LOG("[RED] aot: targets: no memory for %s's rows: not written", m->name); bad++; }
            free(rows);
            continue;
        }
        char name[64], path[sizeof dir + 96], tmp[sizeof path + 8], sha[65];
        file_name(m, name, sizeof name);
        snprintf(path, sizeof path, "%s/%s.targets.tsv", dir, name);
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        sha_hex(p, sha);
        int other = 0;
        const size_t had = earlier(path, sha, &rows, &k, &cap, &other);
        qsort(rows, k, sizeof *rows, trow_cmp);
        size_t w = 0;
        for (size_t j = 0; j < k; j++) {   // one row per site and target: an earlier session's row and this one's added up
            if (w && rows[w - 1].site == rows[j].site && rows[w - 1].target == rows[j].target) { rows[w - 1].n += rows[j].n; rows[w - 1].now |= rows[j].now; continue; }
            rows[w++] = rows[j];
        }
        size_t sites = 0, sites_now = 0;
        uint64_t binds = 0;
        for (size_t j = 0, e; j < w; j = e) {   // per site (its rows are adjacent)
            int seen_now = 0;
            for (e = j; e < w && rows[e].site == rows[j].site; e++) { binds += rows[e].n; seen_now |= rows[e].now; }
            sites++; sites_now += seen_now != 0;
        }
        FILE *f = fopen(tmp, "wb");   // binary: the same bytes on Windows
        if (!f) { LOG("[RED] aot: targets: cannot write %s", tmp); free(rows); bad++; continue; }
        fprintf(f, "# nxaot targets: what the x86-64 engine's inline caches of indirect calls and branches were bound to (dm/host/aot_targets.c, DM_AOT_PROFILE); "
                   "nxaot build --profile <module>.tsv reads it beside the profile\n");
        fprintf(f, "# module %s, build ID %s, image SHA-256 %s\n", m->name, m->build_id[0] ? m->build_id : "none", sha);
        fprintf(f, "# pack %s\n", dm_aot_pack_path(p));
        fprintf(f, "# %zu sites, %zu targets, %llu binds; %zu of the sites this session, the rest from earlier sessions of this image\n", sites, w, (unsigned long long)binds,
                sites_now);
        fprintf(f, "# <site module offset> <target module offset> <binds>: the blr or br, the function its inline cache was bound to, how many times the dispatcher bound it there "
                   "(once per cache for a site with one target, once per switch back to it for a site with several: switches, not calls)\n");
        for (size_t j = 0; j < w; j++)
            fprintf(f, "0x%llx 0x%llx %llu\n", (unsigned long long)rows[j].site, (unsigned long long)rows[j].target, (unsigned long long)rows[j].n);
        free(rows);
        if (fclose(f) || replace_file(tmp, path)) { LOG("[RED] aot: targets: cannot write %s", path); bad++; continue; }
        LOG("aot: targets: %s: %zu sites, %zu targets, %llu binds, %zu of the sites this session (%s%s)", path, sites, w, (unsigned long long)binds, sites_now, m->name,
            other ? "; the file held another image's targets: replaced" : had ? "; merged with an earlier session's" : "");
        files++;
    }
    LOG("aot: targets: this session %zu (site, target) pairs bound %llu times in their own module's code, %d files; not written: %llu misses bound to another pack's code, %llu "
        "handed to another pack of another signature, %llu to host functions, %llu to addresses with no compiled entry (the misses file)", pairs, (unsigned long long)bound, files,
        (unsigned long long)miss[DM_AOT_TARGET_OTHER_PACK], (unsigned long long)miss[DM_AOT_TARGET_HANDOVER], (unsigned long long)miss[DM_AOT_TARGET_HOST],
        (unsigned long long)miss[DM_AOT_TARGET_NOCODE]);
    if (dropped || outside)
        LOG("[RED] aot: targets: %llu notes the table could not keep (aot_targets.c TGT_MAX_SLOTS or no memory), %llu bound targets outside their caller's module", (unsigned long long)dropped,
            (unsigned long long)outside);
    free(all);
    return bad ? -1 : 0;
}
