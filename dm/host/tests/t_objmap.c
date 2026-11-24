// The guest object side table (hle_os.c objmap_*): linear probing with backward-shift deletion.
// A key put again after another key's entry before it in its probe run was deleted keeps one entry (with deleted markers it took the
// marked slot and sat in its run twice: objmap_del removed only the first copy, and objmap_get then still found the old value).
// Deletion moves back exactly the later entries whose home slot is not after the hole, also across the table's end.
#include "../hle_os.c"
#include <assert.h>
#include <stdio.h>

// What the rest of hle_os.c links against, unused here
module_t *g_mod; int g_nmod;
__thread _Atomic int dm_host_work;
void *module_sym(module_t *m, const char *name) { (void)m; (void)name; return NULL; }
void *guest_sym(const char *name) { (void)name; return NULL; }
int dm_sdk_ounce(void) { return 1; }
void crash_watch_frame(void) {}

// The first key above `after` whose home slot is `home`
static const void *key_at(unsigned home, uintptr_t after) {
    for (uintptr_t k = after + 0x10;; k += 0x10) if (om_h((const void *)k) == home) return (const void *)k;
}
static int empty(void) { for (unsigned i = 0; i < OM_SIZE; i++) if (g_om[i].k) return 0; return 1; }

int main(void) {
    int v1, v2, v3, v4;

    // A key put again past a deleted entry in its run: one entry, gone after its delete
    const void *a = (const void *)0x1000; unsigned h = om_h(a);
    const void *b = key_at(h, (uintptr_t)a);
    objmap_put(a, &v1); objmap_put(b, &v2);
    assert(objmap_get(a) == &v1 && objmap_get(b) == &v2);
    assert(objmap_del(a) == &v1 && objmap_get(a) == NULL);
    objmap_put(b, &v3);
    assert(objmap_get(b) == &v3);
    assert(objmap_del(b) == &v3 && objmap_get(b) == NULL);
    assert(objmap_del(b) == NULL);
    assert(empty());

    // The shift: x, y home h; z home h+1, pushed to h+2. Deleting x moves y to h and z to its home h+1
    const void *x = key_at(h, 0x1000), *y = key_at(h, (uintptr_t)x), *z = key_at((h + 1) & OM_MASK, 0x1000);
    objmap_put(x, &v1); objmap_put(y, &v2); objmap_put(z, &v3);
    assert(g_om[h].k == x && g_om[(h + 1) & OM_MASK].k == y && g_om[(h + 2) & OM_MASK].k == z);
    objmap_del(x);
    assert(g_om[h].k == y && g_om[(h + 1) & OM_MASK].k == z && !g_om[(h + 2) & OM_MASK].k);
    assert(objmap_get(x) == NULL && objmap_get(y) == &v2 && objmap_get(z) == &v3);
    objmap_del(y); objmap_del(z);
    assert(empty());

    // An entry at its own home after the hole stays: x home h, w home h+1 at h+1. Deleting x leaves h empty and w in place
    const void *w = key_at((h + 1) & OM_MASK, 0x1000);
    objmap_put(x, &v1); objmap_put(w, &v2);
    objmap_del(x);
    assert(!g_om[h].k && g_om[(h + 1) & OM_MASK].k == w && objmap_get(w) == &v2);
    objmap_del(w);
    assert(empty());

    // Across the table's end: p, q home the last slot (q wraps to 0), r home 0 (pushed to 1), s home 1 (pushed to 2)
    const void *p = key_at(OM_MASK, 0x1000), *q = key_at(OM_MASK, (uintptr_t)p), *r = key_at(0, 0x1000), *s = key_at(1, 0x1000);
    objmap_put(p, &v1); objmap_put(q, &v2); objmap_put(r, &v3); objmap_put(s, &v4);
    assert(g_om[OM_MASK].k == p && g_om[0].k == q && g_om[1].k == r && g_om[2].k == s);
    objmap_del(p);
    assert(g_om[OM_MASK].k == q && g_om[0].k == r && g_om[1].k == s && !g_om[2].k);
    assert(objmap_get(p) == NULL && objmap_get(q) == &v2 && objmap_get(r) == &v3 && objmap_get(s) == &v4);
    objmap_del(r);   // a hole at 0: s (home 1) stays at its home
    assert(g_om[OM_MASK].k == q && !g_om[0].k && g_om[1].k == s && objmap_get(s) == &v4);
    objmap_del(q); objmap_del(s);
    assert(empty());

    // Many keys sharing few homes, deleted in a scattered order: every survivor stays reachable and the table ends empty
    enum { N = 512 };
    static const void *ks[N]; static int vs[N], gone[N];
    for (int i = 0; i < N; i++) { ks[i] = key_at((h + (unsigned)(i % 7)) & OM_MASK, i ? (uintptr_t)ks[i - 1] : 0x1000); objmap_put(ks[i], &vs[i]); }
    for (int step = 0; step < N; step++) {
        int d = (step * 37) % N;
        assert(objmap_del(ks[d]) == &vs[d]); gone[d] = 1;
        for (int i = 0; i < N; i++) assert(objmap_get(ks[i]) == (gone[i] ? NULL : &vs[i]));
    }
    assert(empty());

    puts("t_objmap: ok");
    return 0;
}
