// The foreground-application task role for the Mac host without a window (front_mac.m front_run without DM_WINDOW: the headless runs that benchmarks and A/B comparisons start
// from a shell or a script). A process started from a shell has no task role, and macOS gives a USER_INTERACTIVE thread of it scheduling priority 31, the default, where a focused
// app's gets 47. At 31 every thread of the game waited for a core behind background daemons' bursts (contactsd, AddressBookSourceSync, corespotlightd): up to 30-40 ms in a System
// Trace, NieR:Automata's audio thread among them, which then refilled the 4 blocks it keeps queued (64-85 ms) too late, and the output underran. With this role no thread waited over
// 5 ms in the same bursts (docs/reference/audio.md, Underruns in a run without a window). DM_FOREGROUND_ROLE=0 keeps the shell's role (an A/B against runs from before it).
// Returns 1 when the role was taken, 0 when it was left as it was, -1 when the system refused it (its kern_return_t in *kr)
#include <mach/mach.h>
#include <mach/task_policy.h>
#include <stdlib.h>
static inline int front_foreground_role(const char *v, kern_return_t *kr) {
    *kr = KERN_SUCCESS;
    if (v && *v && atoi(v) <= 0) return 0;
    task_category_policy_data_t cat = {TASK_FOREGROUND_APPLICATION};
    *kr = task_policy_set(mach_task_self(), TASK_CATEGORY_POLICY, (task_policy_t)&cat, TASK_CATEGORY_POLICY_COUNT);
    return *kr == KERN_SUCCESS ? 1 : -1;
}
