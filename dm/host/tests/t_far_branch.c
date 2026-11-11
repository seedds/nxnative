// Execute the exact marked BRK through Darwin's signal-return path for B and BL.
#include "../far_branch.h"
#include <signal.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>

extern uint64_t branch_jump(uint64_t), branch_call(uint64_t);
extern char jump_site[], jump_target[], call_site[], call_target[];
__asm__(".text\n.p2align 2\n.globl _branch_jump, _jump_site, _jump_target\n"
        "_branch_jump:\n_jump_site:\nbrk #0x6e78\nmov x0,#0\nret\n"
        "_jump_target:\nadd x0,x0,#3\nret\n"
        ".globl _branch_call, _call_site, _call_target\n"
        "_branch_call:\nstp x29,x30,[sp,#-16]!\n_call_site:\nbrk #0x6e78\n"
        "add x0,x0,#2\nldp x29,x30,[sp],#16\nret\n"
        "_call_target:\nadd x0,x0,#1\nret\n");
static struct dm_far_branch entries[2];
static void trap(int sig, siginfo_t *info, void *ctx) {
    (void)sig; (void)info;
    if (!dm_far_branch_resume(ctx, 0, entries, 2)) _exit(99);
}
int main(void) {
    entries[0] = (struct dm_far_branch){(int64_t)jump_site, (int64_t)jump_target, 0};
    entries[1] = (struct dm_far_branch){(int64_t)call_site, (int64_t)call_target, 1};
    assert(entries[0].source < entries[1].source);
    struct sigaction sa = {.sa_sigaction = trap, .sa_flags = SA_SIGINFO};
    sigemptyset(&sa.sa_mask); assert(!sigaction(SIGTRAP, &sa, NULL));
    assert(branch_jump(39) == 42);
    assert(branch_call(39) == 42);
    puts("far branch B/BL signal return: PASS");
}
