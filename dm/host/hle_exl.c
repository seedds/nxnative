// exlaunch code plugins (a mod's exefs/subsdk9, Pokémon Compass on Violet 3.0.1): what exlaunch's start-up asks of the kernel, answered for modules that sit in the host.
// exlaunch (github.com/shadowninja108/exlaunch) runs as a module's DT_INIT (exl_module_init). It finds the modules by walking the address space with QueryMemory
// (mem_layout.cpp FindModules: Code+RX, Code+R, CodeData+RW triplets; the first one is taken as rtld, the next as main, the last as sdk), takes its process handle from
// svcGetInfo 65001 (Mesosphere's current process, cur_proc_handle.cpp), and writes its hooks through a writable view of the code that svcMapProcessMemory makes (rw_pages.cpp).
// Apple silicon maps 16 KB pages and exlaunch picks a 4 KB-aligned address for that view, so the same pages cannot appear there: the view is a copy, and the words it changed go
// back into the code when the view is unmapped and once after the modules' DT_INITs (main.c, before nnMain), by which time exl_main has installed its hooks. A word the rewriter
// changed or moved (<module>.changed.tsv) is refused with a red line, as a cheat's code write is (hle_cheats.c).
#include "hle.h"
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <libkern/OSCacheControl.h>
#if !TARGET_OS_IPHONE
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

const uint32_t dm_exl_process_handle = 0xFFFF8001u;   // CUR_PROCESS_HANDLE, what GetInfo 65001 answers: the one process MapProcessMemory takes

// ---------- The fake rtld: exlaunch asserts the first module it finds is rtld, which the host does not load. Three host pages laid out as a module: a text page (b . and the
// MOD0 offset, as every NSO starts), a read-only page with an empty MOD0 and module path, and a data page. QueryMemory reports them as Code+RX, Code+R and CodeData+RW below main ----------
static uint8_t *g_rtld;
static uint64_t g_rtld_page;

void dm_exl_fake_rtld(void) {
    if (g_rtld || !g_nmod) return;
    // Only with an exlaunch module loaded, so no other game sees the stand-in. Its header tag (+0x10, "~~exlaunch uwu~~" in crt0.s) is renamed by some plugins
    // (Compass: "compass_exlaunch"), so the module is known by exlaunch's own assert text for the walk this stand-in serves
    static const char sig[] = "void exl::util::FindModules()";
    int plugin = 0;
    for (int i = 0; i < g_nmod && !plugin; i++) plugin = !g_mod[i].nro && memmem(g_mod[i].base + g_mod[i].ro_off, g_mod[i].key_page_off - g_mod[i].ro_off, sig, sizeof sig - 1) != NULL;
    if (!plugin) return;
    uint8_t *lowest = g_mod[0].base;
    for (int i = 1; i < g_nmod; i++) if (g_mod[i].base < lowest) lowest = g_mod[i].base;
    long pg = sysconf(_SC_PAGESIZE); if (pg <= 0) pg = 0x4000;
    g_rtld_page = (uint64_t)pg;
    // below every module: mmap takes the address as a hint, and one it cannot give comes back elsewhere
    for (uint64_t gap = 3 * g_rtld_page; gap <= 0x10000000; gap += 3 * g_rtld_page) {
        void *a = mmap(lowest - gap, 3 * g_rtld_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (a == MAP_FAILED) continue;
        if ((uint8_t *)a + 3 * g_rtld_page <= lowest && !module_of(a)) { g_rtld = a; break; }
        munmap(a, 3 * g_rtld_page);
    }
    if (!g_rtld) { LOG("[RED] exl: no room below the modules for a stand-in rtld; an exlaunch plugin will not find main"); return; }
    uint32_t *text = (uint32_t *)g_rtld;
    text[0] = 0x14000000;                            // b .
    text[1] = (uint32_t)g_rtld_page;                 // MOD0 at the start of the read-only page
    uint32_t *mod0 = (uint32_t *)(g_rtld + g_rtld_page);
    memcpy(mod0, "MOD0", 4);                         // the other offsets stay 0: no dynamic section, bss, eh_frame_hdr or module object
    uint32_t *path = mod0 + 8;
    path[0] = 0; path[1] = 4; memcpy(path + 2, "rtld", 5);   // nn::ro module path {zero, length, name}, which exlaunch logs
    mprotect(g_rtld, g_rtld_page, PROT_READ | PROT_EXEC);
    mprotect(g_rtld + g_rtld_page, g_rtld_page, PROT_READ);
    LOG("exl: stand-in rtld at %p, below the modules for an exlaunch plugin's module walk", (void *)g_rtld);
}

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

// The rewriter's changed words of a module, {offset, length} runs sorted by offset (<module>.changed.tsv); n = -1 when the list is missing
typedef struct { const module_t *m; uint32_t (*run)[2]; int n; uint8_t *alias; } code_t;
static code_t g_code[16];
static int g_ncode;

static code_t *code_of(module_t *m) {
    for (int i = 0; i < g_ncode; i++) if (g_code[i].m == m) return &g_code[i];
    if (g_ncode == (int)(sizeof g_code / sizeof *g_code)) return NULL;
    code_t *c = &g_code[g_ncode++]; *c = (code_t){m, NULL, -1, NULL};
    char p[1100]; snprintf(p, sizeof p, "%s/%s.changed.tsv", dm_module_dir(), m->name);
    FILE *f = fopen(p, "r");
    if (f) {
        char line[256]; unsigned long off, len; int cap = 0; c->n = 0;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "%lx %lx", &off, &len) != 2) continue;
            if (c->n == cap) { cap = cap ? cap * 2 : 1024; uint32_t (*g)[2] = realloc(c->run, (size_t)cap * sizeof *c->run); if (!g) { c->n = -1; break; } c->run = g; }
            c->run[c->n][0] = (uint32_t)off; c->run[c->n][1] = (uint32_t)len; c->n++;
        }
        fclose(f);
    }
    if (c->n < 0) LOG("[RED] exl: no %s: %s's code cannot be shown to be the game's own, so a plugin's writes to it are refused", p, m->name);
#if !TARGET_OS_IPHONE
    // A second mapping of the module's pages, writable, as hle_cheats.c makes of main: the game's own mapping stays executable while a hook is written
    long pg = sysconf(_SC_PAGESIZE); if (pg <= 0) pg = 0x4000;
    uint64_t size = ((uint64_t)m->size + (uint64_t)pg - 1) & ~((uint64_t)pg - 1);
    mach_vm_address_t dst = 0; vm_prot_t cur = 0, mx = 0;
    if (!mach_vm_remap(mach_task_self(), &dst, size, 0, VM_FLAGS_ANYWHERE, mach_task_self(), (mach_vm_address_t)(uintptr_t)m->base, FALSE, &cur, &mx, VM_INHERIT_SHARE)) {
        if (!mach_vm_protect(mach_task_self(), dst, size, 0, VM_PROT_READ | VM_PROT_WRITE)) c->alias = (uint8_t *)(uintptr_t)dst;
        else mach_vm_deallocate(mach_task_self(), dst, size);
    }
#endif
    if (!c->alias) LOG("[RED] exl: no writable alias of %s: a plugin's hooks into it are not written", m->name);
    return c;
}
static int changed_word(const code_t *c, uint64_t off) {   // 1 when the rewriter changed or moved the word at off (or there is no list to tell)
    if (c->n < 0) return 1;
    int lo = 0, hi = c->n - 1;
    while (lo <= hi) { int mid = (lo + hi) / 2; if (off < c->run[mid][0]) hi = mid - 1; else if (off >= (uint64_t)c->run[mid][0] + c->run[mid][1]) lo = mid + 1; else return 1; }
    return 0;
}

// Whether [src, src + size) is code of one loaded module, which a view may be made of
int dm_exl_view_source_ok(uint64_t src, uint64_t size) {
    module_t *m = module_of((void *)(uintptr_t)src);
    return m && !m->nro && module_of((void *)(uintptr_t)(src + size - 1)) == m && src >= (uintptr_t)m->base && src + size <= (uintptr_t)m->base + m->size;
}
void dm_exl_view_add(uint64_t dst, uint64_t src, uint64_t size) {
    pthread_mutex_lock(&g_view_mu);
    uint32_t *snap = malloc(size);
    if (snap && g_nview < (int)(sizeof g_view / sizeof *g_view)) { memcpy(snap, (const void *)(uintptr_t)dst, size); g_view[g_nview++] = (view_t){dst, src, size, snap}; }
    else if (!snap) LOG("[RED] exl: no memory for a %#llx-byte view's snapshot: the view at %#llx is not written back", (unsigned long long)size, (unsigned long long)dst);
    else LOG("[RED] exl: more than %d writable views of code at once: the one at %#llx is not written back", g_nview, (unsigned long long)dst);
    pthread_mutex_unlock(&g_view_mu);
}

// Writes the words a view changed back into its module's code; caller holds g_view_mu
static void view_sync(view_t *v) {
    module_t *m = module_of((void *)(uintptr_t)v->src);
    code_t *c = m ? code_of(m) : NULL;
    if (!c || !c->alias) return;
    const uint32_t *nw = (const uint32_t *)(uintptr_t)v->dst, *old = (const uint32_t *)(uintptr_t)v->src;
    uint64_t base = v->src - (uintptr_t)m->base;
    unsigned wrote = 0, refused = 0;
    for (uint64_t i = 0; i < v->size / 4; i++) {
        if (nw[i] == v->snap[i]) continue;
        v->snap[i] = nw[i];
        uint64_t off = base + i * 4;
        if (changed_word(c, off)) {
            if (refused++ < 8) LOG("[RED] exl: a plugin writes %s+%#llx, which the rewriter changed or moved: not written", m->name, (unsigned long long)off);
            continue;
        }
        *(volatile uint32_t *)(c->alias + off) = nw[i];
        sys_icache_invalidate((void *)(uintptr_t)(v->src + i * 4), 4);
        if (old[i] != nw[i]) { if (refused++ < 8) LOG("[RED] exl: %s+%#llx written through the alias but the code still reads %#x", m->name, (unsigned long long)off, old[i]); continue; }
        if (wrote < 4 && getenv("DM_EXL_TRACE")) LOG("exl: %s+%#llx = %#x", m->name, (unsigned long long)off, nw[i]);
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
// After the modules' DT_INITs: the views exlaunch keeps open (its claims on main and on its own trampoline pages) carry the hooks exl_main installed
void dm_exl_sync_all(void) {
    pthread_mutex_lock(&g_view_mu);
    for (int i = 0; i < g_nview; i++) view_sync(&g_view[i]);
    pthread_mutex_unlock(&g_view_mu);
}
