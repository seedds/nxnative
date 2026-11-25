#include "dm.h"
#include <pthread.h>
#include <string.h>
#include <zlib.h>

// Host libz takeover for games whose statically linked scalar C zlib causes frame hitches (registered via OBS_ZLIB + CUTS in patches.py).
// Guest z_stream is kept with state == 0 so callers that inline inflateEnd (checking strm->state before calling zfree) safely skip guest zfree.
typedef struct {
    uint64_t next_in;
    uint32_t avail_in;
    uint32_t _pad0;
    uint64_t total_in;
    uint64_t next_out;
    uint32_t avail_out;
    uint32_t _pad1;
    uint64_t total_out;
    uint64_t msg;
    uint64_t state;
    uint64_t zalloc;
    uint64_t zfree;
    uint64_t opaque;
    int32_t  data_type;
    uint32_t _pad2;
    uint64_t adler;
    uint64_t reserved;
} guest_z_stream_t;

_Static_assert(sizeof(guest_z_stream_t) == 0x70, "guest z_stream size must be 112 bytes");

enum { ZLIB_SLOTS = 256 };
typedef struct {
    uint64_t guest;
    z_stream zs;
    uint64_t seq;
    int used;
    int busy;
} zlib_slot_t;

static pthread_mutex_t g_zlib_mu = PTHREAD_MUTEX_INITIALIZER;
static zlib_slot_t g_zlib_slots[ZLIB_SLOTS];
static uint64_t g_zlib_seq;

static zlib_slot_t *zlib_alloc_slot(uint64_t guest) {
    zlib_slot_t *free_slot = NULL, *oldest = NULL;
    for (int i = 0; i < ZLIB_SLOTS; i++) {
        zlib_slot_t *s = &g_zlib_slots[i];
        if (s->used && s->guest == guest && !s->busy) {
            inflateEnd(&s->zs);
            memset(&s->zs, 0, sizeof s->zs);
            s->seq = ++g_zlib_seq;
            return s;
        }
        if (!s->used && !free_slot) free_slot = s;
        if (s->used && !s->busy && (!oldest || s->seq < oldest->seq)) oldest = s;
    }
    if (!free_slot) {
        if (!oldest) return NULL;
        LOG("[RED] hle_zlib: stream table full (%d), evicting oldest guest %#llx", ZLIB_SLOTS, (unsigned long long)oldest->guest);
        inflateEnd(&oldest->zs);
        free_slot = oldest;
    }
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->guest = guest;
    free_slot->seq = ++g_zlib_seq;
    free_slot->used = 1;
    return free_slot;
}

static void obs_zlib(uint64_t *r, int kind) {
    if (kind == 0 || kind == 1) {   // inflateInit_(strm, version, stream_size) / inflateInit2_(strm, windowBits, version, stream_size)
        uint64_t gp = r[0];
        int window_bits = (kind == 1) ? (int)r[1] : MAX_WBITS;
        const char *ver = (const char *)(uintptr_t)(kind == 1 ? r[2] : r[1]);
        int ssize = (int)(kind == 1 ? r[3] : r[2]);
        if (ssize != (int)sizeof(guest_z_stream_t) || !ver || ver[0] != '1') {
            r[0] = (uint64_t)(int64_t)Z_VERSION_ERROR;
            return;
        }
        guest_z_stream_t *g = (guest_z_stream_t *)(uintptr_t)gp;
        if (!g) { r[0] = (uint64_t)(int64_t)Z_STREAM_ERROR; return; }
        g->msg = 0; g->state = 0; g->total_in = 0; g->total_out = 0;
        pthread_mutex_lock(&g_zlib_mu);
        zlib_slot_t *s = zlib_alloc_slot(gp);
        int rc = s ? inflateInit2(&s->zs, window_bits) : Z_MEM_ERROR;
        if (s && rc != Z_OK) s->used = 0;
        pthread_mutex_unlock(&g_zlib_mu);
        r[0] = (uint64_t)(int64_t)rc;
        return;
    }
    if (kind == 2) {   // inflate(strm, flush)
        uint64_t gp = r[0];
        int flush = (int)r[1];
        guest_z_stream_t *g = (guest_z_stream_t *)(uintptr_t)gp;
        if (!g) { r[0] = (uint64_t)(int64_t)Z_STREAM_ERROR; return; }
        pthread_mutex_lock(&g_zlib_mu);
        zlib_slot_t *s = NULL;
        for (int i = 0; i < ZLIB_SLOTS; i++)
            if (g_zlib_slots[i].used && g_zlib_slots[i].guest == gp && !g_zlib_slots[i].busy) { s = &g_zlib_slots[i]; s->busy = 1; break; }
        pthread_mutex_unlock(&g_zlib_mu);
        if (!s) { r[0] = (uint64_t)(int64_t)Z_STREAM_ERROR; return; }
        s->zs.next_in = (Bytef *)(uintptr_t)g->next_in;
        s->zs.avail_in = g->avail_in;
        s->zs.next_out = (Bytef *)(uintptr_t)g->next_out;
        s->zs.avail_out = g->avail_out;
        int rc = inflate(&s->zs, flush);
        g->next_in = (uint64_t)(uintptr_t)s->zs.next_in;
        g->avail_in = s->zs.avail_in;
        g->total_in = s->zs.total_in;
        g->next_out = (uint64_t)(uintptr_t)s->zs.next_out;
        g->avail_out = s->zs.avail_out;
        g->total_out = s->zs.total_out;
        g->data_type = s->zs.data_type;
        g->adler = s->zs.adler;
        pthread_mutex_lock(&g_zlib_mu);
        s->busy = 0;
        // Clean up on terminal return so callers that inline inflateEnd (skipping when state == 0) do not leak host z_stream state.
        if (rc == Z_STREAM_END || (rc < 0 && rc != Z_BUF_ERROR)) {
            inflateEnd(&s->zs);
            s->used = 0;
        }
        pthread_mutex_unlock(&g_zlib_mu);
        r[0] = (uint64_t)(int64_t)rc;
        return;
    }
    if (kind == 3) {   // inflateEnd(strm)
        uint64_t gp = r[0];
        if (!gp) { r[0] = (uint64_t)(int64_t)Z_STREAM_ERROR; return; }
        pthread_mutex_lock(&g_zlib_mu);
        for (int i = 0; i < ZLIB_SLOTS; i++) {
            if (g_zlib_slots[i].used && g_zlib_slots[i].guest == gp && !g_zlib_slots[i].busy) {
                inflateEnd(&g_zlib_slots[i].zs);
                g_zlib_slots[i].used = 0;
                break;
            }
        }
        pthread_mutex_unlock(&g_zlib_mu);
        ((guest_z_stream_t *)(uintptr_t)gp)->state = 0;
        r[0] = Z_OK;
        return;
    }
}

__attribute__((constructor)) static void zlib_attach(void) {
    gate_obs_set(OBS_ZLIB, obs_zlib);
}
