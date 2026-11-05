// Guest queues share one Metal queue, but a forward event wait can suspend a
// guest submission until another guest queue supplies its producer. Never block
// the submitting CPU thread: it may itself submit that producer next.
#include "nvn_capture.h"

typedef struct qe_value { struct qe_value *next; uint64_t address, fence; uint32_t value; } qe_value;
typedef struct qe_work {
    struct qe_work *next;
    uint8_t *bytes; int first, origin;
    uint64_t signal; uint32_t mode, value;
    mtl_submit_clock *clock;   // borrowed from the logical submission
    mtl_submit_alloc *alloc;   // likewise; serialized by the submission lock
    win_t *window; int image, crop[4]; tex_t texture; double period;
} qe_work;
typedef struct { qe_work *head, *tail; mtl_submit_clock *clock; mtl_submit_alloc *alloc; } qe_submission;
static nvn_qs *g_qs;
static qe_value *g_qe_values;
static pthread_cond_t g_qe_cv = PTHREAD_COND_INITIALIZER;
static void *qe_worker(void *unused);

static qe_value *qe_value_find(uint64_t address, int add) {
    qe_value *e = g_qe_values;
    while (e && e->address != address) e = e->next;
    if (!e && add) {
        e = calloc(1, sizeof *e); if (!e) die("NVN event state allocation failed");
        e->address = address; e->next = g_qe_values; g_qe_values = e;
    }
    return e;
}
static int qe_read(void *ctx, uint64_t address, uint32_t *value) {
    (void)ctx;
    if (address & 1) { // a sync wait: all work through its fence must be ordered
        const sync_t *s = (const sync_t *)(uintptr_t)(address & ~1ull);
        *value = s->fenced && s->fenced != 4 && (s->fenced != 3 || nvn_qs_scheduled(g_qs, (nvn_qs_ticket){s->queue, s->fence}));
        return 1;
    }
    qe_value *e = qe_value_find(address, 0);
    // Once post-operations have landed, the mapped word is authoritative again;
    // this includes CPU stores made directly through the memory pool.
    if (e && e->fence > mtl_completed_fence()) *value = e->value;
    else *value = *(volatile uint32_t *)(uintptr_t)address;
    return 1;
}
static void qe_release(void *ctx, nvn_qs_ticket ticket, void *payload) {
    (void)ctx; (void)ticket;
    qe_submission *s = payload;
    if (!s) return;
    for (qe_work *w = s->head, *next; w; w = next) { next = w->next; free(w->bytes); free(w); }
    mtl_submit_clock_release(s->clock); mtl_submit_alloc_release(s->alloc); free(s);
}
static void qe_init(void) {
    if (!g_qs) {
        g_qs = nvn_qs_create(qe_read, qe_release, NULL);
        if (!g_qs) die("NVN queue scheduler allocation failed");
        pthread_t thread; pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);
        int err = pthread_create(&thread, &attr, qe_worker, NULL); pthread_attr_destroy(&attr);
        if (err) die("NVN queue worker creation failed: %d", err); pthread_detach(thread);
        LOG("[nvn] event queue ordering enabled: blocked guest queues yield to their producers");
    }
}
static void qe_pump(void) { // submission lock and graphics gate held
    if (!g_qs) return;
    mtl_encode_batch_begin();
    nvn_qs_item item;
    while (nvn_qs_peek(g_qs, &item)) {
        uint64_t fence = 0;
        if (item.step.kind == NVN_QS_WORK) {
            qe_work *w = item.step.payload;
            if (w->window) {
                // The present thread may submit directly to the Metal queue.
                // All earlier deferred commits must reach it before handoff.
                mtl_encode_batch_flush();
                w->window->ticket[w->image] = mtl_present(&w->texture, w->period, w->crop);
            } else {
                uint32_t signal_value = 0;
                if (w->signal) {
                    uint32_t before; qe_read(NULL, w->signal, &before);
                    if (!nvn_qs_signal_value(before, w->mode, w->value, &signal_value)) die("NVN invalid event signal mode");
                }
                mtl_queue_activate(item.ticket.queue);
                mtl_set_queue_origin_lower_left(w->origin);
                mtl_sub_begin_alloc(w->alloc, !w->first);
                mtl_sub_set_clock(w->clock);
                uint64_t handle = (uint64_t)w->bytes;
                mtl_sub_index(&handle, 1); mtl_exec_rest(NULL, 0); mtl_exec(w->bytes);
                mtl_sub_commit(); mtl_queue_seal(); fence = mtl_last_fence();
                if (!w->next) mtl_submit_alloc_close(w->alloc);
                if (w->signal) {
                    qe_value *e = qe_value_find(w->signal, 1);
                    e->value = signal_value; e->fence = fence;
                }
            }
        }
        // WAIT was satisfied at the ordered scheduling point. SIGNAL's WORK
        // already committed its producer and published the projected value.
        if (!nvn_qs_advance(g_qs, &item, fence)) die("NVN queue scheduler lost its offered step");
    }
    mtl_encode_batch_end();
    nvn_qs_complete(g_qs, mtl_completed_fence());
    pthread_cond_broadcast(&g_qe_cv);
}

static void *qe_worker(void *unused) {
    (void)unused;
    pthread_setname_np("NVN event queues");
    pthread_mutex_lock(&g_sub_mu);
    for (;;) {
        if (nvn_qs_pending(g_qs)) {
            // QueueSubmit returns even when every guest queue is blocked. Keep
            // forward waits progressing when a CPU directly stores an event and
            // then polls downstream work without making another NVN submission.
            dm_host_work++; mtl_prepare(); dm_host_work--;
            gfx_enter(); qe_pump(); gfx_leave();
            struct timespec delay = {0, 1000000};
            pthread_cond_timedwait_relative_np(&g_qe_cv, &g_sub_mu, &delay);
        } else pthread_cond_wait(&g_qe_cv, &g_sub_mu);
    }
    return NULL;
}

typedef struct { size_t offset; const uint8_t *source; uint32_t size; } qe_record;
typedef struct {
    qe_submission *submission; nvn_qs_step *steps; size_t count, cap;
    uint8_t *bytes; size_t size, capacity;
    qe_record *records; size_t nrecords, rcap;
    int first, origin;
} qe_builder;
static void qe_step(qe_builder *b, nvn_qs_step step) {
    if (b->count == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 32;
        nvn_qs_step *steps = realloc(b->steps, b->cap * sizeof *b->steps);
        if (!steps) die("NVN queue step allocation failed");
        b->steps = steps;
    }
    b->steps[b->count++] = step;
}
static int qe_reloc_addr(void *ctx, uint64_t *field, uint64_t len, int writable) { (void)ctx; (void)field; (void)len; (void)writable; return 1; }
static int qe_reloc_obj(void *ctx, int kind, uint64_t *field, uint8_t *payload, uint32_t size) {
    (void)kind; (void)size;
    const qe_record *r = ctx; uint64_t address; memcpy(&address, field, 8);
    uintptr_t source = (uintptr_t)r->source;
    if (address >= source && address - source < r->size) {
        address = (uint64_t)(payload - 8) + (address - source); memcpy(field, &address, 8);
    }
    return 1;
}
static void qe_flush(qe_builder *b, uint64_t signal, uint32_t mode, uint32_t value) {
    if (!b->size) return;
    qe_work *w = calloc(1, sizeof *w); if (!w) die("NVN queue chunk allocation failed");
    w->bytes = malloc(b->size + 8); if (!w->bytes) die("NVN queue command allocation failed");
    memcpy(w->bytes, b->bytes, b->size); put_hdr(w->bytes + b->size, OP_END, 8);
    const cap_ops reloc = {qe_reloc_addr, qe_reloc_obj};
    for (size_t i = 0; i < b->nrecords; i++) {
        const qe_record *r = &b->records[i]; uint8_t *p = w->bytes + r->offset; op_hdr h; memcpy(&h, p, 8);
        if (!cap_rec_fields(h.op, p + 8, h.size, &reloc, (void *)r)) die("NVN queue cannot relocate opcode %u", h.op);
    }
    w->first = b->first; b->first = 0; w->origin = b->origin;
    if (!b->submission->clock) b->submission->clock = mtl_submit_clock_new();
    w->clock = b->submission->clock;
    if (!b->submission->alloc) b->submission->alloc = mtl_submit_alloc_new();
    w->alloc = b->submission->alloc;
    w->signal = signal; w->mode = mode; w->value = value;
    if (b->submission->tail) b->submission->tail->next = w; else b->submission->head = w;
    b->submission->tail = w;
    qe_step(b, (nvn_qs_step){.kind = NVN_QS_WORK, .payload = w});
    b->size = b->nrecords = 0;
}
static void qe_record_copy(qe_builder *b, const uint8_t *p, uint32_t size) {
    if (b->size + size > b->capacity) {
        b->capacity = (b->size + size) * 2;
        uint8_t *bytes = realloc(b->bytes, b->capacity);
        if (!bytes) die("NVN queue recording allocation failed");
        b->bytes = bytes;
    }
    if (b->nrecords == b->rcap) {
        b->rcap = b->rcap ? b->rcap * 2 : 256;
        qe_record *records = realloc(b->records, b->rcap * sizeof *b->records);
        if (!records) die("NVN queue relocation allocation failed");
        b->records = records;
    }
    b->records[b->nrecords++] = (qe_record){b->size, p, size};
    memcpy(b->bytes + b->size, p, size); b->size += size;
}
static void qe_submit(const queue_t *q, int n, const uint64_t *handles) {
    qe_init();
    qe_builder b = {.submission = calloc(1, sizeof(qe_submission)), .first = 1, .origin = q->origin_ll};
    if (!b.submission) die("NVN queue submission allocation failed");
    for (int hi = 0; hi < n; hi++) {
        const uint8_t *p = (const uint8_t *)handles[hi], *returns[64]; unsigned depth = 0;
        size_t records = 0;
        for (;;) {
            op_hdr h; memcpy(&h, p, 8);
            if (h.size < 8 || (h.size & 7) || h.size > 64 * 1024 * 1024 || h.op >= OP_COUNT_ || ++records > 10000000) die("NVN invalid scheduled command stream");
            if (h.op == OP_END) { if (!depth) break; p = returns[--depth]; continue; }
            if (h.op == OP_CALL || h.op == OP_JUMP) {
                if (h.size < 16) die("NVN short command link");
                if (h.op == OP_CALL) { if (depth == 64) die("NVN command call nesting overflow"); returns[depth++] = p + h.size; }
                memcpy(&p, p + 8, 8); continue;
            }
            if (h.op == OP_WAIT_EVENT || h.op == OP_WAIT_SYNC) {
                qe_flush(&b, 0, 0, 0);
                uint64_t address; uint32_t mode = NVN_QS_EQUAL, value = 1; memcpy(&address, p + 8, 8);
                if (h.op == OP_WAIT_EVENT) { memcpy(&mode, p + 16, 4); memcpy(&value, p + 20, 4); }
                else address |= 1;
                qe_step(&b, (nvn_qs_step){.kind = NVN_QS_WAIT, .event = address, .mode = mode, .value = value});
            } else {
                qe_record_copy(&b, p, h.size);
                if (h.op == OP_SIGNAL_EVENT) {
                    uint64_t address; uint32_t mode, value; memcpy(&address, p + 8, 8); memcpy(&mode, p + 16, 4); memcpy(&value, p + 20, 4);
                    qe_flush(&b, address, mode, value);
                    qe_step(&b, (nvn_qs_step){.kind = NVN_QS_SIGNAL, .event = address, .mode = mode, .value = value});
                } else if (h.op == OP_FENCE_SYNC) {
                    sync_t *sync; memcpy(&sync, p + 8, 8); sync->fence = 0; sync->fenced = 4;
                    qe_flush(&b, 0, 0, 0);
                }
            }
            p += h.size;
        }
    }
    qe_flush(&b, 0, 0, 0);
    nvn_qs_ticket ticket;
    if (!nvn_qs_enqueue(g_qs, q->serial, b.steps, b.count, b.submission, &ticket)) die("NVN queue enqueue failed");
    free(b.steps); free(b.bytes); free(b.records); qe_pump();
}
static void qe_queue_fence(const queue_t *q, sync_t *s) {
    dm_host_mutex_lock(&g_sub_mu);
    if (g_qs && nvn_qs_tail(g_qs, q->serial).sequence) { nvn_qs_ticket t = nvn_qs_tail(g_qs, q->serial); s->fenced = 3; s->fence = t.sequence; s->queue = t.queue; }
    else { s->fenced = 1; s->fence = mtl_last_fence(); }
    pthread_mutex_unlock(&g_sub_mu);
}
static int qe_wait(nvn_qs_ticket t, uint64_t timeout, int completed) {
    uint64_t start = now_ns(), fence = 0;
    dm_host_mutex_lock(&g_sub_mu);
    for (;;) {
        gfx_enter(); qe_pump(); gfx_leave();
        if (nvn_qs_fence(g_qs, t, &fence)) break;
        uint64_t elapsed = now_ns() - start;
        if (timeout != UINT64_MAX && elapsed >= timeout) { pthread_mutex_unlock(&g_sub_mu); return 0; }
        // New submissions and CPU signals wake this wait. The bounded fallback
        // also observes mapped event stores made without an NVN API call.
        uint64_t ns = timeout == UINT64_MAX ? 1000000 : timeout - elapsed;
        if (ns > 1000000) ns = 1000000;
        struct timespec delay = {0, (long)ns}; pthread_cond_timedwait_relative_np(&g_qe_cv, &g_sub_mu, &delay);
    }
    pthread_mutex_unlock(&g_sub_mu);
    if (!completed) return 1;
    uint64_t elapsed = now_ns() - start;
    return mtl_wait(fence, timeout == UINT64_MAX ? timeout : elapsed < timeout ? timeout - elapsed : 0);
}
static void qe_finish(const queue_t *q) {
    dm_host_mutex_lock(&g_sub_mu); nvn_qs_ticket t = g_qs ? nvn_qs_tail(g_qs, q->serial) : (nvn_qs_ticket){0}; pthread_mutex_unlock(&g_sub_mu);
    if (t.sequence) qe_wait(t, UINT64_MAX, 1); else mtl_finish();
}
static void qe_cpu_signal(const ev_t *e, int mode, uint32_t value) {
    dm_host_mutex_lock(&g_sub_mu);
    qe_value *v = qe_value_find((uint64_t)e->val, 0);
    // Complete an older queued write before resetting the same CPU-visible word.
    // The Metal post-operation mutex alone cannot establish that order.
    if (v && v->fence > mtl_completed_fence()) mtl_wait(v->fence, UINT64_MAX);
    ev_signal(e, mode, value);
    gfx_enter(); qe_pump(); gfx_leave(); pthread_mutex_unlock(&g_sub_mu);
}
static void qe_sync_wait(const queue_t *q, const sync_t *s) {
    dm_host_mutex_lock(&g_sub_mu); qe_init();
    nvn_qs_step step = {.kind = NVN_QS_WAIT, .event = (uint64_t)s | 1, .mode = NVN_QS_EQUAL, .value = 1}; nvn_qs_ticket ticket;
    if (!nvn_qs_enqueue(g_qs, q->serial, &step, 1, NULL, &ticket)) die("NVN sync wait enqueue failed");
    gfx_enter(); qe_pump(); gfx_leave(); pthread_mutex_unlock(&g_sub_mu);
}
static void qe_present(const queue_t *q, win_t *window, int image, const tex_t *texture, double period, const int crop[4]) {
    qe_submission *s = calloc(1, sizeof *s); qe_work *w = calloc(1, sizeof *w);
    if (!s || !w) die("NVN queued present allocation failed");
    s->head = s->tail = w; w->window = window; w->image = image; w->texture = *texture; w->period = period;
    if (crop) memcpy(w->crop, crop, sizeof w->crop);
    nvn_qs_step step = {.kind = NVN_QS_WORK, .payload = w}; nvn_qs_ticket ticket;
    if (!nvn_qs_enqueue(g_qs, q->serial, &step, 1, s, &ticket)) die("NVN present enqueue failed");
    window->submit_queue[image] = ticket.queue; window->submit_seq[image] = ticket.sequence;
    qe_pump();
}

static int qe_wait_recorded(const sync_t *s, uint64_t timeout) {
    uint64_t start = now_ns(), fence;
    dm_host_mutex_lock(&g_sub_mu);
    while (s->fenced == 4) {
        gfx_enter(); qe_pump(); gfx_leave();
        if (s->fenced != 4) break;
        uint64_t elapsed = now_ns() - start;
        if (timeout != UINT64_MAX && elapsed >= timeout) { pthread_mutex_unlock(&g_sub_mu); return 0; }
        uint64_t ns = timeout == UINT64_MAX ? 1000000 : timeout - elapsed;
        if (ns > 1000000) ns = 1000000;
        struct timespec delay = {0, (long)ns}; pthread_cond_timedwait_relative_np(&g_qe_cv, &g_sub_mu, &delay);
    }
    fence = s->fence; pthread_mutex_unlock(&g_sub_mu);
    uint64_t elapsed = now_ns() - start;
    return mtl_wait(fence, timeout == UINT64_MAX ? timeout : elapsed < timeout ? timeout - elapsed : 0);
}
static void qe_flush_queue(void) {
    dm_host_mutex_lock(&g_sub_mu); gfx_enter(); qe_pump(); gfx_leave(); pthread_mutex_unlock(&g_sub_mu);
}
