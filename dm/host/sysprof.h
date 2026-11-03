// System profile for the per-frame timing rows (sysprof.m): the latest 100 ms sample of what the system did with the GPU and the process. −1 = not measured
#pragma once
typedef struct {
    double gpu_mhz;   // mean clock of the GPU's powered states
    double gpu_on;    // % of the interval the GPU was powered (not OFF): not the same as busy
    double gpu_w;     // GPU power, watts
    double boost;     // the boost controller's requested state, mean P index (1 = lowest)
    double cltm;      // % of the interval a thermal limit applied to the GPU
    double droop;     // % of the interval a voltage droop limit applied
    int role;         // task_policy_get TASK_CATEGORY_POLICY role (1 foreground app, 2 background app, ...)
    int thermal;      // NSProcessInfo thermalState (0 nominal … 3 critical)
    int low_power;    // Low Power Mode
} dm_sysprof;
void dm_sysprof_start(void);       // starts the sampler thread once (Mac; nothing elsewhere)
int dm_sysprof_get(dm_sysprof *out);   // the latest sample; 0 when there is none
