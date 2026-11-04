// Guest queues share one Metal queue, but a forward event wait can suspend a
// guest submission until another guest queue supplies its producer. Never block
// the submitting CPU thread: it may itself submit that producer next.
#include "nvn_capture.h"

typedef struct qe_value {
    struct qe_value *next;
    uint64_t address, fence;
    uint32_t value;
} qe_value;
typedef struct qe_work {
    struct qe_work *next;
    uint8_t *bytes;
    int first, origin;
    uint64_t signal;
    uint32_t mode, value;
    mtl_submit_clock *clock; // borrowed from the logical submission
    mtl_submit_alloc *alloc; // likewise; serialized by the submission lock
    win_t *window;
    int image, crop[4];
    tex_t texture;
    double period;
} qe_work;
typedef struct {
    qe_work *head, *tail;
    mtl_submit_clock *clock;
    mtl_submit_alloc *alloc;
} qe_submission;
static nvn_qs *g_qs;
static qe_value *g_qe_values;
static pthread_cond_t g_qe_cv = PTHREAD_COND_INITIALIZER;
static void *qe_worker(void *unused);

static qe_value *qe_value_find(uint64_t address, int add) {
    qe_value *event = g_qe_values;
    while (event && event->address != address) {
        event = event->next;
    }
    if (!event && add) {
        event = calloc(1, sizeof *event);
        if (!event) {
            die("NVN event state allocation failed");
        }
        event->address = address;
        event->next = g_qe_values;
        g_qe_values = event;
    }
    return event;
}
static int qe_read(void *context, uint64_t address, uint32_t *value) {
    (void)context;
    if (address & 1) { // a sync wait: all work through its fence must be ordered
        const sync_t *sync = (const sync_t *)(uintptr_t)(address & ~1ull);
        *value = sync->fenced && sync->fenced != NVN_SYNC_FENCE_RECORDED_PENDING &&
                 (sync->fenced != NVN_SYNC_FENCE_QUEUE_PREFIX ||
                  nvn_qs_scheduled(g_qs, (nvn_qs_ticket){sync->queue, sync->fence}));
        return 1;
    }
    qe_value *event = qe_value_find(address, 0);
    // Once post-operations have landed, the mapped word is authoritative again;
    // this includes CPU stores made directly through the memory pool.
    if (event && event->fence > mtl_completed_fence()) {
        *value = event->value;
    } else {
        *value = *(volatile uint32_t *)(uintptr_t)address;
    }
    return 1;
}
static void qe_release(void *context, nvn_qs_ticket ticket, void *payload) {
    (void)context;
    (void)ticket;
    qe_submission *submission = payload;
    if (!submission) {
        return;
    }
    for (qe_work *work = submission->head, *next; work; work = next) {
        next = work->next;
        free(work->bytes);
        free(work);
    }
    mtl_submit_clock_release(submission->clock);
    mtl_submit_alloc_release(submission->alloc);
    free(submission);
}
static void qe_init(void) {
    if (!g_qs) {
        g_qs = nvn_qs_create(qe_read, qe_release, NULL);
        if (!g_qs) {
            die("NVN queue scheduler allocation failed");
        }
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);
        int err = pthread_create(&thread, &attr, qe_worker, NULL);
        pthread_attr_destroy(&attr);
        if (err) {
            die("NVN queue worker creation failed: %d", err);
        }
        pthread_detach(thread);
        LOG("[nvn] event queue ordering enabled: blocked guest queues yield to their producers");
    }
}
static void qe_pump(void) { // submission lock and graphics gate held
    if (!g_qs) {
        return;
    }
    nvn_qs_item item;
    while (nvn_qs_peek(g_qs, &item)) {
        uint64_t fence = 0;
        if (item.step.kind == NVN_QS_WORK) {
            qe_work *work = item.step.payload;
            if (work->window) {
                work->window->ticket[work->image] = mtl_present(&work->texture, work->period, work->crop);
            } else {
                uint32_t signal_value = 0;
                if (work->signal) {
                    uint32_t before;
                    qe_read(NULL, work->signal, &before);
                    if (!nvn_qs_signal_value(before, work->mode, work->value, &signal_value)) {
                        die("NVN invalid event signal mode");
                    }
                }
                mtl_queue_activate(item.ticket.queue);
                mtl_set_queue_origin_lower_left(work->origin);
                mtl_sub_begin_alloc(work->alloc, !work->first);
                mtl_sub_set_clock(work->clock);
                uint64_t handle = (uint64_t)work->bytes;
                mtl_sub_index(&handle, 1);
                mtl_exec_rest(NULL, 0);
                mtl_exec(work->bytes);
                mtl_sub_commit();
                mtl_queue_seal();
                fence = mtl_last_fence();
                if (!work->next) {
                    mtl_submit_alloc_close(work->alloc);
                }
                if (work->signal) {
                    qe_value *event = qe_value_find(work->signal, 1);
                    event->value = signal_value;
                    event->fence = fence;
                }
            }
        } else if (item.step.kind == NVN_QS_WAIT) {
            // The producer is ordered, but may still be running. A wait-only
            // submission must retain that GPU completion dependency as well.
            fence = mtl_last_fence();
        }
        // WAIT was satisfied at the ordered scheduling point. SIGNAL's WORK
        // already committed its producer and published the projected value.
        if (!nvn_qs_advance(g_qs, &item, fence)) {
            die("NVN queue scheduler lost its offered step");
        }
    }
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
            dm_host_work++;
            mtl_prepare();
            dm_host_work--;
            gfx_enter();
            qe_pump();
            gfx_leave();
            struct timespec delay = {0, 1000000};
            pthread_cond_timedwait_relative_np(&g_qe_cv, &g_sub_mu, &delay);
        } else {
            pthread_cond_wait(&g_qe_cv, &g_sub_mu);
        }
    }
    return NULL;
}

typedef struct {
    size_t offset;
    const uint8_t *source;
    uint32_t size;
} qe_record;
typedef struct {
    qe_submission *submission;
    nvn_qs_step *steps;
    size_t count, step_capacity;
    uint8_t *bytes;
    size_t size, capacity;
    qe_record *records;
    size_t record_count, record_capacity;
    int first, origin;
} qe_builder;
static void qe_step(qe_builder *builder, nvn_qs_step step) {
    if (builder->count == builder->step_capacity) {
        builder->step_capacity = builder->step_capacity ? builder->step_capacity * 2 : 32;
        nvn_qs_step *next = realloc(builder->steps, builder->step_capacity * sizeof *builder->steps);
        if (!next) {
            die("NVN queue step allocation failed");
        }
        builder->steps = next;
    }
    builder->steps[builder->count++] = step;
}
static int qe_reloc_addr(void *context, uint64_t *field, uint64_t len, int writable) {
    (void)context;
    (void)field;
    (void)len;
    (void)writable;
    return 1;
}
static int qe_reloc_obj(void *context, int kind, uint64_t *field, uint8_t *payload, uint32_t size) {
    (void)kind;
    (void)size;
    const qe_record *record = context;
    uint64_t address;
    memcpy(&address, field, 8);
    uintptr_t source = (uintptr_t)record->source;
    if (address >= source && address - source < record->size) {
        address = (uint64_t)(payload - 8) + (address - source);
        memcpy(field, &address, 8);
    }
    return 1;
}
static void qe_flush(qe_builder *builder, uint64_t signal, uint32_t mode, uint32_t value) {
    if (!builder->size) {
        return;
    }
    qe_work *work = calloc(1, sizeof *work);
    if (!work) {
        die("NVN queue chunk allocation failed");
    }
    work->bytes = malloc(builder->size + 8);
    if (!work->bytes) {
        die("NVN queue command allocation failed");
    }
    memcpy(work->bytes, builder->bytes, builder->size);
    put_hdr(work->bytes + builder->size, OP_END, 8);
    const cap_ops reloc = {qe_reloc_addr, qe_reloc_obj};
    for (size_t i = 0; i < builder->record_count; i++) {
        const qe_record *record = &builder->records[i];
        uint8_t *command = work->bytes + record->offset;
        op_hdr header;
        memcpy(&header, command, 8);
        if (!cap_rec_fields(header.op, command + 8, header.size, &reloc, (void *)record)) {
            die("NVN queue cannot relocate opcode %u", header.op);
        }
    }
    work->first = builder->first;
    builder->first = 0;
    work->origin = builder->origin;
    if (!builder->submission->clock) {
        builder->submission->clock = mtl_submit_clock_new();
    }
    work->clock = builder->submission->clock;
    if (!builder->submission->alloc) {
        builder->submission->alloc = mtl_submit_alloc_new();
    }
    work->alloc = builder->submission->alloc;
    work->signal = signal;
    work->mode = mode;
    work->value = value;
    if (builder->submission->tail) {
        builder->submission->tail->next = work;
    } else {
        builder->submission->head = work;
    }
    builder->submission->tail = work;
    qe_step(builder, (nvn_qs_step){.kind = NVN_QS_WORK, .payload = work});
    builder->size = builder->record_count = 0;
}
static void qe_record_copy(qe_builder *builder, const uint8_t *command, uint32_t size) {
    if (builder->size + size > builder->capacity) {
        builder->capacity = (builder->size + size) * 2;
        uint8_t *next = realloc(builder->bytes, builder->capacity);
        if (!next) {
            die("NVN queue recording allocation failed");
        }
        builder->bytes = next;
    }
    if (builder->record_count == builder->record_capacity) {
        builder->record_capacity = builder->record_capacity ? builder->record_capacity * 2 : 256;
        qe_record *next = realloc(builder->records, builder->record_capacity * sizeof *builder->records);
        if (!next) {
            die("NVN queue relocation allocation failed");
        }
        builder->records = next;
    }
    builder->records[builder->record_count++] = (qe_record){builder->size, command, size};
    memcpy(builder->bytes + builder->size, command, size);
    builder->size += size;
}
static void qe_submit(const queue_t *queue, int handle_count, const uint64_t *handles) {
    qe_init();
    qe_builder builder = {.submission = calloc(1, sizeof(qe_submission)), .first = 1, .origin = queue->origin_ll};
    if (!builder.submission) {
        die("NVN queue submission allocation failed");
    }
    for (int handle_index = 0; handle_index < handle_count; handle_index++) {
        const uint8_t *command = (const uint8_t *)handles[handle_index], *returns[64];
        unsigned depth = 0;
        size_t records = 0;
        for (;;) {
            op_hdr header;
            memcpy(&header, command, 8);
            if (header.size < 8 || (header.size & 7) || header.size > 64 * 1024 * 1024 || header.op >= OP_COUNT_ ||
                ++records > 10000000) {
                die("NVN invalid scheduled command stream");
            }
            if (header.op == OP_END) {
                if (!depth) {
                    break;
                }
                command = returns[--depth];
                continue;
            }
            if (header.op == OP_CALL || header.op == OP_JUMP) {
                if (header.size < 16) {
                    die("NVN short command link");
                }
                if (header.op == OP_CALL) {
                    if (depth == 64) {
                        die("NVN command call nesting overflow");
                    }
                    returns[depth++] = command + header.size;
                }
                memcpy(&command, command + 8, 8);
                continue;
            }
            if (header.op == OP_WAIT_EVENT || header.op == OP_WAIT_SYNC) {
                qe_flush(&builder, 0, 0, 0);
                uint64_t address;
                uint32_t mode = NVN_QS_EQUAL, value = 1;
                memcpy(&address, command + 8, 8);
                if (header.op == OP_WAIT_EVENT) {
                    memcpy(&mode, command + 16, 4);
                    memcpy(&value, command + 20, 4);
                } else {
                    address |= 1;
                }
                qe_step(&builder, (nvn_qs_step){.kind = NVN_QS_WAIT, .event = address, .mode = mode, .value = value});
            } else {
                qe_record_copy(&builder, command, header.size);
                if (header.op == OP_SIGNAL_EVENT) {
                    uint64_t address;
                    uint32_t mode, value;
                    memcpy(&address, command + 8, 8);
                    memcpy(&mode, command + 16, 4);
                    memcpy(&value, command + 20, 4);
                    qe_flush(&builder, address, mode, value);
                    qe_step(&builder,
                            (nvn_qs_step){.kind = NVN_QS_SIGNAL, .event = address, .mode = mode, .value = value});
                } else if (header.op == OP_FENCE_SYNC) {
                    sync_t *sync;
                    memcpy(&sync, command + 8, 8);
                    sync->fence = 0;
                    sync->fenced = NVN_SYNC_FENCE_RECORDED_PENDING;
                    qe_flush(&builder, 0, 0, 0);
                }
            }
            command += header.size;
        }
    }
    qe_flush(&builder, 0, 0, 0);
    nvn_qs_ticket ticket;
    if (!nvn_qs_enqueue(g_qs, queue->serial, builder.steps, builder.count, builder.submission, &ticket)) {
        die("NVN queue enqueue failed");
    }
    free(builder.steps);
    free(builder.bytes);
    free(builder.records);
    qe_pump();
}
static void qe_queue_fence(const queue_t *queue, sync_t *sync) {
    dm_host_mutex_lock(&g_sub_mu);
    if (g_qs && nvn_qs_tail(g_qs, queue->serial).sequence) {
        nvn_qs_ticket ticket = nvn_qs_tail(g_qs, queue->serial);
        sync->fenced = NVN_SYNC_FENCE_QUEUE_PREFIX;
        sync->fence = ticket.sequence;
        sync->queue = ticket.queue;
    } else {
        sync->fenced = NVN_SYNC_FENCE_GPU;
        sync->fence = mtl_last_fence();
    }
    pthread_mutex_unlock(&g_sub_mu);
}
static int qe_wait(nvn_qs_ticket ticket, uint64_t timeout, int completed) {
    uint64_t start = now_ns(), fence = 0;
    dm_host_mutex_lock(&g_sub_mu);
    for (;;) {
        gfx_enter();
        qe_pump();
        gfx_leave();
        if (nvn_qs_fence(g_qs, ticket, &fence)) {
            break;
        }
        uint64_t elapsed = now_ns() - start;
        if (timeout != UINT64_MAX && elapsed >= timeout) {
            pthread_mutex_unlock(&g_sub_mu);
            return 0;
        }
        // New submissions and CPU signals wake this wait. The bounded fallback
        // also observes mapped event stores made without an NVN API call.
        uint64_t ns = timeout == UINT64_MAX ? 1000000 : timeout - elapsed;
        if (ns > 1000000) {
            ns = 1000000;
        }
        struct timespec delay = {0, (long)ns};
        pthread_cond_timedwait_relative_np(&g_qe_cv, &g_sub_mu, &delay);
    }
    pthread_mutex_unlock(&g_sub_mu);
    if (!completed) {
        return 1;
    }
    uint64_t elapsed = now_ns() - start;
    return mtl_wait(fence, timeout == UINT64_MAX ? timeout : elapsed < timeout ? timeout - elapsed : 0);
}
static void qe_finish(const queue_t *queue) {
    dm_host_mutex_lock(&g_sub_mu);
    nvn_qs_ticket ticket = g_qs ? nvn_qs_tail(g_qs, queue->serial) : (nvn_qs_ticket){0};
    pthread_mutex_unlock(&g_sub_mu);
    if (ticket.sequence) {
        qe_wait(ticket, UINT64_MAX, 1);
    } else {
        mtl_finish();
    }
}
static void qe_cpu_signal(const ev_t *event, int mode, uint32_t value) {
    dm_host_mutex_lock(&g_sub_mu);
    qe_value *projection = qe_value_find((uint64_t)event->val, 0);
    // Complete an older queued write before resetting the same CPU-visible word.
    // The Metal post-operation mutex alone cannot establish that order.
    if (projection && projection->fence > mtl_completed_fence()) {
        mtl_wait(projection->fence, UINT64_MAX);
    }
    ev_signal(event, mode, value);
    gfx_enter();
    qe_pump();
    gfx_leave();
    pthread_mutex_unlock(&g_sub_mu);
}
static void qe_sync_wait(const queue_t *queue, const sync_t *sync) {
    dm_host_mutex_lock(&g_sub_mu);
    qe_init();
    nvn_qs_step step = {.kind = NVN_QS_WAIT, .event = (uint64_t)sync | 1, .mode = NVN_QS_EQUAL, .value = 1};
    nvn_qs_ticket ticket;
    if (!nvn_qs_enqueue(g_qs, queue->serial, &step, 1, NULL, &ticket)) {
        die("NVN sync wait enqueue failed");
    }
    gfx_enter();
    qe_pump();
    gfx_leave();
    pthread_mutex_unlock(&g_sub_mu);
}
static void qe_present(const queue_t *queue, win_t *window, int image, const tex_t *texture, double period,
                       const int crop[4]) {
    qe_submission *submission = calloc(1, sizeof *submission);
    qe_work *work = calloc(1, sizeof *work);
    if (!submission || !work) {
        die("NVN queued present allocation failed");
    }
    submission->head = submission->tail = work;
    work->window = window;
    work->image = image;
    work->texture = *texture;
    work->period = period;
    if (crop) {
        memcpy(work->crop, crop, sizeof work->crop);
    }
    nvn_qs_step step = {.kind = NVN_QS_WORK, .payload = work};
    nvn_qs_ticket ticket;
    if (!nvn_qs_enqueue(g_qs, queue->serial, &step, 1, submission, &ticket)) {
        die("NVN present enqueue failed");
    }
    window->submit_queue[image] = ticket.queue;
    window->submit_seq[image] = ticket.sequence;
    qe_pump();
}

static int qe_wait_recorded(const sync_t *sync, uint64_t timeout) {
    uint64_t start = now_ns(), fence;
    dm_host_mutex_lock(&g_sub_mu);
    while (sync->fenced == NVN_SYNC_FENCE_RECORDED_PENDING) {
        gfx_enter();
        qe_pump();
        gfx_leave();
        if (sync->fenced != NVN_SYNC_FENCE_RECORDED_PENDING) {
            break;
        }
        uint64_t elapsed = now_ns() - start;
        if (timeout != UINT64_MAX && elapsed >= timeout) {
            pthread_mutex_unlock(&g_sub_mu);
            return 0;
        }
        uint64_t ns = timeout == UINT64_MAX ? 1000000 : timeout - elapsed;
        if (ns > 1000000) {
            ns = 1000000;
        }
        struct timespec delay = {0, (long)ns};
        pthread_cond_timedwait_relative_np(&g_qe_cv, &g_sub_mu, &delay);
    }
    fence = sync->fence;
    pthread_mutex_unlock(&g_sub_mu);
    uint64_t elapsed = now_ns() - start;
    return mtl_wait(fence, timeout == UINT64_MAX ? timeout : elapsed < timeout ? timeout - elapsed : 0);
}
static void qe_flush_queue(void) {
    dm_host_mutex_lock(&g_sub_mu);
    gfx_enter();
    qe_pump();
    gfx_leave();
    pthread_mutex_unlock(&g_sub_mu);
}
