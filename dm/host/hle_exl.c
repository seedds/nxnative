// exlaunch code plugins (a mod's exefs/subsdk<N>, Pokémon Compass on Violet 3.0.1): what exlaunch's start-up asks of the kernel, answered for modules that sit in the host.
// exlaunch (github.com/shadowninja108/exlaunch) runs as a module's DT_INIT (exl_module_init). It finds the modules by walking the address space with QueryMemory
// (mem_layout.cpp FindModules: Code+RX, Code+R, CodeData+RW triplets; the first one is taken as rtld, the next as main, the last as sdk), takes its process handle from
// svcGetInfo 65001 (Mesosphere's current process, cur_proc_handle.cpp), and writes its hooks through a writable view of the code that svcMapProcessMemory makes (rw_pages.cpp).
// Apple silicon maps 16 KB pages and exlaunch picks a 4 KB-aligned address for that view, so the same pages cannot appear there: the view is a copy, and the words it changed go
// back into the code when the view is unmapped and after each module's DT_INIT (loader.c run_module_inits). A word the rewriter changed or moved (<module>.changed.tsv,
// verified against build_id via code_write.h) is refused with a red line, as a cheat's code write is (hle_cheats.c). Mac only: iOS code pages are signed and cannot be written.
#include "hle.h"
#include "code_write.h"
#include <ctype.h>
#include <dirent.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

const uint32_t dm_exl_process_handle = 0xFFFF8001u;   // CUR_PROCESS_HANDLE, what GetInfo 65001 answers: the one process MapProcessMemory takes

#if !TARGET_OS_IPHONE
// Number of mod code plugins loaded via dm_exl_mod_modules; triggers the stand-in rtld without scanning unmodded games
static int g_mod_plugins;
#endif

// ---------- The fake rtld: exlaunch asserts the first module it finds is rtld, which the host does not load. Three host pages laid out as a module: a text page (b . and the
// MOD0 offset, as every NSO starts), a read-only page with an empty MOD0 and module path, and a data page. QueryMemory reports them as Code+RX, Code+R and CodeData+RW below main ----------
static uint8_t *g_rtld;
static uint64_t g_rtld_page;

#if TARGET_OS_IPHONE
void dm_exl_fake_rtld(void) {}
#else
static int has_exl_sig(const module_t *m) {
    // Fallback detection for a manually loaded subsdk plugin (outside DM_ROMFS_OVERLAY): scan only its .rodata, never .bss
    if (m->nro || !m->loaded || !strcmp(m->name, "main") || !strcmp(m->name, "sdk")) return 0;
    if (!m->ro_off || m->data_off <= m->ro_off || m->data_off > m->key_page_off) return 0;
    static const char sig[] = "void exl::util::FindModules()";
    return memmem(m->base + m->ro_off, (size_t)(m->data_off - m->ro_off), sig, sizeof sig - 1) != NULL;
}

static int span_overlaps_modules(const uint8_t *a, size_t len) {
    const uint8_t *end = a + len;
    for (int i = 0; i < g_nmod; i++) {
        const uint8_t *mb = g_mod[i].base;
        if (!mb) continue;
        const uint8_t *lo = g_mod[i].patch_off < 0 ? mb + g_mod[i].patch_off : mb;
        const uint8_t *hi = mb + g_mod[i].size;
        if (a < hi && lo < end) return 1;
    }
    return 0;
}

void dm_exl_fake_rtld(void) {
    if (g_rtld || !g_nmod) return;
    int plugin = g_mod_plugins > 0;
    for (int i = 0; i < g_nmod && !plugin; i++) plugin = has_exl_sig(&g_mod[i]);
    if (!plugin) return;

    size_t pg = 0;
    uint8_t *slot = dm_mod_rtld_slot(&pg);
    if (slot) {
        if (!mprotect(slot, 3 * pg, PROT_READ | PROT_WRITE)) { g_rtld = slot; g_rtld_page = (uint64_t)pg; }
    }
    if (!g_rtld) {
        // Fallback when modules were not placed by the contiguous startup reservation (e.g. DM_JIT_SERVE)
        uint8_t *lowest = NULL;
        for (int i = 0; i < g_nmod; i++) {
            if (!g_mod[i].loaded) continue;
            uint8_t *lo = g_mod[i].patch_off < 0 ? g_mod[i].base + g_mod[i].patch_off : g_mod[i].base;
            if (!lowest || lo < lowest) lowest = lo;
        }
        if (!lowest) return;
        long p = sysconf(_SC_PAGESIZE);
        g_rtld_page = p >= 0x4000 ? (uint64_t)p : 0x4000;
        for (uint64_t gap = 3 * g_rtld_page; gap <= 0x10000000 && (uintptr_t)lowest > gap; gap += 3 * g_rtld_page) {
            void *a = mmap(lowest - gap, 3 * g_rtld_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (a == MAP_FAILED) continue;
            if ((uint8_t *)a + 3 * g_rtld_page <= lowest && !span_overlaps_modules(a, (size_t)(3 * g_rtld_page))) { g_rtld = a; break; }
            munmap(a, 3 * g_rtld_page);
        }
    }
    if (!g_rtld) { LOG("[RED] exl: no room below the modules for a stand-in rtld; an exlaunch plugin will not find main"); return; }

    uint32_t *text = (uint32_t *)g_rtld;
    text[0] = 0x14000000;                            // b .
    text[1] = (uint32_t)g_rtld_page;                 // MOD0 at the start of the read-only page
    uint32_t *mod0 = (uint32_t *)(g_rtld + g_rtld_page);
    memcpy(mod0, "MOD0", 4);                         // the other offsets stay 0: no dynamic section, bss, eh_frame_hdr or module object
    uint32_t *path = mod0 + 8;
    path[0] = 0; path[1] = 4; memcpy(path + 2, "rtld", 5);   // nn::ro module path {zero, length, name}, which exlaunch logs
    if (mprotect(g_rtld, g_rtld_page, PROT_READ | PROT_EXEC) || mprotect(g_rtld + g_rtld_page, g_rtld_page, PROT_READ)) {
        LOG("[RED] exl: mprotect failed for stand-in rtld at %p (errno=%d)", (void *)g_rtld, errno);
        if (g_rtld == slot) (void)mprotect(g_rtld, 3 * g_rtld_page, PROT_NONE);
        else munmap(g_rtld, 3 * g_rtld_page);
        g_rtld = NULL; g_rtld_page = 0;
        return;
    }
    LOG("exl: stand-in rtld at %p, below the modules for an exlaunch plugin's module walk", (void *)g_rtld);
}
#endif

// QueryMemory for an address in the stand-in rtld: 1 with its page's region, 0 elsewhere
int dm_exl_rtld_query(uint64_t addr, uint64_t *base, uint64_t *size, uint64_t *state, uint64_t *perm) {
    uint64_t b = (uintptr_t)g_rtld;
    if (!g_rtld || addr < b || addr - b >= 3 * g_rtld_page) return 0;
    unsigned k = (unsigned)((addr - b) / g_rtld_page);
    *base = b + k * g_rtld_page; *size = g_rtld_page;
    *state = k < 2 ? 3 /*Code*/ : 4 /*CodeData*/; *perm = k == 0 ? 5 : k == 1 ? 1 : 3;
    return 1;
}
// The stand-in rtld's span (base 0 when there is none), for QueryMemory's free ranges outside the guest address space
uint64_t dm_exl_rtld_base(void) { return (uintptr_t)g_rtld; }
uint64_t dm_exl_rtld_size(void) { return g_rtld ? 3 * g_rtld_page : 0; }

// ---------- Writable views of module code (MapProcessMemory) ----------
// snap: the view as it was mapped. A view's changes are the words that differ from it, not from the code: exlaunch keeps a view of all of main open while it hooks through
// short-lived ones, and that view's stale copy of each hooked word would otherwise be written back over the hook
typedef struct { uint64_t dst, src, size; uint32_t *snap; } view_t;
static view_t g_view[64];
static int g_nview;
static pthread_mutex_t g_view_mu = PTHREAD_MUTEX_INITIALIZER;

typedef struct { const module_t *m; dm_changed_t changed; uint8_t *alias; } code_t;
static code_t g_code[16];
static int g_ncode;

static code_t *code_find_locked(const module_t *m) {
    for (int i = 0; i < g_ncode; i++) if (g_code[i].m == m) return &g_code[i];
    return NULL;
}

// Prepare the writable alias and <module>.changed.tsv before taking g_map_lock or g_view_mu (no file I/O inside critical sections)
void dm_exl_code_prepare(uint64_t src) {
    const module_t *m = module_of((void *)(uintptr_t)src);
    if (!m || m->nro) return;
    pthread_mutex_lock(&g_view_mu);
    int have = code_find_locked(m) != NULL || g_ncode == (int)(sizeof g_code / sizeof *g_code);
    pthread_mutex_unlock(&g_view_mu);
    if (have) return;

    dm_changed_t ch = {0};
    dm_changed_load(&ch, m, m->dir ? m->dir : dm_module_dir(), "exl");
    uint8_t *alias = dm_code_alias_map(m);
    if (!alias) LOG("[RED] exl: no writable alias of %s: a plugin's hooks into it are not written", m->name);

    pthread_mutex_lock(&g_view_mu);
    if (!code_find_locked(m) && g_ncode < (int)(sizeof g_code / sizeof *g_code)) {
        g_code[g_ncode++] = (code_t){m, ch, alias};
        ch.run = NULL; alias = NULL;
    }
    pthread_mutex_unlock(&g_view_mu);
    free(ch.run);
#if !TARGET_OS_IPHONE
    if (alias) {
        long pg = sysconf(_SC_PAGESIZE); if (pg <= 0) pg = 0x4000;
        uint64_t sz = ((uint64_t)m->size + (uint64_t)pg - 1) & ~((uint64_t)pg - 1);
        mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(uintptr_t)alias, sz);
    }
#endif
}

// Whether [src, src + size) lies inside one loaded NSO's guest image (excluding host key page, frame-step page, and patch areas)
int dm_exl_view_source_ok(uint64_t src, uint64_t size) {
    if (!size || size > UINT64_MAX - src) return 0;
    module_t *m = module_of((void *)(uintptr_t)src);
    if (!m || m->nro || !m->loaded) return 0;
    uint64_t mb = (uintptr_t)m->base, limit = m->key_page_off ? m->key_page_off : m->size;
    return src >= mb && src - mb <= limit && size <= limit - (src - mb);
}

int dm_exl_view_add(uint64_t dst, uint64_t src, uint64_t size) {
    uint32_t *snap = malloc((size_t)size);
    if (!snap) {
        LOG("[RED] exl: no memory for a %#llx-byte view's snapshot: refusing MapProcessMemory at %#llx", (unsigned long long)size, (unsigned long long)dst);
        return -1;
    }
    memcpy(snap, (const void *)(uintptr_t)dst, (size_t)size);
    pthread_mutex_lock(&g_view_mu);
    if (g_nview < (int)(sizeof g_view / sizeof *g_view)) {
        g_view[g_nview++] = (view_t){dst, src, size, snap};
        pthread_mutex_unlock(&g_view_mu);
        return 0;
    }
    pthread_mutex_unlock(&g_view_mu);
    free(snap);
    LOG("[RED] exl: more than %d writable views of code at once: refusing MapProcessMemory at %#llx", (int)(sizeof g_view / sizeof *g_view), (unsigned long long)dst);
    return -1;
}

// Writes the words a view changed back into its module's code; caller holds g_view_mu
static void view_sync(view_t *v) {
    const module_t *m = module_of((void *)(uintptr_t)v->src);
    code_t *c = m ? code_find_locked(m) : NULL;
    if (!c || !c->alias) return;
    static int trace = -1;
    if (trace < 0) trace = getenv("DM_EXL_TRACE") != NULL;
    long pg = sysconf(_SC_PAGESIZE); if (pg <= 0) pg = 0x4000;
    uint64_t rx_end = (m->text_end + (uint64_t)pg - 1) & ~((uint64_t)pg - 1);
    const uint32_t *nw = (const uint32_t *)(uintptr_t)v->dst, *old = (const uint32_t *)(uintptr_t)v->src;
    uint64_t base = v->src - (uintptr_t)m->base;
    unsigned wrote = 0, refused = 0;
    for (uint64_t i = 0; i < v->size / 4; i++) {
        if (nw[i] == v->snap[i]) continue;
        v->snap[i] = nw[i];
        uint64_t off = base + i * 4;
        if (off < rx_end && dm_changed_hit(&c->changed, off, 4) != -1) {
            if (refused++ < 8) LOG("[RED] exl: a plugin writes %s+%#llx, which the rewriter changed or moved (or changed.tsv is missing): not written", m->name, (unsigned long long)off);
            continue;
        }
        dm_guest_write_prepare((void *)(uintptr_t)(v->src + i * 4), 4);
        *(volatile uint32_t *)(c->alias + off) = nw[i];
        if (off < rx_end) sys_icache_invalidate((void *)(uintptr_t)(v->src + i * 4), 4);
        if (old[i] != nw[i]) { if (refused++ < 8) LOG("[RED] exl: %s+%#llx written through the alias but the code still reads %#x", m->name, (unsigned long long)off, old[i]); continue; }
        if (wrote < 4 && trace) LOG("exl: %s+%#llx = %#x", m->name, (unsigned long long)off, nw[i]);
        wrote++;
    }
    if (wrote || refused) LOG("exl: %u words written into %s+%#llx..+%#llx%s", wrote, m->name, (unsigned long long)base, (unsigned long long)(base + v->size),
                              refused ? " (some refused, above)" : "");
}

// UnmapProcessMemory: the view's changes go into the code and the view is forgotten; 1 when [dst, dst + size) was a view of src
int dm_exl_view_remove(uint64_t dst, uint64_t src, uint64_t size) {
    pthread_mutex_lock(&g_view_mu);
    for (int i = 0; i < g_nview; i++) if (g_view[i].dst == dst && g_view[i].src == src && g_view[i].size == size) {
        view_sync(&g_view[i]); free(g_view[i].snap); g_view[i] = g_view[--g_nview];
        pthread_mutex_unlock(&g_view_mu); return 1;
    }
    pthread_mutex_unlock(&g_view_mu);
    return 0;
}

// After each module's DT_INIT: flush views exlaunch keeps open (its claims on main and on its own trampoline pages) so hooks are live before main's DT_INIT and nnMain
void dm_exl_sync_all(void) {
    pthread_mutex_lock(&g_view_mu);
    for (int i = 0; i < g_nview; i++) view_sync(&g_view[i]);
    pthread_mutex_unlock(&g_view_mu);
}

// ---------- Code plugins of the mods switched on ----------
// A front end switches a mod on by putting its RomFS folder in DM_ROMFS_OVERLAY (e.g. mods/<title ID>/<mod>/romfs). A mod with code has an Atmosphère exefs/ beside that folder
// (subsdk<N>, main.npdm); the rewriter cannot run at launch, so scripts/prepare_mod.py rewrites the plugins once into <mod>/exefs/nxnative/ (<name>.patched.bin and its tables),
// and they load here before sdk in overlay order and numeric subsdk order. A mod's exefs/main replaces the game's code itself: refused.
static int is_subsdk_name(const char *s) {
    if (strncmp(s, "subsdk", 6) || !isdigit((unsigned char)s[6]) || strlen(s) >= 16) return 0;
    for (const char *p = s + 7; *p; p++) if (!isdigit((unsigned char)*p)) return 0;
    return 1;
}

static int subsdk_cmp(const void *a, const void *b) {
    const char *sa = *(const char *const *)a, *sb = *(const char *const *)b;
    unsigned long na = strtoul(sa + 6, NULL, 10), nb = strtoul(sb + 6, NULL, 10);
    return na < nb ? -1 : na > nb ? 1 : strcmp(sa, sb);
}

int dm_exl_mod_modules(const char **names, int n, int room) {
#if TARGET_OS_IPHONE
    (void)names; (void)room;
#else
    int added = 0;
#endif
    const char *ov = getenv("DM_ROMFS_OVERLAY");
    if (!ov || !*ov || n < 1) return n;
    char *list = strdup(ov); if (!list) return n;
    for (char *save = NULL, *rd = strtok_r(list, ":", &save); rd; rd = strtok_r(NULL, ":", &save)) {
        char root[1024]; strlcpy(root, rd, sizeof root);
        size_t l = strlen(root); while (l > 1 && root[l - 1] == '/') root[--l] = 0;
        char *sl = strrchr(root, '/'); if (sl && !strcasecmp(sl + 1, "romfs")) *sl = 0;   // <mod>/romfs → <mod>; a mod whose files are in the folder itself stays as it is
        char exefs[1100], f[1300];
        snprintf(exefs, sizeof exefs, "%s/exefs", root);
#if !TARGET_OS_IPHONE
        char prep[1200], fx[1300];
        snprintf(prep, sizeof prep, "%s/nxnative", exefs);
#endif
        DIR *d = opendir(exefs); if (!d) continue;
        char *found[32]; int nf = 0;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, "main")) {
                LOG("[RED] exl: %s/main replaces the game's code, which is not supported: left out", exefs);
                continue;
            }
            if (!is_subsdk_name(e->d_name)) continue;
            snprintf(f, sizeof f, "%s/%s", exefs, e->d_name);
            struct stat st;
            if (stat(f, &st) || !S_ISREG(st.st_mode)) continue;
            if (nf < (int)(sizeof found / sizeof *found)) {
                char *cp = strdup(e->d_name);
                if (cp) found[nf++] = cp;
            }
        }
        closedir(d);
        if (nf > 1) qsort(found, (size_t)nf, sizeof *found, subsdk_cmp);
        for (int j = 0; j < nf; j++) {
#if TARGET_OS_IPHONE
            LOG("[RED] exl: %s/%s left out: mod code plugins are not supported on iOS (signed code pages)", exefs, found[j]);
#else
            snprintf(f, sizeof f, "%s/%s.patched.bin", prep, found[j]);
            snprintf(fx, sizeof fx, "%s/%s.fixups.tsv", prep, found[j]);
            if (access(f, R_OK) || access(fx, R_OK)) {
                LOG("[RED] exl: %s/%s is not prepared (scripts/prepare_mod.py): the mod's code is left out", exefs, found[j]);
                continue;
            }
            int clash = 0;
            for (int k = 0; k < n; k++) { const char *b = strrchr(names[k], '/'); clash |= !strcmp(b ? b + 1 : names[k], found[j]); }
            if (clash) { LOG("[RED] exl: %s/%s has the name of a module already loaded: left out", exefs, found[j]); continue; }
            if (added == room) { LOG("[RED] exl: more than %d mod code plugins: %s/%s left out", room, exefs, found[j]); continue; }
            size_t nml = strlen(prep) + strlen(found[j]) + 2;
            char *nm = malloc(nml);
            if (!nm) continue;
            snprintf(nm, nml, "%s/%s", prep, found[j]);
            // Insert before sdk (if present) so load order stays main, subsdk*, sdk; otherwise append at the end
            int pos = n;
            for (int k = 0; k < n; k++) { const char *b = strrchr(names[k], '/'); if (!strcmp(b ? b + 1 : names[k], "sdk")) { pos = k; break; } }
            memmove(&names[pos + 1], &names[pos], (size_t)(n - pos) * sizeof *names);
            names[pos] = nm; n++; added++; g_mod_plugins++;
            LOG("exl: mod code plugin %s/%s", exefs, found[j]);
#endif
        }
        for (int j = 0; j < nf; j++) free(found[j]);
    }
    free(list);
    return n;
}
