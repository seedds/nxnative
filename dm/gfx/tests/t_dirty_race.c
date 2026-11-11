// Ordering race between "set read-only" and "unprotect before write" in dirty-page tracking (originally a test of the host this one started as, stubs adapted to this repository's tracking table).
// Scene: thread A, in nvn_dirty_protect, has already recorded the page state as "protected" while its mprotect has not landed yet; thread B now arrives with prep_write (the unprotect done before the host writes into guest memory,
// for example the movie decoder thread writing a frame into texture storage, or a disk read into a guest buffer). Old implementation: B sets the state back to "none", then A's mprotect lands => the page is read-only while its state is "none" --
//   when the host reads a file into it again: prep_write sees state "none" and skips, the system call does not go through the page-fault handler and fails with EFAULT; when a game thread writes directly: the page-fault handler only returns "re-execute" and spins forever.
// How it is staged: the mprotect in nvn_dirty.c is replaced by t_mprotect here -- before the call with which A sets read-only lands, B is let in and given 100 ms, so the order is deterministic.
// Expected values do not depend on the implementation under test: after B's prep_write returns, this page must really be writable (reading 16 bytes into it gives 16; writing one byte directly does not hang and reads back as written),
// and this write must be recorded as dirty (nvn_dirty_since returns 1 for a stamp taken before the page was set read-only).
#include <stdatomic.h>
static void t_before_store(const volatile void *object);
// Observe the clock increment before its value reaches the page generation, at the real publication seam in both unprotect paths.
#undef atomic_store
#define atomic_store(object, desired) do { \
    __auto_type object_ = (object); \
    __auto_type desired_ = (desired); \
    t_before_store(object_); \
    __c11_atomic_store(object_, desired_, __ATOMIC_SEQ_CST); \
} while (0)
#define mprotect t_mprotect
#include "../nvn/nvn_dirty.c"
#undef mprotect
int mprotect(void *, size_t, int);
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>

module_t *g_mod;
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);

static int g_fail;
#define EXPECT(name, want, got) do { \
    long long want_ = (long long)(want), got_ = (long long)(got); \
    printf("%-58s expected %-4lld → got %-4lld %s\n", name, want_, got_, want_ == got_ ? "✓" : "✗"); \
    if (want_ != got_) g_fail++; \
} while (0)

static uint8_t *g_page, *g_shared_write; static _Atomic int g_armed, g_go, g_b_done;
static int g_publish_armed, g_observed_clean;
static uint8_t *g_publish_page;
static const volatile void *g_publish_gen;
static size_t g_publish_len;
static uint64_t g_publish_stamp, g_observed_now;
static void t_before_store(const volatile void *object) {
    if (!g_publish_armed || object != g_publish_gen) return;
    g_publish_armed = 0;
    g_observed_now = nvn_dirty_now();
    g_observed_clean = !nvn_dirty_since(g_publish_page, g_publish_len, g_publish_stamp);
}
static void *thread_b(void *arg) { (void)arg; while (!atomic_load(&g_go)) usleep(200); prep_write(g_page, 16); atomic_store(&g_b_done, 1); return NULL; }
int t_mprotect(void *a, size_t n, int prot) {
    if (prot == PROT_READ && atomic_exchange(&g_armed, 0)) { atomic_store(&g_go, 1); usleep(100000); }   // Before A's mprotect lands: let B in and wait until it runs to the end or gets stuck waiting
    int result = mprotect(a, n, prot);
    // A guest store after the first range opens a shared page, before the next range hashes it.
    if (!result && prot == (PROT_READ | PROT_WRITE) && g_shared_write &&
        (uintptr_t)g_shared_write >= (uintptr_t)a && (uintptr_t)g_shared_write < (uintptr_t)a + n) {
        *g_shared_write = 9; g_shared_write = NULL;
    }
    return result;
}

// A page another thread is unprotecting (its mprotect to writable done and the new generation stored, the state still BUSY) must be waited out by nvn_dirty_protect and
// protected again: skipped, it stayed writable with a generation the caller's stamp covers, and later writes to it were never seen (MacNativeGames a166602, missing letters
// in a glyph cache). Thread C plays the last step of that unprotect 50 ms late
static _Atomic uint8_t *g_c_state;
static void *thread_c(void *arg) { (void)arg; usleep(50000); atomic_store(g_c_state, PG_NONE); return NULL; }
static void *thread_c_unprotect(void *page) { usleep(50000); mprotect(page, (size_t)getpagesize(), PROT_READ | PROT_WRITE); atomic_store(g_c_state, PG_NONE); return NULL; }
static void busy_protect(size_t pg) {
    uint8_t *page = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED) { perror("mmap"); g_fail++; return; }
    nvn_dirty_add_range(page, pg);
    range_t *r = find_span((uintptr_t)page, 1); uintptr_t i = r ? ((uintptr_t)page - r->base) >> g_shift : 0;
    nvn_dirty_protect(page, pg);
    uint8_t st = PG_PROT;
    if (!r || !atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY)) { printf("busy page: not protected\n"); g_fail++; return; }
    mprotect(page, pg, PROT_READ | PROT_WRITE);   // what unprotect_page does before it stores the generation
    atomic_store(&r->gen[i], atomic_fetch_add(&g_now, 1) + 1);
    g_c_state = &r->state[i];
    pthread_t tc; pthread_create(&tc, NULL, thread_c, NULL);
    uint64_t stamp = nvn_dirty_now();   // an uploader's stamp, then its protect
    nvn_dirty_protect(page, pg);
    pthread_join(tc, NULL);
    EXPECT("protect waits out a page being unprotected (state PROT)", PG_PROT, atomic_load(&r->state[i]));
    EXPECT("no write after the stamp yet: clean", 0, nvn_dirty_since(page, 16, stamp));
    page[0] = 7;
    EXPECT("a later write to that page is seen", 1, nvn_dirty_since(page, 16, stamp));
}
// Why the uploads take their stamp before nvn_dirty_protect: a write that faults while the pages are protected or read gets a generation above a stamp taken before and
// leaves its page writable; a stamp taken after covers it, so that write and every later one are missed (the second check is the old order, expected to miss)
static void stamp_order(size_t pg) {
    uint8_t *page = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED) { perror("mmap"); g_fail++; return; }
    nvn_dirty_add_range(page, pg);
    uint64_t before = nvn_dirty_now();
    nvn_dirty_protect(page, pg);
    page[0] = 1;   // faults during the upload
    uint64_t after = nvn_dirty_now();
    page[0] = 2;   // after the read: no fault, the page is writable
    EXPECT("stamp before protect: the writes are seen", 1, nvn_dirty_since(page, 16, before));
    EXPECT("stamp after protect (the old order): the writes are missed", 0, nvn_dirty_since(page, 16, after));
}

// A lookup can see the new clock before the page generation is stored and cache a clean result. Completing either unprotect path must invalidate that cached result.
static void generation_publish(size_t pg, int batch) {
    size_t len = batch ? 2 * pg : pg;
    uint8_t *page = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED) { perror("mmap"); g_fail++; return; }
    nvn_dirty_add_range(page, len);
    g_publish_page = page + len - pg; g_publish_len = pg; g_publish_stamp = nvn_dirty_now();
    range_t *r = find_span((uintptr_t)page, len);
    g_publish_gen = &r->gen[((uintptr_t)g_publish_page - r->base) >> g_shift];
    nvn_dirty_protect(page, len);
    g_observed_clean = 0; g_publish_armed = 1;
    if (batch) prep_write(page, len); else on_fault(page, 1, NULL);
    page[0] = 3;
    if (batch) page[pg] = 4;
    printf("generation publication (%s):\n", batch ? "host pre-write, batched pages" : "page fault");
    EXPECT("observer ran before the generation was stored", 1, g_observed_clean);
    EXPECT("publication invalidates the observer's cached clock", 1, nvn_dirty_now() != g_observed_now);
    EXPECT("lookup using the cached clock detects the write", 0,
           nvn_dirty_now() == g_observed_now || !nvn_dirty_since(g_publish_page, pg, g_publish_stamp));
    nvn_dirty_remove_range(page); munmap(page, len);
}

// nvn_dirty_open / nvn_dirty_close, around making a pool buffer (issue #1591): the protected pages are opened without counting as written, and closing protects them again and counts
// only those whose bytes changed. A write to an open page does not fault, so only its hash sees it. A host write into an open page (a file read) takes the page as it would a protected one:
// stamped and left writable; protected again, the system call that follows would fail with EFAULT
static void open_close(size_t pg) {
    uint8_t *m = mmap(NULL, 3 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); g_fail++; return; }
    nvn_dirty_add_range(m, 3 * pg);
    range_t *r = find_span((uintptr_t)m, 3 * pg); uintptr_t p0 = ((uintptr_t)m - r->base) >> g_shift;
    uint64_t stamp = nvn_dirty_now();
    nvn_dirty_protect(m, 3 * pg);
    uint64_t op0, oc0, op, oc; nvn_dirty_open_stats(&op0, &oc0);
    nvn_dirty_open(m, 3 * pg);
    printf("open pages around a pool buffer:\n");
    EXPECT("opened: the pages are open", PG_OPEN, atomic_load(&r->state[p0 + 1]));
    EXPECT("opened: not counted as written", 0, nvn_dirty_since(m, 3 * pg, stamp));
    m[0] = 9;   // a guest write meanwhile: no fault
    prep_write(m + pg, 16);   // a host write meanwhile
    nvn_dirty_close();
    nvn_dirty_open_stats(&op, &oc);
    EXPECT("three pages opened, one changed while open", 3 * 1000 + 1, (op - op0) * 1000 + (oc - oc0));
    EXPECT("written while open: protected again", PG_PROT, atomic_load(&r->state[p0]));
    EXPECT("written while open: counted as written", 1, nvn_dirty_since(m, 16, stamp));
    EXPECT("the host's page: left writable", PG_NONE, atomic_load(&r->state[p0 + 1]));
    int fd = open("/dev/zero", O_RDONLY); ssize_t n = read(fd, m + pg, 16); close(fd);
    EXPECT("the host's page: a file read after the close lands (no EFAULT)", 16, n);
    EXPECT("the host's page: counted as written", 1, nvn_dirty_since(m + pg, 16, stamp));
    EXPECT("unwritten page: protected again", PG_PROT, atomic_load(&r->state[p0 + 2]));
    EXPECT("unwritten page: not counted as written", 0, nvn_dirty_since(m + 2 * pg, pg, stamp));
    uint64_t later = nvn_dirty_now();
    m[2 * pg] = 1;   // protected again: a write faults and is seen
    EXPECT("unwritten page: a later write is seen", 1, nvn_dirty_since(m + 2 * pg, pg, later));
    nvn_dirty_remove_range(m); munmap(m, 3 * pg);
}
// Adjacent 4 KB-aligned pools share a 16 KB host page. Opening A makes B's bytes
// writable too, before B takes its own hash. A write in that window must dirty B
// even though B's own before/after hashes match. No game data or GPU is involved.
static void open_shared_page(size_t pg) {
    if (pg <= 4096) return;   // no shared host page for 4 KB-aligned pools
    uint8_t *m = mmap(NULL, 3 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); g_fail++; return; }
    size_t na = pg - 4096, nb = pg + 4096;
    uint8_t *b = m + na;
    nvn_dirty_add_range(m, na); nvn_dirty_add_range(b, nb);
    uint64_t stamp = nvn_dirty_now();
    nvn_dirty_protect(m, na); nvn_dirty_protect(b, nb);
    nvn_dirty_open(m, 2 * pg); nvn_dirty_close();
    EXPECT("shared page with no writes: B stays clean", 0, nvn_dirty_since(b, nb, stamp));
    g_shared_write = b + 16;
    nvn_dirty_open(m, 2 * pg); nvn_dirty_close();
    EXPECT("the guest store reached B's shared-page bytes", 9, b[16]);
    EXPECT("write while A opens: B's texture is dirty", 1, nvn_dirty_since(b, nb, stamp));
    nvn_dirty_remove_range(b); nvn_dirty_remove_range(m); munmap(m, 3 * pg);
}
// A page another thread is unprotecting (BUSY, still read-only) when nvn_dirty_open looks: the buffer must not be made over it while it is read-only, so the open waits for it
static void open_waits_busy(size_t pg) {
    uint8_t *m = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); g_fail++; return; }
    nvn_dirty_add_range(m, pg);
    range_t *r = find_span((uintptr_t)m, pg); uintptr_t i = ((uintptr_t)m - r->base) >> g_shift;
    nvn_dirty_protect(m, pg);
    uint8_t st = PG_PROT; atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);   // thread C plays the rest of the unprotect 50 ms late, its mprotect included
    g_c_state = &r->state[i];
    pthread_t tc; pthread_create(&tc, NULL, thread_c_unprotect, m);
    nvn_dirty_open(m, pg);
    EXPECT("open waits out a page being unprotected (state NONE)", PG_NONE, atomic_load(&r->state[i]));
    nvn_dirty_close();
    pthread_join(tc, NULL);
    nvn_dirty_remove_range(m); munmap(m, pg);
}

int main(void) {
    crash_install();
    size_t pg = (size_t)getpagesize();
    g_page = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_page == MAP_FAILED) { perror("mmap"); return 2; }
    nvn_dirty_add_range(g_page, 2 * pg);
    uint64_t stamp = nvn_dirty_now();
    pthread_t tb; pthread_create(&tb, NULL, thread_b, NULL);
    atomic_store(&g_armed, 1);
    nvn_dirty_protect(g_page, 2 * pg);                       // A: set the state -> (B cuts in) -> mprotect lands
    for (int i = 0; i < 2000 && !atomic_load(&g_b_done); i++) usleep(1000);
    EXPECT("B's unprotect-before-write returns within 2 seconds", 1, atomic_load(&g_b_done));
    if (!atomic_load(&g_b_done)) { printf("t_dirty_race: B did not return, skipping the remaining checks\n"); return 1; }
    pthread_join(tb, NULL);
    int fd = open("/dev/zero", O_RDONLY); g_page[pg] = 0;     // Nobody has unprotected the second page, so this write goes through the page-fault handler (control: the normal path works)
    ssize_t r = read(fd, g_page, 16);                        // The host write that B is about to make: a system call writing straight into the page
    EXPECT("file read into the page after B unprotects it: bytes read = the 16 requested", 16, r);
    if (r == 16) { ((volatile uint8_t *)g_page)[1] = 0x5a; EXPECT("direct write to the page does not hang and reads back as written", 0x5a, g_page[1]); }
    else { printf("  (read failed errno=%d; the page is read-only while its state is 'none', a direct write would spin in the page-fault handler, so this check is skipped)\n", errno); g_fail++; }
    EXPECT("this write is recorded as dirty (against a stamp from before set-read-only)", 1, nvn_dirty_since(g_page, 16, stamp));
    close(fd);
    busy_protect(pg);
    stamp_order(pg);
    generation_publish(pg, 0);
    generation_publish(pg, 1);
    open_close(pg);
    open_waits_busy(pg);
    open_shared_page(pg);
    printf("t_dirty_race: %s\n", g_fail ? "FAILED" : "all passed"); return g_fail != 0;
}
