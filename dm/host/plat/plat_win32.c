// Host platform layer, Windows 10 1803+ (x86-64), built with mingw-w64 (llvm-mingw). Nothing here needs Windows 11.
// Paths are UTF-8 in the host and UTF-16 for the system calls made here; the C runtime's own file calls (open, fopen, opendir) take the process code page
#include "plat.h"
#include <windows.h>
#include <bcrypt.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002   // Windows 10 1803
#endif

// Files opened by the C runtime are binary, as on POSIX: the host reads and writes game data, save files and tables through open / fopen
__attribute__((constructor)) static void plat_win32_init(void) { _set_fmode(_O_BINARY); }

static int win_errno(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE: return ENOENT;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: return EACCES;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return EEXIST;
    case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return ENOSPC;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: case ERROR_COMMITMENT_LIMIT: return ENOMEM;
    case ERROR_INVALID_ADDRESS: case ERROR_INVALID_PARAMETER: return EINVAL;
    case ERROR_NOT_SUPPORTED: return ENOTSUP;
    case ERROR_FILENAME_EXCED_RANGE: return ENAMETOOLONG;
    default: return EIO;
    }
}
static int fail(void) { errno = win_errno(GetLastError()); return -1; }
// UTF-8 → UTF-16 into a caller buffer (MAX_PATH-sized paths and longer: the host's own buffers are 1–4 KB)
static int wide(const char *s, wchar_t *w, int n) { if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) { errno = ENAMETOOLONG; return -1; } return 0; }
static int narrow(const wchar_t *w, char *s, int n) { if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL)) { errno = ENAMETOOLONG; return -1; } return 0; }

// ---------- Time ----------
uint64_t plat_now_ns(void) {
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((unsigned __int128)(uint64_t)c.QuadPart * 1000000000u / (uint64_t)f.QuadPart);
}
uint64_t plat_boot_ns(void) { return GetTickCount64() * 1000000u; }   // counts sleep; millisecond steps are enough for its callers (seconds, commit timing)
// One waitable timer per thread, high resolution when the system has it (1803+: wakes within about 0.5 ms instead of the 15.6 ms scheduler tick).
// Older systems fall back to an ordinary timer with the system timer at 1 ms (timeBeginPeriod, once)
static pthread_key_t g_timer_key; static pthread_once_t g_timer_once = PTHREAD_ONCE_INIT;
static void timer_free(void *h) { if (h) CloseHandle(h); }
static void timer_key(void) { pthread_key_create(&g_timer_key, timer_free); }
typedef UINT (WINAPI *period_fn)(UINT);
void plat_sleep_ns(uint64_t ns) {
    pthread_once(&g_timer_once, timer_key);
    HANDLE t = pthread_getspecific(g_timer_key);
    if (!t) {
        t = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!t) {
            static LONG period; if (!InterlockedExchange(&period, 1)) { HMODULE w = LoadLibraryW(L"winmm.dll"); period_fn p = w ? (period_fn)(void *)GetProcAddress(w, "timeBeginPeriod") : NULL; if (p) p(1); }
            t = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
        }
        if (!t) { Sleep((DWORD)((ns + 999999) / 1000000)); return; }
        pthread_setspecific(g_timer_key, t);
    }
    LARGE_INTEGER due; due.QuadPart = -(LONGLONG)((ns + 99) / 100);   // relative, in 100 ns units
    if (!SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE)) { Sleep((DWORD)((ns + 999999) / 1000000)); return; }
    WaitForSingleObject(t, INFINITE);
}

// ---------- Threads ----------
static __thread plat_qos_t t_qos = PLAT_QOS_UNSPECIFIED;
int plat_qos_set_self(plat_qos_t q) {
    int p = q == PLAT_QOS_UTILITY ? THREAD_PRIORITY_LOWEST : q == PLAT_QOS_USER_INITIATED ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL;
    if (!SetThreadPriority(GetCurrentThread(), p)) return fail();
    t_qos = q; return 0;
}
plat_qos_t plat_qos_self(void) { return t_qos; }
void *plat_qos_override_start(pthread_t t, plat_qos_t q) { (void)t; (void)q; return NULL; }   // no priority loan between threads (the system boosts lock owners itself)
void plat_qos_override_end(void *ov) { (void)ov; }
typedef HRESULT (WINAPI *describe_fn)(HANDLE, PCWSTR);
void plat_thread_set_name(const char *name) {   // SetThreadDescription: Windows 10 1607, looked up so the binary starts on older systems too
    static describe_fn f; static int looked;
    if (!looked) { HMODULE k = GetModuleHandleW(L"kernel32.dll"); f = k ? (describe_fn)(void *)GetProcAddress(k, "SetThreadDescription") : NULL; looked = 1; }
    wchar_t w[256]; if (f && !wide(name, w, 256)) f(GetCurrentThread(), w);
}
plat_thread_t plat_thread_self(void) { return GetCurrentThreadId(); }
int plat_thread_cpu_us(plat_thread_t t, uint64_t *us) {
    HANDLE h = t ? OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)t) : NULL;
    if (!h) return -1;
    FILETIME c, x, k, u; BOOL ok = GetThreadTimes(h, &c, &x, &k, &u); CloseHandle(h);
    if (!ok) return -1;
    *us = ((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 10; return 0;
}
int plat_thread_stack(uint64_t *top, uint64_t *size) {
    ULONG_PTR lo = 0, hi = 0; GetCurrentThreadStackLimits(&lo, &hi);
    if (!hi) return -1;
    *top = hi; *size = hi - lo; return 0;
}
int plat_cpu_count(void) { return (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS); }

// ---------- Semaphores ----------
plat_sema *plat_sema_create(long value) { return (plat_sema *)CreateSemaphoreW(NULL, value, LONG_MAX, NULL); }
void plat_sema_signal(plat_sema *s) { ReleaseSemaphore((HANDLE)s, 1, NULL); }
int plat_sema_wait(plat_sema *s, int64_t timeout_ns) {
    DWORD ms = timeout_ns < 0 ? INFINITE : (DWORD)((timeout_ns + 999999) / 1000000);
    return WaitForSingleObject((HANDLE)s, ms) == WAIT_OBJECT_0 ? 0 : -1;
}
void plat_sema_destroy(plat_sema *s) { if (s) CloseHandle((HANDLE)s); }

// ---------- Deferred work and termination ----------
typedef struct { unsigned ms; void (*fn)(void); } after_t;
static void *after_main(void *p) { after_t a = *(after_t *)p; free(p); plat_sleep_ns((uint64_t)a.ms * 1000000u); a.fn(); return NULL; }
void plat_after_ms(unsigned ms, void (*fn)(void)) {
    after_t *a = malloc(sizeof *a); if (!a) return;
    a->ms = ms; a->fn = fn; pthread_t t;
    if (pthread_create(&t, NULL, after_main, a)) { free(a); return; }
    pthread_detach(t);
}
// The console control handler runs on a thread the system starts for it; the callback ends the process (dm_host_shutdown exits), and returning TRUE would only end it later
static void (*g_term_fn)(const char *);
static BOOL WINAPI term_handler(DWORD ev) {
    const char *why = ev == CTRL_C_EVENT ? "Ctrl+C" : ev == CTRL_BREAK_EVENT ? "Ctrl+Break" : ev == CTRL_CLOSE_EVENT ? "console close" : ev == CTRL_LOGOFF_EVENT ? "log-off" : ev == CTRL_SHUTDOWN_EVENT ? "shutdown" : "console event";
    if (g_term_fn) g_term_fn(why);
    return TRUE;
}
void plat_on_terminate(void (*fn)(const char *why)) { g_term_fn = fn; SetConsoleCtrlHandler(term_handler, TRUE); }

// ---------- Virtual memory ----------
size_t plat_page_size(void) { SYSTEM_INFO si; GetSystemInfo(&si); return si.dwPageSize; }
size_t plat_vm_granularity(void) { SYSTEM_INFO si; GetSystemInfo(&si); return si.dwAllocationGranularity; }
void *plat_vm_reserve(size_t n) { void *p = VirtualAlloc(NULL, n, MEM_RESERVE, PAGE_NOACCESS); if (!p) fail(); return p; }
void *plat_vm_alloc(size_t n) { void *p = VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); if (!p) fail(); return p; }
static DWORD win_prot(int prot) {
    switch (prot & 7) {
    case PLAT_PROT_NONE: return PAGE_NOACCESS;
    case PLAT_PROT_READ: return PAGE_READONLY;
    case PLAT_PROT_EXEC: return PAGE_EXECUTE;
    case PLAT_PROT_READ | PLAT_PROT_EXEC: return PAGE_EXECUTE_READ;
    case PLAT_PROT_WRITE | PLAT_PROT_EXEC: case PLAT_PROT_READ | PLAT_PROT_WRITE | PLAT_PROT_EXEC: return PAGE_EXECUTE_READWRITE;
    default: return PAGE_READWRITE;   // write implies read, as on the other platforms' hardware
    }
}
// mprotect over a range that may mix reserved and committed pages: an accessible protection commits the reserved runs (zero pages), PROT_NONE leaves them reserved;
// committed runs change protection and keep their contents
int plat_vm_protect(void *a, size_t n, int prot) {
    uint8_t *p = a, *end = p + n; DWORD wp = win_prot(prot);
    while (p < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(p, &mi, sizeof mi) || mi.State == MEM_FREE) { errno = ENOMEM; return -1; }
        uint8_t *re = (uint8_t *)mi.BaseAddress + mi.RegionSize; size_t k = (size_t)((re < end ? re : end) - p);
        if (mi.State == MEM_RESERVE) { if (prot != PLAT_PROT_NONE && !VirtualAlloc(p, k, MEM_COMMIT, wp)) return fail(); }
        else { DWORD old; if (!VirtualProtect(p, k, wp, &old)) return fail(); }
        p += k;
    }
    return 0;
}
int plat_vm_discard(void *a, size_t n) {   // decommit (uncommitted pages in the range are fine); the next accessible protect brings zero pages
    uint8_t *p = a, *end = p + n;
    while (p < end) {   // one allocation at a time: VirtualFree stays within a reservation
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(p, &mi, sizeof mi) || mi.State == MEM_FREE) { errno = ENOMEM; return -1; }
        uint8_t *re = (uint8_t *)mi.BaseAddress + mi.RegionSize; size_t k = (size_t)((re < end ? re : end) - p);
        if (mi.State == MEM_COMMIT && !VirtualFree(p, k, MEM_DECOMMIT)) return fail();
        p += k;
    }
    return 0;
}
int plat_vm_release(void *a, size_t n) { (void)n; return VirtualFree(a, 0, MEM_RELEASE) ? 0 : fail(); }
int plat_vm_query(uint64_t addr, uint64_t *base, uint64_t *size, int *prot) {
    MEMORY_BASIC_INFORMATION mi; uint8_t *p = (uint8_t *)(uintptr_t)addr;
    while (VirtualQuery(p, &mi, sizeof mi)) {   // the region holding addr, or the next one that is not free (vm_region's answer)
        if (mi.State != MEM_FREE) {
            DWORD w = mi.State == MEM_COMMIT ? mi.Protect & 0xff : PAGE_NOACCESS;
            *base = (uint64_t)(uintptr_t)mi.BaseAddress; *size = mi.RegionSize;
            *prot = w == PAGE_READONLY ? PLAT_PROT_READ : w == PAGE_READWRITE || w == PAGE_WRITECOPY ? PLAT_PROT_READ | PLAT_PROT_WRITE : w == PAGE_EXECUTE ? PLAT_PROT_EXEC :
                    w == PAGE_EXECUTE_READ ? PLAT_PROT_READ | PLAT_PROT_EXEC : w == PAGE_EXECUTE_READWRITE || w == PAGE_EXECUTE_WRITECOPY ? 7 : PLAT_PROT_NONE;
            if (mi.State == MEM_COMMIT && (mi.Protect & PAGE_GUARD)) *prot = PLAT_PROT_NONE;
            return 0;
        }
        p = (uint8_t *)mi.BaseAddress + mi.RegionSize;
    }
    return -1;
}
int plat_mem_read(uint64_t addr, void *out, size_t n, size_t *got) {
    SIZE_T g = 0; BOOL ok = ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)addr, out, n, &g);
    if (got) *got = g;
    return !ok || g != n;
}

// ---------- A separate heap: a private Win32 heap, with a header before each block (its size, and the system block it sits in when aligned past 16) ----------
struct plat_zone { HANDLE heap; };
typedef struct { void *raw; size_t size; } zhdr;
_Static_assert(sizeof(zhdr) == 16, "header keeps 16-byte alignment");
plat_zone *plat_zone_create(const char *name) { (void)name; plat_zone *z = calloc(1, sizeof *z); if (z && !(z->heap = HeapCreate(0, 0, 0))) { free(z); z = NULL; } return z; }
void *plat_zone_memalign(plat_zone *z, size_t align, size_t n) {
    if (align < 16) align = 16;
    if (n > SIZE_MAX - align - sizeof(zhdr)) return NULL;
    uint8_t *raw = HeapAlloc(z->heap, 0, n + align + sizeof(zhdr)); if (!raw) return NULL;
    uint8_t *p = (uint8_t *)(((uintptr_t)raw + sizeof(zhdr) + align - 1) & ~(uintptr_t)(align - 1));
    ((zhdr *)p)[-1] = (zhdr){raw, n};
    return p;
}
void *plat_zone_malloc(plat_zone *z, size_t n) { return plat_zone_memalign(z, 16, n); }
void plat_zone_free(plat_zone *z, void *p) { if (p) HeapFree(z->heap, 0, ((zhdr *)p)[-1].raw); }
size_t plat_zone_size(plat_zone *z, const void *p) { (void)z; return ((const zhdr *)p)[-1].size; }
void *plat_zone_realloc(plat_zone *z, void *p, size_t n) {
    if (!p) return plat_zone_malloc(z, n);
    void *q = plat_zone_malloc(z, n); if (!q) return NULL;
    size_t old = plat_zone_size(z, p); memcpy(q, p, old < n ? old : n); plat_zone_free(z, p); return q;
}
size_t plat_zone_good_size(size_t n) { return n <= 16 ? 16 : (n + 15) & ~(size_t)15; }

// ---------- Files ----------
// Named attributes are NTFS alternate data streams, path:name (on files and directories alike)
static int stream_path(const char *path, const char *name, wchar_t *w, int n) {
    char b[4096]; if ((size_t)snprintf(b, sizeof b, "%s:%s", path, name) >= sizeof b) { errno = ENAMETOOLONG; return -1; }
    return wide(b, w, n);
}
ssize_t plat_xattr_get(const char *path, const char *name, void *buf, size_t n) {
    wchar_t wp[4096], ws[4096];
    if (wide(path, wp, 4096) || stream_path(path, name, ws, 4096)) return -1;
    if (GetFileAttributesW(wp) == INVALID_FILE_ATTRIBUTES) return fail();
    HANDLE h = CreateFileW(ws, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { DWORD e = GetLastError(); errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENODATA : win_errno(e); return -1; }
    DWORD got = 0; BOOL ok = ReadFile(h, buf, (DWORD)(n > 0x7fffffff ? 0x7fffffff : n), &got, NULL); CloseHandle(h);
    if (!ok) return fail();
    return (ssize_t)got;
}
int plat_xattr_set(const char *path, const char *name, const void *v, size_t n) {
    wchar_t ws[4096]; if (stream_path(path, name, ws, 4096)) return -1;
    HANDLE h = CreateFileW(ws, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return fail();
    DWORD put = 0; BOOL ok = WriteFile(h, v, (DWORD)n, &put, NULL); CloseHandle(h);
    return ok && put == n ? 0 : fail();
}
int plat_xattr_remove(const char *path, const char *name) {
    wchar_t ws[4096]; if (stream_path(path, name, ws, 4096)) return -1;
    if (DeleteFileW(ws)) return 0;
    DWORD e = GetLastError(); errno = e == ERROR_FILE_NOT_FOUND ? ENODATA : win_errno(e); return -1;
}
static void copy_streams(const wchar_t *src, const wchar_t *dst) {   // a directory's alternate data streams (CopyFileW copies a file's own); best effort
    WIN32_FIND_STREAM_DATA sd; HANDLE f = FindFirstStreamW(src, FindStreamInfoStandard, &sd, 0);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(sd.cStreamName, L"::$DATA")) continue;   // the unnamed stream
        wchar_t s[4200], d[4200], nm[300]; wcsncpy(nm, sd.cStreamName, 299); nm[299] = 0;
        wchar_t *c = wcsrchr(nm, L':'); if (c && c != nm) *c = 0;   // ":name:$DATA" → ":name"
        _snwprintf(s, 4200, L"%ls%ls", src, nm); _snwprintf(d, 4200, L"%ls%ls", dst, nm); s[4199] = d[4199] = 0;
        CopyFileW(s, d, FALSE);
    } while (FindNextStreamW(f, &sd));
    FindClose(f);
}
static int rm_tree_w(const wchar_t *p) {
    DWORD a = GetFileAttributesW(p); if (a == INVALID_FILE_ATTRIBUTES) return 0;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(p) ? 0 : -1;
    wchar_t pat[4200]; _snwprintf(pat, 4200, L"%ls\\*", p); pat[4199] = 0;
    WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW(pat, &fd); int r = 0;
    if (f != INVALID_HANDLE_VALUE) {
        do { if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
             wchar_t c[4200]; _snwprintf(c, 4200, L"%ls\\%ls", p, fd.cFileName); c[4199] = 0; r |= rm_tree_w(c); } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    return r | (RemoveDirectoryW(p) ? 0 : -1);
}
static int clone_w(const wchar_t *src, const wchar_t *dst) {
    DWORD a = GetFileAttributesW(src); if (a == INVALID_FILE_ATTRIBUTES) return fail();
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return CopyFileW(src, dst, TRUE) ? 0 : fail();   // with the file's streams and attributes
    if (!CreateDirectoryW(dst, NULL)) return fail();
    copy_streams(src, dst);
    wchar_t pat[4200]; _snwprintf(pat, 4200, L"%ls\\*", src); pat[4199] = 0;
    WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW(pat, &fd); int r = 0;
    if (f == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : fail();
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        wchar_t s[4200], d[4200]; _snwprintf(s, 4200, L"%ls\\%ls", src, fd.cFileName); _snwprintf(d, 4200, L"%ls\\%ls", dst, fd.cFileName); s[4199] = d[4199] = 0;
        r = clone_w(s, d);
    } while (!r && FindNextFileW(f, &fd));
    FindClose(f); return r;
}
// A copy of the tree with its streams, not an atomic clone (NTFS has no copy-on-write clones; ReFS block cloning is not used): on failure the partial copy is removed
int plat_clone_tree(const char *src, const char *dst) {
    wchar_t s[4096], d[4096]; if (wide(src, s, 4096) || wide(dst, d, 4096)) return -1;
    if (GetFileAttributesW(d) != INVALID_FILE_ATTRIBUTES) { errno = EEXIST; return -1; }
    if (!clone_w(s, d)) return 0;
    int e = errno; rm_tree_w(d); errno = e; return -1;
}
// Windows cannot exchange two directories in one step: three renames, b out of the way, a into b, the old b into a. Not atomic (open: a crash between them leaves b under
// "<b>.swap"); the save transaction (hle_fs.c Commit) relies on the swap being atomic on the other platforms
int plat_rename_swap(const char *a, const char *b) {
    wchar_t wa[4096], wb[4096], wt[4200]; if (wide(a, wa, 4096) || wide(b, wb, 4096)) return -1;
    _snwprintf(wt, 4200, L"%ls.swap", wb); wt[4199] = 0;
    if (!MoveFileExW(wb, wt, 0)) return fail();
    if (!MoveFileExW(wa, wb, 0)) { int e = win_errno(GetLastError()); MoveFileExW(wt, wb, 0); errno = e; return -1; }
    return MoveFileExW(wt, wa, 0) ? 0 : fail();
}
int plat_fd_path(int fd, char *out, size_t n) {
    HANDLE h = (HANDLE)_get_osfhandle(fd); if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    wchar_t w[4096]; DWORD k = GetFinalPathNameByHandleW(h, w, 4096, FILE_NAME_NORMALIZED);
    if (!k || k >= 4096) return fail();
    return narrow(w, out, (int)n);
}
int plat_data_path(char *buf, size_t n, const char *rel) {   // %APPDATA%\dm-work (the roaming application data folder)
    const char *a = getenv("APPDATA");
    return snprintf(buf, n, "%s/dm-work/%s", a && *a ? a : ".", rel);
}

// ---------- Random bytes ----------
void plat_random_buf(void *p, size_t n) {
    for (uint8_t *b = p; n; ) { ULONG k = n > 0x40000000u ? 0x40000000u : (ULONG)n; if (BCryptGenRandom(NULL, b, k, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) abort(); b += k; n -= k; }
}
uint32_t plat_random32(void) { uint32_t v; plat_random_buf(&v, sizeof v); return v; }

// ---------- Symbols ----------
const char *plat_addr_symbol(const void *a, uintptr_t *start) { (void)a; (void)start; return NULL; }   // no exported-symbol lookup without debug information (DbgHelp) yet

// ---------- POSIX functions mingw-w64 lacks ----------
int plat_win32_mkdir(const char *path, int mode) {
    (void)mode; wchar_t w[4096]; if (wide(path, w, 4096)) return -1;
    return CreateDirectoryW(w, NULL) ? 0 : fail();
}
static ssize_t prw(int fd, void *buf, size_t n, off_t off, int write) {
    HANDLE h = (HANDLE)_get_osfhandle(fd); if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (off < 0) { errno = EINVAL; return -1; }
    OVERLAPPED o = {0}; o.Offset = (DWORD)off; o.OffsetHigh = (DWORD)((uint64_t)off >> 32);
    DWORD k = 0, want = (DWORD)(n > 0x7fffffff ? 0x7fffffff : n);
    BOOL ok = write ? WriteFile(h, buf, want, &k, &o) : ReadFile(h, buf, want, &k, &o);
    if (!ok) { DWORD e = GetLastError(); if (!write && e == ERROR_HANDLE_EOF) return 0; errno = win_errno(e); return -1; }
    return (ssize_t)k;
}
ssize_t pread(int fd, void *buf, size_t n, off_t off) { return prw(fd, buf, n, off, 0); }
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) { return prw(fd, (void *)buf, n, off, 1); }
size_t strlcpy(char *dst, const char *src, size_t n) {
    size_t l = strlen(src);
    if (n) { size_t k = l < n - 1 ? l : n - 1; memcpy(dst, src, k); dst[k] = 0; }
    return l;
}
size_t strlcat(char *dst, const char *src, size_t n) {
    size_t d = strnlen(dst, n);
    if (d == n) return n + strlen(src);
    return d + strlcpy(dst + d, src, n - d);
}
void *memmem(const void *h, size_t hn, const void *nd, size_t nn) {
    if (!nn) return (void *)h;
    if (hn < nn) return NULL;
    for (const uint8_t *p = h, *last = p + (hn - nn); p <= last; ) {
        const uint8_t *q = memchr(p, *(const uint8_t *)nd, (size_t)(last - p) + 1);
        if (!q) return NULL;
        if (!memcmp(q, nd, nn)) return (void *)q;
        p = q + 1;
    }
    return NULL;
}
time_t timegm(struct tm *tm) { return _mkgmtime(tm); }
int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value) ? (errno = EINVAL, -1) : 0;
}
int unsetenv(const char *name) { return _putenv_s(name, "") ? (errno = EINVAL, -1) : 0; }
char *mkdtemp(char *tmpl) {   // the trailing XXXXXX become random letters until a new directory is made
    size_t n = strlen(tmpl); if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX")) { errno = EINVAL; return NULL; }
    static const char a[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (int tries = 0; tries < 100; tries++) {
        uint8_t r[6]; plat_random_buf(r, sizeof r);
        for (int i = 0; i < 6; i++) tmpl[n - 6 + i] = a[r[i] % 36];
        if (!plat_win32_mkdir(tmpl, 0700)) return tmpl;
        if (errno != EEXIST) return NULL;
    }
    return NULL;
}
