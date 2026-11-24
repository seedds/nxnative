#pragma once
#include <mach/mach.h>
#include <mach/task_policy.h>
#include <string.h>

// The default application role lifts the unspecified role's QoS ceiling for windowless games.
// See docs/reference/audio.md, Underruns in a run without a window.
// Returns 1 on success, 0 when disabled, and -1 on refusal with the Mach error in *kr.
static inline int front_windowless_role(const char *value, kern_return_t *kr) {
    *kr = KERN_SUCCESS;
    if (value && !strcmp(value, "0")) return 0;
    task_category_policy_data_t cat = {TASK_DEFAULT_APPLICATION};
    *kr = task_policy_set(mach_task_self(), TASK_CATEGORY_POLICY, (task_policy_t)&cat, TASK_CATEGORY_POLICY_COUNT);
    return *kr == KERN_SUCCESS ? 1 : -1;
}
