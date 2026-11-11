#ifndef DM_SIGNPOST_H
#define DM_SIGNPOST_H
#include <os/signpost.h>
#include <stdint.h>
extern int dm_signpost_on;
extern os_log_t dm_signpost_log;
// Initialized by the loader before guest threads start. No log is created when off.
#define DM_SP_BEGIN(name, id, frame) do { if (dm_signpost_on) os_signpost_interval_begin(dm_signpost_log, (id), name, "frame=%{public}llu", (unsigned long long)(frame)); } while (0)
#define DM_SP_END(name, id) do { if (dm_signpost_on) os_signpost_interval_end(dm_signpost_log, (id), name); } while (0)
#define DM_SP_START(name, id, frame) os_signpost_id_t id = 0; do { if (dm_signpost_on) { id = os_signpost_id_generate(dm_signpost_log); os_signpost_interval_begin(dm_signpost_log, id, name, "frame=%{public}llu", (unsigned long long)(frame)); } } while (0)
#endif
