// Host platform layer: what the host's portable code needs from the operating system, one implementation per platform.
//   plat_apple.c  macOS / iOS: the code the host always ran there, moved here unchanged (Mach, libdispatch, CommonCrypto, clonefile, xattr)
//   plat_linux.c  Linux (glibc), x86-64
//   plat_win32.c  Windows 10 1803+ (x86-64, mingw-w64 / llvm-mingw); pthreads come from mingw-w64's winpthreads
//   sha256.c      the SHA-256 Linux and Windows use (Apple keeps CommonCrypto)
// Only what portable host code calls is here. Apple frontends (front_mac.m, the iOS app) stay Apple-only, and so does the code tied to running the guest's ARM64 code
// natively (DM_NATIVE_EXEC below). Errors follow POSIX: 0 or a pointer on success, -1 / NULL with errno set on failure, unless a function says otherwise
#pragma once
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// ---------- Execution engine ----------
// 1: the guest's ARM64 code runs natively (Apple arm64): entry.S gates, hle_bigstack.c's stack switch, the rewriter's thread-slot scheme read through tpidrro_el0,
// Mach thread-state reads of running guest threads (pausing for GetThreadContext3, hitch and crash dumps), probe.c, crash.c and DM_HLECOUNT's generated stubs.
// 0 elsewhere: those parts compile out; the x86-64 execution engine (the ahead-of-time ARM64 -> x86-64 recompiler, Phase 2) will provide the other implementation
#if defined(__APPLE__) && defined(__aarch64__)
#define DM_NATIVE_EXEC 1
#else
#define DM_NATIVE_EXEC 0
#endif

// ---------- Time ----------
uint64_t plat_now_ns(void);          // monotonic nanoseconds (Apple: mach_absolute_time)
uint64_t plat_boot_ns(void);         // monotonic nanoseconds that also count the time the machine slept (Apple: CLOCK_MONOTONIC; Linux CLOCK_BOOTTIME; Windows GetTickCount64, ms)
void plat_sleep_ns(uint64_t ns);     // sleep at the clock's best resolution (Windows: a high-resolution waitable timer; nanosleep elsewhere)

// ---------- Threads ----------
// Scheduling class of a thread, in Apple's QoS terms (the values are Apple's qos_class_t, so the numeric order is the priority order on every platform).
// Linux maps them to nice values (raising one back needs CAP_SYS_NICE: an unprivileged process can only lower its threads), Windows to thread priorities
#if defined(__APPLE__)
#include <pthread/qos.h>
typedef qos_class_t plat_qos_t;
#define PLAT_QOS_UNSPECIFIED QOS_CLASS_UNSPECIFIED
#define PLAT_QOS_UTILITY QOS_CLASS_UTILITY
#define PLAT_QOS_USER_INITIATED QOS_CLASS_USER_INITIATED
#define PLAT_QOS_USER_INTERACTIVE QOS_CLASS_USER_INTERACTIVE
#else
typedef enum { PLAT_QOS_UNSPECIFIED = 0x00, PLAT_QOS_UTILITY = 0x11, PLAT_QOS_USER_INITIATED = 0x19, PLAT_QOS_USER_INTERACTIVE = 0x21 } plat_qos_t;
#endif
int plat_qos_set_self(plat_qos_t q);                     // 0 on success (Apple: pthread_set_qos_class_self_np)
plat_qos_t plat_qos_self(void);                          // the calling thread's class
void *plat_qos_override_start(pthread_t t, plat_qos_t q); // lend t at least class q until the end call (Apple's QoS override; NULL elsewhere: no override)
void plat_qos_override_end(void *ov);
void plat_thread_set_name(const char *name);             // the calling thread's name for debuggers and profilers (Linux keeps the first 15 bytes)
// A host thread as the diagnostics see it, for its CPU time: the Mach thread port (Apple), the kernel thread ID (Linux), the thread ID (Windows); 0 = none
#if defined(__APPLE__)
typedef unsigned int plat_thread_t;   // mach_port_t (plat_apple.c checks the size), without pulling the Mach headers into every host file
#else
typedef uint64_t plat_thread_t;
#endif
plat_thread_t plat_thread_self(void);
int plat_thread_cpu_us(plat_thread_t t, uint64_t *us);   // user + system CPU time in microseconds; 0 on success, nonzero when the thread is gone
int plat_thread_stack(uint64_t *top, uint64_t *size);    // the calling thread's stack: its highest address and size; 0 on success
int plat_cpu_count(void);                                // processors online

// ---------- Semaphores (Apple: dispatch_semaphore) ----------
typedef struct plat_sema plat_sema;
plat_sema *plat_sema_create(long value);
void plat_sema_signal(plat_sema *s);
int plat_sema_wait(plat_sema *s, int64_t timeout_ns);    // timeout < 0: wait forever. 0 when signalled, nonzero when the timeout passed
void plat_sema_destroy(plat_sema *s);

// ---------- Deferred work and termination (Apple: libdispatch) ----------
void plat_after_ms(unsigned ms, void (*fn)(void));       // call fn once on another thread after ms milliseconds
void plat_on_terminate(void (*fn)(const char *why));     // call fn on an ordinary thread when the process is asked to end (SIGTERM; Windows: console close / Ctrl+C / log-off / shutdown)

// ---------- Virtual memory ----------
// Protections: the POSIX PROT_* values. Windows commits reserved pages when a range is made accessible and keeps their contents when it is made inaccessible, like mprotect
enum { PLAT_PROT_NONE = 0, PLAT_PROT_READ = 1, PLAT_PROT_WRITE = 2, PLAT_PROT_EXEC = 4 };
size_t plat_page_size(void);                             // the granule of plat_vm_protect (Apple arm64 16 KiB, Linux and Windows x86-64 4 KiB)
size_t plat_vm_granularity(void);                        // the granule of plat_vm_reserve / plat_vm_alloc addresses (Windows 64 KiB, the page size elsewhere)
void *plat_vm_reserve(size_t n);                         // address space only, inaccessible, no memory charged; NULL on failure
void *plat_vm_alloc(size_t n);                           // readable and writable zero pages; NULL on failure
int plat_vm_protect(void *a, size_t n, int prot);        // a and n in whole pages
int plat_vm_discard(void *a, size_t n);                  // the pages' contents may be dropped (MADV_FREE; Windows decommits them, and a later protect makes them zero pages)
int plat_vm_release(void *a, size_t n);                  // a whole range from plat_vm_reserve / plat_vm_alloc
// The mapped region at or after addr and its protection (PLAT_PROT_*); 0 on success, nonzero when there is none
int plat_vm_query(uint64_t addr, uint64_t *base, uint64_t *size, int *prot);
// Copy n bytes from addr without faulting: 0 when all were read; nonzero when part of the range is unreadable (*got = the bytes read)
int plat_mem_read(uint64_t addr, void *out, size_t n, size_t *got);

// ---------- A separate heap (Apple: a malloc zone), for an engine allocator the host takes over (hle_uemalloc.c) ----------
typedef struct plat_zone plat_zone;
plat_zone *plat_zone_create(const char *name);
void *plat_zone_malloc(plat_zone *z, size_t n);
void *plat_zone_memalign(plat_zone *z, size_t align, size_t n);
void *plat_zone_realloc(plat_zone *z, void *p, size_t n);   // keeps 16-byte alignment only
void plat_zone_free(plat_zone *z, void *p);
size_t plat_zone_size(plat_zone *z, const void *p);          // usable size of a block of z
size_t plat_zone_good_size(size_t n);                        // the size a request of n is rounded up to

// ---------- Files ----------
int plat_clone_tree(const char *src, const char *dst);   // copy a directory tree to dst (which must not exist) with its guest-name attributes; Apple: clonefile (APFS copy-on-write)
int plat_rename_swap(const char *a, const char *b);      // exchange two directory entries atomically (Apple RENAME_SWAP, Linux RENAME_EXCHANGE; Windows: three renames, not atomic)
// Named attributes of a file or directory, not following a final symbolic link (hle_fs.c's guest names): extended attributes (Apple; Linux in the user. namespace), alternate data streams on Windows.
// Get returns the length, or -1 with errno = PLAT_ENOATTR when there is none
ssize_t plat_xattr_get(const char *path, const char *name, void *buf, size_t n);
int plat_xattr_set(const char *path, const char *name, const void *v, size_t n);
int plat_xattr_remove(const char *path, const char *name);
#if defined(__APPLE__)
#define PLAT_ENOATTR ENOATTR
#else
#define PLAT_ENOATTR ENODATA
#endif
int plat_fd_path(int fd, char *out, size_t n);           // the path an open descriptor refers to; 0 on success
const char *plat_data_dir(void);                         // the host's default work folder when DM_ROOT is unset: Apple $HOME/dm-work, Linux $XDG_DATA_HOME/dm-work (~/.local/share), Windows %APPDATA%/dm-work

// ---------- Random bytes and SHA-256 ----------
uint32_t plat_random32(void);                            // cryptographically random (Apple: arc4random)
void plat_random_buf(void *p, size_t n);
typedef struct { _Alignas(8) unsigned char opaque[128]; } plat_sha256_ctx;
void plat_sha256_init(plat_sha256_ctx *c);
void plat_sha256_update(plat_sha256_ctx *c, const void *p, size_t n);
void plat_sha256_final(plat_sha256_ctx *c, uint8_t out[32]);
void plat_sha256(const void *p, size_t n, uint8_t out[32]);

// ---------- Symbols ----------
const char *plat_addr_symbol(const void *a, uintptr_t *start);   // the exported symbol containing host address a and its start (dladdr); NULL when unknown (always on Windows)

// ---------- Registration tables ----------
// Static entries that object files add to one named table (hle.h HLE / HLE_FORCE; hle_table.c), read back as one array: a Mach-O section __DATA,<name> (getsectiondata),
// an ELF section <name> between the linker's __start_<name> / __stop_<name>, a PE/COFF grouped section .<name>$m between markers in .<name>$a and .<name>$z (name: at most 7 characters).
// PLAT_TABLE_DEFINE(name) defines const void *plat_table_<name>(size_t *bytes), the entries and their total size (NULL / 0 when there are none), in exactly one file of a program
#if defined(__APPLE__)
struct mach_header_64;
extern uint8_t *getsectiondata(const struct mach_header_64 *mhp, const char *segname, const char *sectname, unsigned long *size);
extern const struct mach_header_64 _mh_execute_header;
#define PLAT_TABLE_ENTRY(name) __attribute__((used, section("__DATA," #name)))
#define PLAT_TABLE_DEFINE(name) \
    const void *plat_table_##name(size_t *bytes) { unsigned long sz = 0; const void *p = getsectiondata(&_mh_execute_header, "__DATA", #name, &sz); *bytes = p ? (size_t)sz : 0; return p; }
#elif defined(_WIN32)
#define PLAT_TABLE_ENTRY(name) __attribute__((used, section("." #name "$m")))
#define PLAT_TABLE_DEFINE(name) \
    static const uint64_t plat_table_a_##name __attribute__((used, section("." #name "$a"))) = 0, plat_table_z_##name __attribute__((used, section("." #name "$z"))) = 0; \
    const void *plat_table_##name(size_t *bytes) { \
        const char *a = (const char *)(&plat_table_a_##name + 1), *z = (const char *)&plat_table_z_##name; \
        __asm__("" : "+r"(a)); __asm__("" : "+r"(z));   /* two different objects: keep the compiler from reasoning about their distance */ \
        *bytes = (size_t)(z - a); return *bytes ? a : NULL; }
#else
#define PLAT_TABLE_ENTRY(name) __attribute__((used, section(#name)))
#define PLAT_TABLE_DEFINE(name) \
    extern const char __start_##name[] __attribute__((weak, visibility("hidden"))), __stop_##name[] __attribute__((weak, visibility("hidden"))); \
    const void *plat_table_##name(size_t *bytes) { \
        const char *a = __start_##name, *z = __stop_##name; __asm__("" : "+r"(a)); __asm__("" : "+r"(z)); \
        *bytes = a && z ? (size_t)(z - a) : 0; return *bytes ? a : NULL; }
#endif

// ---------- POSIX functions mingw-w64 lacks (Windows) ----------
#if defined(_WIN32)
#include <io.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
_Static_assert(sizeof(off_t) == 8, "build with _FILE_OFFSET_BITS=64: game files pass 2 GB");
#define mkdir(path, mode) plat_win32_mkdir(path, mode)
#define lstat(path, st) stat(path, st)   // no symbolic links in the host's own folders
int plat_win32_mkdir(const char *path, int mode);
ssize_t pread(int fd, void *buf, size_t n, off_t off);
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off);
size_t strlcpy(char *dst, const char *src, size_t n);
size_t strlcat(char *dst, const char *src, size_t n);
void *memmem(const void *h, size_t hn, const void *n, size_t nn);
time_t timegm(struct tm *tm);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
char *mkdtemp(char *tmpl);
#ifndef ENODATA
#define ENODATA 96
#endif
#endif
