// Low-cost on-device accounting during preload and gameplay. No Metal device
// is created here, so the launcher's HUD setting still applies at Start Game.
#import <Foundation/Foundation.h>
#include <mach/mach.h>
#include <os/proc.h>
#include <stdio.h>
#include <time.h>

void dm_ios_memory_budget_start(NSString *documents) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSString *path = [documents stringByAppendingPathComponent:@"memory-budget.tsv"];
        FILE *out = fopen(path.fileSystemRepresentation, "w");
        if (out) {
            fputs("# All sizes in MiB. Estimated process budget = footprint + available; limits can change.\nseconds\tfootprint\tavailable\testimated_limit\tpeak_footprint\tgraphics\n", out);
            fflush(out);
        }
        fprintf(stderr, "memory budget: physical RAM %llu MiB; available to app %llu MiB\n",
                (unsigned long long)(NSProcessInfo.processInfo.physicalMemory >> 20),
                (unsigned long long)(os_proc_available_memory() >> 20));
        dispatch_queue_t queue = dispatch_queue_create("org.nxnative.memory-budget", DISPATCH_QUEUE_SERIAL);
        static dispatch_source_t timer;
        timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
        uint64_t start = clock_gettime_nsec_np(CLOCK_MONOTONIC);
        dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, NSEC_PER_SEC, NSEC_PER_SEC / 10);
        dispatch_source_set_event_handler(timer, ^{
            task_vm_info_data_t info = {0};
            mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
            if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return;
            uint64_t available = os_proc_available_memory();
            uint64_t seconds = (clock_gettime_nsec_np(CLOCK_MONOTONIC) - start) / NSEC_PER_SEC;
            uint64_t graphics = info.ledger_tag_graphics_footprint + info.ledger_tag_graphics_footprint_compressed;
            if (out) {
                fprintf(out, "%llu\t%llu\t%llu\t%llu\t%llu\t%llu\n",
                        (unsigned long long)seconds, info.phys_footprint >> 20,
                        (unsigned long long)(available >> 20),
                        (unsigned long long)((info.phys_footprint + available) >> 20),
                        info.ledger_phys_footprint_peak >> 20, (unsigned long long)(graphics >> 20));
                fflush(out);
            }
            if (seconds % 10 == 0 || available < (512ull << 20))
                fprintf(stderr, "memory budget: footprint %llu MiB, available %llu MiB, estimated limit %llu MiB, peak %llu MiB\n",
                        info.phys_footprint >> 20, (unsigned long long)(available >> 20),
                        (unsigned long long)((info.phys_footprint + available) >> 20), info.ledger_phys_footprint_peak >> 20);
        });
        dispatch_resume(timer);
    });
}
