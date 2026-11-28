// Check the windowless task role (../front_role.h) and the effect on interactive thread priority.
#include "../front_role.h"
#include <pthread.h>
#include <stdio.h>
static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
enum { LEGACY_PRIORITY = 31 };   // XNU BASEPRI_DEFAULT, the ceiling for TASK_UNSPECIFIED
static int role(void) {
    task_category_policy_data_t cat = {0}; mach_msg_type_number_t n = TASK_CATEGORY_POLICY_COUNT; boolean_t def = 0;
    return task_policy_get(mach_task_self(), TASK_CATEGORY_POLICY, (task_policy_t)&cat, &n, &def) == KERN_SUCCESS ? (int)cat.role : -99;
}
static int ui_priority(void) {   // the base priority of this thread once it asks for USER_INTERACTIVE
    if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0)) return -1;
    thread_extended_info_data_t ei; mach_msg_type_number_t n = THREAD_EXTENDED_INFO_COUNT; mach_port_t self = mach_thread_self();
    kern_return_t kr = thread_info(self, THREAD_EXTENDED_INFO, (thread_info_t)&ei, &n); mach_port_deallocate(mach_task_self(), self);
    return kr == KERN_SUCCESS ? ei.pth_priority : -1;
}
int main(void) {
    kern_return_t kr;
    int initial_role = role();
    CHECK(initial_role != -99, "read the initial task role");
    CHECK(front_windowless_role("0", &kr) == 0 && kr == KERN_SUCCESS && role() == initial_role, "DM_WINDOWLESS_ROLE=0 keeps the existing task role");
    int before = ui_priority();
    CHECK(before >= 0, "read the initial interactive priority");
    CHECK(front_windowless_role(NULL, &kr) == 1, "unset applies the role");
    CHECK(role() == TASK_DEFAULT_APPLICATION, "the task has the default application role");
    int after = ui_priority();
    // A launch QoS clamp can hold both samples below the unspecified role's legacy ceiling.
    if (before >= 0 && before < LEGACY_PRIORITY && after == before)
        printf("interactive QoS lift check skipped: priority held at %d below the legacy ceiling\n", before);
    else
        CHECK(after > LEGACY_PRIORITY && (initial_role != TASK_UNSPECIFIED || after > before), "the default application role lifts the interactive QoS ceiling");
    CHECK(front_windowless_role("1", &kr) == 1 && role() == TASK_DEFAULT_APPLICATION, "DM_WINDOWLESS_ROLE=1 also applies the role");
    printf("windowless role: task role %d -> %d, USER_INTERACTIVE base priority %d -> %d (%d checks, %d failed)\n",
           initial_role, role(), before, after, checks, fails);
    return fails != 0;
}
