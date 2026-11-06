// Guest queue ordering, independent of command decoding and Metal.
// The caller serializes all access, including the callbacks. WORK payloads and
// event keys are opaque; command streams and their resources remain caller-owned.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct nvn_qs nvn_qs;
typedef struct {
    uint64_t queue, sequence;
} nvn_qs_ticket;
enum { NVN_QS_WORK, NVN_QS_WAIT, NVN_QS_SIGNAL, NVN_QS_END };
enum { NVN_QS_EQUAL, NVN_QS_GEQUAL_WRAP };
enum { NVN_QS_WRITE, NVN_QS_ADD };
typedef struct {
    uint32_t kind, mode;
    uint64_t event;
    uint32_t value;
    void *payload;
} nvn_qs_step;
typedef struct {
    nvn_qs_ticket ticket;
    size_t index;
    uint64_t dispatch; // validates advance; copy the item unchanged
    nvn_qs_step step;
} nvn_qs_item;

// read_event returns 1 with the value at the integration's ordered scheduling
// point, or 0 when unknown. It must account for committed signals not yet CPU-
// visible; the core does not predict signals or mutate event storage itself.
// release runs exactly once after a submission's END and GPU fence complete.
// Neither callback may reenter or mutate this scheduler.
typedef int (*nvn_qs_read_event_fn)(void *ctx, uint64_t event, uint32_t *value);
typedef void (*nvn_qs_release_fn)(void *ctx, nvn_qs_ticket ticket, void *submission);
nvn_qs *nvn_qs_create(nvn_qs_read_event_fn read_event, nvn_qs_release_fn release, void *ctx);
// Refuses destruction while any submission remains, retaining its payloads.
int nvn_qs_destroy(nvn_qs *s);

// Copies step descriptors, not payloads. queue must be a nonzero initialization
// serial. Appends an implicit END, including for an empty submission. On failure
// no ownership transfers and ticket is zeroed. Per-queue sequence never wraps.
int nvn_qs_enqueue(nvn_qs *s, uint64_t queue, const nvn_qs_step *steps, size_t count, void *submission,
                   nvn_qs_ticket *ticket);
// Offers the oldest ready submission head across queues; later submissions on
// a blocked queue cannot pass it. A successful offer latches WAIT satisfaction.
// Repeated peeks return that same item until advance, even if the event changes.
// Returns 0 when all queues are blocked or no unscheduled work remains.
int nvn_qs_peek(nvn_qs *s, nvn_qs_item *item);
// Consume the offered item only after its work/signal has been ordered on the
// host queue. SIGNAL's producer prefix must already be committed and reflected
// in read_event's state. END commits any remaining work before being consumed.
// gpu_fence is the latest single-host-queue fence protecting this submission's
// work, or 0 when this step adds none. Nonzero fences cannot decrease within a
// submission. Stale/mismatched items fail without changing the scheduler.
int nvn_qs_advance(nvn_qs *s, const nvn_qs_item *item, uint64_t gpu_fence);
// The host queue completed every fence <= gpu_fence. Retires finished submission
// prefixes per guest queue, invokes release, and returns the number retired.
// Older notifications are harmless. This is not an independent-queue fence API.
size_t nvn_qs_complete(nvn_qs *s, uint64_t gpu_fence);
nvn_qs_ticket nvn_qs_tail(const nvn_qs *s, uint64_t queue);
// True once this queue prefix has consumed END, whether or not the GPU finished.
int nvn_qs_scheduled(const nvn_qs *s, nvn_qs_ticket ticket);
// Returns a host fence protecting the scheduled queue prefix, including earlier
// submissions when this one has no GPU work. Fails and zeroes fence before END.
// After retirement the bound is the queue's latest completed fence: possibly
// newer than this ticket's own fence, but already complete. No per-ticket history
// is retained once its payload is released. A sequence-zero prefix has fence 0.
int nvn_qs_fence(const nvn_qs *s, nvn_qs_ticket ticket, uint64_t *fence);
// A sequence-zero ticket denotes an empty queue prefix and is already complete.
// Completed tickets remain queryable after their payload/metadata are released.
int nvn_qs_done(const nvn_qs *s, nvn_qs_ticket ticket);
size_t nvn_qs_pending(const nvn_qs *s);

// Invalid modes fail closed. GEQUAL_WRAP compares the unsigned modular distance
// with half the 32-bit range, avoiding signed overflow or implementation-defined
// unsigned-to-signed conversion. ADD wraps modulo 2^32.
int nvn_qs_wait_ready(uint32_t actual, uint32_t mode, uint32_t target);
int nvn_qs_signal_value(uint32_t before, uint32_t mode, uint32_t value, uint32_t *after);
