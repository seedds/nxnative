// Host platform layer, Linux (glibc 2.30+, kernel 4.8+)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "plat.h"
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <malloc.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

// ---------- Time ----------
uint64_t plat_now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec; }
uint64_t plat_boot_ns(void) { struct timespec ts; clock_gettime(CLOCK_BOOTTIME, &ts); return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec; }
void plat_sleep_ns(uint64_t ns) { struct timespec ts = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)}; nanosleep(&ts, 0); }

// ---------- Threads ----------
// QoS classes as nice values of the thread (setpriority on its kernel thread ID). Lowering a thread's priority always works; raising it back needs CAP_SYS_NICE or RLIMIT_NICE,
// and without them the class stays where it was, as a refused pthread_set_qos_class_self_np does on Apple
static __thread plat_qos_t t_qos = PLAT_QOS_UNSPECIFIED;
static int qos_nice(plat_qos_t q) { return q == PLAT_QOS_UTILITY ? 10 : q == PLAT_QOS_USER_INITIATED ? 5 : 0; }
int plat_qos_set_self(plat_qos_t q) {
    if (setpriority(PRIO_PROCESS, (id_t)gettid(), qos_nice(q))) return -1;
    t_qos = q; return 0;
}
plat_qos_t plat_qos_self(void) { return t_qos; }
void *plat_qos_override_start(pthread_t t, plat_qos_t q) { (void)t; (void)q; return NULL; }   // Linux has no priority loan between threads of a process
void plat_qos_override_end(void *ov) { (void)ov; }
void plat_thread_set_name(const char *name) { char b[16]; snprintf(b, sizeof b, "%s", name); pthread_setname_np(pthread_self(), b); }
plat_thread_t plat_thread_self(void) { return (plat_thread_t)gettid(); }
int plat_thread_cpu_us(plat_thread_t t, uint64_t *us) {
    // The kernel's per-thread CPU clock of a thread ID (MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED), what pthread_getcpuclockid returns): unlike a pthread_t it can be asked after the thread exited, and then fails
    if (!t) return -1;
    struct timespec ts; clockid_t c = (clockid_t)((~(unsigned)t << 3) | 6);
    if (clock_gettime(c, &ts)) return -1;
    *us = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u; return 0;
}
int plat_thread_stack(uint64_t *top, uint64_t *size) {
    pthread_attr_t a; void *lo = NULL; size_t n = 0;
    if (pthread_getattr_np(pthread_self(), &a)) return -1;
    int r = pthread_attr_getstack(&a, &lo, &n); pthread_attr_destroy(&a);
    if (r) return -1;
    *top = (uint64_t)(uintptr_t)lo + n; *size = n; return 0;
}
int plat_cpu_count(void) { return (int)sysconf(_SC_NPROCESSORS_ONLN); }

// ---------- Semaphores ----------
struct plat_sema { sem_t s; };
plat_sema *plat_sema_create(long value) {
    plat_sema *s = malloc(sizeof *s);
    if (s && sem_init(&s->s, 0, (unsigned)value)) { free(s); return NULL; }
    return s;
}
void plat_sema_signal(plat_sema *s) { sem_post(&s->s); }
int plat_sema_wait(plat_sema *s, int64_t timeout_ns) {
    if (timeout_ns < 0) { while (sem_wait(&s->s)) if (errno != EINTR) return -1; return 0; }
    struct timespec dl; clock_gettime(CLOCK_MONOTONIC, &dl);
    dl.tv_sec += timeout_ns / 1000000000; dl.tv_nsec += timeout_ns % 1000000000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
    while (sem_clockwait(&s->s, CLOCK_MONOTONIC, &dl)) if (errno != EINTR) return -1;
    return 0;
}
void plat_sema_destroy(plat_sema *s) { if (s) { sem_destroy(&s->s); free(s); } }

// ---------- Deferred work and termination ----------
typedef struct { unsigned ms; void (*fn)(void); } after_t;
static void *after_main(void *p) { after_t a = *(after_t *)p; free(p); plat_sleep_ns((uint64_t)a.ms * 1000000u); a.fn(); return NULL; }
void plat_after_ms(unsigned ms, void (*fn)(void)) {
    after_t *a = malloc(sizeof *a); if (!a) return;
    a->ms = ms; a->fn = fn; pthread_t t;
    if (pthread_create(&t, NULL, after_main, a)) { free(a); return; }
    pthread_detach(t);
}
// SIGTERM: the handler only posts a semaphore (async-signal-safe); a watcher thread runs the callback on an ordinary thread, as the GCD signal source does on Apple
static sem_t g_term_sem; static void (*g_term_fn)(const char *);
static void term_handler(int sig) { (void)sig; sem_post(&g_term_sem); }
static void *term_main(void *p) { (void)p; while (sem_wait(&g_term_sem)) {} g_term_fn("SIGTERM"); return NULL; }
void plat_on_terminate(void (*fn)(const char *why)) {
    static int done; if (done++) return;
    g_term_fn = fn; sem_init(&g_term_sem, 0, 0);
    pthread_t t; if (pthread_create(&t, NULL, term_main, NULL)) return;
    pthread_detach(t);
    struct sigaction sa = {.sa_handler = term_handler, .sa_flags = SA_RESTART}; sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
}

// ---------- Virtual memory ----------
size_t plat_page_size(void) { return (size_t)sysconf(_SC_PAGESIZE); }
size_t plat_vm_granularity(void) { return (size_t)sysconf(_SC_PAGESIZE); }
// MAP_NORESERVE: the reservation charges no commit, and neither does making parts of it writable later (mprotect skips the accounting of a VM_NORESERVE mapping),
// so a 5 GB guest block does not fail under the kernel's heuristic overcommit on a smaller machine; pages count when first written
void *plat_vm_reserve(size_t n) { void *p = mmap(NULL, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0); return p == MAP_FAILED ? NULL : p; }
void *plat_vm_alloc(size_t n) { void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); return p == MAP_FAILED ? NULL : p; }
int plat_vm_protect(void *a, size_t n, int prot) { return mprotect(a, n, prot); }
int plat_vm_discard(void *a, size_t n) {
#ifdef MADV_FREE
    if (!madvise(a, n, MADV_FREE)) return 0;
    if (errno != EINVAL) return -1;   // EINVAL: a kernel before 4.5, which has only MADV_DONTNEED
#endif
    return madvise(a, n, MADV_DONTNEED);
}
int plat_vm_release(void *a, size_t n) { return munmap(a, n); }
int plat_vm_query(uint64_t addr, uint64_t *base, uint64_t *size, int *prot) {
    FILE *f = fopen("/proc/self/maps", "r"); if (!f) return -1;
    char line[512]; int r = -1;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi; char perm[5];
        if (sscanf(line, "%lx-%lx %4s", &lo, &hi, perm) != 3 || hi <= addr) continue;
        *base = lo; *size = hi - lo;   // the region holding addr, or the next one (vm_region's answer)
        *prot = (perm[0] == 'r' ? PLAT_PROT_READ : 0) | (perm[1] == 'w' ? PLAT_PROT_WRITE : 0) | (perm[2] == 'x' ? PLAT_PROT_EXEC : 0);
        r = 0; break;
    }
    fclose(f); return r;
}
// process_vm_readv on the process itself reads without faulting (an unreadable page ends the copy). Where a seccomp filter refuses it (old container runtimes), a pipe does the
// same: write() of an unreadable buffer fails with EFAULT instead of faulting
static pthread_mutex_t g_rd_lock = PTHREAD_MUTEX_INITIALIZER; static int g_rd_pipe[2] = {-1, -1}, g_rd_mode;   // mode 0 untried, 1 process_vm_readv, 2 pipe
static int read_pipe(uint64_t addr, void *out, size_t n, size_t *got) {
    pthread_mutex_lock(&g_rd_lock);
    if (g_rd_pipe[0] < 0 && pipe2(g_rd_pipe, O_CLOEXEC)) { pthread_mutex_unlock(&g_rd_lock); return -1; }
    size_t done = 0; int r = 0;
    while (done < n) {
        size_t k = n - done < 4096 ? n - done : 4096;
        ssize_t w = write(g_rd_pipe[1], (const void *)(uintptr_t)(addr + done), k);
        if (w <= 0) { r = -1; break; }
        for (ssize_t m = 0; m < w; ) { ssize_t q = read(g_rd_pipe[0], (uint8_t *)out + done + m, (size_t)(w - m)); if (q <= 0) { r = -1; break; } m += q; }
        if (r) break;
        done += (size_t)w;
    }
    pthread_mutex_unlock(&g_rd_lock);
    if (got) *got = done;
    return r || done != n;
}
int plat_mem_read(uint64_t addr, void *out, size_t n, size_t *got) {
    if (g_rd_mode != 2) {
        struct iovec l = {out, n}, rm = {(void *)(uintptr_t)addr, n};
        ssize_t k = process_vm_readv(getpid(), &l, 1, &rm, 1, 0);
        if (k >= 0 || (errno != ENOSYS && errno != EPERM)) { g_rd_mode = 1; if (got) *got = k > 0 ? (size_t)k : 0; return k != (ssize_t)n; }
        g_rd_mode = 2;
    }
    return read_pipe(addr, out, n, got);
}

// ---------- A separate heap: glibc's malloc (its blocks know their usable size) ----------
struct plat_zone { int unused; };
plat_zone *plat_zone_create(const char *name) { (void)name; static plat_zone z; return &z; }
void *plat_zone_malloc(plat_zone *z, size_t n) { (void)z; return malloc(n); }
void *plat_zone_memalign(plat_zone *z, size_t align, size_t n) { (void)z; void *p = NULL; return posix_memalign(&p, align, n) ? NULL : p; }
void *plat_zone_realloc(plat_zone *z, void *p, size_t n) { (void)z; return realloc(p, n); }
void plat_zone_free(plat_zone *z, void *p) { (void)z; free(p); }
size_t plat_zone_size(plat_zone *z, const void *p) { (void)z; return malloc_usable_size((void *)p); }
size_t plat_zone_good_size(size_t n) { return n <= 16 ? 16 : (n + 15) & ~(size_t)15; }

// ---------- Files ----------
// Attributes the host keeps are in the user. namespace. A file system without extended attributes cannot hold any: get and remove answer "none" there, as on a volume without one
static void xname(char *out, size_t n, const char *name) { snprintf(out, n, "user.%s", name); }
ssize_t plat_xattr_get(const char *path, const char *name, void *buf, size_t n) {
    char x[256]; xname(x, sizeof x, name);
    ssize_t k = lgetxattr(path, x, buf, n);
    if (k < 0 && errno == ENOTSUP) errno = ENODATA;
    return k;
}
int plat_xattr_set(const char *path, const char *name, const void *v, size_t n) { char x[256]; xname(x, sizeof x, name); return lsetxattr(path, x, v, n, 0); }
int plat_xattr_remove(const char *path, const char *name) {
    char x[256]; xname(x, sizeof x, name);
    int r = lremovexattr(path, x);
    if (r && errno == ENOTSUP) errno = ENODATA;
    return r;
}
static void copy_xattrs(const char *src, const char *dst) {   // the user. attributes (guest names); best effort, a file system without them has none to copy
    char names[4096]; ssize_t n = llistxattr(src, names, sizeof names);
    for (ssize_t i = 0; i < n; i += (ssize_t)strlen(names + i) + 1) {
        if (strncmp(names + i, "user.", 5)) continue;
        char v[1024]; ssize_t k = lgetxattr(src, names + i, v, sizeof v);
        if (k >= 0) lsetxattr(dst, names + i, v, (size_t)k, 0);
    }
}
static int copy_file(const char *src, const char *dst, mode_t mode) {
    int in = open(src, O_RDONLY | O_CLOEXEC); if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode & 07777);
    if (out < 0) { int e = errno; close(in); errno = e; return -1; }
    int r = 0;
    if (ioctl(out, FICLONE, in)) {   // a reflink where the file system has them (Btrfs, XFS): the copy-on-write clone clonefile makes on APFS
        for (;;) {
            ssize_t k = copy_file_range(in, NULL, out, NULL, 1 << 30, 0);
            if (k > 0) continue;
            if (!k) break;
            if (errno != EXDEV && errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP) { r = -1; break; }
            char b[65536]; ssize_t q;   // no copy_file_range between these file systems: an ordinary copy from where it stopped
            while ((q = read(in, b, sizeof b)) > 0) for (ssize_t m = 0; m < q; ) { ssize_t w = write(out, b + m, (size_t)(q - m)); if (w <= 0) { r = -1; break; } m += w; }
            if (q < 0) r = -1;
            break;
        }
    }
    int e = errno; close(in); if (close(out)) r = -1;
    if (r) { errno = e; return -1; }
    copy_xattrs(src, dst); return 0;
}
static int rm_tree_any(const char *p) {
    struct stat st; if (lstat(p, &st)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(p);
    DIR *d = opendir(p); if (!d) return -1;
    struct dirent *e; char c[4096]; int r = 0;
    while ((e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; snprintf(c, sizeof c, "%s/%s", p, e->d_name); r |= rm_tree_any(c); }
    closedir(d); return r | rmdir(p);
}
static int clone_into(const char *src, const char *dst) {
    struct stat st; if (lstat(src, &st)) return -1;
    if (S_ISLNK(st.st_mode)) { char t[4096]; ssize_t k = readlink(src, t, sizeof t - 1); if (k < 0) return -1; t[k] = 0; return symlink(t, dst); }
    if (!S_ISDIR(st.st_mode)) return copy_file(src, dst, st.st_mode);
    if (mkdir(dst, st.st_mode & 07777)) return -1;
    copy_xattrs(src, dst);
    DIR *d = opendir(src); if (!d) return -1;
    struct dirent *e; char s[4096], t[4096]; int r = 0;
    while (!r && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if ((size_t)snprintf(s, sizeof s, "%s/%s", src, e->d_name) >= sizeof s || (size_t)snprintf(t, sizeof t, "%s/%s", dst, e->d_name) >= sizeof t) { errno = ENAMETOOLONG; r = -1; break; }
        r = clone_into(s, t);
    }
    int e2 = errno; closedir(d); errno = e2; return r;
}
// A copy of the tree (reflinks where the file system has them), not one atomic clone: on failure the partial copy is removed, so dst is left as clonefile leaves it, absent
int plat_clone_tree(const char *src, const char *dst) {
    struct stat st; if (!lstat(dst, &st)) { errno = EEXIST; return -1; }
    if (!clone_into(src, dst)) return 0;
    int e = errno; rm_tree_any(dst); errno = e; return -1;
}
// RENAME_EXCHANGE swaps the two names in one step (ext4, Btrfs, XFS, tmpfs). File systems without it (some FUSE and network mounts) take three renames: not atomic, a crash between them
// leaves a's tree under a temporary name next to b (a platform limit there, the price of having saves at all on such a mount)
int plat_rename_swap(const char *a, const char *b) {
    if (!renameat2(AT_FDCWD, a, AT_FDCWD, b, RENAME_EXCHANGE)) return 0;
    if (errno != EINVAL && errno != ENOSYS && errno != ENOTSUP) return -1;
    char t[4096]; if ((size_t)snprintf(t, sizeof t, "%s.swap", b) >= sizeof t) { errno = ENAMETOOLONG; return -1; }
    if (rename(b, t)) return -1;
    if (rename(a, b)) { int e = errno; rename(t, b); errno = e; return -1; }
    return rename(t, a);
}
int plat_fd_path(int fd, char *out, size_t n) {
    char l[64]; snprintf(l, sizeof l, "/proc/self/fd/%d", fd);
    ssize_t k = readlink(l, out, n ? n - 1 : 0);
    if (k <= 0) return -1;
    out[k] = 0; return 0;
}
const char *plat_data_dir(void) {   // XDG base directories: $XDG_DATA_HOME, else ~/.local/share
    static char d[1024];
    if (!d[0]) { const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
        if (x && *x == '/') snprintf(d, sizeof d, "%s/dm-work", x); else snprintf(d, sizeof d, "%s/.local/share/dm-work", h ? h : "."); }
    return d;
}

// ---------- Random bytes ----------
void plat_random_buf(void *p, size_t n) {
    for (uint8_t *b = p; n; ) { ssize_t k = getrandom(b, n, 0); if (k < 0) { if (errno == EINTR) continue; abort(); } b += k; n -= (size_t)k; }
}
uint32_t plat_random32(void) { uint32_t v; plat_random_buf(&v, sizeof v); return v; }

// ---------- Symbols ----------
const char *plat_addr_symbol(const void *a, uintptr_t *start) {
    Dl_info di;
    if (!dladdr(a, &di) || !di.dli_sname) return NULL;
    if (start) *start = (uintptr_t)di.dli_saddr;
    return di.dli_sname;
}
