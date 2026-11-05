// Horizon kernel system calls (triggered by the SDK runtime once the SDK is loaded). Only what the P1 boot chain needs is implemented: memory layout + heap + query + debug output + timing.
// Register convention: inputs w1.. / outputs w0=Result, x1=value (GetInfo / SetHeapSize), consistent with the public Horizon ABI.
#include "dm.h"
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <stdatomic.h>
#include <mach/mach.h>
#include <TargetConditionals.h>

// Reserve one whole block of guest address space, partitioned the Horizon way (address values are the real addresses returned by the host's mmap).
// It only takes address space, not memory.
// Size 5 GB: the game needs the first 1 GB (alias region, stacks) + one whole heap block (application memory pool of 3285 MiB, see APP_POOL below),
// about 4.2 GB in total; on iOS without the "extended virtual addressing" entitlement a single reservation can be at most 6 GB
// (read on a real device: 6 GB succeeds, 7 GB fails), so it cannot be made any wider --
// with an 8 GB reservation, builds from developer accounts that cannot sign that entitlement (free accounts) would exit at this step.
#define RESERVE DM_GAME_MEM_RESERVE   // SMBW: the heap needs only 1.47 GB, so the block stays at 5 GB (the total proven on a real device; a single 6 GB block can be reserved, but Metal etc. still need address space)
static uint8_t *g_as;              // ASLR region base = the whole block
static size_t g_reserve;           // its size: RESERVE, or more with a DM_ALIAS_SIZE alias region
static uint8_t *g_heap; static _Atomic size_t g_heap_cur; static size_t g_heap_cap;
static size_t g_heap_seen;        // the largest heap opened so far: with the guest in files, SetHeapSize zeroes a heap that grows back below it
// The address range lies entirely within the currently opened (readable and writable) part of the guest heap: such reads need no kernel trap to check, memcpy directly (used where guest memory is read every tick, such as diag.c tk_rd)
int dm_guest_heap_readable(uint64_t a, size_t n) { uint64_t b = (uint64_t)(uintptr_t)g_heap; size_t cur = g_heap_cur; return g_heap && a >= b && n <= cur && a - b <= cur - n; }
// The opened heap span for callers that address guest memory by region (hle_cheats.c region 1, like dmnt's heap extents): the
// reservation's base (guest address) and the length the game has actually opened with SetHeapSize right now, which grows and
// shrinks with it; both 0 until the game's first SetHeapSize. There is no lock: the caller uses the pair only to bounds-check and
// restarts the check on each access, so a concurrent resize can at worst give it an answer about the span a moment ago
void dm_guest_heap_range(uint64_t *base, size_t *open_len) {
    if (base) *base = (uint64_t)(uintptr_t)g_heap;
    if (open_len) *open_len = g_heap ? atomic_load(&g_heap_cur) : 0;
}
static uint8_t *g_alias; static size_t g_alias_cap;
static _Atomic uint64_t *g_alias_bits;   // alias region page bitmap (alias_pages)
static uint64_t *g_alias_exposed;   // one bit per host page ever opened; protected by g_map_lock
static uint8_t *g_stack; static size_t g_stack_cap;
// The address lies in the guest address block (every guest thread stack is here: the game's own heap or the stack region); the big-stack entry (hle_bigstack.c) uses it to decide whether to switch stacks
int dm_guest_as_contains(uintptr_t a) { uintptr_t b = (uintptr_t)g_as; return g_as && a >= b && a - b < g_reserve; }

// A size switch: bytes (decimal or 0x hex), or whole K / M / G (DM_GUEST_FILE_CHUNK=1G); 0 when unset. At least min, in whole units of align
static size_t env_size(const char *name, size_t min, size_t align) {
    const char *s = getenv(name);
    if (!s || !*s) return 0;
    char *end; unsigned long long v = strtoull(s, &end, 0); int sh = 0;
    switch (*end) { case 'G': case 'g': sh = 30; end++; break; case 'M': case 'm': sh = 20; end++; break; case 'K': case 'k': sh = 10; end++; break; default: break; }
    if (*end || v > (~0ull >> sh) || (v << sh) < min || ((v << sh) & (align - 1)))
        die("%s=%s: at least %zu MB, in whole %zu KB units (bytes, or whole K / M / G)", name, s, min >> 20, align >> 10);
    return (size_t)(v << sh);
}
// DM_GUEST_FILE=1: the guest's memory in a file as large as the block, mapped shared over the reservation at the same addresses. File pages do not count in phys_footprint, the figure iOS ends an
// app at (about 6 GB on a 12 GB iPhone), which otherwise charges every page the game has touched; they take the same memory while they stay in it, and the kernel can write them to the file when
// it needs the room, which it cannot do with an iPhone app's anonymous pages. A page never written reads 0 as before, and mprotect (dirty tracking), vm_remap (virtual pools) and Metal's no-copy
// buffers work on them as on anonymous pages. The file is sparse and unlinked at once, so it goes with the process, and lies in the user's temporary folder (on iOS NSTemporaryDirectory:
// DM_GUEST_FILE_DIR, from main.m); the host never flushes it (no msync, no fsync). It takes disk space as pages are written out, at most what
// the game has touched, and guest_file_thread stops the game when its disk has less free space than DM_GUEST_FILE_MIN_FREE (default 5G; 0 = no stop). The guest stays in anonymous memory,
// with a [RED] line saying why, when the file cannot be made, unlinked (it would outlive the process), sized or mapped; on iOS when its protection class is not C or D and cannot be set to C (in a
// class that closes when the phone locks, its pages could not be read or written out while it is locked); and on the Mac when its disk has less free space than that minimum and 8 GB, where
// the system can swap anonymous memory (on iOS anonymous memory means a jetsam kill, so the file stays and the stop guards the disk). [RED] lines also say what could go wrong with the file
// (not on a local APFS volume, not sparse, under 10 GB free). What the game gives back (UnmapPhysicalMemory, a smaller heap) only goes PROT_NONE in the file (guest_release), and its pages stay there,
// out of the footprint, until the kernel writes them out; they read 0 when the game opens them again (alias_initialize, SetHeapSize). Files an earlier run left in the folder (an unlink that failed, or a run that ended between making a file and unlinking it) are removed first.
// DM_GUEST_FILE_CHUNK=<size> (whole MB, K / M / G; off by default) puts the block in several files of that size instead, the last one shorter when the block is not a multiple: each is made,
// unlinked and checked as the one file is, then sized and mapped over its own part of the block from its offset 0 (guest_files_map). The stall at the system's periodic flush, about every 30 s,
// grows with the file: on a 16 GB M5 Mac, without the game and with 1 GB of it touched, threads touching the memory stalled up to 126-148 ms with one file of 20 GB, 11-16 ms with one of 2 or
// 5 GB and 4 ms with 20 files of 1 GB, threads that did not touch it not at all, which points to a lock per file or VM object held over the flush (a deduction); the bytes written stay the same.
// The size is rounded up to the host page, at least 64 MB, raised to make at most 1024 files, and one file at or above the block's size. When the files cannot all be made, unlinked, given
// class C (iOS), sized or mapped, or the Mac's disk has less free space than the minimum and 8 GB, none is mapped and every one is closed (a file whose unlink failed stays in the folder,
// empty, until the next start removes it), a [RED] line says why, and the one file is tried instead (the guest stays anonymous when that fails too). A memory pool or a virtual pool's run
// that crosses from one file into the next puts two VM objects under one no-copy Metal buffer or one remapped run, which the switch2ios fork's tools/nocopy_cross.m checked on a 16 GB M5 Mac
// (Metal takes such a buffer, and a run remapped over two files reads and writes as one; a refused buffer stops the host, mtl_pool_add). Code that reads the files takes an address's file and offset from svc_guest_file_at (dm.h); svc_guest_file_fd, which knows only one file, gives -1 with several
int proc_pid_rusage(int pid, int flavor, rusage_info_t *buffer);   // libproc.h, which the iOS SDK does not have
static uint64_t g_gf_min_free;     // DM_GUEST_FILE_MIN_FREE in bytes: the free space on the file's disk under which the game is stopped; 0 = never
// The guest files, open for reading and writing (their shared mappings are writable): file i maps the block from g_as + i * g_gf_chunk to the next file's start, the last one to the block's end, from
// its offset 0. Filled once at init, then published by the release store of g_gf_n and never changed, so that any thread reads them without a lock (svc_guest_files and the others, dm.h)
#define GF_MAX 1024
static int g_gf_tab[GF_MAX];
static size_t g_gf_chunk;          // the size of every file but the last, which holds the rest of the block (g_reserve with one file)
static _Atomic int g_gf_n;         // how many files; 0 while the guest is in anonymous memory

// The guest files' blocks on disk in bytes, summed; -1 when one of them cannot be read
static double guest_file_held(const int *fd, int n) {
    double b = 0; struct stat st;
    for (int i = 0; i < n; i++) { if (fstat(fd[i], &st)) return -1; b += st.st_blocks * 512.0; }
    return b;
}
// Every second the free space on the file's disk (the first file's, with several: they all lie on one volume): under DM_GUEST_FILE_MIN_FREE the game is stopped (exit code 87; the memory
// watchdog's is 86, hle_nvn.c), before a full disk leaves the kernel no room to write the guest's pages out, nor a phone's system any room. Every 10th second one line, [RED] under 2 GB free:
// the blocks on disk (of every file, and how many files with more than one), what the process wrote, read and dirtied (logical writes) and its page-ins (pages read back from the files among
// them), the footprint's parts and its peak so far beside the file pages it has mapped (external: PROT_NONE takes pages out of it, as out of internal), and the room left on the disk
static void *guest_file_thread(void *arg) {
    (void)arg; const double G = 1073741824.0; int fd = svc_guest_file_nth(0, NULL, NULL), nf = svc_guest_files();   // the first file's volume (svc_guest_file_fd is -1 with several)
    for (unsigned sec = 1;; sec++) {
        sleep(1);
        struct statfs fs; struct rusage_info_v4 ru; task_vm_info_data_t vi; mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
        int fs_ok = !fstatfs(fd, &fs); double avail = fs_ok ? (double)fs.f_bavail * fs.f_bsize / G : -1;
        if (fs_ok && g_gf_min_free && (uint64_t)fs.f_bavail * fs.f_bsize < g_gf_min_free) {
            LOG("[RED] [mem] guest file: %.1f GB free on its disk, under %.1f: stopping the game (the free space statfs gives, f_bavail, which leaves out space the system could purge)", avail, g_gf_min_free / G);
            _exit(87);
        }
        if (sec % 10) continue;
        double held = guest_file_held(g_gf_tab, nf), blocks = held < 0 ? -1 : held / G;
        int ru_ok = !proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&ru);
        if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n) != KERN_SUCCESS) memset(&vi, 0, sizeof vi);
        char files[24] = "";
        if (nf > 1) snprintf(files, sizeof files, " in %d files", nf);
        LOG("%s[mem] guest file: %.2f GB allocated on disk%s; process wrote %.2f GB to disk and read %.2f GB, %llu page-ins, logical writes %.2f GB; footprint %.2f GB (internal %.2f, compressed %.2f, peak so far %.2f), file pages mapped (external) %.2f GB; %.1f GB free on its disk",
            avail >= 0 && avail < 2 ? "[RED] " : "", blocks, files, ru_ok ? ru.ri_diskio_byteswritten / G : -1, ru_ok ? ru.ri_diskio_bytesread / G : -1, ru_ok ? (unsigned long long)ru.ri_pageins : 0ull,
            ru_ok ? ru.ri_logical_writes / G : -1, vi.phys_footprint / G, vi.internal / G, vi.compressed / G, ru_ok ? ru.ri_lifetime_max_phys_footprint / G : -1, vi.external / G, avail);
    }
    return NULL;
}
// A new file of this process's own, never a link's target (O_EXCL, O_NOFOLLOW): one left by an earlier process with the same number is removed first
static int guest_file_open(const char *path) {
    const int fl = O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC;
    for (int i = 0;; i++) {
#if TARGET_OS_IPHONE
        // Protection class C (PROTECTION_CLASS_C, which the SDK's headers do not name), whose key stays from the first unlock until the phone restarts: in a class that closes when the phone locks,
        // the file's pages could no longer be read (SIGBUS). It is also the class an app's files get by default, hence the plain open if this call fails
        int fd = open_dprotected_np(path, fl, 3, 0, 0600);
        if (fd < 0 && errno != EEXIST) fd = open(path, fl, 0600);
#else
        int fd = open(path, fl, 0600);
#endif
        if (fd >= 0 || errno != EEXIST || i) return fd;
        unlink(path);
    }
}
// The file just made unlinked: 0, also when its name is gone already (ENOENT: another process's guest_file_stale took it in between, the number having been a dead process's a moment
// before), since the descriptor holds the file all the same; -1 with errno otherwise
static int guest_file_unlink(const char *path) { return unlink(path) && errno != ENOENT ? -1 : 0; }
#if TARGET_OS_IPHONE
// Protection class C or D, or the guest stays anonymous: in a class that closes when the phone locks, the file's pages could not be read or written out while it is locked (SIGBUS).
// A file that did not get C when it was made (guest_file_open's plain open takes the folder's class) is set to C while it is still empty. The class the file has then, with the one it had
// in *was and in *err why setting C failed
static int guest_file_class(int fd, int *was, int *err) {
    int cls = *was = fcntl(fd, F_GETPROTECTIONCLASS);
    *err = 0;
    if (cls != 3 && cls != 4) {
#ifdef F_SETPROTECTIONCLASS
        *err = fcntl(fd, F_SETPROTECTIONCLASS, 3) ? errno : 0;
        cls = fcntl(fd, F_GETPROTECTIONCLASS);
#else
        *err = ENOTSUP;   // an SDK without F_SETPROTECTIONCLASS
#endif
    }
    return cls;
}
#else
// The free space the Mac wants on the file's disk at start: DM_GUEST_FILE_MIN_FREE and 8 GB (guest_file_one)
static uint64_t guest_file_want(void) { return g_gf_min_free < UINT64_MAX - ((uint64_t)8 << 30) ? g_gf_min_free + ((uint64_t)8 << 30) : UINT64_MAX; }
#endif
// The pid in a name this code gives a file, guest-<pid>.mem or guest-<pid>-<i>.mem with the numbers as %d writes them (decimal digits, no sign, no leading 0); 0 for any other name
static int guest_file_name_pid(const char *s) {
    if (strncmp(s, "guest-", 6)) return 0;
    long long pid = 0; s += 6;
    for (int k = 0; k < 2; k++, s++) {   // the pid, then the file's number
        const char *d = s;
        for (; *s >= '0' && *s <= '9' && s - d < 10; s++) if (!k) pid = pid * 10 + (*s - '0');
        if (s == d || (*d == '0' && s - d > 1)) return 0;   // no digit, or a leading 0
        if (*s != '-' || k) break;
    }
    return !strcmp(s, ".mem") && pid > 0 && pid <= INT_MAX ? (int)pid : 0;
}
// Files an earlier run left in the folder: one whose unlink failed (that run stayed anonymous, the empty file there), or one made by a run that ended before it unlinked it. Removed: regular files
// of this user's (fstatat does not follow a link, and leaves it) under such a name, whose process has ended (kill(pid, 0) fails with ESRCH; EPERM is a process that runs; files under this
// process's own number are left, guest_file_open replacing those it makes). One line when any were removed; nothing is done when the folder cannot be read
static void guest_file_stale(const char *dir) {
    DIR *dp = opendir(dir); if (!dp) return;
    int dfd = dirfd(dp); if (dfd < 0) { closedir(dp); return; }   // the folder's descriptor, for fstatat and unlinkat
    int n = 0, self = (int)getpid(); uid_t me = getuid(); double held = 0; struct dirent *de;
    while ((de = readdir(dp))) {
        int pid = guest_file_name_pid(de->d_name); struct stat st;
        if (!pid || pid == self || !kill(pid, 0) || errno != ESRCH) continue;
        if (fstatat(dfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) || st.st_uid != me) continue;
        if (!unlinkat(dfd, de->d_name, 0)) { n++; held += st.st_blocks * 512.0; }
    }
    closedir(dp);
    if (n) LOG("[mem] guest file: %d file%s that earlier runs left in its folder removed (%.2f GB on disk)", n, n == 1 ? "" : "s", held / 1073741824.0);
}
// DM_GUEST_FILE_CHUNK in bytes: rounded up to the host page, at least 64 MB, raised to make at most GF_MAX files; 0 for one file (unset, 0, or not under the block's size)
static size_t guest_file_chunk(void) {
    const double G = 1073741824.0; const char *s = getenv("DM_GUEST_FILE_CHUNK"); size_t pg = (size_t)getpagesize(), c = env_size("DM_GUEST_FILE_CHUNK", 0, 0x100000), n;
    if (!c) return 0;
    c = (c + pg - 1) & ~(pg - 1);
    if (c < ((size_t)64 << 20)) { LOG("[RED] [mem] guest file: DM_GUEST_FILE_CHUNK=%s, under 64 MB: files of 64 MB", s); c = (size_t)64 << 20; }
    if (c >= g_reserve) { LOG("[mem] guest file: DM_GUEST_FILE_CHUNK=%s, not under the block's %.2f GB: one file", s, g_reserve / G); return 0; }
    if ((n = (g_reserve + c - 1) / c) > GF_MAX) {
        size_t r = (((g_reserve + GF_MAX - 1) / GF_MAX + 0xfffff) & ~(size_t)0xfffff) + pg - 1;
        r &= ~(pg - 1);
        LOG("[mem] guest file: DM_GUEST_FILE_CHUNK=%s would make %zu files, more than %d: files of %.2f GB", s, n, GF_MAX, r / G);
        c = r;
    }
    return c;
}
// Room for n more descriptors: a soft RLIMIT_NOFILE under the descriptors open now, the n files and 64 to spare is raised toward the hard limit, by the n files at least, so that the rest of
// the host keeps the room it had. When it cannot be raised enough a line says so, and the files are tried anyway: an open that fails with EMFILE then leaves them all for the one file
// (guest_files_map), and when they all open the rest of the host has fewer than 64 left. A limit raised here stays raised, whether the files are kept or not
static void guest_files_nofile(int n) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl)) { LOG("[RED] [mem] guest file: no descriptor limit to check for %d files (getrlimit errno %d)", n, errno); return; }
    if (rl.rlim_cur == RLIM_INFINITY) return;
    rlim_t used = 0, want;
    for (rlim_t f = 0; f < rl.rlim_cur && f < 65536; f++) used += fcntl((int)f, F_GETFD) != -1;   // the descriptors open now
    if (rl.rlim_cur >= (want = used + (rlim_t)n + 64)) return;
    rlim_t to = rl.rlim_cur + (rlim_t)n > want ? rl.rlim_cur + (rlim_t)n : want;   // the files beside the room the host had, and 64 to spare at least
    struct rlimit nl = { .rlim_cur = rl.rlim_max != RLIM_INFINITY && rl.rlim_max < to ? rl.rlim_max : to, .rlim_max = rl.rlim_max };
    int err = 0;
    if (nl.rlim_cur > rl.rlim_cur && setrlimit(RLIMIT_NOFILE, &nl)) {
        err = errno;   // refused (macOS: EINVAL above kern.maxfilesperproc): tried again with the files and 64 to spare only, without the room kept beside them
        if (nl.rlim_cur > want) { nl.rlim_cur = want; err = setrlimit(RLIMIT_NOFILE, &nl) ? errno : 0; }
    }
    if (!err && nl.rlim_cur >= want) {
        LOG("[mem] guest file: descriptor limit raised from %llu to %llu for %d files (%llu open)", (unsigned long long)rl.rlim_cur, (unsigned long long)nl.rlim_cur, n, (unsigned long long)used);
        return;
    }
    char why[40] = ""; if (err) snprintf(why, sizeof why, "; setrlimit errno %d", err);
    LOG("[RED] [mem] guest file: descriptor limit %llu (hard limit %llu%s), under the %llu that %d files want with %llu open and 64 to spare: they may not all open, and if they do the rest of the host has fewer than 64 left",
        (unsigned long long)(!err && nl.rlim_cur > rl.rlim_cur ? nl.rlim_cur : rl.rlim_cur), (unsigned long long)rl.rlim_max, why, (unsigned long long)want, n, (unsigned long long)used);
}
static void guest_files_name(char *path, size_t size, const char *dir, const char *sep, int k) { snprintf(path, size, "%s%sguest-%d-%d.mem", dir, sep, (int)getpid(), k); }
// DM_GUEST_FILE_CHUNK: the block in n files of chunk bytes, the last one holding the rest, guest-<pid>-<i>.mem in the folder. Each is made, unlinked at once and (on iOS) given its protection
// class as the one file is; the free space is checked once, on the first (all lie on one volume); then each is sized, then each mapped over its part of the block from its offset 0. 1 with
// the files in fd[], the first one's protection class in *cls (-1 on a Mac) and its volume in *fs (*fs_ok when fstatfs read it); 0 with why filled in, nothing open and nothing mapped:
// every file closed (one whose unlink failed stays in the folder, empty, for the next start's guest_file_stale) and the whole block anonymous PROT_NONE again, as reserved. A descriptor limit
// guest_files_nofile raised stays raised
static int guest_files_map(const char *dir, const char *sep, size_t chunk, int n, int *fd, int *cls, struct statfs *fs, int *fs_ok, char *why, size_t nwhy) {
    const double G = 1073741824.0; char path[1100]; int made = 0, ok = n > 0 && n <= GF_MAX, err;
    *cls = -1; *fs_ok = 0;
    if (!ok) snprintf(why, nwhy, "%d files", n);   // guest_file_chunk keeps n from 2 to GF_MAX
#if TARGET_OS_IPHONE
    int fixed = 0, was0 = -1, set0 = -1;
#endif
    if (ok) guest_files_nofile(n);
    for (int k = 0; ok && k < n; k++) {
        guest_files_name(path, sizeof path, dir, sep, k);
        if ((fd[k] = guest_file_open(path)) < 0) { snprintf(why, nwhy, "cannot create %s, errno %d", path, errno); ok = 0; break; }
        made++;
        if (guest_file_unlink(path)) { snprintf(why, nwhy, "cannot unlink %s, errno %d; it stays there empty", path, errno); ok = 0; break; }
#if TARGET_OS_IPHONE
        int was, c = guest_file_class(fd[k], &was, &err);
        if (!k) *cls = c;
        if (c != 3 && c != 4) { snprintf(why, nwhy, "file %d: protection class %d, not C (3) or D (4), and %d after setting C (errno %d)", k, was, c, err); ok = 0; break; }
        if (c != was && !fixed++) { was0 = was; set0 = c; }
#endif
    }
    if (ok) {
        *fs_ok = !fstatfs(fd[0], fs);
#if !TARGET_OS_IPHONE
        // The Mac's rule, as for the one file (guest_file_one): room for the files to grow before the stop
        uint64_t want = guest_file_want();
        if (*fs_ok && (uint64_t)fs->f_bavail * fs->f_bsize < want) {
            snprintf(why, nwhy, "%.1f GB free on its disk, under %.1f, 8 GB for the files to grow above DM_GUEST_FILE_MIN_FREE's %.1f", (double)fs->f_bavail * fs->f_bsize / G, want / G, g_gf_min_free / G);
            ok = 0;
        }
#endif
    }
    for (int k = 0; ok && k < n; k++) {
        size_t sz = k == n - 1 ? g_reserve - (size_t)k * chunk : chunk;
        if (ftruncate(fd[k], (off_t)sz)) { err = errno; guest_files_name(path, sizeof path, dir, sep, k); snprintf(why, nwhy, "cannot size %s to %.2f GB, errno %d", path, sz / G, err); ok = 0; }
    }
    for (int k = 0; ok && k < n; k++) {
        size_t sz = k == n - 1 ? g_reserve - (size_t)k * chunk : chunk; uint8_t *at = g_as + (size_t)k * chunk;
        if (mmap(at, sz, PROT_NONE, MAP_SHARED | MAP_FIXED, fd[k], 0) != at) { err = errno; guest_files_name(path, sizeof path, dir, sep, k); snprintf(why, nwhy, "cannot map %s, errno %d", path, err); ok = 0; }
    }
#if TARGET_OS_IPHONE
    if (ok && fixed) LOG("[mem] guest file: protection class %d, set to %d, in %d of the %d files", was0, set0, fixed, n);   // once the files are kept
#endif
    if (ok) return 1;
    for (int k = 0; k < made; k++) close(fd[k]);
    if (mmap(g_as, g_reserve, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != g_as) die("reserve guest AS again after DM_GUEST_FILE (errno %d)", errno);   // whatever the maps left there
    return 0;
}
// The block in files of chunk bytes (guest_files_map), with the line and the checks the one file has, the sparse one per file: how many, or 0 when guest_files_map could not keep them, after
// one [RED] line saying why (the one file is tried then)
static int guest_files_init(const char *dir, const char *sep, size_t chunk, int *fd) {
    const double G = 1073741824.0; int n = (int)((g_reserve + chunk - 1) / chunk), cls = -1, fs_ok = 0, nfull = 0; struct statfs fs; char why[1400];
    if (!guest_files_map(dir, sep, chunk, n, fd, &cls, &fs, &fs_ok, why, sizeof why)) {
        LOG("[RED] [mem] guest file: %d files of %.2f GB not possible (%s): one file instead", n, chunk / G, why);
        return 0;
    }
    size_t last = g_reserve - (size_t)(n - 1) * chunk;
    double held = guest_file_held(fd, n), full = 0, avail = fs_ok ? (double)fs.f_bavail * fs.f_bsize / G : -1;
    for (int i = 0; i < n; i++) {   // not sparse: more than 1/16 GB on disk as soon as it was sized, or a 16th of a file under 1 GB
        struct stat st; double sz = i == n - 1 ? (double)last : (double)chunk, h = fstat(fd[i], &st) ? 0 : st.st_blocks * 512.0;
        if (h > (sz < G ? sz : G) / 16) { nfull++; full += h; }
    }
    LOG("[mem] guest memory: %d files %s%sguest-%d-0..%d.mem of %.2f GB (the last %.2f GB) (unlinked, sparse, shared), %.2f GB, %.2f GB of it on disk, on a%s %s volume, protection class %d (the first file's; -1 on a Mac), %.1f GB free on its disk",
        n, dir, sep, (int)getpid(), n - 1, chunk / G, last / G, g_reserve / G, held < 0 ? -1 : held / G, fs_ok && (fs.f_flags & MNT_LOCAL) ? " local" : "", fs_ok ? fs.f_fstypename : "unknown", cls, avail);
    if (!fs_ok || strcmp(fs.f_fstypename, "apfs") || !(fs.f_flags & MNT_LOCAL)) LOG("[RED] [mem] guest file: not on a local APFS volume, where it may not be sparse, or be slow");
    if (nfull) LOG("[RED] [mem] guest file: %d of the %d files held %.2f GB on disk as soon as they were sized: not sparse", nfull, n, full / G);
    if (avail >= 0 && avail < 10) LOG("[RED] [mem] guest file: %.1f GB free on its disk, under 10: the files take up to what the game has touched (Mario Kart World: 6 GB in 10 minutes on a Mac), and a full disk was not tried", avail);
    return n;
}
// The one file, guest-<pid>.mem, over the whole block: its descriptor, or -1 with the guest left in anonymous memory and a [RED] line saying why
static int guest_file_one(const char *dir, const char *sep) {
    const double G = 1073741824.0; char path[1100];
    snprintf(path, sizeof path, "%s%sguest-%d.mem", dir, sep, (int)getpid());
    int fd = guest_file_open(path), cls = -1;
    if (fd < 0) { LOG("[RED] [mem] guest memory: anonymous (DM_GUEST_FILE: cannot create %s, errno %d)", path, errno); return -1; }
    if (guest_file_unlink(path)) {
        LOG("[RED] [mem] guest memory: anonymous (DM_GUEST_FILE: cannot unlink %s, errno %d: it would stay after the game exits, with what was written out to it; it stays there empty)", path, errno);
        close(fd); return -1;
    }
#if TARGET_OS_IPHONE
    int was, err; cls = guest_file_class(fd, &was, &err);
    if (cls != 3 && cls != 4) {
        LOG("[RED] [mem] guest memory: anonymous (DM_GUEST_FILE: protection class %d, not C (3) or D (4), and %d after setting C (errno %d): in a class that closes when the phone locks, its pages could not be read or written out while it is locked)",
            was, cls, err);
        close(fd); return -1;
    }
    if (cls != was) LOG("[mem] guest file: protection class %d, set to %d", was, cls);
#endif
    struct stat st; struct statfs fs; int fs_ok = !fstatfs(fd, &fs);
    double avail = fs_ok ? (double)fs.f_bavail * fs.f_bsize / G : -1;
#if !TARGET_OS_IPHONE
    // On the Mac the guest stays anonymous without room for the file to grow before the stop (guest_file_thread): the minimum and 8 GB. The system can swap anonymous memory there, while on
    // iOS anonymous memory means a jetsam kill, so the file stays whatever the disk has, and the stop guards it
    uint64_t want = guest_file_want();
    if (fs_ok && (uint64_t)fs.f_bavail * fs.f_bsize < want) {
        LOG("[RED] [mem] guest memory: anonymous (DM_GUEST_FILE: %.1f GB free on its disk, under %.1f, 8 GB for the file to grow above DM_GUEST_FILE_MIN_FREE's %.1f)", avail, want / G, g_gf_min_free / G);
        close(fd); return -1;
    }
#endif
    if (ftruncate(fd, (off_t)g_reserve)) { LOG("[RED] [mem] guest memory: anonymous (DM_GUEST_FILE: cannot size %s to %.2f GB, errno %d)", path, g_reserve / G, errno); close(fd); return -1; }
    if (mmap(g_as, g_reserve, PROT_NONE, MAP_SHARED | MAP_FIXED, fd, 0) != g_as) {
        LOG("[RED] [mem] guest memory: anonymous (DM_GUEST_FILE: cannot map %s, errno %d)", path, errno); close(fd);
        if (mmap(g_as, g_reserve, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != g_as) die("reserve guest AS again after DM_GUEST_FILE (errno %d)", errno);   // whatever the failed call left there
        return -1;
    }
    double held = fstat(fd, &st) ? -1 : st.st_blocks * 512.0 / G;
    LOG("[mem] guest memory: file %s (unlinked, sparse, shared), %.2f GB, %.2f GB of it on disk, on a%s %s volume, protection class %d (-1 on a Mac), %.1f GB free on its disk", path, g_reserve / G, held,
        fs_ok && (fs.f_flags & MNT_LOCAL) ? " local" : "", fs_ok ? fs.f_fstypename : "unknown", cls, avail);
    if (!fs_ok || strcmp(fs.f_fstypename, "apfs") || !(fs.f_flags & MNT_LOCAL)) LOG("[RED] [mem] guest file: not on a local APFS volume, where it may not be sparse, or be slow");
    if (held > 1.0 / 16) LOG("[RED] [mem] guest file: %.2f GB on disk as soon as it was sized: not sparse", held);
    if (avail >= 0 && avail < 10) LOG("[RED] [mem] guest file: %.1f GB free on its disk, under 10: the file takes up to what the game has touched (Mario Kart World: 6 GB in 10 minutes on a Mac), and a full disk was not tried", avail);
    return fd;
}
// The files are made (several with DM_GUEST_FILE_CHUNK, the one file when they cannot be), then published whole, then the thread started: nothing is published, nor the thread started,
// while the guest stays anonymous
static void guest_file_init(void) {
    const char *e = getenv("DM_GUEST_FILE"); if (!e || atoi(e) <= 0) return;
    char dir[1024] = ""; const char *d = getenv("DM_GUEST_FILE_DIR"), *mf = getenv("DM_GUEST_FILE_MIN_FREE");
    g_gf_min_free = mf && *mf ? env_size("DM_GUEST_FILE_MIN_FREE", 0, 0x100000) : (uint64_t)5 << 30;
    size_t chunk = guest_file_chunk();
    if (d && *d) snprintf(dir, sizeof dir, "%s", d);
    else if (!confstr(_CS_DARWIN_USER_TEMP_DIR, dir, sizeof dir)) { LOG("[RED] [mem] guest memory: anonymous (DM_GUEST_FILE: no temporary folder, errno %d)", errno); return; }
    const char *sep = dir[0] && dir[strlen(dir) - 1] == '/' ? "" : "/";
    guest_file_stale(dir);
    int fd[GF_MAX], n = chunk ? guest_files_init(dir, sep, chunk, fd) : 0;
    if (!n) { if ((fd[0] = guest_file_one(dir, sep)) < 0) return; n = 1; chunk = g_reserve; }
    memcpy(g_gf_tab, fd, (size_t)n * sizeof *fd); g_gf_chunk = chunk;
    atomic_store_explicit(&g_gf_n, n, memory_order_release);   // published: read from any thread without a lock from here on
    pthread_t th; int rc = pthread_create(&th, NULL, guest_file_thread, NULL);
    if (!rc) pthread_detach(th);
    else LOG("[RED] [mem] guest file: no thread to watch it (error %d): no 10-s line, and nothing stops the game when its disk fills", rc);
}

static void *alias_windows_thread(void *arg);
static void *regions_thread(void *arg);
static void mem_init_once(void) {
    size_t pg = (size_t)getpagesize();
    if (RESERVE < 0x10000000 || DM_GAME_MEM_ALIAS > RESERVE - 0x10000000 ||
        DM_GAME_MEM_STACK > RESERVE - 0x10000000 - DM_GAME_MEM_ALIAS ||
        ((RESERVE | DM_GAME_MEM_ALIAS | DM_GAME_MEM_STACK) & (pg - 1)))
        die("guest address-space profile has invalid native boundaries");
    // DM_ALIAS_SIZE=<bytes>: an alias region of that size instead of the profile's, the heap and stacks as they are; the block grows with it. A Switch-sized one (64 GB) holds what
    // a game reserves unchanged, for measuring its address regions on the Mac (the NSP script's heap check); an iPhone has no room for more than the profile's
    size_t alias = DM_GAME_MEM_ALIAS; const char *as = getenv("DM_ALIAS_SIZE");
    if (as && *as) { alias = (size_t)strtoull(as, NULL, 0); if (alias < 0x10000000 || (alias & (pg - 1))) die("DM_ALIAS_SIZE %s: at least 256 MB, in whole host pages", as); }
    g_reserve = RESERVE - DM_GAME_MEM_ALIAS + alias;
    g_as = mmap(NULL, g_reserve, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_as == MAP_FAILED) die("reserve guest AS");
    guest_file_init();
    // Fixed partition (all inside the reserved block): alias 2.875 GB, stack 256 MB, heap the remaining 1.625 GB.
    // In newer SDKs the AddressRegionManager always takes 0x3fe00000 (~1 GB) at the start of the alias region as its node area (sdk 0x134728) before handing out regions; with a 512 MB alias region the very first AllocateAddressRegion fails (0x1803).
    // SMBW also defines a 1792000 KB (1.75 GB) VirtualAddressMemory in HeapDef (System/HeapSize): it is subtracted from the memory pool before the heap is opened (SetHeapSize asks for only 0x5be00000),
    //   and resource loading then MapPhysicalMemory's into the alias region after the node area on demand. With a 1.25 GB alias region only about 256 MB was left there, and 3 MB-class layout archives (WipeCourseIn / WipeWarp)
    //   got no address, were skipped, and crashed on a null pointer when used (main +0x373e04). So alias = node area + 1.75 GB + slack; the heap needs only 1.47 GB, and 2.625 GB is plenty.
    // The alias and stack sizes come from the game profile (mem_alias / mem_stack), the heap takes the rest; Mario: alias 0xB8000000, stack 0x10000000 → stack at +0xC8000000, heap at +0xD8000000
    g_alias = g_as + 0x10000000; g_alias_cap = alias;
    g_stack = g_alias + g_alias_cap; g_stack_cap = DM_GAME_MEM_STACK;
    g_heap  = g_stack + g_stack_cap; g_heap_cap  = g_reserve - (size_t)(g_heap - g_as);
    if (!(g_alias_bits = calloc((g_alias_cap >> 18) + 1, sizeof *g_alias_bits))) die("alias region page bitmap calloc");
    if (!(g_alias_exposed = calloc(g_alias_cap / pg / 64 + 1, sizeof *g_alias_exposed))) die("alias exposure bitmap calloc");
    LOG("guest AS %p +%#llx  heap %p alias %p stack %p", g_as, (unsigned long long)g_reserve, g_heap, g_alias, g_stack);
    const char *aw = getenv("DM_ALIAS_WINDOWS");
    if (aw && atoi(aw) > 0) { pthread_t th; if (!pthread_create(&th, NULL, alias_windows_thread, (void *)(uintptr_t)atoi(aw))) pthread_detach(th); }
    const char *rf = getenv("DM_REGIONS");
    if (rf && *rf) { pthread_t th; if (!pthread_create(&th, NULL, regions_thread, (void *)rf)) pthread_detach(th); }
}
static void mem_init(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, mem_init_once);
}

// Application memory pool = the 3285 MB the Switch (4 GB model) gives applications. At startup (main+0x1b66160) the game sets the whole total reported by QueryMemoryInfo as the heap, retrying with 2 MB less on failure,
// and hands the whole block to its own StandardAllocator (main+0x32137a0) -- so heap size = pool − image footprint, it has only as much as the host reports.
// Previously 3 GB was reported here and SetHeapSize never failed: the heap was about 150 MB smaller than on the real device, at the run entry peak the main heap had only about 18 MB left, and under high local load a timing shift made a 128 KB request return null,
// which the guest writes to without a null check (CRASH main+0x1b36138, re-verified after merge with inv_tap).
#define APP_POOL DM_GAME_APP_POOL   // game profile app_pool (Mario 0xCD500000)
static size_t image_bytes(void) { size_t n = 0; for (int i = 0; i < g_nmod; i++) if (!g_mod[i].nro) n += g_mod[i].size;   // an NRO lives in memory the game allocated (its image and bss buffers)
    return (n + 0x1fffff) & ~(size_t)0x1fffff; }
// Giant bss (modules with the patch area before the base, patch_nso.py FAR): the image bytes beyond the file part. Used memory was reported as "heap + 64 MB", where the 64 MB covers ordinary modules' code and data (Mario's two modules are about 74 MB together,
// and the heap it opens = pool − 64 MB − 1.75 GB virtual address memory − 1 MB = 0x5be00000, as on hardware); Metroid Dread's main has about 1.8 GB of bss, the engine's static heap, which the Switch counts as used;
// without it the game asks for pool − 64 MB = 3.15 GB of heap
// Like image_bytes, exclude NROs: their buffers already belong to the game's heap. Counting every offline-mapped NRO's bss made FFX/X-2's used memory exceed its pool before any LoadModule.
static size_t big_bss_bytes(void) { size_t n = 0; for (int i = 0; i < g_nmod; i++) if (!g_mod[i].nro && g_mod[i].patch_off < 0) n += g_mod[i].size - g_mod[i].back_len - g_mod[i].file_image_len; return n; }

// For memory accounting (memstat.m): the range of the whole guest address space block (base = NULL when not initialized)
void svc_guest_range(uint8_t **base, size_t *size) { *base = g_as; *size = g_reserve; }
void svc_alias_range(uint8_t **base, size_t *size) { *base = g_alias; *size = g_alias_cap; }   // the alias region (GetInfo's), NULL before mem_init
// The guest files (DM_GUEST_FILE, dm.h), read from any thread without a lock: their table is filled once at init, published by the release store of its count and never changed afterwards
int svc_guest_files(void) { return atomic_load_explicit(&g_gf_n, memory_order_acquire); }
int svc_guest_file_nth(int i, uintptr_t *lo, uintptr_t *hi) {
    int n = svc_guest_files();
    if (i < 0 || i >= n) return -1;
    uintptr_t b = (uintptr_t)g_as + (uintptr_t)i * g_gf_chunk;
    if (lo) *lo = b;
    if (hi) *hi = i == n - 1 ? (uintptr_t)g_as + g_reserve : b + g_gf_chunk;
    return g_gf_tab[i];
}
// O(1): every file but the last is g_gf_chunk long, and the last one no longer
int svc_guest_file_at(uintptr_t a, off_t *off, uintptr_t *hi) {
    int n = svc_guest_files();
    if (!n || a < (uintptr_t)g_as || a - (uintptr_t)g_as >= g_reserve) return -1;
    size_t r = a - (uintptr_t)g_as, i = r / g_gf_chunk;   // under n: the block ends within the last file
    if (off) *off = (off_t)(r - i * g_gf_chunk);
    if (hi) *hi = (uintptr_t)g_as + (i == (size_t)n - 1 ? g_reserve : (i + 1) * g_gf_chunk);
    return g_gf_tab[i];
}
// The guest file's descriptor, open for reading and writing, while the whole block lies in one file, which maps it from offset 0 (an address's offset there is its distance from the block's
// base). -1 while the guest is in anonymous memory, before the block is reserved, and with several files (DM_GUEST_FILE_CHUNK), where that offset would be wrong past the first file: code
// written for one file then does without the file, as in anonymous memory, rather than ask the wrong one. Code that knows about the files uses svc_guest_file_at and svc_guest_file_nth (the
// volume they all lie on: fstatfs of svc_guest_file_nth(0, NULL, NULL))
int svc_guest_file_fd(void) { return svc_guest_files() == 1 ? g_gf_tab[0] : -1; }

enum { I_CoreMask=0, I_PriorityMask=1, I_AliasAddr=2, I_AliasSize=3, I_HeapAddr=4, I_HeapSize=5,
       I_TotalMem=6, I_UsedMem=7, I_DebuggerAttached=8, I_ResourceLimit=9, I_IdleTick=10, I_RandomEntropy=11,
       I_AslrAddr=12, I_AslrSize=13, I_StackAddr=14, I_StackSize=15, I_SysResTotal=16, I_SysResUsed=17,
       I_ProgramId=18, I_InitProcIdRange=19, I_UserExcAddr=20, I_TotalNonSys=21, I_UsedNonSys=22, I_IsApplication=23, I_FreeThreadCount=24,
       I_ThreadTickCount=25, I_IsSvcPermitted=26, I_IoRegionHint=27, I_AliasExtraSize=28,
       I_RemoteAddr=29, I_RemoteSize=30, I_RemoteUsage0=31, I_RemoteUsage1=32, I_PageSize=33 };

static uint8_t g_seen[256];

// Stack destinations and their borrowed sources. Physical alias commits live in
// g_alias_bits. One lock covers memory validation, protections, copying, budgets
// and publication; QueryMemory and used-memory queries take the same snapshot.
// g_map_lock is held over a whole lookup + check + change (and over QueryMemory's reads): map_find's index is only valid under it, since map_del moves the last entry into the freed slot.
// The lock used to cover only the add and the delete, so two threads unmapping at once (stacks of exiting threads) could delete through a stale index and drop another live mapping
#define MAX_MAP 4096
static struct { uint64_t base, size, src; uint32_t state; } g_map[MAX_MAP];
static int g_nmap;
static pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;
// A hole-free read / write of guest heap bytes for the cheat VM (hle_cheats.c): under g_map_lock no concurrent SetHeapSize
// can mprotect the span away between the bounds check and the copy (both hold this same lock), so a live heap address cannot
// turn into a fault while the cheat VM touches it. Both return 0 when every byte was inside the currently opened (readable
// and writable) span, 1 otherwise. The address is the guest heap address the cheat VM computed (heap base + rel + offset register)
int dm_guest_heap_write(uint64_t addr, const void *src, size_t len) {
    pthread_mutex_lock(&g_map_lock);
    int out = !dm_guest_heap_readable(addr, len);
    if (!out) { dm_guest_write_prepare((void *)(uintptr_t)addr, len); memcpy((void *)(uintptr_t)addr, src, len); }
    pthread_mutex_unlock(&g_map_lock);
    return out;
}
int dm_guest_heap_read(uint64_t addr, void *out, size_t len) {
    pthread_mutex_lock(&g_map_lock);
    int bad = !dm_guest_heap_readable(addr, len);
    if (!bad) memcpy(out, (void *)(uintptr_t)addr, len);
    pthread_mutex_unlock(&g_map_lock);
    return bad;
}
static int map_find(uint64_t a, uint64_t n) {       // returns a 1-based index, 0 = no overlap; caller holds g_map_lock
    for (int i = 0; i < g_nmap; i++) if (a < g_map[i].base + g_map[i].size && g_map[i].base < a + n) return i + 1;
    return 0;
}
static void map_add(uint64_t b, uint64_t n, uint64_t src, uint32_t st) {   // caller holds g_map_lock
    if (g_nmap >= MAX_MAP) die("mapping table full");
    g_map[g_nmap++] = (typeof(g_map[0])){b, n, src, st};
}
static void map_del(int i) { g_map[i] = g_map[--g_nmap]; }   // caller holds g_map_lock

// One bit per 4 KB page of the alias region (mapped = 1): Map sets bits, Unmap clears them; returns the number of pages whose state really changed (signed: positive for Map, negative for Unmap)
static int64_t alias_pages(uint64_t off, uint64_t n, int map) {
    int64_t ch = 0;
    for (uint64_t p = off >> 12, e = (off + n + 0xFFF) >> 12; p < e; ) {
        uint64_t sh = p & 63, k = 64 - sh < e - p ? 64 - sh : e - p, m = (k == 64 ? ~0ull : (1ull << k) - 1) << sh;
        uint64_t old = map ? atomic_fetch_or(&g_alias_bits[p >> 6], m) : atomic_fetch_and(&g_alias_bits[p >> 6], ~m);
        ch += __builtin_popcountll((map ? ~old : old) & m);
        p += k;
    }
    return map ? ch : -ch;
}
// Pages of the range not mapped yet (the bitmap is left alone)
static int64_t alias_unmapped(uint64_t off, uint64_t n) {
    int64_t c = 0;
    for (uint64_t p = off >> 12, e = (off + n + 0xFFF) >> 12; p < e; ) {
        uint64_t sh = p & 63, k = 64 - sh < e - p ? 64 - sh : e - p, m = (k == 64 ? ~0ull : (1ull << k) - 1) << sh;
        c += __builtin_popcountll(~atomic_load(&g_alias_bits[p >> 6]) & m);
        p += k;
    }
    return c;
}
static _Atomic int64_t g_alias_mapped;   // bytes mapped in the alias region (page bitmap)
// What the game gave back so far: bytes UnmapPhysicalMemory unmapped (only pages that were mapped), and bytes alias_initialize zeroed for a new map over host pages opened before. For the
// "[mem] alias region mapped" line: what the game gave back (with the guest file, pages that stay in the files), and what zeroing on map writes again (with the guest file, pages the
// kernel then writes out). With the guest file, the 4 KB pages of such a map that read 0 already (never written, or zeroed before) are left as they are and counted apart (g_alias_skipped)
static _Atomic uint64_t g_alias_given, g_alias_zeroed, g_alias_skipped;

// Word-at-a-time boundaries of the bitmap run containing p (guest page index).
static uint64_t alias_mask(unsigned n) { return n == 64 ? UINT64_MAX : (UINT64_C(1) << n) - 1; }
static uint64_t alias_run_end(uint64_t p, uint64_t end, int mapped) {
    for (uint64_t first = p + 1; first < end;) {
        unsigned shift = first & 63, n = (unsigned)(end - first < 64 - shift ? end - first : 64 - shift);
        uint64_t bits = atomic_load(&g_alias_bits[first >> 6]);
        uint64_t different = (mapped ? ~bits : bits) & (alias_mask(n) << shift);
        if (different) return (first & ~UINT64_C(63)) + __builtin_ctzll(different);
        first += n;
    }
    return end;
}
static void alias_query(uint64_t addr, uint64_t *base, uint64_t *size, int *mapped) {
    uint64_t p = (addr - (uintptr_t)g_alias) >> 12, lo = p;
    *mapped = (atomic_load(&g_alias_bits[p >> 6]) >> (p & 63)) & 1;
    while (lo) {
        uint64_t first = (lo - 1) & ~UINT64_C(63);
        uint64_t bits = atomic_load(&g_alias_bits[first >> 6]);
        uint64_t different = (*mapped ? ~bits : bits) & alias_mask((unsigned)(lo - first));
        if (different) { lo = first + 64 - __builtin_clzll(different); break; }
        lo = first;
    }
    uint64_t hi = alias_run_end(p, g_alias_cap >> 12, *mapped);
    *base = (uintptr_t)g_alias + (lo << 12); *size = (hi - lo) << 12;
}
// Give guest memory back to the system: a fresh inaccessible mapping over [lo, hi), whole host pages. mprotect(PROT_NONE) alone keeps every page resident and dirty, only
// out of phys_footprint, and madvise(MADV_FREE) does nothing to pages that are not writable, though it returns 0 (1 GiB on the Mac: 1,024 MiB still resident and dirty; this: 0).
// A memory pool's no-copy buffer over the range keeps the old pages for the GPU work still using it until the buffer goes. Returns 1 when the range now holds zero pages;
// on failure the range is only inaccessible, with its old bytes.
// With the guest in files (DM_GUEST_FILE), an anonymous mapping over the range would turn it into anonymous memory, which phys_footprint charges again as soon as the game maps
// it back, so the files' range only goes PROT_NONE: its pages stay file pages, out of the footprint, which the kernel writes out when it needs the room;
// they keep their old bytes, so the call returns 0 and alias_initialize zeroes them when they are mapped again
static int guest_release(uintptr_t lo, uintptr_t hi) {
    if (svc_guest_files() && lo >= (uintptr_t)g_as && hi <= (uintptr_t)g_as + g_reserve) {
        if (mprotect((void *)lo, hi - lo, PROT_NONE)) die("guest_release mprotect %#lx+%#lx errno=%d", (unsigned long)lo, (unsigned long)(hi - lo), errno);
        return 0;
    }
    if (mmap((void *)lo, hi - lo, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != MAP_FAILED) return 1;
    static _Atomic int nred; if (atomic_fetch_add(&nred, 1) < 8) LOG("[RED] guest memory %#lx+%#lx not given back (errno %d): it stays resident", (unsigned long)lo, (unsigned long)(hi - lo), errno);
    if (mprotect((void *)lo, hi - lo, PROT_NONE)) die("guest_release mprotect %#lx+%#lx errno=%d", (unsigned long)lo, (unsigned long)(hi - lo), errno);
    return 0;
}
// Zero n bytes at a, whole 4 KB pages of the guest block in its files: a page that reads 0 already is left as it is (its bytes add to *skipped, the others' to *zeroed). Reading a hole
// maps a clean zero page and dirties nothing, where the memset would dirty it and the kernel then write zeros back
static void guest_zero(uint8_t *a, uint64_t n, uint64_t *zeroed, uint64_t *skipped) {
    for (uint8_t *e = a + n; a < e; a += 4096) {
        uint64_t w[8], any = 0;
        for (size_t i = 0; !any && i < 4096; i += sizeof w) { memcpy(w, a + i, sizeof w); any = w[0] | w[1] | w[2] | w[3] | w[4] | w[5] | w[6] | w[7]; }
        if (any) { memset(a, 0, 4096); *zeroed += 4096; } else *skipped += 4096;
    }
}
// Zero the 4 KB pages [p, q) of the alias region for a new map: with the guest in files through guest_zero, in anonymous memory the run whole, as before
static void alias_zero(uint64_t p, uint64_t q, uint64_t *zeroed, uint64_t *skipped) {
    if (!svc_guest_files()) { memset(g_alias + (p << 12), 0, (q - p) << 12); *zeroed += (q - p) << 12; return; }
    guest_zero(g_alias + (p << 12), (q - p) << 12, zeroed, skipped);
}
// Initialize holes using the OLD commit bitmap. Fresh anonymous host pages stay
// lazy; after any exposure, even an untouched 4 KiB sibling may hold stale bytes.
// The bytes zeroed add to g_alias_zeroed, those alias_zero leaves as they are (reading 0 already, with the guest file) to g_alias_skipped.
static void alias_initialize(uint64_t off, size_t size) {
    size_t pg = (size_t)getpagesize(), ratio = pg >> 12;
    uint64_t end = (off + size) >> 12, zeroed = 0, skipped = 0;
    for (uint64_t p = off >> 12; p < end;) {
        int mapped = (atomic_load(&g_alias_bits[p >> 6]) >> (p & 63)) & 1;
        uint64_t next = alias_run_end(p, end, mapped);
        if (!mapped) {
            uint64_t clear = p;
            for (uint64_t q = p; q < next;) {
                uint64_t host = q / ratio, stop = (host + 1) * ratio;
                if (stop > next) stop = next;
                if (!(g_alias_exposed[host >> 6] & (UINT64_C(1) << (host & 63)))) {
                    if (clear < q) alias_zero(clear, q, &zeroed, &skipped);
                    clear = stop;
                }
                q = stop;
            }
            if (clear < next) alias_zero(clear, next, &zeroed, &skipped);
        }
        p = next;
    }
    for (uint64_t h = off / pg, last = (off + size - 1) / pg; h <= last; ++h)
        g_alias_exposed[h >> 6] |= UINT64_C(1) << (h & 63);
    if (zeroed) atomic_fetch_add_explicit(&g_alias_zeroed, zeroed, memory_order_relaxed);
    if (skipped) atomic_fetch_add_explicit(&g_alias_skipped, skipped, memory_order_relaxed);
}

static int memory_contains(uint64_t base, uint64_t capacity, uint64_t addr, uint64_t size) {
    return addr >= base && size <= capacity && addr - base <= capacity - size;
}
static uint32_t map_arguments(uint64_t dst, uint64_t src, uint64_t size) {
    if ((dst | src) & 0xfff) return 0xCC01;
    if (!size || (size & 0xfff)) return 0xCA01;
    if (size > UINT64_MAX - src || size > UINT64_MAX - dst) return 0xD401;
    return memory_contains((uintptr_t)g_stack, g_stack_cap, dst, size) ? 0 : 0xDC01;
}
static int map_source_busy(uint64_t addr, uint64_t size) { // g_map_lock held; nonwrapping ranges
    for (int i = 0; i < g_nmap; ++i)
        if (addr < g_map[i].src + g_map[i].size && g_map[i].src < addr + size) return 1;
    return 0;
}
// Loader text/patch seals use 16 KiB boundaries, independently of host page size.
static void module_readonly_run(uint64_t off, uint64_t start, uint64_t end,
                                uint64_t *lo, uint64_t *hi, uint64_t *perm) {
    if (start >= end) return;
    if (off < start) { if (start < *hi) *hi = start; }
    else if (off >= end) { if (end > *lo) *lo = end; }
    else { if (start > *lo) *lo = start; if (end < *hi) *hi = end; *perm &= ~UINT64_C(2); }
}
static void module_data_query(const module_t *m, uint64_t off, uint64_t *lo, uint64_t *hi, uint64_t *perm) {
    module_readonly_run(off, 0, (m->text_end + 0x3fff) & ~UINT64_C(0x3fff), lo, hi, perm);
    if (m->patch_off > 0) module_readonly_run(off, (uint64_t)m->patch_off & ~UINT64_C(0x3fff), m->size, lo, hi, perm);
    if (m->back_len) module_readonly_run(off, m->back_off, m->back_off + m->back_len, lo, hi, perm);
    module_readonly_run(off, m->key_page_off, m->key_page_off + m->key_page_size, lo, hi, perm);
}
static int map_source_backed(uint64_t src, uint64_t size) {
    if (memory_contains((uintptr_t)g_heap, g_heap_cur, src, size)) return 1;
    int dm_ue_heap_owns(uint64_t a, uint64_t n);   // Unreal's heap on a host malloc zone outside the guest block (hle_uemalloc.c): Unreal allocates its threads' stacks there
    if (dm_ue_heap_owns(src, size)) return 1;
    if (memory_contains((uintptr_t)g_alias, g_alias_cap, src, size))
        return !alias_unmapped(src - (uintptr_t)g_alias, size);
    module_t *m = module_of((void *)(uintptr_t)src);
    // Active NRO data is valid backing too (FFX/X-2's launcher allocates thread stacks from its bss). Deactivation below checks borrowers under this same lock before reload can restore it.
    if (!m || !m->loaded || !memory_contains((uintptr_t)m->base, m->size, src, size)) return 0;
    uint64_t lo = m->data_off ? m->data_off : (m->ro_off ? m->ro_off : m->text_end);
    uint64_t hi = m->patch_off > 0 ? (uint64_t)m->patch_off : m->back_len ? m->back_off : m->size;
    uint64_t off = src - (uintptr_t)m->base, perm = 3;
    module_data_query(m, off, &lo, &hi, &perm);
    return perm == 3 && hi >= lo && memory_contains(lo, hi - lo, off, size);
}
int svc_nro_deactivate(module_t *m) {
    pthread_mutex_lock(&g_map_lock);
    uint64_t base = (uintptr_t)m->base;
    for (int i = 0; i < g_nmap; i++)
        if (g_map[i].src < base + m->size && base < g_map[i].src + g_map[i].size) {
            pthread_mutex_unlock(&g_map_lock); return 0;
        }
    m->loaded = 0;
    pthread_mutex_unlock(&g_map_lock); return 1;
}
static void map_source_query(uint64_t addr, uint64_t *base, uint64_t *size, uint32_t *attr, uint64_t *perm) {
    uint64_t lo = *base, remaining = *size; // the final free page can end at 2^64
    for (int i = 0; i < g_nmap; ++i) {
        uint64_t src = g_map[i].src, end = src + g_map[i].size;
        uint64_t lower = lo, upper = remaining;
        if (addr >= src && addr < end) {
            if (src > lower) lower = src;
            if (end - lo < upper) upper = end - lo;
            *attr = 1; *perm = 0; // Locked / IsBorrowed, not PermissionLocked (bit 4)
        } else if (end <= addr && end > lower) lower = end;
        else if (src > addr && src - lo < upper) upper = src - lo;
        remaining = upper - (lower - lo); lo = lower;
    }
    *base = lo; *size = remaining;
}

// Diagnostic DM_ALIAS_WINDOWS=<window MB>: every 30 s, the mapped memory of each window of that size in the alias region (windows aligned to the size in absolute addresses, counted from the page
// bitmap; windows with nothing mapped are left out). For sizing an allocator that reserves fixed, aligned windows and commits pages from each window's start: Unreal's FMallocBinned3 takes
// 74 pools of 512 MB (Octopath Traveler 0, main+0x9f748)
static void *alias_windows_thread(void *arg) {
    uint64_t win = (uint64_t)(uintptr_t)arg << 20, a0 = (uint64_t)(uintptr_t)g_alias, a1 = a0 + g_alias_cap;
    for (;;) {
        sleep(30);
        char line[8192] = {0}; size_t o = 0; uint64_t tot = 0;
        pthread_mutex_lock(&g_map_lock);
        for (uint64_t w = a0 & ~(win - 1); w < a1 && o < sizeof line - 64; w += win) {
            uint64_t n = 0, p0 = (w > a0 ? w - a0 : 0) >> 12, p1 = ((w + win < a1 ? w + win : a1) - a0) >> 12;
            for (uint64_t p = p0; p < p1; p++) n += atomic_load(&g_alias_bits[p >> 6]) >> (p & 63) & 1;
            if (n) { o += snprintf(line + o, sizeof line - o, " +%#llx:%.1f", (unsigned long long)(w - (a0 & ~(win - 1))), n * 4096 / 1048576.0); tot += n; }
        }
        pthread_mutex_unlock(&g_map_lock);
        LOG("[mem] alias windows of %llu MB (window offset:MB mapped), %.1f MB in all:%s", (unsigned long long)(win >> 20), tot * 4096 / 1048576.0, line);
    }
    return NULL;
}

// ---- Address regions ----
// The SDK's VammManager hands out the alias region as address regions. The rewriter's observation gates in the sdk (patch_nso.py "Address regions", placed by its symbols in any
// build) show each one: its size, where the request returns to in the game (the first return address outside the sdk on the frame chain: the function that asked, or its caller
// when it tail-called the SDK API, whose frame is gone by then), the SDK API it came through (such as StandardAllocator) and its address. With the page bitmaps that gives how much of each region a game uses, which decides how its reservations can fit an iPhone's alias region.
// A region of 256 MB or more is logged when made; DM_REGIONS=<file> writes the live table every 2 s (the NSP script's heap check reads it, with a Switch-sized DM_ALIAS_SIZE)
typedef struct { uint64_t off, size, via; char who[40]; } region_t;   // via: the outermost sdk return address on the way (the SDK API the request came through)
static region_t *g_rgn; static int g_nrgn, g_rgn_cap; static pthread_mutex_t g_rgn_lock = PTHREAD_MUTEX_INITIALIZER;   // grows as needed: Unity 6 takes all its memory as regions
static _Thread_local uint64_t t_rgn_size, t_rgn_via; static _Thread_local char t_rgn_who[40];
// Where a request at an sdk function's entry gate returns to in the game: buf gets "main+0x…", *via the outermost sdk return address on the way (0 = none)
static void guest_caller(const uint64_t *r, char *buf, size_t n, uint64_t *via) {
    // at the function's entry the return address is the x30 the gate block pushed (at the block's sp, regs[99]: x29, x30; entry.S), x30 itself holds the gate's own
    uint64_t lr = ((const uint64_t *)(uintptr_t)r[99])[1], fp = r[29]; module_t *m = module_of((void *)(uintptr_t)lr);
    *via = 0;
    for (int i = 0; i < 32 && m && !strcmp(m->name, "sdk"); i++) {
        *via = lr - (uint64_t)(uintptr_t)m->base;   // AArch64 frame records: [fp] = the caller's fp, [fp + 8] = its return address
        uint64_t rec[2]; vm_size_t got = 0;   // read safely: static constructors run on the host's main thread, whose stack is outside the guest block
        if (!fp || (fp & 15) || vm_read_overwrite(mach_task_self(), (vm_address_t)fp, sizeof rec, (vm_address_t)rec, &got) || got != sizeof rec) break;
        lr = rec[1]; fp = rec[0]; m = module_of((void *)(uintptr_t)lr);
    }
    if (m) snprintf(buf, n, "%s+%#llx", m->name, (unsigned long long)(lr - (uint64_t)(uintptr_t)m->base));
    else snprintf(buf, n, "%#llx", (unsigned long long)lr);
}
static void region_ask(uint64_t *r, int param) {
    (void)param;
    t_rgn_size = r[2]; guest_caller(r, t_rgn_who, sizeof t_rgn_who, &t_rgn_via);
}
static void region_got(uint64_t *r, int param) {
    mem_init();
    uint64_t a = param >= 0 && param < 31 ? r[param] : 0, ab = (uint64_t)(uintptr_t)g_alias;
    if (a < ab || a - ab >= g_alias_cap) return;
    pthread_mutex_lock(&g_rgn_lock);
    if (g_nrgn == g_rgn_cap) {
        region_t *t = realloc(g_rgn, sizeof *t * (size_t)(g_rgn_cap ? 2 * g_rgn_cap : 256));
        if (t) { g_rgn = t; g_rgn_cap = g_rgn_cap ? 2 * g_rgn_cap : 256; }
    }
    int kept = g_nrgn < g_rgn_cap;
    if (kept) { g_rgn[g_nrgn].off = a - ab; g_rgn[g_nrgn].size = t_rgn_size; g_rgn[g_nrgn].via = t_rgn_via; memcpy(g_rgn[g_nrgn].who, t_rgn_who, sizeof t_rgn_who); g_nrgn++; }
    pthread_mutex_unlock(&g_rgn_lock);
    if (!kept) { static _Atomic int once; if (!atomic_fetch_add(&once, 1)) LOG("[RED] address region table: out of memory at %d regions, later ones are not in it", g_nrgn); }
    if (t_rgn_size >= 0x10000000) LOG("[mem] address region %.2f GB at alias +%.2f GB, returning to %s", t_rgn_size / 1073741824.0, (a - ab) / 1073741824.0, t_rgn_who);
}
static void region_free(uint64_t *r, int param) {
    (void)param;
    uint64_t off = r[1] - (uint64_t)(uintptr_t)g_alias;
    pthread_mutex_lock(&g_rgn_lock);
    for (int i = 0; i < g_nrgn; i++) if (g_rgn[i].off == off) { g_rgn[i] = g_rgn[--g_nrgn]; break; }
    pthread_mutex_unlock(&g_rgn_lock);
}
// ---- StandardAllocator's address space ----
// With virtual address memory, StandardAllocator::Initialize(nullptr, size) takes a region of size from the alias region and lays its heap out over all of it (CentralHeap), and
// games pass sizes made for the Switch's 64 GB alias region: Pokémon Legends: Z-A 12 GB, Pokémon Scarlet 12 GB, Paper Mario: The Thousand-Year Door 62 GB, Absolum 40 GB. Where the
// size is more than the alias region has room for, the request would fail (an nn::diag abort in nninitStartup), so the gate at the function's entry (patch_nso.py) gives it the room
// instead: the alias region less the regions already made (at least the SDK's 62 MB node area) and 256 MB for regions made later, in 32 MB steps; the profile key stdalloc_cap sets
// it instead (Absolum's .NET garbage collector takes 1.5 GB after it). The SDK lays the region out from that size, so it holds; the pool, not the region, bounds what the heap
// can map. A workaround for the iPhone's address space, not the game: with room for the request (a Switch-sized DM_ALIAS_SIZE) nothing changes. Found by symbol in every sdk
#define STDALLOC_LATER 0x10000000ull
// What the alias region has room for in one more region: past the end of the regions made so far, less STDALLOC_LATER for those made later, in 32 MB steps (0 = none)
static uint64_t alias_room(void) {
    mem_init();
    uint64_t reach = 0x3e00000;   // the node area with the SDK-wide cut (sdk_patches.py); a build without it keeps 1 GB, and a 12 GB request fails there as before
    pthread_mutex_lock(&g_rgn_lock);
    for (int i = 0; i < g_nrgn; i++) if (g_rgn[i].off + g_rgn[i].size > reach) reach = g_rgn[i].off + g_rgn[i].size;
    pthread_mutex_unlock(&g_rgn_lock);
    return g_alias_cap > reach + STDALLOC_LATER ? (g_alias_cap - reach - STDALLOC_LATER) & ~0x1ffffffull : 0;
}
static void stdalloc_init(uint64_t *r, int param) {
    (void)param;
    if (r[1]) return;   // a heap over memory the game passes in: no address region
    uint64_t cap = DM_GAME_STDALLOC_CAP ? DM_GAME_STDALLOC_CAP : alias_room();
    if (!cap || r[2] <= cap) return;
    char who[40]; uint64_t via; guest_caller(r, who, sizeof who, &via);
    LOG("[mem] StandardAllocator::Initialize(nullptr, %.2f GB) returning to %s: %.2f GB of address space, %s", r[2] / 1073741824.0, who, cap / 1073741824.0,
        DM_GAME_STDALLOC_CAP ? "the profile's stdalloc_cap" : "what the alias region has room for (the profile key stdalloc_cap sets it)");
    r[2] = cap;
}
// ---- Unreal's engine region ----
// Unreal Engine 4 / 5's nninitStartup reserves one address region for the engine, which the engine hands out itself: 62 GB by default (16 GB in Unreal Engine 4.18), made for the
// Switch's 64 GB alias region. The rewriter's gate on the store of its size right before the reservation (dm/tools/ue_region.py, in any Unreal main) gives it what the alias region
// has room for, as StandardAllocator's gate above: the size variable (the register the gate number names) and the request (x1) both take it, so the engine lays the region out from
// the size it got. -SwitchVirtualAddressSpaceSize= gives a size too, but UE 4.26 then skips the reservation (Pikmin 4); a smaller size given that way stays. A workaround for the
// iPhone's address space, not the game: with room for the request (a Switch-sized DM_ALIAS_SIZE) nothing changes
static void ue_region(uint64_t *r, int param) {
    if (param < 1 || param > 28 || param == 18) return;
    uint64_t cap = alias_room();
    if (!cap || r[param] <= cap) return;
    LOG("[mem] Unreal's engine region: %.2f GB asked, %.2f GB of address space, what the alias region has room for", r[param] / 1073741824.0, cap / 1073741824.0);
    r[param] = r[1] = cap;
}
__attribute__((constructor)) static void regions_attach(void) {
    gate_obs_set(OBS_REGION_ASK, region_ask); gate_obs_set(OBS_REGION_GOT, region_got); gate_obs_set(OBS_REGION_FREE, region_free);
    gate_obs_set(OBS_STDALLOC_INIT, stdalloc_init); gate_obs_set(OBS_UE_REGION, ue_region);
}
// Pages set in a bitmap's bits [lo, hi), a word at a time: a 62 GB region is 16 million pages, counted under g_map_lock, which every MapPhysicalMemory takes
static uint64_t bits_set(_Atomic uint64_t *b, uint64_t lo, uint64_t hi) {
    uint64_t n = 0;
    for (; lo < hi && (lo & 63); lo++) n += atomic_load(&b[lo >> 6]) >> (lo & 63) & 1;
    for (; lo + 64 <= hi; lo += 64) n += (uint64_t)__builtin_popcountll(atomic_load(&b[lo >> 6]));
    for (; lo < hi; lo++) n += atomic_load(&b[lo >> 6]) >> (lo & 63) & 1;
    return n;
}
// The first and last bit set in [lo, hi) (-1 when none), skipping empty words
static void bits_span(const uint64_t *b, uint64_t lo, uint64_t hi, int64_t *first, int64_t *last) {
    *first = *last = -1;
    for (uint64_t i = lo; i < hi; ) {
        uint64_t w = b[i >> 6] >> (i & 63);
        if (!w) { i = (i | 63) + 1; continue; }
        i += (uint64_t)__builtin_ctzll(w);
        if (i >= hi) break;
        if (*first < 0) *first = (int64_t)i;
        *last = (int64_t)i; i++;
    }
}
// The table: alias offset, size, where the request returns to, bytes mapped now (4 KB pages), the part ever opened (host pages: from its first to its last, region-relative),
// the sdk offset it came through
static void *regions_thread(void *arg) {
    const char *path = arg; char tmp[1024]; snprintf(tmp, sizeof tmp, "%s.partial", path);
    uint64_t hpg = (uint64_t)getpagesize();
    for (;;) {
        FILE *f = fopen(tmp, "w");
        if (f) {
            fprintf(f, "# alias region %#llx; offset\tsize\treturns to\tmapped\tfirst opened\tend of the last opened (region-relative; - = none)\tvia (sdk offset)\n", (unsigned long long)g_alias_cap);
            pthread_mutex_lock(&g_rgn_lock); int n = g_nrgn; region_t *t = malloc(sizeof *t * (size_t)(n ? n : 1)); if (t && n) memcpy(t, g_rgn, sizeof *t * (size_t)n); pthread_mutex_unlock(&g_rgn_lock);
            pthread_mutex_lock(&g_map_lock);
            for (int i = 0; t && i < n; i++) {
                uint64_t lo = t[i].off, hi = t[i].off + t[i].size > g_alias_cap ? g_alias_cap : t[i].off + t[i].size, mapped = bits_set(g_alias_bits, lo >> 12, hi >> 12);
                int64_t first, last; bits_span(g_alias_exposed, lo / hpg, (hi + hpg - 1) / hpg, &first, &last);
                char fo[24] = "-", lo2[24] = "-";
                if (first >= 0) { snprintf(fo, sizeof fo, "%#llx", (unsigned long long)((uint64_t)first * hpg > lo ? (uint64_t)first * hpg - lo : 0));
                                  snprintf(lo2, sizeof lo2, "%#llx", (unsigned long long)((uint64_t)(last + 1) * hpg - lo)); }
                fprintf(f, "%#llx\t%#llx\t%s\t%#llx\t%s\t%s\t%#llx\n", (unsigned long long)t[i].off, (unsigned long long)t[i].size, t[i].who, (unsigned long long)mapped * 4096, fo, lo2,
                        (unsigned long long)t[i].via);
            }
            pthread_mutex_unlock(&g_map_lock); free(t);
            fclose(f); rename(tmp, path);
        }
        sleep(2);
    }
    return NULL;
}

int svc_thread(int num, uint64_t *regs);   // hle_thread.c: thread and synchronization kind

uint64_t svc_dispatch(int num, uint64_t *regs, uint64_t orig_addr) {
    { void hlecount_svc(int); hlecount_svc(num); }   // DM_HLECOUNT: svc counts by number
    mem_init();
    if (svc_thread(num, regs)) return regs[0];
    switch (num) {
    case 0x01: {  // SetHeapSize(out_addr*, size)  → x1 = heap base
        size_t size = regs[1];
        // Larger than the heap region: ResultOutOfMemory (0xD001), as Horizon's KPageTable::SetHeapSize returns. nn::os::SetMemoryHeapSize turns only that, 103 and LimitReached into
        // os::ResultOutOfMemory and aborts on anything else; Donkey Kong Country: Tropical Freeze finds its heap size by a binary search between 3 and 6 GB, which needs the error back
        if (size > g_heap_cap) { LOG("SetHeapSize OOM: req %#zx > cap %#zx", size, g_heap_cap); regs[0] = 0xD001; return 0; }
        pthread_mutex_lock(&g_map_lock);
        size_t previous = g_heap_cur, image = image_bytes();
        if (size < previous && map_source_busy((uintptr_t)g_heap + size, previous - size)) {
            pthread_mutex_unlock(&g_map_lock); regs[0] = 0xD401; return 0;
        }
        if (image > APP_POOL || size > APP_POOL - image) {   // ResultLimitReached; games retry in 2 MB decrements
            // Logged once (Mario's retries would repeat it): a game that sizes its heap as pool − used memory and does not retry stops here when its modules are larger than the used memory reported
            // (profile mem_used; the NSP script's probe boot reads this line, and sets mem_used when no SetHeapSize succeeds afterwards)
            static int once; if (!once++) LOG("[mem] SetHeapSize %#zx + modules %#zx > app pool %#llx (used memory reported %#llx + bss %#zx)", size, image_bytes() - big_bss_bytes(),
                                              (unsigned long long)APP_POOL, (unsigned long long)DM_GAME_MEM_USED, big_bss_bytes());
            pthread_mutex_unlock(&g_map_lock); regs[0] = 0x10801; return 0;
        }
        uint64_t charged = (uint64_t)DM_GAME_MEM_USED + big_bss_bytes() + atomic_load(&g_alias_mapped);
        if (DM_GAME_MEM_MAP_LIMIT && (charged > APP_POOL || size > APP_POOL - charged)) {
            pthread_mutex_unlock(&g_map_lock); regs[0] = 0x10801; return 0;
        }
        int rc = 0;
        if (size > g_heap_cur) rc = mprotect(g_heap + g_heap_cur, size - g_heap_cur, PROT_READ | PROT_WRITE);
        else if (size < g_heap_cur) (void)guest_release((uintptr_t)g_heap + size, (uintptr_t)g_heap + g_heap_cur);
        if (rc) die("SetHeapSize mprotect failed: req=%#zx errno=%d", size, errno);
        // With the guest in files a heap given back kept its old bytes (guest_release): one that grows back over them reads 0 there, as the fresh mapping reads without the files
        if (size > g_heap_cur && g_heap_seen > g_heap_cur && svc_guest_files()) {
            uint64_t zeroed = 0, skipped = 0;
            guest_zero(g_heap + g_heap_cur, (size < g_heap_seen ? size : g_heap_seen) - g_heap_cur, &zeroed, &skipped);
        }
        if (size > g_heap_seen) g_heap_seen = size;
        if (g_flag.svc) LOG("SetHeapSize %#zx -> base %p mprotect=%d errno=%d", size, g_heap, rc, errno);
        else if (size > g_heap_cur) LOG("[mem] heap %.2f GB (heap region %.2f GB)", size / 1073741824.0, g_heap_cap / 1073741824.0);
        g_heap_cur = size;
        pthread_mutex_unlock(&g_map_lock);
        regs[0] = 0; regs[1] = (uint64_t)g_heap; return 0;
    }
    case 0x02: case 0x03: { // Permission/attribute stubs must not modify a borrowed source.
        uint64_t addr = regs[0], size = regs[1];
        pthread_mutex_lock(&g_map_lock);
        int busy = size > UINT64_MAX - addr || (size && map_source_busy(addr, size));
        pthread_mutex_unlock(&g_map_lock);
        regs[0] = busy ? 0xD401 : 0; return 0;
    }
    case 0x04: {  // MapMemory(dst, src, size): when creating a thread the SDK moves the stack on the heap into the stack region
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        uint32_t result = map_arguments(dst, src, size);
        if (result) { regs[0] = result; return 0; }
        pthread_mutex_lock(&g_map_lock);
        if (map_find(dst, size) || map_source_busy(src, size) || !map_source_backed(src, size)) result = 0xD401;
        else if (g_nmap == MAX_MAP) result = 0xCE01;
        if (result) { pthread_mutex_unlock(&g_map_lock); regs[0] = result; LOG("[RED] MapMemory refused (%#x): %#llx <- %#llx +%#llx", result, (unsigned long long)dst, (unsigned long long)src, (unsigned long long)size); return 0; }
        // Apple pages are 16K, Horizon pages 4K: the accessible region is extended to a 16K boundary (the extra pages opened are unused pages of the same region, semantics unaffected)
        uint64_t mask = (size_t)getpagesize() - 1;
        uint64_t lo = dst & ~mask, hi = (dst + size + mask) & ~mask;
        if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE)) die("MapMemory mprotect errno=%d", errno);
        dm_guest_write_prepare((void *)lo, hi - lo);
        memcpy((void *)dst, (void *)src, size);   // move semantics: the source region is not accessed while mapped (Horizon makes it inaccessible)
        map_add(dst, size, src, 0x0B /*MappedMemory*/);
        pthread_mutex_unlock(&g_map_lock);
        if (g_flag.svc) LOG("MapMemory %#llx <- %#llx +%#llx", dst, src, size);
        regs[0] = 0; return 0;
    }
    case 0x05: {  // UnmapMemory(dst, src, size): contents are moved back to the source region; a 16K page may be shared with an adjacent mapping, permissions are not revoked
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        uint32_t result = map_arguments(dst, src, size);
        if (result) { regs[0] = result; return 0; }
        pthread_mutex_lock(&g_map_lock);
        int k = map_find(dst, size);
        if (!k || g_map[k - 1].base != dst || g_map[k - 1].size != size || g_map[k - 1].src != src) {
            pthread_mutex_unlock(&g_map_lock); LOG("UnmapMemory does not match a mapping %#llx+%#llx", dst, size); regs[0] = 0xD401; return 0; }
        dm_guest_write_prepare((void *)src, size);
        memcpy((void *)src, (void *)dst, size);
        map_del(k - 1);
        pthread_mutex_unlock(&g_map_lock);
        regs[0] = 0; return 0;
    }
    case 0x06: {  // QueryMemory(MemoryInfo* out, PageInfo* pout, addr) → x1 = pageinfo
        uint64_t mi = regs[0], addr = regs[2];
        // MemoryInfo: base(0) size(8) state(0x10) attr(0x14) perm(0x18) ...
        uint8_t *a = (uint8_t *)addr;
        uint64_t base, size, state, perm;
        uint32_t attr = 0;
        int k;
        pthread_mutex_lock(&g_map_lock);   // the registry branches read g_map; the other branches are cheap, so the lock simply spans the whole chain
        uint64_t ab = (uintptr_t)g_alias, ae = ab + g_alias_cap, rb = 0, rn = 0;
        int mapped = 0, in_alias = addr >= ab && addr < ae;
        if (in_alias) alias_query(addr, &rb, &rn, &mapped);
        if (a >= g_heap && a < g_heap + g_heap_cur) { base=(uint64_t)g_heap; size=g_heap_cur; state=5/*Heap*/; perm=3; }
        else if ((k = map_find(addr, 1))) { base=g_map[k-1].base; size=g_map[k-1].size; state=g_map[k-1].state; perm=3; }
        else if (in_alias && mapped) { base=rb; size=rn; state=5; perm=3; }
        else if (a >= g_as && a < g_as + g_reserve) {   // free: up to the adjacent mapping/heap boundary
            uint64_t lo = (uint64_t)g_as, hi = (uint64_t)g_as + g_reserve;
            if (a >= g_heap) lo = (uint64_t)g_heap + g_heap_cur;
            else if (hi > (uint64_t)g_heap) hi = (uint64_t)g_heap;
            if (in_alias) {
                if (rb > ab && rb > lo) lo = rb;
                if (rb + rn < ae && rb + rn < hi) hi = rb + rn;
            } else if (g_alias_cap && addr < ab) {
                alias_query(ab, &rb, &rn, &mapped);
                uint64_t edge = mapped ? ab : rb + rn;
                if ((mapped || edge < ae) && edge < hi) hi = edge;
            } else if (g_alias_cap) {
                alias_query(ae - 1, &rb, &rn, &mapped);
                uint64_t edge = mapped ? ae : rb;
                if ((mapped || edge > ab) && edge > lo) lo = edge;
            }
            for (int i = 0; i < g_nmap; i++) {
                uint64_t e = g_map[i].base + g_map[i].size;
                if (e <= addr && e > lo) lo = e;
                if (g_map[i].base > addr && g_map[i].base < hi) hi = g_map[i].base;
            }
            base=lo; size=hi-lo; state=0; perm=0; }
        else if (module_of(a)) {
            // Loaded module: report the regions in Horizon's layout -- code [0, ro) Code+RX, read-only [ro, data) Code+R, data and bss [data, patch area) CodeData+RW.
            // The SDK's C++ exception unwinding (nn::ro::detail::GetExceptionInfo) walks back from the PC over "Code+RX" regions to find the module start, forward over Code then CodeData to find its end, then reads MOD0 at the module start for .eh_frame_hdr.
            // Previously "the 4 KiB page around it, Code+RX" was reported: the backward walk ran on into other memory, found no MOD0 and aborted (Engage's IL2CPP throws a C++ exception for every managed exception).
            // The rewriter's patch area is reported as CodeData+RX: modules sit back to back in the host, and an executable patch area reported as Code+RX would merge the previous module into the backward walk
            // Modules with the patch area before the base (patch_off < 0): the patch area is reported alone as CodeData+RX, and data and bss run to the end of the image, or to a back patch area
            // (patch_nso.py TWO), which is reported as CodeData+RX like a patch area after the image
            module_t *mm = module_of(a); uint64_t off = (uint64_t)(a - mm->base), mb = (uint64_t)mm->base;
            uint64_t ro = mm->ro_off ? mm->ro_off : mm->text_end, da = mm->data_off ? mm->data_off : ro, pa = mm->patch_off > 0 ? (uint64_t)mm->patch_off : mm->back_len ? mm->back_off : mm->size;
            if (a < mm->base)  { base = mb + mm->patch_off; size = mm->patch_len; state = 4/*CodeData*/; perm = 5; }
            else if (off < ro) { base = mb;      size = ro;            state = 3/*Code*/;     perm = 5; }
            else if (off < da) { base = mb + ro; size = da - ro;       state = 3/*Code*/;     perm = 1; }
            else if (off < pa) { base = mb + da; size = pa - da;       state = 4/*CodeData*/; perm = 3; }
            else               { base = mb + pa; size = mm->size - pa; state = 4/*CodeData*/; perm = 5; }
            if (state == 4 && perm == 3) {
                uint64_t lo = base - mb, hi = lo + size;
                module_data_query(mm, off, &lo, &hi, &perm);
                base = mb + lo; size = hi - lo;
            }
        }
        else {
            // The address lies in "the current host thread's stack": report the whole stack (state 0xB = stack). When the SDK initializes the main thread record it queries here with the sp at that moment and takes the reported range as the main thread stack;
            // later nn::os::GetCurrentStackInfo asserts that sp is still inside that range and aborts if not. Previously every address outside the guest address space was reported as "the 4 KiB page containing it",
            // so the main thread stack was recorded as one page. Whether the sp at init and a later sp share a page depends on the absolute position of the host stack top, which moves with the total length of environment variables / launch arguments (a different environment occasionally aborts at startup).
            uint64_t top = (uint64_t)pthread_get_stackaddr_np(pthread_self()), ssz = pthread_get_stacksize_np(pthread_self());
            if (addr < top && addr >= top - ssz) { base = top - ssz; size = ssz; state = 0xB/*Stack*/; perm = 3; }
            // Other host memory: report "free, no permissions" (the 4 KiB page around it) as Horizon does for unmapped addresses. It used to report Code+RX, and GetExceptionInfo walking back from main merged on through host memory down to 0
            else { base=addr & ~0xFFFull; size=0x1000; state=0/*Free*/; perm=0; } }
        map_source_query(addr, &base, &size, &attr, &perm);
        pthread_mutex_unlock(&g_map_lock);
        if (mi) {
            struct { uint64_t base, size; uint32_t state, attr, perm, ipc, device, reserved; }
                out = {base, size, (uint32_t)state, attr, (uint32_t)perm, 0, 0, 0};
            _Static_assert(sizeof out == 40, "MemoryInfo ABI size");
            memcpy((void *)(uintptr_t)mi, &out, sizeof out);
        }
        regs[0] = 0; regs[1] = 0; return 0;
    }
    case 0x2C: case 0x2D: {  // MapPhysicalMemory / UnmapPhysicalMemory(addr, size): only allowed inside the alias region (used by the SDK's VammManager)
        uintptr_t a = regs[0]; size_t size = regs[1];
        if (!size || ((a | size) & 0xfff) || !memory_contains((uintptr_t)g_alias, g_alias_cap, a, size)) {
            LOG("SVC %#x outside the alias region %p+%#zx", num, (void *)a, size); regs[0] = 0xD401; return 0;
        }
        pthread_mutex_lock(&g_map_lock);
        // The SDK extends committed windows across borrowed stack sources
        // (Raging Loop, StandardAllocator). Recommit preserves every live byte;
        // only decommit can invalidate backing required for eventual copyback.
        if (num == 0x2D && map_source_busy(a, size)) { pthread_mutex_unlock(&g_map_lock); regs[0] = 0xD401; return 0; }
        uint64_t off = a - (uintptr_t)g_alias;
        int64_t need = alias_unmapped(off, size) * 4096;
        // Profile mem_map_limit: a map that would take the process past the application pool fails with ResultLimitReached, as on Horizon (the pool less the used memory GetInfo reports).
        // Pokémon Legends: Z-A's nninitStartup allocates and frees everything allocatable (pool − 64 MB, mapped at the top of its StandardAllocator region: 12 GB, 4 GB after the cut in its patches.py); the SDK keeps the freed pages mapped
        // and maps new ones for later allocations, until a map fails: then it unmaps the cached block and retries (measured on the Mac: the refused 4 MB map at +0x43e2c000 was followed by
        // UnmapPhysicalMemory of the 3.15 GB block and the same map again). Without the limit the mapped total went past the pool in the first minute and the freed block stayed mapped.
        // Off by default: Ace Attorney Investigations Collection maps a little more than the 1 GB pool its profile reports
        if (DM_GAME_MEM_MAP_LIMIT && num == 0x2C) {
            int64_t room = (int64_t)APP_POOL - (int64_t)(g_heap_cur + DM_GAME_MEM_USED + big_bss_bytes()) - atomic_load(&g_alias_mapped);
            if (need && need > room) {
                pthread_mutex_unlock(&g_map_lock);
                static _Atomic int nref; if (atomic_fetch_add(&nref, 1) < 8) LOG("[mem] MapPhysicalMemory +%#zx refused: %.2f GB mapped, the application pool is full (ResultLimitReached, the SDK frees cached pages and retries)",
                                                                       size, atomic_load(&g_alias_mapped) / 1073741824.0);
                regs[0] = 0x10801; return 0;
            }
        }
        // Host pages are 16 KB, Horizon pages 4 KB: a map is rounded out to host pages (the extra neighboring 4K pages are inside the alias region and merely become writable early, harmless);
        // An edge host page is reclaimed only when no live logical neighbor remains.
        uintptr_t pg = (uintptr_t)getpagesize(), lo = (uintptr_t)a, hi = (uintptr_t)a + size;
        uintptr_t mlo = lo & ~(pg - 1), mhi = (hi + pg - 1) & ~(pg - 1);
        int commit = num == 0x2C;
        int64_t changed = commit ? need : (int64_t)size - need;
        if (!commit) {
            atomic_fetch_add_explicit(&g_alias_given, (uint64_t)changed, memory_order_relaxed);   // what the game gives back: the pages of the range that were mapped
            if ((lo - mlo) / 4096 != (uint64_t)alias_unmapped(mlo - (uintptr_t)g_alias, lo - mlo)) mlo += pg;
            if ((mhi - hi) / 4096 != (uint64_t)alias_unmapped(hi - (uintptr_t)g_alias, mhi - hi)) mhi -= pg;
        }
        if (changed && mhi > mlo) {
            if (commit) {
                if (mprotect((void *)mlo, mhi - mlo, PROT_READ | PROT_WRITE))
                    die("SVC %#x mprotect %p+%#zx errno=%d", num, (void *)a, size, errno);
                dm_guest_write_prepare((void *)mlo, mhi - mlo);
            } else if (guest_release(mlo, mhi)) {   // fresh zero pages: alias_initialize has nothing to clear when they are mapped again
                for (uint64_t h = (mlo - (uintptr_t)g_alias) / pg, last = (mhi - 1 - (uintptr_t)g_alias) / pg; h <= last; ++h)
                    g_alias_exposed[h >> 6] &= ~(UINT64_C(1) << (h & 63));
            }
        }
        if (commit && changed) alias_initialize(off, size);
        if (g_flag.svc) LOG("%sPhysicalMemory %p+%#zx", commit ? "Map" : "Unmap", (void *)a, size);
        // how deep the alias region is used (one line per 256 MB crossed): for sizing the reservation and alias region (iOS caps a single reservation at 6 GB)
        if (num == 0x2C) { static _Atomic uint64_t hw; uint64_t end = off + size, old = atomic_load(&hw);
            while (end > old && !atomic_compare_exchange_weak(&hw, &old, end)) {}
            if (end > old && end >> 28 != old >> 28) LOG("[mem] alias region mapped up to +%.2f GB (alias region %.2f GB)", end / 1073741824.0, g_alias_cap / 1073741824.0); }
        // Total actually mapped in the alias region (one line per 256 MB crossed, up or down): how much physical memory the game uses and whether it keeps growing. Counted with a page bitmap, only pages whose state really changed --
        // the SDK maps pages that are already mapped and unmaps ranges larger than what it mapped (Engage unmaps 0x1000000 and then 0x103f000 at the same address), so counting requested sizes went negative
        { int64_t d = alias_pages(off, size, commit) * 4096, before = atomic_fetch_add(&g_alias_mapped, d), after = before + d;
          pthread_mutex_unlock(&g_map_lock);
          if (before >> 28 != after >> 28) {
              char left[96] = "";   // with the guest file: what alias_zero left as it was, reading 0 already
              if (svc_guest_files()) snprintf(left, sizeof left, " (%.2f GB more there read 0 already and were left as they were)", atomic_load(&g_alias_skipped) / 1073741824.0);
              LOG("[mem] alias region mapped %.2f GB (application pool %.2f GB); so far %.2f GB given back by UnmapPhysicalMemory, %.2f GB zeroed on mapping over host pages opened before%s",
                  after / 1073741824.0, APP_POOL / 1073741824.0, atomic_load(&g_alias_given) / 1073741824.0, atomic_load(&g_alias_zeroed) / 1073741824.0, left);
          } }
        regs[0] = 0; return 0;
    }
    case 0x26: {  // Break(reason, ptr, size): SDK fatal abort
        uint32_t res = (regs[1] && regs[2] >= 4) ? *(uint32_t *)regs[1] : 0;
        module_t *mm = module_of((void *)regs[30]);
        static int nb;
        LOG("Break#%d reason=%#llx Result=%#x svc at %s+%#llx", ++nb, regs[0], res, mm ? mm->name : "?", orig_addr);
        dm_backtrace(regs[29]);
        if (regs[0] & 0x80000000ull) return 0;   // notification only, can continue
        _exit(42);
    }
    case 0x1E: guest_clock_read_wait(); regs[0] = 0; regs[1] = guest_ticks(); return 0;   // GetSystemTick
    case 0x27: {  // OutputDebugString(str, len)
        if (g_flag.dbg) fprintf(stderr, "[guest] %.*s\n", (int)regs[1], (char *)regs[0]);
        regs[0] = 0; return 0;
    }
    case 0x29: {  // GetInfo(out*, id, handle, subid) → x1 = value
        uint64_t id = regs[1], v = 0;
        switch (id) {
        case I_CoreMask: v = 7; break;
        case I_PriorityMask: v = 0; break;
        case I_AliasAddr: v = (uint64_t)g_alias; break;   case I_AliasSize: v = g_alias_cap; break;
        case I_HeapAddr:  v = (uint64_t)g_heap;  break;   case I_HeapSize:  v = g_heap_cap;  break;
        case I_AslrAddr:  v = (uint64_t)g_as;    break;   case I_AslrSize:  v = g_reserve;   break;
        case I_StackAddr: v = (uint64_t)g_stack; break;   case I_StackSize: v = g_stack_cap; break;
        case I_TotalMem: case I_TotalNonSys: v = APP_POOL; break;
        case I_UsedMem:  case I_UsedNonSys:  pthread_mutex_lock(&g_map_lock);
                                             v = g_heap_cur + DM_GAME_MEM_USED + big_bss_bytes();   // profile mem_used, 64 MB unless the game needs more (see mem_used in dm/games/README.md)
                                             // with mem_map_limit, the pages mapped in the alias region count too, as on Horizon (the limit in MapPhysicalMemory is the pool less this)
                                             if (DM_GAME_MEM_MAP_LIMIT) v += (uint64_t)atomic_load(&g_alias_mapped);
                                             pthread_mutex_unlock(&g_map_lock); break;
        // the system resource the NPDM declares (profile sys_resource; 32 MB without the key). The SDK's IsVirtualAddressMemoryEnabled is "total != 0": Big Brain Academy's nninitStartup
        // asks StandardAllocator for 60 GB of virtual address memory when it is, and sets a heap only when it is not (its NPDM declares none)
        case I_SysResTotal: v = DM_GAME_SYS_RESOURCE; break; case I_SysResUsed: v = DM_GAME_SYS_RESOURCE ? 0x400000 : 0; break;
        case I_DebuggerAttached: v = 0; break;
        case I_RandomEntropy: { uint64_t dm_entropy64(void); v = dm_entropy64(); break; }   // a deterministic stream when DM_DETCLOCK is on (hle_os.c)
        case I_ProgramId: { static uint64_t pid; if (!pid) { const char *e = getenv("DM_PROGRAM_ID"); pid = e ? strtoull(e, NULL, 16) : DM_GAME_PROGRAM_ID; } v = pid; break; }   // default from the game profile's program_id; DM_PROGRAM_ID=hex overrides
        case I_IsApplication: v = 1; break;
        // nn::os::GetFreeThreadCount (SDK 11+): the thread resource limit less the threads alive, as the kernel answers it (Atmosphère kern_svc_info.cpp). pm gives an application a limit of
        // 96 threads (pm_spec.cpp BaseApplicationThreads; a boost raises it to 256). Monster Train 2's Unity 2022.3 player creates a thread only when this is at least 1: the 0 it got here
        // left its main thread waiting at boot for a thread it never created (main+0x3e1ac4)
        case I_FreeThreadCount: { int thr_limit_used(void); int u = thr_limit_used(); v = u < 96 ? 96 - u : 0; break; }
        case I_UserExcAddr: { static _Alignas(0x1000) uint8_t ctx[0x1000]; v = (uint64_t)ctx; break; }   // user exception context area: newer SDKs' SetDyingMessageRegion writes to +0x1c0 (0 would write through a null pointer)
        // The kernel's page size (Switch 2 SDKs): nn::ro::ProtectRelro takes it as the alignment of the RELRO bounds (value − 1 as a mask, 0 aborts), the VammManager maps that much up front.
        // Horizon's pages are 4 KB, the alignment of every module's segments (Donkey Kong Bananza)
        case I_PageSize: v = 0x1000; break;
        // Horizon's other types (Atmosphère kern_svc_info.cpp), answered 0 as before they were named: no thread tick count, no SVC permission bits, no I/O region, and no extra alias
        // region size (SDK 18 and later ask for it; only a process created with the extra-size flag has one)
        case I_ThreadTickCount: case I_IsSvcPermitted: case I_IoRegionHint: case I_AliasExtraSize: v = 0; break;
        // The Switch 2's remote region: nnosInitialize builds nn::os::detail::RemoteRegionManager's address space allocator over [type 29, type 29 + type 30), and
        // nn::os::GetRemoteMemoryUsage returns types 31 and 32. The SDK's only user is the GPU driver's boot (nv::InitializeGraphics → nn::gs::sdk::BootGraphicsSystem →
        // BootWithRemoteMemory), which the host's own nv::InitializeGraphics replaces (dm/gfx/hle_nv.c), so the process has no remote region and no remote memory (Donkey Kong Bananza)
        case I_RemoteAddr: case I_RemoteSize: case I_RemoteUsage0: case I_RemoteUsage1: v = 0; break;
        default: { static uint8_t seen[64]; if (id < 64 && !seen[id]) { seen[id] = 1; LOG("[RED] svc GetInfo: unknown info type %llu (answered 0)", (unsigned long long)id); } v = 0; break; }
        }
        regs[0] = 0; regs[1] = v; return 0;
    }
    default:
        if (!g_seen[num & 0xff]) { g_seen[num & 0xff] = 1; LOG("SVC %#x x0=%#llx x1=%#llx x2=%#llx (placeholder returns success)", num, regs[0], regs[1], regs[2]); }
        regs[0] = 0; return 0;
    }
}
