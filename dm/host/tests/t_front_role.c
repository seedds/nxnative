// The foreground-application task role a host without a window takes (../front_role.h): DM_FOREGROUND_ROLE=0 leaves the task's role, unset or 1 sets it, and then a
// USER_INTERACTIVE thread runs at the priority a focused app's gets, above the default 31 a process started from a shell gets
#include "../front_role.h"
#include <pthread.h>
#include <stdio.h>
static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
static int role(void) {
    task_category_policy_data_t cat = {0}; mach_msg_type_number_t n = TASK_CATEGORY_POLICY_COUNT; boolean_t def = 0;
    return task_policy_get(mach_task_self(), TASK_CATEGORY_POLICY, (task_policy_t)&cat, &n, &def) == KERN_SUCCESS ? (int)cat.role : -99;
}
static int ui_priority(void) {   // the base priority of this thread once it asks for USER_INTERACTIVE
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    thread_extended_info_data_t ei; mach_msg_type_number_t n = THREAD_EXTENDED_INFO_COUNT; mach_port_t self = mach_thread_self();
    kern_return_t kr = thread_info(self, THREAD_EXTENDED_INFO, (thread_info_t)&ei, &n); mach_port_deallocate(mach_task_self(), self);
    return kr == KERN_SUCCESS ? ei.pth_priority : -1;
}
int main(void) {
    kern_return_t kr; int r0 = role();
    CHECK(front_foreground_role("0", &kr) == 0 && kr == KERN_SUCCESS && role() == r0, "DM_FOREGROUND_ROLE=0: the task's role is left as it was");
    int before = ui_priority();
    CHECK(front_foreground_role(NULL, &kr) == 1, "unset: the role is taken");
    CHECK(role() == TASK_FOREGROUND_APPLICATION, "the task's role is now the foreground application's");
    CHECK(front_foreground_role("1", &kr) == 1 && role() == TASK_FOREGROUND_APPLICATION, "DM_FOREGROUND_ROLE=1: the same");
    int after = ui_priority();
    CHECK(after > 31, "a USER_INTERACTIVE thread runs above the default priority 31");
    printf("foreground role: task role %d -> %d, USER_INTERACTIVE base priority %d -> %d; %d checks, %d failed\n", r0, role(), before, after, checks, fails);
    return fails != 0;
}
