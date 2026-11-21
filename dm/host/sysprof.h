// System profile sampler (sysprof.m): DM_SYSPROF=<tsv path> writes what the system did with the GPU and the process every 100 ms, Mac only
#pragma once
void dm_sysprof_start(void);   // starts the sampler thread once when DM_SYSPROF is set (nothing elsewhere)
