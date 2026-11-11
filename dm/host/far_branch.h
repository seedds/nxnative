// Host-resumed far branches preserve all registers; only BL writes LR.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <sys/ucontext.h>
struct dm_far_branch { int64_t source, target; unsigned link; };
static inline int dm_far_branch_resume(ucontext_t *uc, uint64_t base, const struct dm_far_branch *entries, size_t count) {
    uint64_t pc = uc->uc_mcontext->__ss.__pc;
    int64_t off = (int64_t)(pc - base);
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (entries[mid].source < off) lo = mid + 1;
        else hi = mid;
    }
    if (lo == count || entries[lo].source != off || *(const uint32_t *)pc != 0xd42dcf00u) return 0;
    if (entries[lo].link) uc->uc_mcontext->__ss.__lr = pc + 4;
    uc->uc_mcontext->__ss.__pc = base + entries[lo].target;
    return 1;
}
