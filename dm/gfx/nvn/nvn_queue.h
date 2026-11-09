// Guest queue completion is distinct from a physical Metal submission fence.
#pragma once
#include "nvn_obj.h"
void nvn_queue_submit(queue_t *q, int n, const uint64_t *handles);
uint64_t nvn_queue_present(queue_t *q,const tex_t *t,double period,const int crop[4]);
void nvn_queue_present_wait(uint64_t ticket);
void nvn_queue_present_release(uint64_t ticket);
// Unit tests: while held (on=1), replay takes no queued work, so a test can change state between a submit or QueuePresent and its replay; work already replaying finishes
void nvn_queue_test_hold(int on);
void nvn_queue_fence(queue_t *q, sync_t *s);
void nvn_queue_wait_sync(queue_t *q, const sync_t *s);
void nvn_queue_finish(queue_t *q);
void nvn_queue_forget(queue_t *q);
void nvn_sync_release(sync_t *s);
int nvn_sync_wait(const sync_t *s, uint64_t timeout);
void nvn_event_publish(volatile uint32_t *word, unsigned mode, uint32_t value);
void nvn_event_publish_gpu(volatile uint32_t *word, unsigned mode, uint32_t value, uint64_t fence);
void nvn_queue_gpu_progress(void);
