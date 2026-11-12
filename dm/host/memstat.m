// Memory accounting: when DM_MEMSTAT=<tsv path> is set, one line is logged per second (common to Mac / iOS).
//   Totals: phys_footprint (task_info TASK_VM_INFO, which the iOS memory limit is based on), os_proc_available_memory (iOS only), Metal currentAllocatedSize
//   Breakdown: walk this process's VM regions (vm_region_recurse_64, VM_REGION_SUBMAP_INFO_64), classify by region, and for each class record
//         virtual = address space size (reserved-only counts too), resident = pages_resident, charged = dirty pages + compressed/swapped-out pages, deduplicated by VM object (≈ the part that goes into phys_footprint)
//         dedup key = (object id, offset within the object); reason for dedup: Metal wraps the guest memory pool with "no copy", so the same object is mapped in several places (share_mode TRUESHARED, ref_count>1), and summing region by region would double count;
// measured on Mac, after per-object dedup Σcharged is 2,139 MiB against phys_footprint 2,111 MiB (+1.3%)
//   Classes: guest (the whole guest address space block, including the SetHeapSize heap, alias, stacks; data read from RomFS through pread also lands here), code (embedded / loaded module images),
//         malloc (host heap), gpu (IOAccelerator / IOSurface / Metal tagged regions), stack, file (file mappings), other
//   The part where charged ≠ phys_footprint (GPU private memory accounted by IOKit is not in any VM region) is listed separately as "unattributed" = footprint − Σcharged.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <mach/vm_statistics.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif
#include "dm.h"

void svc_guest_range(uint8_t **base, size_t *size);
enum { C_GUEST, C_CODE, C_MALLOC, C_GPU, C_STACK, C_FILE, C_OTHER, NC };
static const char *k_cls[NC] = {"guest", "code", "malloc", "gpu", "stack", "file", "other"};

static int classify(vm_address_t a, const vm_region_submap_info_data_64_t *in) {
    uint8_t *gb; size_t gs; svc_guest_range(&gb, &gs);
    if (gb && a >= (vm_address_t)gb && a < (vm_address_t)gb + gs) return C_GUEST;
    for (int i = 0; i < g_nmod; i++)
        if (a >= (vm_address_t)g_mod[i].base && a < (vm_address_t)g_mod[i].base + g_mod[i].size) return C_CODE;
    switch (in->user_tag) {
        case VM_MEMORY_MALLOC: case VM_MEMORY_MALLOC_SMALL: case VM_MEMORY_MALLOC_LARGE: case VM_MEMORY_MALLOC_HUGE:
        case VM_MEMORY_MALLOC_TINY: case VM_MEMORY_MALLOC_LARGE_REUSABLE: case VM_MEMORY_MALLOC_LARGE_REUSED:
        case VM_MEMORY_MALLOC_NANO: case VM_MEMORY_MALLOC_MEDIUM: return C_MALLOC;
        case VM_MEMORY_IOACCELERATOR: case VM_MEMORY_IOSURFACE: case VM_MEMORY_IOKIT: return C_GPU;
        case VM_MEMORY_STACK: return C_STACK;
    }
    if (in->external_pager) return C_FILE;
    return C_OTHER;
}

// DM_MEMSTAT_DUMP=seconds: at that second, write every VM region ≥1 MiB line by line to <tsv>.regions (for diagnosing the accounting method)
static void dump_regions(const char *path) {
    FILE *f = fopen(path, "w"); if (!f) return;
    fprintf(f, "start	size MiB	class	user_tag	share_mode	obj_id	ref_count	resident MiB	dirty MiB	swapped MiB	file\n");
    vm_address_t a = 0; vm_size_t sz; natural_t depth = 0; vm_size_t pg = vm_page_size;
    for (;;) {
        vm_region_submap_info_data_64_t in; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(mach_task_self(), &a, &sz, &depth, (vm_region_recurse_info_t)&in, &cnt) != KERN_SUCCESS) break;
        if (in.is_submap) { depth++; continue; }
        if (((uint64_t)in.pages_resident * pg >> 20) >= 1 || sz >= (64ull << 20))
            fprintf(f, "%#lx\t%lu\t%s\t%u\t%u\t%#x\t%u\t%llu\t%llu\t%llu\t%d\n", a, sz >> 20, k_cls[classify(a, &in)], in.user_tag, in.share_mode,
                    in.object_id, in.ref_count, (uint64_t)in.pages_resident * pg >> 20, (uint64_t)in.pages_dirtied * pg >> 20, (uint64_t)in.pages_swapped_out * pg >> 20, in.external_pager);
        a += sz;
    }
    fclose(f);
}

static void sample(FILE *f, double t, id<MTLDevice> dev) {
    task_vm_info_data_t vi; mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    uint64_t fp = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n) == KERN_SUCCESS ? vi.phys_footprint : 0;
    uint64_t avail = 0;
#if TARGET_OS_IPHONE
    avail = os_proc_available_memory();
#endif
    uint64_t virt[NC] = {0}, res[NC] = {0}, bill[NC] = {0};
    static uint32_t seen[1 << 16]; static uint32_t gen; gen++; if (!gen) { memset(seen, 0, sizeof seen); gen = 1; }   // Objects already counted in this sample (open addressing)
    vm_address_t a = 0; vm_size_t sz; natural_t depth = 0; vm_size_t pg = vm_page_size;
    for (;;) {
        vm_region_submap_info_data_64_t in; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(mach_task_self(), &a, &sz, &depth, (vm_region_recurse_info_t)&in, &cnt) != KERN_SUCCESS) break;
        if (in.is_submap) { depth++; continue; }
        int c = classify(a, &in);
        virt[c] += sz; res[c] += (uint64_t)in.pages_resident * pg;
        if (!in.external_pager) {                             // File pages are reclaimable and do not count toward footprint
            static uint64_t key[1 << 16];                     // Key = (object id, offset within the object): the pieces of one object split apart by mprotect cover different pages and must not be merged
            uint64_t k = (uint64_t)in.object_id << 32 ^ (in.offset >> 14);
            uint32_t h = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 48), dup = 0;
            while (seen[h] == gen) { if (key[h] == k) { dup = 1; break; } h = (h + 1) & 0xFFFF; }
            if (!dup) { seen[h] = gen; key[h] = k; bill[c] += ((uint64_t)in.pages_dirtied + in.pages_swapped_out) * pg; }
        }
        a += sz; continue;
    }
    uint64_t sum = 0; for (int c = 0; c < NC; c++) sum += bill[c];
    fprintf(f, "%.0f\t%llu\t%llu\t%llu\t%llu\t%llu\t%lld\t%lld", t, fp >> 20, avail >> 20, (unsigned long long)(dev.currentAllocatedSize >> 20),
            vi.internal >> 20, vi.compressed >> 20, (vi.ledger_tag_graphics_footprint + vi.ledger_tag_graphics_footprint_compressed) >> 20,
            (long long)(vi.limit_bytes_remaining >> 20));
    for (int c = 0; c < NC; c++) fprintf(f, "\t%llu/%llu/%llu", virt[c] >> 20, res[c] >> 20, bill[c] >> 20);
    fprintf(f, "\t%llu\t%lld\n", sum >> 20, ((int64_t)fp - (int64_t)sum) >> 20);
    fflush(f);
}

static void *memstat_thread(void *p) {
    FILE *f = fopen((const char *)p, "w");
    if (!f) { LOG("memstat: cannot open %s", (const char *)p); return NULL; }
    fprintf(f, "# unit MiB; kernel accounting internal / compressed / graphics ledger / headroom to the limit; class columns = virtual/resident/charged (deduplicated per object); charged total = Σ private charged; unattributed = phys_footprint − charged total\nseconds\tphys_footprint\tavailable\tmetal_alloc\tinternal\tcompressed\tgraphics\theadroom");
    for (int c = 0; c < NC; c++) fprintf(f, "\t%s", k_cls[c]);
    fprintf(f, "\tcharged total\tunattributed\n");
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();   // The same device object as the graphics backend (the system default device singleton)
    uint64_t t0 = now_ns();
    const char *ds = getenv("DM_MEMSTAT_DUMP"); int dump_at = ds ? atoi(ds) : -1;
    char dp[1100]; snprintf(dp, sizeof dp, "%s.regions", (const char *)p);
    for (int sec = 0;; sec++) { sample(f, (now_ns() - t0) / 1e9, dev); if (sec == dump_at) dump_regions(dp); sleep(1); }
    return NULL;
}

__attribute__((constructor)) static void memstat_start(void) {
    const char *p = getenv("DM_MEMSTAT");
    if (!p || !*p) return;
    char *path = strdup(p);
#if TARGET_OS_IPHONE
    if (*p != '/') { size_t n = strlen(getenv("HOME")) + strlen(p) + 16; path = malloc(n); snprintf(path, n, "%s/Documents/%s", getenv("HOME"), p); }   // Relative name → the container's Documents (the container path is not known before launch)
#endif
    pthread_t th; pthread_create(&th, NULL, memstat_thread, path); pthread_detach(th);
}

// ---------- Per-thread CPU: DM_THRCPU=<tsv path>[,interval in seconds (default 10)] (common to Mac / iOS; on iOS a relative name → the container's Documents) ----------
// Every interval, walk all threads of this process (task_threads), take the thread name and user + system CPU time (ns) from THREAD_EXTENDED_INFO, and difference them by thread id:
// each line is "seconds, thread id, thread name, CPU% (100 = one core)"; the last line is "seconds, -, total, CPU%". Used for heat attribution (which thread eats CPU); enable together with DM_HEAT to compare against thermal state.
#include <mach/thread_info.h>
#define THR_MAX 512
static void *thrcpu_thread(void *p) {
    char path[1024]; int iv = 10; { const char *c = strchr((const char *)p, ','); snprintf(path, sizeof path, "%.*s", c ? (int)(c - (const char *)p) : (int)strlen(p), (const char *)p); if (c) iv = atoi(c + 1); }
    FILE *f = fopen(path, "w"); if (!f) return NULL;
    fprintf(f, "seconds\tthread ID\tthread name\tCPU%%\n"); fflush(f);
    static uint64_t ids[THR_MAX], last[THR_MAX]; int n = 0; uint64_t t0 = now_ns(), tl = t0;
    for (;;) {
        sleep(iv); uint64_t t = now_ns(); double dt = (t - tl) / 1e9, tot = 0; tl = t;
        thread_act_array_t th; mach_msg_type_number_t nt = 0;
        if (task_threads(mach_task_self(), &th, &nt) != KERN_SUCCESS) continue;
        for (mach_msg_type_number_t i = 0; i < nt; i++) {
            thread_extended_info_data_t ei; mach_msg_type_number_t c1 = THREAD_EXTENDED_INFO_COUNT;
            thread_identifier_info_data_t ii; mach_msg_type_number_t c2 = THREAD_IDENTIFIER_INFO_COUNT;
            if (thread_info(th[i], THREAD_EXTENDED_INFO, (thread_info_t)&ei, &c1) == KERN_SUCCESS && thread_info(th[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&ii, &c2) == KERN_SUCCESS) {
                uint64_t cpu = ei.pth_user_time + ei.pth_system_time; int k = 0; while (k < n && ids[k] != ii.thread_id) k++;
                if (k == n && n < THR_MAX) { ids[n] = ii.thread_id; last[n] = cpu; n++; }   // New thread: only record the baseline this round
                else if (k < n) { double pct = (cpu - last[k]) / 1e9 / dt * 100; last[k] = cpu; tot += pct;
                    if (pct >= 0.5) { char gn[96]; int thr_name_of(pthread_t, char *, size_t); pthread_t pt = pthread_from_mach_thread_np(th[i]);
                        const char *nm = pt && thr_name_of(pt, gn, sizeof gn) ? gn : ei.pth_name[0] ? ei.pth_name : "-";   // Guest thread: handle + guest name; host thread: pthread name
                        fprintf(f, "%.0f\t%llu\t%s\t%.1f\n", (t - t0) / 1e9, ii.thread_id, nm, pct); } }
            }
            mach_port_deallocate(mach_task_self(), th[i]);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)th, nt * sizeof *th);
        fprintf(f, "%.0f\t-\ttotal\t%.1f\n", (t - t0) / 1e9, tot); fflush(f);
    }
    return NULL;
}
__attribute__((constructor)) static void thrcpu_start(void) {
    const char *p = getenv("DM_THRCPU");
    if (!p || !*p) return;
    char *spec = strdup(p);
#if TARGET_OS_IPHONE
    if (*p != '/') { size_t n = strlen(getenv("HOME")) + strlen(p) + 16; spec = malloc(n); snprintf(spec, n, "%s/Documents/%s", getenv("HOME"), p); }
#endif
    pthread_t th; pthread_create(&th, NULL, thrcpu_thread, spec); pthread_detach(th);
}

#if TARGET_OS_OSX
#include <IOKit/IOKitLib.h>
// GPU time by process over the window since the last line (hle_nvn.c's power summary calls it): the accelerator's user clients each list their creator and, per command queue,
// the GPU time it has run (AppUsage accumulatedGPUTime, ns). A game whose frame leaves the GPU little time to spare misses its frame rate when other apps draw, and nothing in
// the game's own timing says so: its command buffers' GPU spans include the time the GPU ran someone else's work (docs/reference/frame-rate.md "Other apps' GPU work")
enum { GU_N = 96 };
typedef struct { int pid; char name[24]; uint64_t ns; } gpu_user;
static int gpu_users(gpu_user *u) {
    io_iterator_t accs = 0; int n = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"), &accs) != KERN_SUCCESS) return 0;
    for (io_service_t acc; (acc = IOIteratorNext(accs)); IOObjectRelease(acc)) {
        io_iterator_t kids = 0; if (IORegistryEntryGetChildIterator(acc, kIOServicePlane, &kids) != KERN_SUCCESS) continue;
        for (io_registry_entry_t c; (c = IOIteratorNext(kids)); IOObjectRelease(c)) {
            NSString *who = CFBridgingRelease(IORegistryEntryCreateCFProperty(c, CFSTR("IOUserClientCreator"), NULL, 0));
            NSArray *use = CFBridgingRelease(IORegistryEntryCreateCFProperty(c, CFSTR("AppUsage"), NULL, 0));
            int pid = 0; char name[24] = "";
            if (![who isKindOfClass:NSString.class] || ![use isKindOfClass:NSArray.class] || sscanf(who.UTF8String, "pid %d, %23[^\n]", &pid, name) < 1) continue;
            uint64_t ns = 0; for (NSDictionary *q in use) if ([q isKindOfClass:NSDictionary.class]) ns += [q[@"accumulatedGPUTime"] unsignedLongLongValue];
            int k = 0; while (k < n && u[k].pid != pid) k++;
            if (k < n) u[k].ns += ns;
            else if (n < GU_N && ns) { u[n].pid = pid; snprintf(u[n].name, sizeof u[n].name, "%s", name); u[n].ns = ns; n++; }
        }
        IOObjectRelease(kids);
    }
    IOObjectRelease(accs); return n;
}
void dm_gpu_share_line(uint64_t frame, double frames) {
    static dispatch_queue_t q; if (!q) q = dispatch_queue_create("dm.gpu-share", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    dispatch_async(q, ^{ @autoreleasepool {   // off the frame's thread: the registry walk takes a few milliseconds
        static gpu_user prev[GU_N]; static int pn; static uint64_t pt;
        gpu_user u[GU_N]; int n = gpu_users(u), me = getpid(); uint64_t t = now_ns();
        if (pt && t > pt && n) {
            double s = (t - pt) / 1e9, own = 0, others = 0; struct { double ms; const char *name; } top[4] = {{0}};
            for (int i = 0; i < n; i++) {
                int k = 0; while (k < pn && prev[k].pid != u[i].pid) k++;
                uint64_t was = k < pn ? prev[k].ns : 0; double ms = u[i].ns > was ? (u[i].ns - was) / 1e6 / s : 0;   // a closed command queue takes its time out of the sum
                if (u[i].pid == me) { own = ms; continue; }
                others += ms;
                for (int j = 0; j < 4; j++) if (ms > top[j].ms) { memmove(&top[j + 1], &top[j], sizeof top[0] * (size_t)(3 - j)); top[j].ms = ms; top[j].name = u[i].name; break; }
            }
            char list[160] = ""; size_t o = 0;
            for (int j = 0; j < 4 && top[j].ms >= 1 && o < sizeof list; j++) o += (size_t)snprintf(list + o, sizeof list - o, "%s%s %.0f", j ? ", " : ": ", top[j].name, top[j].ms);
            LOG("[nvn] GPU by process up to frame %llu: the game %.0f ms/s (%.2f ms per frame), other processes %.0f ms/s%s", (unsigned long long)frame, own, frames > 0 ? own * s / frames : 0, others, list);
        }
        memcpy(prev, u, sizeof u); pn = n; pt = t;
    } });
}
#endif
