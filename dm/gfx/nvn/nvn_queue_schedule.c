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
    void *context;
};

int nvn_qs_wait_ready(uint32_t actual, uint32_t mode, uint32_t target) {
    if (mode == NVN_QS_EQUAL) {
        return actual == target;
    }
    if (mode == NVN_QS_GEQUAL_WRAP) {
        return (uint32_t)(actual - target) < UINT32_C(0x80000000);
    }
    return 0;
}
int nvn_qs_signal_value(uint32_t before, uint32_t mode, uint32_t value, uint32_t *after) {
    if (!after) {
        return 0;
    }
    if (mode == NVN_QS_WRITE) {
        *after = value;
    } else if (mode == NVN_QS_ADD) {
        *after = before + value;
    } else {
        return 0;
    }
    return 1;
}
static qs_queue *queue_find(const nvn_qs *scheduler, uint64_t serial) {
    for (qs_queue *queue = scheduler->queues; queue; queue = queue->next) {
        if (queue->serial == serial) {
            return queue;
        }
    }
    return NULL;
}
nvn_qs *nvn_qs_create(nvn_qs_read_event_fn read_event, nvn_qs_release_fn release, void *context) {
    nvn_qs *scheduler = calloc(1, sizeof *scheduler);
    if (scheduler) {
        scheduler->read_event = read_event;
        scheduler->release = release;
        scheduler->context = context;
    }
    return scheduler;
}
int nvn_qs_destroy(nvn_qs *scheduler) {
    if (!scheduler) {
        return 1;
    }
    if (scheduler->pending) {
        return 0;
    }
    while (scheduler->queues) {
        qs_queue *queue = scheduler->queues;
        scheduler->queues = queue->next;
        free(queue);
    }
    free(scheduler);
    return 1;
}
int nvn_qs_enqueue(nvn_qs *scheduler, uint64_t queue_serial, const nvn_qs_step *steps, size_t count, void *submission,
                   nvn_qs_ticket *ticket) {
    if (ticket) {
        *ticket = (nvn_qs_ticket){0};
    }
    if (!scheduler || !queue_serial || !ticket || (count && !steps) || scheduler->order == UINT64_MAX ||
        scheduler->pending == SIZE_MAX || count > (SIZE_MAX - sizeof(qs_job)) / sizeof(nvn_qs_step)) {
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        const nvn_qs_step *step = &steps[i];
        if (step->kind > NVN_QS_SIGNAL ||
            ((step->kind == NVN_QS_WAIT || step->kind == NVN_QS_SIGNAL) && step->mode > 1)) {
            return 0;
        }
    }
    qs_queue *queue = queue_find(scheduler, queue_serial), *new_queue = NULL;
    if (queue && queue->submitted == UINT64_MAX) {
        return 0;
    }
    if (!queue) {
        new_queue = calloc(1, sizeof *new_queue);
        if (!new_queue) {
            return 0;
        }
        queue = new_queue;
        queue->serial = queue_serial;
    }
    qs_job *job = calloc(1, sizeof *job + count * sizeof *steps);
    if (!job) {
        free(new_queue);
        return 0;
    }
    if (count) {
        memcpy(job->steps, steps, count * sizeof *steps);
    }
    job->count = count;
    job->submission = submission;
    job->sequence = ++queue->submitted;
    job->order = ++scheduler->order;
    if (new_queue) {
        queue->next = scheduler->queues;
        scheduler->queues = queue;
    }
    if (queue->tail) {
        queue->tail->next = job;
    } else {
        queue->head = job;
    }
    queue->tail = job;
    if (!queue->cursor) {
        queue->cursor = job;
    }
    scheduler->pending++;
    *ticket = (nvn_qs_ticket){queue_serial, job->sequence};
    return 1;
}
static nvn_qs_step job_step(const qs_job *job) {
    return job->index == job->count ? (nvn_qs_step){.kind = NVN_QS_END} : job->steps[job->index];
}
static void offered_item(const nvn_qs *scheduler, nvn_qs_item *item) {
    *item = (nvn_qs_item){.ticket = {scheduler->offered_queue->serial, scheduler->offered->sequence},
                          .index = scheduler->offered->index,
                          .dispatch = scheduler->dispatch,
                          .step = job_step(scheduler->offered)};
}
int nvn_qs_peek(nvn_qs *scheduler, nvn_qs_item *item) {
    if (!item) {
        return 0;
    }
    memset(item, 0, sizeof *item);
    if (!scheduler) {
        return 0;
    }
    if (scheduler->offered) {
        offered_item(scheduler, item);
        return 1;
    }
    qs_queue *best = NULL;
    for (qs_queue *queue = scheduler->queues; queue; queue = queue->next) {
        qs_job *job = queue->cursor;
        if (!job || (best && best->cursor->order < job->order)) {
            continue;
        }
        nvn_qs_step step = job_step(job);
        if (step.kind == NVN_QS_WAIT) {
            uint32_t value;
            if (!scheduler->read_event || !scheduler->read_event(scheduler->context, step.event, &value) ||
                !nvn_qs_wait_ready(value, step.mode, step.value)) {
                continue;
            }
        }
        best = queue;
    }
    if (!best) {
        return 0;
    }
    scheduler->offered_queue = best;
    scheduler->offered = best->cursor;
    if (!++scheduler->dispatch) {
        ++scheduler->dispatch; // ticket + index also distinguish stale offers across wrap
    }
    offered_item(scheduler, item);
    return 1;
}
static size_t retire(nvn_qs *scheduler) {
    size_t retired = 0;
    for (qs_queue *queue = scheduler->queues; queue; queue = queue->next) {
        while (queue->head && queue->head->finished && queue->head->fence <= scheduler->completed_fence) {
            qs_job *job = queue->head;
            nvn_qs_ticket ticket = {queue->serial, job->sequence};
            void *payload = job->submission;
            queue->head = job->next;
            if (!queue->head) {
                queue->tail = NULL;
            }
            queue->completed = job->sequence;
            queue->completed_fence = job->fence;
            scheduler->pending--;
            retired++;
            free(job);
            if (scheduler->release) {
                scheduler->release(scheduler->context, ticket, payload);
            }
        }
    }
    return retired;
}
int nvn_qs_advance(nvn_qs *scheduler, const nvn_qs_item *item, uint64_t gpu_fence) {
    if (!scheduler || !item || !scheduler->offered || item->dispatch != scheduler->dispatch ||
        item->ticket.queue != scheduler->offered_queue->serial ||
        item->ticket.sequence != scheduler->offered->sequence || item->index != scheduler->offered->index ||
        (gpu_fence && gpu_fence < scheduler->offered->fence)) {
        return 0;
    }
    qs_job *job = scheduler->offered;
    if (gpu_fence) {
        job->fence = gpu_fence;
    }
    if (job->index == job->count) {
        qs_queue *queue = scheduler->offered_queue;
        if (job->fence < queue->scheduled_fence) {
            job->fence = queue->scheduled_fence;
        }
        queue->scheduled = job->sequence;
        queue->scheduled_fence = job->fence;
        job->finished = 1;
        queue->cursor = job->next;
    } else {
        job->index++;
    }
    scheduler->offered = NULL;
    scheduler->offered_queue = NULL;
    retire(scheduler);
    return 1;
}
size_t nvn_qs_complete(nvn_qs *scheduler, uint64_t gpu_fence) {
    if (!scheduler) {
        return 0;
    }
    if (gpu_fence > scheduler->completed_fence) {
        scheduler->completed_fence = gpu_fence;
    }
    return retire(scheduler);
}
nvn_qs_ticket nvn_qs_tail(const nvn_qs *scheduler, uint64_t queue_serial) {
    qs_queue *queue = scheduler ? queue_find(scheduler, queue_serial) : NULL;
    return (nvn_qs_ticket){queue_serial, queue ? queue->submitted : 0};
}
int nvn_qs_scheduled(const nvn_qs *scheduler, nvn_qs_ticket ticket) {
    if (!ticket.sequence) {
        return 1;
    }
    qs_queue *queue = scheduler ? queue_find(scheduler, ticket.queue) : NULL;
    return queue && ticket.sequence <= queue->scheduled;
}
int nvn_qs_fence(const nvn_qs *scheduler, nvn_qs_ticket ticket, uint64_t *fence) {
    if (!fence) {
        return 0;
    }
    *fence = 0;
    if (!ticket.sequence) {
        return 1;
    }
    qs_queue *queue = scheduler ? queue_find(scheduler, ticket.queue) : NULL;
    if (!queue || ticket.sequence > queue->scheduled) {
        return 0;
    }
    if (ticket.sequence <= queue->completed) {
        *fence = queue->completed_fence;
        return 1;
    }
    for (qs_job *job = queue->head; job; job = job->next) {
        if (job->sequence == ticket.sequence) {
            *fence = job->fence;
            return 1;
        }
    }
    return 0;
}
int nvn_qs_done(const nvn_qs *scheduler, nvn_qs_ticket ticket) {
    if (!ticket.sequence) {
        return 1;
    }
    qs_queue *queue = scheduler ? queue_find(scheduler, ticket.queue) : NULL;
    return queue && ticket.sequence <= queue->completed;
}
size_t nvn_qs_pending(const nvn_qs *scheduler) {
    return scheduler ? scheduler->pending : 0;
}
