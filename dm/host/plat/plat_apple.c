// Host platform layer, macOS / iOS: the calls the host made inline before the layer existed, moved here unchanged (the file each came from is named at it)
#include "plat.h"
#include <CommonCrypto/CommonDigest.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <malloc/malloc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/clonefile.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

_Static_assert(sizeof(plat_thread_t) == sizeof(mach_port_t), "plat_thread_t holds a Mach thread port");
_Static_assert(sizeof(CC_SHA256_CTX) <= sizeof(plat_sha256_ctx), "plat_sha256_ctx holds a CC_SHA256_CTX");

// ---------- Time ----------
uint64_t plat_now_ns(void) {   // hle_os.c now_ns
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}
uint64_t plat_boot_ns(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }   // hle_time.c steady clock, hle_fs.c commit timing
void plat_sleep_ns(uint64_t ns) { struct timespec ts = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)}; nanosleep(&ts, 0); }

// ---------- Threads ----------
int plat_qos_set_self(plat_qos_t q) { return pthread_set_qos_class_self_np(q, 0); }   // hle_thread.c
plat_qos_t plat_qos_self(void) { qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0; pthread_get_qos_class_np(pthread_self(), &qc, &rp); return qc; }
void *plat_qos_override_start(pthread_t t, plat_qos_t q) { return pthread_override_qos_class_start_np(t, q, 0); }
void plat_qos_override_end(void *ov) { if (ov) pthread_override_qos_class_end_np(ov); }
void plat_thread_set_name(const char *name) { pthread_setname_np(name); }
plat_thread_t plat_thread_self(void) { return pthread_mach_thread_np(pthread_self()); }
int plat_thread_cpu_us(plat_thread_t t, uint64_t *us) {   // hle_thread.c dm_thread_cpu / svc_thread_dump
    thread_basic_info_data_t bi; mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
    if (!t || thread_info(t, THREAD_BASIC_INFO, (thread_info_t)&bi, &c) != KERN_SUCCESS) return -1;
    *us = (uint64_t)(bi.user_time.seconds + bi.system_time.seconds) * 1000000u + (uint64_t)(bi.user_time.microseconds + bi.system_time.microseconds);
    return 0;
}
int plat_thread_stack(uint64_t *top, uint64_t *size) {   // hle_svc.c QueryMemory
    *top = (uint64_t)pthread_get_stackaddr_np(pthread_self()); *size = pthread_get_stacksize_np(pthread_self());
    return 0;
}
int plat_cpu_count(void) { return (int)sysconf(_SC_NPROCESSORS_ONLN); }   // hle_movie.c

// ---------- Semaphores (hle_audio.c) ----------
plat_sema *plat_sema_create(long value) { return (plat_sema *)dispatch_semaphore_create(value); }
void plat_sema_signal(plat_sema *s) { dispatch_semaphore_signal((dispatch_semaphore_t)s); }
int plat_sema_wait(plat_sema *s, int64_t timeout_ns) {
    return (int)dispatch_semaphore_wait((dispatch_semaphore_t)s, timeout_ns < 0 ? DISPATCH_TIME_FOREVER : dispatch_time(DISPATCH_TIME_NOW, timeout_ns));
}
void plat_sema_destroy(plat_sema *s) { dispatch_release((dispatch_semaphore_t)s); }

// ---------- Deferred work and termination (hle_lifecycle.c) ----------
void plat_after_ms(unsigned ms, void (*fn)(void)) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, ms * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{ fn(); });
}
void plat_on_terminate(void (*fn)(const char *why)) {
    signal(SIGTERM, SIG_IGN);                       // do not take the default action: hand over to the GCD signal source to wrap up on an ordinary thread
    static dispatch_source_t src;
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    dispatch_source_set_event_handler(src, ^{ fn("SIGTERM"); });
    dispatch_resume(src);
}

// ---------- Virtual memory (hle_svc.c, loader.c, hle_sdk16.c) ----------
size_t plat_page_size(void) { return (size_t)getpagesize(); }
size_t plat_vm_granularity(void) { return (size_t)getpagesize(); }
void *plat_vm_reserve(size_t n) { void *p = mmap(NULL, n, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0); return p == MAP_FAILED ? NULL : p; }
void *plat_vm_alloc(size_t n) { void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); return p == MAP_FAILED ? NULL : p; }
int plat_vm_protect(void *a, size_t n, int prot) { return mprotect(a, n, prot); }
int plat_vm_discard(void *a, size_t n) { return madvise(a, n, MADV_FREE); }
int plat_vm_release(void *a, size_t n) { return munmap(a, n); }
int plat_vm_query(uint64_t addr, uint64_t *base, uint64_t *size, int *prot) {   // hle_uemalloc.c, hle_fs.c: vm_region_64 (iOS has no mach_vm.h)
    vm_address_t at = (vm_address_t)addr; vm_size_t sz = 0; vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj = MACH_PORT_NULL;
    kern_return_t kr = vm_region_64(mach_task_self(), &at, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj);
    if (kr != KERN_SUCCESS) return (int)kr;
    *base = at; *size = sz;
    *prot = (info.protection & VM_PROT_READ ? PLAT_PROT_READ : 0) | (info.protection & VM_PROT_WRITE ? PLAT_PROT_WRITE : 0) | (info.protection & VM_PROT_EXECUTE ? PLAT_PROT_EXEC : 0);
    return 0;
}
int plat_mem_read(uint64_t addr, void *out, size_t n, size_t *got) {   // vm_read_overwrite fails instead of faulting
    vm_size_t g = 0; kern_return_t kr = vm_read_overwrite(mach_task_self(), (vm_address_t)addr, n, (vm_address_t)out, &g);
    if (got) *got = g;
    return (int)kr;
}

// ---------- A separate heap: a malloc zone (hle_uemalloc.c) ----------
plat_zone *plat_zone_create(const char *name) { malloc_zone_t *z = malloc_create_zone(0, 0); malloc_set_zone_name(z, name); return (plat_zone *)z; }
void *plat_zone_malloc(plat_zone *z, size_t n) { return malloc_zone_malloc((malloc_zone_t *)z, n); }
void *plat_zone_memalign(plat_zone *z, size_t align, size_t n) { return malloc_zone_memalign((malloc_zone_t *)z, align, n); }
void *plat_zone_realloc(plat_zone *z, void *p, size_t n) { return malloc_zone_realloc((malloc_zone_t *)z, p, n); }
void plat_zone_free(plat_zone *z, void *p) { malloc_zone_free((malloc_zone_t *)z, p); }
size_t plat_zone_size(plat_zone *z, const void *p) { (void)z; return malloc_size(p); }
size_t plat_zone_good_size(size_t n) { return malloc_good_size(n); }

// ---------- Files (hle_fs.c, hle_fdstat.c) ----------
int plat_clone_tree(const char *src, const char *dst) { return clonefile(src, dst, 0); }
int plat_rename_swap(const char *a, const char *b) { return renamex_np(a, b, RENAME_SWAP); }
ssize_t plat_xattr_get(const char *path, const char *name, void *buf, size_t n) { return getxattr(path, name, buf, n, 0, XATTR_NOFOLLOW); }
int plat_xattr_set(const char *path, const char *name, const void *v, size_t n) { return setxattr(path, name, v, n, 0, XATTR_NOFOLLOW); }
int plat_xattr_remove(const char *path, const char *name) { return removexattr(path, name, XATTR_NOFOLLOW); }
int plat_fd_path(int fd, char *out, size_t n) {
    char p[MAXPATHLEN] = "";
    if (fcntl(fd, F_GETPATH, p) || !p[0]) return -1;
    snprintf(out, n, "%s", p); return 0;
}
int plat_data_path(char *buf, size_t n, const char *rel) {   // hle_fs.c envdir without DM_ROOT: the snprintf it made, "%s%s/%s" with $HOME, "/dm-work", rel
    return snprintf(buf, n, "%s%s/%s", getenv("HOME"), "/dm-work", rel);
}

// ---------- Random bytes and SHA-256 (hle_os.c; hle_ro.c, loader.c) ----------
uint32_t plat_random32(void) { return arc4random(); }
void plat_random_buf(void *p, size_t n) { arc4random_buf(p, n); }
void plat_sha256_init(plat_sha256_ctx *c) { CC_SHA256_Init((CC_SHA256_CTX *)c); }
void plat_sha256_update(plat_sha256_ctx *c, const void *p, size_t n) {
    for (const uint8_t *b = p; n; ) { CC_LONG k = n > 0x40000000u ? 0x40000000u : (CC_LONG)n; CC_SHA256_Update((CC_SHA256_CTX *)c, b, k); b += k; n -= k; }
}
void plat_sha256_final(plat_sha256_ctx *c, uint8_t out[32]) { CC_SHA256_Final(out, (CC_SHA256_CTX *)c); }
void plat_sha256(const void *p, size_t n, uint8_t out[32]) { plat_sha256_ctx c; plat_sha256_init(&c); plat_sha256_update(&c, p, n); plat_sha256_final(&c, out); }

// ---------- Symbols ----------
const char *plat_addr_symbol(const void *a, uintptr_t *start) {
    Dl_info di;
    if (!dladdr(a, &di) || !di.dli_sname) return NULL;
    if (start) *start = (uintptr_t)di.dli_saddr;
    return di.dli_sname;
}
