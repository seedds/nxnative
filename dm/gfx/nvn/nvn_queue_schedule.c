#include "nvn_queue_schedule.h"
#include <stdlib.h>
#include <string.h>

typedef struct qs_job {
    struct qs_job *next;
    uint64_t sequence, order, fence;
    size_t count, index;
    int finished;
    void *submission;
    nvn_qs_step steps[];
} qs_job;
typedef struct qs_queue {
    struct qs_queue *next;
    uint64_t serial, submitted, completed, scheduled, scheduled_fence, completed_fence;
    qs_job *head, *tail, *cursor;
} qs_queue;
struct nvn_qs {
    qs_queue *queues, *offered_queue;
    qs_job *offered;
    uint64_t order, dispatch, completed_fence;
    size_t pending;
    nvn_qs_read_event_fn read_event;
    nvn_qs_release_fn release;
    void *ctx;
};

int nvn_qs_wait_ready(uint32_t actual, uint32_t mode, uint32_t target) {
    if (mode == NVN_QS_EQUAL) return actual == target;
    if (mode == NVN_QS_GEQUAL_WRAP) return (uint32_t)(actual - target) < UINT32_C(0x80000000);
    return 0;
}
int nvn_qs_signal_value(uint32_t before, uint32_t mode, uint32_t value, uint32_t *after) {
    if (!after) return 0;
    if (mode == NVN_QS_WRITE) *after = value;
    else if (mode == NVN_QS_ADD) *after = before + value;
    else return 0;
    return 1;
}
static qs_queue *queue_find(const nvn_qs *s, uint64_t serial) {
    for (qs_queue *q = s->queues; q; q = q->next) if (q->serial == serial) return q;
    return NULL;
}
nvn_qs *nvn_qs_create(nvn_qs_read_event_fn read_event, nvn_qs_release_fn release, void *ctx) {
    nvn_qs *s = calloc(1, sizeof *s);
    if (s) { s->read_event = read_event; s->release = release; s->ctx = ctx; }
    return s;
}
int nvn_qs_destroy(nvn_qs *s) {
    if (!s) return 1;
    if (s->pending) return 0;
    while (s->queues) { qs_queue *q = s->queues; s->queues = q->next; free(q); }
    free(s);
    return 1;
}
int nvn_qs_enqueue(nvn_qs *s, uint64_t queue, const nvn_qs_step *steps, size_t count,
                   void *submission, nvn_qs_ticket *ticket) {
    if (ticket) *ticket = (nvn_qs_ticket){0};
    if (!s || !queue || !ticket || (count && !steps) || s->order == UINT64_MAX || s->pending == SIZE_MAX ||
        count > (SIZE_MAX - sizeof(qs_job)) / sizeof(nvn_qs_step)) return 0;
    for (size_t i = 0; i < count; i++) {
        const nvn_qs_step *p = &steps[i];
        if (p->kind > NVN_QS_SIGNAL || ((p->kind == NVN_QS_WAIT || p->kind == NVN_QS_SIGNAL) && p->mode > 1)) return 0;
    }
    qs_queue *q = queue_find(s, queue), *new_queue = NULL;
    if (q && q->submitted == UINT64_MAX) return 0;
    if (!q) {
        new_queue = calloc(1, sizeof *new_queue);
        if (!new_queue) return 0;
        q = new_queue; q->serial = queue;
    }
    qs_job *j = calloc(1, sizeof *j + count * sizeof *steps);
    if (!j) { free(new_queue); return 0; }
    if (count) memcpy(j->steps, steps, count * sizeof *steps);
    j->count = count; j->submission = submission;
    j->sequence = ++q->submitted; j->order = ++s->order;
    if (new_queue) { q->next = s->queues; s->queues = q; }
    if (q->tail) q->tail->next = j; else q->head = j;
    q->tail = j; if (!q->cursor) q->cursor = j;
    s->pending++; *ticket = (nvn_qs_ticket){queue, j->sequence};
    return 1;
}
static nvn_qs_step job_step(const qs_job *j) {
    return j->index == j->count ? (nvn_qs_step){.kind = NVN_QS_END} : j->steps[j->index];
}
static void offered_item(const nvn_qs *s, nvn_qs_item *item) {
    *item = (nvn_qs_item){.ticket = {s->offered_queue->serial, s->offered->sequence},
        .index = s->offered->index, .dispatch = s->dispatch, .step = job_step(s->offered)};
}
int nvn_qs_peek(nvn_qs *s, nvn_qs_item *item) {
    if (!item) return 0;
    memset(item, 0, sizeof *item);
    if (!s) return 0;
    if (s->offered) { offered_item(s, item); return 1; }
    qs_queue *best = NULL;
    for (qs_queue *q = s->queues; q; q = q->next) {
        qs_job *j = q->cursor;
        if (!j || (best && best->cursor->order < j->order)) continue;
        nvn_qs_step p = job_step(j);
        if (p.kind == NVN_QS_WAIT) {
            uint32_t value;
            if (!s->read_event || !s->read_event(s->ctx, p.event, &value) || !nvn_qs_wait_ready(value, p.mode, p.value)) continue;
        }
        best = q;
    }
    if (!best) return 0;
    s->offered_queue = best; s->offered = best->cursor;
    if (!++s->dispatch) ++s->dispatch;   // ticket + index also distinguish stale offers across wrap
    offered_item(s, item);
    return 1;
}
static size_t retire(nvn_qs *s) {
    size_t n = 0;
    for (qs_queue *q = s->queues; q; q = q->next) {
        while (q->head && q->head->finished && q->head->fence <= s->completed_fence) {
            qs_job *j = q->head;
            nvn_qs_ticket ticket = {q->serial, j->sequence}; void *payload = j->submission;
            q->head = j->next; if (!q->head) q->tail = NULL;
            q->completed = j->sequence; q->completed_fence = j->fence; s->pending--; n++;
            free(j);
            if (s->release) s->release(s->ctx, ticket, payload);
        }
    }
    return n;
}
int nvn_qs_advance(nvn_qs *s, const nvn_qs_item *item, uint64_t gpu_fence) {
    if (!s || !item || !s->offered || item->dispatch != s->dispatch ||
        item->ticket.queue != s->offered_queue->serial || item->ticket.sequence != s->offered->sequence ||
        item->index != s->offered->index || (gpu_fence && gpu_fence < s->offered->fence)) return 0;
    qs_job *j = s->offered;
    if (gpu_fence) j->fence = gpu_fence;
    if (j->index == j->count) {
        qs_queue *q = s->offered_queue;
        if (j->fence < q->scheduled_fence) j->fence = q->scheduled_fence;
        q->scheduled = j->sequence; q->scheduled_fence = j->fence;
        j->finished = 1; q->cursor = j->next;
    }
    else j->index++;
    s->offered = NULL; s->offered_queue = NULL;
    retire(s);
    return 1;
}
size_t nvn_qs_complete(nvn_qs *s, uint64_t gpu_fence) {
    if (!s) return 0;
    if (gpu_fence > s->completed_fence) s->completed_fence = gpu_fence;
    return retire(s);
}
nvn_qs_ticket nvn_qs_tail(const nvn_qs *s, uint64_t queue) {
    qs_queue *q = s ? queue_find(s, queue) : NULL;
    return (nvn_qs_ticket){queue, q ? q->submitted : 0};
}
int nvn_qs_scheduled(const nvn_qs *s, nvn_qs_ticket ticket) {
    if (!ticket.sequence) return 1;
    qs_queue *q = s ? queue_find(s, ticket.queue) : NULL;
    return q && ticket.sequence <= q->scheduled;
}
int nvn_qs_fence(const nvn_qs *s, nvn_qs_ticket ticket, uint64_t *fence) {
    if (!fence) return 0;
    *fence = 0;
    if (!ticket.sequence) return 1;
    qs_queue *q = s ? queue_find(s, ticket.queue) : NULL;
    if (!q || ticket.sequence > q->scheduled) return 0;
    if (ticket.sequence <= q->completed) { *fence = q->completed_fence; return 1; }
    for (qs_job *j = q->head; j; j = j->next) if (j->sequence == ticket.sequence) { *fence = j->fence; return 1; }
    return 0;
}
int nvn_qs_done(const nvn_qs *s, nvn_qs_ticket ticket) {
    if (!ticket.sequence) return 1;
    qs_queue *q = s ? queue_find(s, ticket.queue) : NULL;
    return q && ticket.sequence <= q->completed;
}
size_t nvn_qs_pending(const nvn_qs *s) { return s ? s->pending : 0; }
