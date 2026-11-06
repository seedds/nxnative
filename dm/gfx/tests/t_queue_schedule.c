// Pure-C queue dependency tests; no Metal, guest process, timing or sleeps.
// Build: cc -std=c11 -Wall -Wextra -Werror t_queue_schedule.c ../nvn/nvn_queue_schedule.c -o t_queue_schedule
#include "../nvn/nvn_queue_schedule.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failed;
#define CHECK(why, cond) \
    do { \
        checks++; \
        if (!(cond)) { \
            failed++; \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, why); \
        } \
    } while (0)
enum { BASE = 11, COMPUTE = 12, AFTER = 13, LATER = 14 };
typedef struct {
    uint64_t id;
    uint32_t projected, visible;
} event;
typedef struct {
    uint64_t fence, event;
    uint32_t value;
} publication;
typedef struct {
    unsigned id;
} payload;
typedef struct {
    event events[16];
    size_t nevents;
    publication publications[128];
    size_t npublications, published;
    unsigned work[128];
    size_t nwork;
    unsigned released[32], nreleased;
    uint64_t fence;
} host;
static event *event_find(host *h, uint64_t id) {
    for (size_t i = 0; i < h->nevents; i++) {
        if (h->events[i].id == id) {
            return &h->events[i];
        }
    }
    return NULL;
}
static void event_add(host *h, uint64_t id, uint32_t value) {
    CHECK("event table has space", h->nevents < 16);
    if (h->nevents < 16) {
        h->events[h->nevents++] = (event){id, value, value};
    }
}
static int event_read(void *ctx, uint64_t id, uint32_t *value) {
    event *e = event_find(ctx, id);
    if (!e) {
        return 0;
    }
    *value = e->projected;
    return 1;
}
static void release(void *ctx, nvn_qs_ticket ticket, void *data) {
    host *h = ctx;
    payload *p = data;
    CHECK("release has a logical queue position", ticket.queue && ticket.sequence);
    if (p) {
        CHECK("release id has space", p->id < 32);
        if (p->id < 32) {
            CHECK("payload released once", ++h->released[p->id] == 1);
        }
        free(p);
    }
    h->nreleased++;
}
static nvn_qs_ticket enqueue(nvn_qs *s, uint64_t q, const nvn_qs_step *p, size_t n, unsigned id) {
    payload *owned = malloc(sizeof *owned);
    nvn_qs_ticket t = {0};
    CHECK("submission allocation", owned != NULL);
    if (!owned) {
        return t;
    }
    owned->id = id;
    int ok = nvn_qs_enqueue(s, q, p, n, owned, &t);
    CHECK("enqueue succeeds", ok);
    if (!ok) {
        free(owned);
    }
    return t;
}
static int run_one(nvn_qs *s, host *h) {
    nvn_qs_item it;
    if (!nvn_qs_peek(s, &it)) {
        return 0;
    }
    if (it.step.kind == NVN_QS_WORK) {
        CHECK("work log has space", h->nwork < 128);
        if (h->nwork < 128) {
            h->work[h->nwork++] = (unsigned)(uintptr_t)it.step.payload;
        }
        h->fence++;
    } else if (it.step.kind == NVN_QS_SIGNAL) {
        event *e = event_find(h, it.step.event);
        CHECK("signal names known event", e != NULL);
        if (!e) {
            return 0;
        }
        CHECK("signal arithmetic valid", nvn_qs_signal_value(e->projected, it.step.mode, it.step.value, &e->projected));
        CHECK("publication log has space", h->npublications < 128);
        if (h->npublications < 128) {
            h->publications[h->npublications++] = (publication){++h->fence, e->id, e->projected};
        }
    } else if (it.step.kind == NVN_QS_END) {
        h->fence++;
    }
    CHECK("advance offered step", nvn_qs_advance(s, &it, h->fence));
    return 1;
}
static void drain(nvn_qs *s, host *h) {
    unsigned steps = 0;
    while (steps < 500 && run_one(s, h)) {
        steps++;
    }
    CHECK("bounded scheduler progress", steps < 500);
}
static size_t complete(nvn_qs *s, host *h, uint64_t fence) {
    while (h->published < h->npublications && h->publications[h->published].fence <= fence) {
        const publication *p = &h->publications[h->published++];
        event_find(h, p->event)->visible = p->value;
    }
    return nvn_qs_complete(s, fence);
}
static nvn_qs_step work(unsigned id) {
    return (nvn_qs_step){.kind = NVN_QS_WORK, .payload = (void *)(uintptr_t)id};
}
static nvn_qs_step wait_for(uint64_t e, uint32_t mode, uint32_t v) {
    return (nvn_qs_step){.kind = NVN_QS_WAIT, .mode = mode, .event = e, .value = v};
}
static nvn_qs_step signal_step(uint64_t e, uint32_t mode, uint32_t v) {
    return (nvn_qs_step){.kind = NVN_QS_SIGNAL, .mode = mode, .event = e, .value = v};
}

static void predicates(void) {
    struct {
        uint32_t actual, mode, target;
        int ready;
    } cases[] = {{0, NVN_QS_EQUAL, 0, 1},
                 {2, NVN_QS_EQUAL, 1, 0},
                 {0, NVN_QS_EQUAL, UINT32_MAX, 0},
                 {1, NVN_QS_GEQUAL_WRAP, 1, 1},
                 {2, NVN_QS_GEQUAL_WRAP, 1, 1},
                 {0, NVN_QS_GEQUAL_WRAP, 1, 0},
                 {0, NVN_QS_GEQUAL_WRAP, UINT32_MAX, 1},
                 {UINT32_MAX, NVN_QS_GEQUAL_WRAP, 0, 0},
                 {0x7fffffff, NVN_QS_GEQUAL_WRAP, 0, 1},
                 {0x80000000, NVN_QS_GEQUAL_WRAP, 0, 0},
                 {0, NVN_QS_GEQUAL_WRAP, 0x80000000, 0},
                 {0, 99, 0, 0}};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        CHECK("wait predicate boundary",
              nvn_qs_wait_ready(cases[i].actual, cases[i].mode, cases[i].target) == cases[i].ready);
    }
    uint32_t after = 77;
    CHECK("WRITE replaces", nvn_qs_signal_value(17, NVN_QS_WRITE, 3, &after) && after == 3);
    CHECK("ADD wraps at 32 bits", nvn_qs_signal_value(UINT32_MAX, NVN_QS_ADD, 2, &after) && after == 1);
    CHECK("invalid signal mode preserves output", !nvn_qs_signal_value(0, 99, 0, &after) && after == 1);
    CHECK("null signal output rejected", !nvn_qs_signal_value(0, NVN_QS_WRITE, 0, NULL));
}
static void water_forward_wait(void) {
    // Captured dependency: q6 waits for base event >=2 before compute; q1 is
    // submitted later, rasterizes the base, then signals 2. q1 also waits on q6.
    const uint64_t base = UINT64_C(0x7424cc7000), ack = UINT64_C(0x7424cc7040);
    host h = {0};
    event_add(&h, base, 0);
    event_add(&h, ack, 0);
    nvn_qs *s = nvn_qs_create(event_read, release, &h);
    CHECK("create", s != NULL);
    nvn_qs_step a[] = {wait_for(base, NVN_QS_GEQUAL_WRAP, 1), signal_step(ack, NVN_QS_WRITE, 1),
                       wait_for(base, NVN_QS_GEQUAL_WRAP, 2), work(COMPUTE), signal_step(ack, NVN_QS_WRITE, 3)};
    nvn_qs_step later[] = {work(LATER)};
    nvn_qs_ticket ta = enqueue(s, 6, a, 5, 1), tl = enqueue(s, 6, later, 1, 2);
    nvn_qs_item it;
    CHECK("forward wait yields instead of blocking caller", !nvn_qs_peek(s, &it));
    CHECK("blocked head also holds next submit", nvn_qs_pending(s) == 2 && h.nwork == 0);
    CHECK("tail includes blocked later submit",
          nvn_qs_tail(s, 6).sequence == tl.sequence && tl.sequence == ta.sequence + 1);
    uint64_t fence = 99;
    CHECK("blocked ticket is not yet scheduled", !nvn_qs_scheduled(s, ta) && !nvn_qs_scheduled(s, tl));
    CHECK("blocked ticket has no GPU fence", !nvn_qs_fence(s, ta, &fence) && fence == 0);
    CHECK("cannot destroy retained work", !nvn_qs_destroy(s));
    nvn_qs_step b[] = {signal_step(base, NVN_QS_WRITE, 1), work(BASE), signal_step(base, NVN_QS_WRITE, 2),
                       wait_for(ack, NVN_QS_GEQUAL_WRAP, 3), work(AFTER)};
    nvn_qs_ticket tb = enqueue(s, 1, b, 5, 3);
    drain(s, &h);
    CHECK("producer base runs before water compute", h.nwork == 4 && h.work[0] == BASE && h.work[1] == COMPUTE);
    CHECK("consumer queue FIFO and reverse dependency", h.work[2] == LATER && h.work[3] == AFTER);
    CHECK("scheduling signal does not publish CPU event",
          event_find(&h, base)->visible == 0 && event_find(&h, ack)->visible == 0);
    CHECK("dequeued submissions retain GPU lifetime", nvn_qs_pending(s) == 3 && h.nreleased == 0);
    CHECK("logical fence waits for GPU completion", !nvn_qs_done(s, ta) && !nvn_qs_done(s, tb) && !nvn_qs_done(s, tl));
    CHECK("scheduled prefix exposes fence before completion",
          nvn_qs_scheduled(s, ta) && nvn_qs_fence(s, ta, &fence) && fence > 0 && fence <= h.fence);
    CHECK("all submissions retire", complete(s, &h, h.fence) == 3);
    CHECK("CPU signals now visible", event_find(&h, base)->visible == 2 && event_find(&h, ack)->visible == 3);
    CHECK("completed tickets survive payload release", nvn_qs_done(s, ta) && nvn_qs_done(s, tl) && nvn_qs_done(s, tb));
    CHECK("duplicate completion never repeats release", complete(s, &h, h.fence) == 0 && h.nreleased == 3);
    CHECK("destroy drained scheduler", nvn_qs_destroy(s));
}
static void handshake(void) {
    host h = {0};
    event_add(&h, 10, 0);
    event_add(&h, 20, 0);
    nvn_qs *s = nvn_qs_create(event_read, release, &h);
    nvn_qs_step a[10], b[10];
    for (uint32_t i = 0; i < 5; i++) {
        a[2 * i] = wait_for(10, NVN_QS_GEQUAL_WRAP, i + 1);
        a[2 * i + 1] = signal_step(20, NVN_QS_WRITE, i + 1);
        b[2 * i] = signal_step(10, NVN_QS_WRITE, i + 1);
        b[2 * i + 1] = wait_for(20, NVN_QS_GEQUAL_WRAP, i + 1);
    }
    nvn_qs_ticket ta = enqueue(s, 6, a, 10, 1), tb = enqueue(s, 1, b, 10, 2);
    drain(s, &h);
    CHECK("interleaved handshake reaches both tails",
          h.npublications == 10 && event_find(&h, 10)->projected == 5 && event_find(&h, 20)->projected == 5);
    CHECK("prefix signals progress before whole submit completes", h.nreleased == 0);
    complete(s, &h, h.fence);
    CHECK("both handshake fences complete", nvn_qs_done(s, ta) && nvn_qs_done(s, tb));
    CHECK("destroy handshake", nvn_qs_destroy(s));
}
static void equality_and_unknown(void) {
    host h = {0};
    event_add(&h, 10, 2);
    nvn_qs *s = nvn_qs_create(event_read, release, &h);
    nvn_qs_step a[] = {wait_for(10, NVN_QS_EQUAL, 1), work(AFTER)};
    enqueue(s, 1, a, 2, 1);
    nvn_qs_item it, again;
    CHECK("EQUAL overshoot does not satisfy wait", !nvn_qs_peek(s, &it));
    event_find(&h, 10)->projected = 1;
    CHECK("CPU reset can make wait ready", nvn_qs_peek(s, &it) && it.step.kind == NVN_QS_WAIT);
    event_find(&h, 10)->projected = 2;
    CHECK("successful wait is latched",
          nvn_qs_peek(s, &again) && again.dispatch == it.dispatch && again.index == it.index);
    CHECK("latched wait advances after subsequent event change", nvn_qs_advance(s, &it, 0));
    CHECK("stale item rejected", !nvn_qs_advance(s, &it, 0));
    drain(s, &h);
    complete(s, &h, h.fence);
    nvn_qs_step unknown[] = {wait_for(99, NVN_QS_EQUAL, 0)};
    enqueue(s, 1, unknown, 1, 2);
    CHECK("unknown event is not assumed zero", !nvn_qs_peek(s, &it));
    event_add(&h, 99, 0);
    drain(s, &h);
    complete(s, &h, h.fence);
    CHECK("destroy equality scheduler", nvn_qs_destroy(s));
}
static void cyclic_waits(void) {
    host h = {0};
    event_add(&h, 10, 0);
    event_add(&h, 20, 0);
    nvn_qs *s = nvn_qs_create(event_read, release, &h);
    nvn_qs_step a[] = {wait_for(20, NVN_QS_EQUAL, 1), signal_step(10, NVN_QS_WRITE, 1)};
    nvn_qs_step b[] = {wait_for(10, NVN_QS_EQUAL, 1), signal_step(20, NVN_QS_WRITE, 1)};
    enqueue(s, 1, a, 2, 1);
    enqueue(s, 2, b, 2, 2);
    nvn_qs_item it;
    CHECK("genuine cycle remains blocked", !nvn_qs_peek(s, &it) && nvn_qs_pending(s) == 2);
    CHECK("cycle does not discard work", !nvn_qs_destroy(s));
    event_find(&h, 20)->projected = 1; // an external CPU signal resolves it
    drain(s, &h);
    complete(s, &h, h.fence);
    CHECK("external producer resolves cycle", nvn_qs_pending(s) == 0);
    CHECK("destroy cycle scheduler", nvn_qs_destroy(s));
}
static void lifetime_and_empty(void) {
    host h = {0};
    nvn_qs *s = nvn_qs_create(NULL, release, &h);
    nvn_qs_item it;
    nvn_qs_ticket t = enqueue(s, 3, NULL, 0, 1);
    CHECK("empty submission still has END", nvn_qs_peek(s, &it) && it.step.kind == NVN_QS_END);
    CHECK("empty submit can fence earlier GPU work", nvn_qs_advance(s, &it, 7));
    CHECK("earlier GPU completion retains payload",
          nvn_qs_complete(s, 6) == 0 && !nvn_qs_done(s, t) && h.nreleased == 0);
    CHECK("matching GPU completion releases payload", nvn_qs_complete(s, 7) == 1 && nvn_qs_done(s, t));
    CHECK("late callback cannot move watermark backward", nvn_qs_complete(s, 2) == 0 && nvn_qs_done(s, t));
    nvn_qs_ticket t2 = enqueue(s, 3, NULL, 0, 2);
    CHECK("queue sequence survives retirement", t2.sequence == t.sequence + 1);
    CHECK("completed old fence excludes later pending submit", nvn_qs_done(s, t) && !nvn_qs_done(s, t2));
    CHECK("peek second empty END", nvn_qs_peek(s, &it));
    CHECK("no-GPU empty END retires immediately", nvn_qs_advance(s, &it, 0) && nvn_qs_done(s, t2));
    CHECK("unknown queue empty prefix is done", nvn_qs_done(s, nvn_qs_tail(s, 999)));
    CHECK("unknown nonempty ticket is not done", !nvn_qs_done(s, (nvn_qs_ticket){999, 1}));
    CHECK("future ticket is not done", !nvn_qs_done(s, (nvn_qs_ticket){3, t2.sequence + 1}));
    CHECK("destroy lifetime scheduler", nvn_qs_destroy(s));
}
static void descriptor_and_fence_validation(void) {
    host h = {0};
    nvn_qs *s = nvn_qs_create(NULL, release, &h);
    nvn_qs_step p = work(BASE);
    nvn_qs_ticket t = enqueue(s, 9, &p, 1, 1);
    p.payload = (void *)(uintptr_t)COMPUTE;
    nvn_qs_item it, invalid;
    CHECK("enqueue owns descriptor copy", nvn_qs_peek(s, &it) && (uintptr_t)it.step.payload == BASE);
    invalid = it;
    invalid.ticket.queue++;
    CHECK("wrong queue item rejected", !nvn_qs_advance(s, &invalid, 5));
    invalid = it;
    invalid.ticket.sequence++;
    CHECK("wrong sequence item rejected", !nvn_qs_advance(s, &invalid, 5));
    invalid = it;
    invalid.index++;
    CHECK("wrong position rejected", !nvn_qs_advance(s, &invalid, 5));
    CHECK("valid WORK consumes", nvn_qs_advance(s, &it, 5));
    CHECK("END is offered", nvn_qs_peek(s, &it) && it.step.kind == NVN_QS_END);
    CHECK("decreasing fence rejected without consuming END", !nvn_qs_advance(s, &it, 4));
    CHECK("zero fence preserves previous work's fence", nvn_qs_advance(s, &it, 0));
    CHECK("previous GPU work still protects payload", nvn_qs_complete(s, 4) == 0 && !nvn_qs_done(s, t));
    CHECK("previous GPU fence retires payload", nvn_qs_complete(s, 5) == 1 && nvn_qs_done(s, t));
    CHECK("destroy validation scheduler", nvn_qs_destroy(s));
}
static void fence_queries(void) {
    host h = {0};
    nvn_qs *s = nvn_qs_create(NULL, release, &h);
    nvn_qs_item it;
    uint64_t fence = 99;
    nvn_qs_ticket a = enqueue(s, 4, NULL, 0, 1), b = enqueue(s, 4, NULL, 0, 2);
    CHECK("first END offered", nvn_qs_peek(s, &it) && it.ticket.sequence == a.sequence);
    CHECK("first prefix committed", nvn_qs_advance(s, &it, 10));
    CHECK("first prefix scheduled with exact fence",
          nvn_qs_scheduled(s, a) && nvn_qs_fence(s, a, &fence) && fence == 10);
    CHECK("later prefix not yet scheduled", !nvn_qs_scheduled(s, b) && !nvn_qs_fence(s, b, &fence) && fence == 0);
    CHECK("second END offered", nvn_qs_peek(s, &it) && it.ticket.sequence == b.sequence);
    CHECK("empty prefix scheduled without new GPU work", nvn_qs_advance(s, &it, 0));
    CHECK("empty prefix inherits earlier queue fence", nvn_qs_fence(s, b, &fence) && fence == 10);
    CHECK("neither prefix completes early", nvn_qs_complete(s, 9) == 0 && !nvn_qs_done(s, b));
    CHECK("both prefixes retire together", nvn_qs_complete(s, 10) == 2 && nvn_qs_done(s, b));
    CHECK("retired prefix remains scheduled and queryable",
          nvn_qs_scheduled(s, a) && nvn_qs_fence(s, a, &fence) && fence == 10);
    nvn_qs_ticket c = enqueue(s, 4, NULL, 0, 3);
    CHECK("third END offered", nvn_qs_peek(s, &it));
    CHECK("third prefix committed", nvn_qs_advance(s, &it, 17));
    CHECK("later scheduled work cannot raise old completed bound", nvn_qs_fence(s, a, &fence) && fence == 10);
    CHECK("third prefix retires", nvn_qs_complete(s, 17) == 1 && nvn_qs_done(s, c));
    CHECK("old retired query returns safe completed watermark", nvn_qs_fence(s, a, &fence) && fence == 17);
    CHECK("empty nonexistent prefix is scheduled", nvn_qs_scheduled(s, (nvn_qs_ticket){8, 0}));
    CHECK("empty nonexistent prefix has zero fence", nvn_qs_fence(s, (nvn_qs_ticket){8, 0}, &fence) && fence == 0);
    CHECK("unknown ticket is unscheduled", !nvn_qs_scheduled(s, (nvn_qs_ticket){8, 1}));
    CHECK("unknown ticket query clears output", !nvn_qs_fence(s, (nvn_qs_ticket){8, 1}, &fence) && fence == 0);
    CHECK("missing fence output rejected", !nvn_qs_fence(s, a, NULL));
    CHECK("destroy fence-query scheduler", nvn_qs_destroy(s));
}
static void invalid_inputs(void) {
    host h = {0};
    nvn_qs *s = nvn_qs_create(NULL, release, &h);
    nvn_qs_ticket t = {7, 7};
    nvn_qs_step p = work(BASE);
    nvn_qs_item it;
    CHECK("null scheduler rejected", !nvn_qs_enqueue(NULL, 1, &p, 1, NULL, &t) && !t.queue && !t.sequence);
    CHECK("zero queue serial rejected", !nvn_qs_enqueue(s, 0, &p, 1, NULL, &t));
    CHECK("missing descriptors rejected", !nvn_qs_enqueue(s, 1, NULL, 1, NULL, &t));
    CHECK("allocation size overflow rejected before reading", !nvn_qs_enqueue(s, 1, &p, SIZE_MAX, NULL, &t));
    CHECK("missing ticket rejected", !nvn_qs_enqueue(s, 1, &p, 1, NULL, NULL));
    p.kind = NVN_QS_END;
    CHECK("explicit END rejected", !nvn_qs_enqueue(s, 1, &p, 1, NULL, &t));
    p = wait_for(0, 2, 0);
    CHECK("unknown wait mode rejected", !nvn_qs_enqueue(s, 1, &p, 1, NULL, &t));
    p = signal_step(0, 2, 0);
    CHECK("unknown signal mode rejected", !nvn_qs_enqueue(s, 1, &p, 1, NULL, &t));
    CHECK("failed enqueue transfers no ownership",
          nvn_qs_pending(s) == 0 && h.nreleased == 0 && nvn_qs_tail(s, 1).sequence == 0);
    CHECK("peek empty scheduler", !nvn_qs_peek(s, &it));
    CHECK("advance without offered item rejected", !nvn_qs_advance(s, &it, 0));
    CHECK("null output rejected", !nvn_qs_peek(s, NULL));
    CHECK("destroy empty scheduler", nvn_qs_destroy(s));
    CHECK("destroy null scheduler", nvn_qs_destroy(NULL));
}
int main(void) {
    predicates();
    water_forward_wait();
    handshake();
    equality_and_unknown();
    cyclic_waits();
    lifetime_and_empty();
    descriptor_and_fence_validation();
    fence_queries();
    invalid_inputs();
    printf("queue scheduling: %u checks, %u failed\n", checks, failed);
    return failed != 0;
}
