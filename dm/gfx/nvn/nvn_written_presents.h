// DM_NVN_PRESENT_DRAWN: which texture serials a render pass has stored into, and what each serial's own previous
// present saw, so a present whose picture would be the same one can be dropped and the screen keeps what it shows.
// The encode path marks stores (nvn_written_stored, sd_finish) while the display-link and present threads count
// presents (nvn_written_presented, present_now), so the table keeps a private leaf mutex: it never acquires
// renderer or queue locks, and the two sides only race on this table.
// A serial that is not in the table counts as fresh (never dropped): the first two presents go through whatever
// the texture holds, and a latch that ran out of slots drops nothing.
#pragma once
#include <pthread.h>
#include <stdint.h>

#define NVN_WRITTEN_TABLE 4096
typedef struct {
    uint64_t serial;
    uint32_t stored;    // the passes that stored into this serial since it entered the table
    uint32_t shown;      // the stored count this serial's previous present saw
    uint8_t presented;   // this serial was presented once already
} nvn_written_slot;
typedef struct { pthread_mutex_t mutex; nvn_written_slot slot[NVN_WRITTEN_TABLE]; } nvn_written_presents;
#define NVN_WRITTEN_PRESENTS_INIT {.mutex = PTHREAD_MUTEX_INITIALIZER}

static inline nvn_written_slot *nvn_written_find(nvn_written_presents *w, uint64_t serial) {   // the slot, inserted while the table has room; NULL: full
    unsigned i = (unsigned)(serial % NVN_WRITTEN_TABLE);
    for (unsigned n = 0; n < NVN_WRITTEN_TABLE; n++, i = (i + 1) % NVN_WRITTEN_TABLE) {
        if (!w->slot[i].serial) { w->slot[i] = (nvn_written_slot){.serial = serial}; return &w->slot[i]; }
        if (w->slot[i].serial == serial) return &w->slot[i];
    }
    return NULL;
}

// A pass stored into this serial: its next present carries a new picture.
static inline void nvn_written_stored(nvn_written_presents *w, uint64_t serial) {
    pthread_mutex_lock(&w->mutex);
    if (serial) { nvn_written_slot *s = nvn_written_find(w, serial); if (s) s->stored++; }
    pthread_mutex_unlock(&w->mutex);
}

// This serial is presented. Returns 1 when no pass stored into it since its own previous present: the picture
// would be the same one, and a caller that drops the present keeps what the screen already shows.
static inline int nvn_written_presented(nvn_written_presents *w, uint64_t serial) {
    int unchanged = 0;
    pthread_mutex_lock(&w->mutex);
    if (serial) {
        nvn_written_slot *s = nvn_written_find(w, serial);
        if (s) {
            unchanged = s->presented && s->shown == s->stored;
            s->presented = 1;
            s->shown = s->stored;
        }
    }
    pthread_mutex_unlock(&w->mutex);
    return unchanged;
}
