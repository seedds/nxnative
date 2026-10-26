// Asset-free policy regression: metadata + compiled member IDs -> final rw.
#include "../nvn/nvn_storage_access.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static nvn_refl fixture(void) {
    nvn_refl r = {.route_b = 1, .nb = 5, .abi = 0};
    r.b[0] = (nvn_rbind){.kind = NVN_RB_SBUFP, .reg = 0, .aid = 2};
    r.b[1] = (nvn_rbind){.kind = NVN_RB_SBUFP, .reg = 1, .aid = 7};
    r.b[2] = (nvn_rbind){.kind = NVN_RB_SBUFP, .reg = 2, .aid = 11};
    r.b[3] = (nvn_rbind){.kind = NVN_RB_SBUFP, .reg = 3, .aid = 15};
    r.b[4] = (nvn_rbind){.kind = NVN_RB_CBUF, .reg = 0, .aid = 17};
    return r;
}
static nvn_storage_access_member member(unsigned aid, int access) {
    return (nvn_storage_access_member){.aid = aid, .offset = 8 * aid,
        .is_pointer = 1, .has_pointer_type = 1, .access = access};
}
int main(void) {
    // Deliberately shuffled sparse IDs, not member ordinals. The pure fixture
    // uses the route-B raw-slot layout; a separate Metal test observes real ABI.
    nvn_storage_access_member m[] = {member(11, NVN_STORAGE_WRITE_ONLY), member(17, NVN_STORAGE_READ_WRITE),
        member(2, NVN_STORAGE_READ_ONLY), member(7, NVN_STORAGE_READ_WRITE)};
    const size_t len = 18 * 8, n = sizeof m / sizeof *m;
    nvn_refl r = fixture(); nvn_storage_access_result v[5];
    for (int i = 0; i < r.nb; i++) v[i] = nvn_storage_access_apply(&r, i, m, n, 1, len);
    assert(!r.b[0].rw && r.b[1].rw && r.b[2].rw && !r.b[3].rw && !r.b[4].rw);
    assert(v[1].promoted && v[2].promoted && !v[4].promoted);
    assert(v[3].status == NVN_STORAGE_MEMBER_MISSING && v[4].status == NVN_STORAGE_NOT_STORAGE);
    assert(!nvn_storage_access_apply(&r, 1, m, n, 1, len).promoted); // idempotent
    r.b[0].rw = 1;
    assert(nvn_storage_access_apply(&r, 0, m, n, 1, len).status == NVN_STORAGE_COMPILED_READ_ONLY && r.b[0].rw);
    assert(!strcmp(nvn_storage_access_status(v[1].status), "compiled-write"));

    // Every unsuccessful/unknown outcome preserves both false and true flags.
    for (int old = 0; old < 2; old++) {
        r = fixture(); r.b[1].rw = (uint8_t)old;
        nvn_storage_access_member bad[] = {m[3], m[3]};
        v[0] = nvn_storage_access_apply(&r, 1, m, n, 0, len);
        assert(v[0].status == NVN_STORAGE_NO_REFLECTION && r.b[1].rw == old);
        v[0] = nvn_storage_access_apply(&r, 1, NULL, n, 1, len);
        assert(v[0].status == NVN_STORAGE_NO_REFLECTION && r.b[1].rw == old);
        v[0] = nvn_storage_access_apply(&r, 1, bad, 2, 1, len);
        assert(v[0].status == NVN_STORAGE_MEMBER_DUPLICATE && r.b[1].rw == old);
        bad[0] = m[3]; bad[0].offset++;
        v[0] = nvn_storage_access_apply(&r, 1, bad, 1, 1, len);
        assert(v[0].status == NVN_STORAGE_BAD_OFFSET && r.b[1].rw == old);
        bad[0] = m[3]; bad[0].access = 17;
        assert(nvn_storage_access_apply(&r, 1, bad, 1, 1, len).status == NVN_STORAGE_UNKNOWN_ACCESS && r.b[1].rw == old);
        bad[0] = m[3]; bad[0].is_pointer = 0;
        assert(nvn_storage_access_apply(&r, 1, bad, 1, 1, len).status == NVN_STORAGE_NOT_POINTER && r.b[1].rw == old);
        bad[0] = m[3]; bad[0].has_pointer_type = 0;
        assert(nvn_storage_access_apply(&r, 1, bad, 1, 1, len).status == NVN_STORAGE_NOT_POINTER && r.b[1].rw == old);
        bad[0] = m[3]; bad[0].nested_argument_buffer = 1;
        assert(nvn_storage_access_apply(&r, 1, bad, 1, 1, len).status == NVN_STORAGE_NOT_POINTER && r.b[1].rw == old);
        assert(nvn_storage_access_apply(&r, 1, m, n, 1, 0).status == NVN_STORAGE_BAD_METADATA && r.b[1].rw == old);
        assert(nvn_storage_access_apply(&r, 1, m, n, 1, 63).status == NVN_STORAGE_BAD_METADATA && r.b[1].rw == old);
        r.route_b = 0;
        assert(nvn_storage_access_apply(&r, 1, m, n, 1, len).status == NVN_STORAGE_BAD_METADATA && r.b[1].rw == old);
        r.route_b = 1; r.b[1].reg = 16;
        assert(nvn_storage_access_apply(&r, 1, m, n, 1, len).status == NVN_STORAGE_BAD_METADATA && r.b[1].rw == old);
        r.b[1].reg = 1; r.b[1].aid = NVN_REFL_MAXARG;
        assert(nvn_storage_access_apply(&r, 1, m, n, 1, 4096).status == NVN_STORAGE_BAD_METADATA && r.b[1].rw == old);
        r.b[1].aid = 7; r.b[4].aid = 7;
        assert(nvn_storage_access_apply(&r, 1, m, n, 1, len).status == NVN_STORAGE_BAD_METADATA && r.b[1].rw == old);
        r.b[4].aid = 5; r.b[4].n = 4;
        assert(nvn_storage_access_apply(&r, 1, m, n, 1, len).status == NVN_STORAGE_BAD_METADATA && r.b[1].rw == old);
    }
    puts("storage-access policy: sparse IDs, writer promotion, preservation, ABI/kind/range/missing/duplicate rejection PASS");
    return 0;
}
