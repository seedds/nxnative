// The guest object side table (hle_os.c objmap_*): putting a key again after another key's entry before it in its probe chain was deleted
// replaces the key's own entry. It used to take the deleted slot, leaving the key twice in the chain: a later objmap_del removed only
// the first copy, and objmap_get then still found the old value.
#include "../hle_os.c"
#include <assert.h>
#include <stdio.h>

// What the rest of hle_os.c links against, unused here
module_t *g_mod; int g_nmod;
__thread _Atomic int dm_host_work;
void *module_sym(module_t *m, const char *name) { (void)m; (void)name; return NULL; }
void *guest_sym(const char *name) { (void)name; return NULL; }
int dm_sdk_ounce(void) { return 1; }
void dm_guest_clock_advanced(void) {}
void crash_watch_frame(void) {}

int main(void) {
    // Two keys in the same bucket: b sits one slot after a
    const void *a = (const void *)0x1000, *b = NULL;
    for (uintptr_t k = 0x1010; !b; k += 0x10) if (om_h((const void *)k) == om_h(a)) b = (const void *)k;
    int va, vb1, vb2;
    objmap_put(a, &va);
    objmap_put(b, &vb1);
    assert(objmap_get(a) == &va && objmap_get(b) == &vb1);
    assert(objmap_del(a) == &va);   // a's slot is now a tombstone before b
    objmap_put(b, &vb2);            // b again: its own entry takes the new value
    assert(objmap_get(b) == &vb2);
    assert(objmap_del(b) == &vb2);
    assert(objmap_get(b) == NULL);  // no second copy of b left behind
    // A new key still reuses a tombstone in its chain
    const void *c = NULL;
    for (uintptr_t k = (uintptr_t)b + 0x10; !c; k += 0x10) if (om_h((const void *)k) == om_h(a)) c = (const void *)k;
    int vc; objmap_put(a, &va); assert(objmap_del(a) == &va);
    objmap_put(c, &vc);
    assert(g_om[om_h(a)].k == c && objmap_get(c) == &vc);
    puts("t_objmap: ok");
    return 0;
}
