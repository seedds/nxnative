// A frontend startup gate, independent of the guest loading-screen timeout.
#pragma once
#include "nvn_pipeline_cache.h"
enum { NVN_PREP_IDLE, NVN_PREP_LOADING, NVN_PREP_WORKING, NVN_PREP_SAVING,
       NVN_PREP_READY, NVN_PREP_PARTIAL, NVN_PREP_FAILED };
typedef struct {
    int state;
    uint32_t total, completed, failed, skipped;
    nvn_cache_status cache;
} nvn_prepare_status;
void mtl_prepare_start(void);
void mtl_prepare_snapshot(nvn_prepare_status *out);
void mtl_prepare_active(int active);
