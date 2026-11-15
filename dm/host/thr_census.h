// Where a frame's time goes when no frame thread is busy: what the guest main thread waits on, and which host threads use the CPU the guest threads do not.
// hle_thread.c feeds both (svc_thread times the main thread's waits; dm_host_thread_cpu walks the task's threads) and hle_nvn.c prints them beside the threads line every 300 frames.
// Why: in Ocarina of Time's stuck slow state on an iPhone 17 (2026-11-15) a frame did the same CPU work as at 29 fps, at the same clocks, yet took 60-80 ms: every frame thread
// was idle more than half of it, and the process ran 0.6-0.9 of a core outside the guest threads and the frame threads. The lines before these could name neither the wait nor
// that CPU (docs/reference/threads.md, "Frames that wait with every frame thread idle"). Pure logic, no system calls (dm/host/tests/t_thr_census.c).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// The guest main thread's waits by kind. SleepThread 0 / -1 / -2 are yields, a positive time is a sleep; the same-core handoff after an SVC is counted on its own
enum { TW_ADDRESS, TW_SYNC, TW_LOCK, TW_CONDVAR, TW_SLEEP, TW_YIELD, TW_HANDOFF, TW_KINDS };
static const char *const thr_wait_names[TW_KINDS] = {"address waits", "WaitSynchronization", "ArbitrateLock", "condition variables", "sleeps", "yields", "same-core handoffs"};
static inline int thr_wait_kind(int svc, uint64_t x0) {
    switch (svc) {
    case 0x34: return TW_ADDRESS;
    case 0x18: return TW_SYNC;
    case 0x1A: return TW_LOCK;
    case 0x1C: return TW_CONDVAR;
    case 0x0B: return (int64_t)x0 <= 0 ? TW_YIELD : TW_SLEEP;
    default: return -1;
    }
}
typedef struct { uint64_t n[TW_KINDS], ns[TW_KINDS]; } thr_waits;
// The window's waits per frame, the longest kind first, each with its count; kinds that took no time are left out. frame_ms = the window's mean frame time
static inline size_t thr_waits_line(char *b, size_t sz, const thr_waits *now, const thr_waits *was, double frames, double frame_ms) {
    double ms[TW_KINDS], all = 0; int ord[TW_KINDS], k = 0; size_t o = 0;
    if (frames < 1) frames = 1;
    for (int i = 0; i < TW_KINDS; i++) { ms[i] = (double)(now->ns[i] - was->ns[i]) / 1e6 / frames; if (now->ns[i] > was->ns[i]) { ord[k++] = i; all += ms[i]; } }
    for (int i = 1; i < k; i++) for (int j = i; j > 0 && ms[ord[j]] > ms[ord[j - 1]]; j--) { int t = ord[j]; ord[j] = ord[j - 1]; ord[j - 1] = t; }
    if (sz) b[0] = 0;
    if (!k) { snprintf(b, sz, "no waits"); return strlen(b); }
    for (int i = 0; i < k && o < sz; i++) { int w = ord[i]; double c = (double)(now->n[w] - was->n[w]) / frames; o += (size_t)snprintf(b + o, sz - o, c < 10 ? "%s%s %.1f ms (%.1f)" : "%s%s %.1f ms (%.0f)", i ? ", " : "", thr_wait_names[w], ms[w], c); }   // a count under 10 a frame keeps a decimal: a rare wait is not "(0)"
    if (o < sz) o += (size_t)snprintf(b + o, sz - o, "; %.1f ms in all, %.0f%% of the %.1f ms frame", all, frame_ms > 0 ? 100.0 * all / frame_ms : 0, frame_ms);
    return o < sz ? o : sz;
}

// The addresses the main thread waited on longest in the window: a small table where a new address takes the slot with the least time once it is full,
// so an address that keeps coming back stays while one-off waits come and go
#define THR_ADDR_N 8
typedef struct { uint64_t addr, ns, n; } thr_addr;
static inline void thr_addr_add(thr_addr *t, uint64_t addr, uint64_t ns) {
    int lo = 0;
    for (int i = 0; i < THR_ADDR_N; i++) {
        if (t[i].n && t[i].addr == addr) { t[i].ns += ns; t[i].n++; return; }
        if (!t[i].n) { t[i].addr = addr; t[i].ns = ns; t[i].n = 1; return; }
        if (t[i].ns < t[lo].ns) lo = i;
    }
    t[lo].addr = addr; t[lo].ns = ns; t[lo].n = 1;
}
static inline void thr_addr_top2(const thr_addr *t, const thr_addr **a1, const thr_addr **a2) {
    *a1 = *a2 = NULL;
    for (int i = 0; i < THR_ADDR_N; i++) { if (!t[i].n) continue;
        if (!*a1 || t[i].ns > (*a1)->ns) { *a2 = *a1; *a1 = &t[i]; } else if (!*a2 || t[i].ns > (*a2)->ns) *a2 = &t[i]; }
}
static inline void thr_addr_reset(thr_addr *t) { memset(t, 0, sizeof(thr_addr) * THR_ADDR_N); }

// One host thread at a census: its ID, its CPU time and the kernel's part of it (µs), whether a GCD worker runs it (an unnamed one is told apart by its tid), its priority and name
typedef struct { uint64_t tid, cpu_us, sys_us; int gcd, pri; char name[40]; } thr_sample;
typedef struct { const thr_sample *s; double ms, sys_ms; } thr_rank;   // CPU and kernel milliseconds per second over the window
// The threads of `cur` that used the most CPU since `was` (matched by tid: a thread born in the window counts all its CPU, one that ended is gone from cur), the busiest first.
// Returns how many went into top; *all_ms gets every thread's CPU per second, the ones past the top too
static inline int thr_census(const thr_sample *was, int nwas, const thr_sample *cur, int ncur, double secs, thr_rank *top, int ntop, double *all_ms) {
    int n = 0; double all = 0;
    if (secs <= 0) secs = 1;
    for (int i = 0; i < ncur; i++) {
        uint64_t c0 = 0, s0 = 0;
        for (int j = 0; j < nwas; j++) if (was[j].tid == cur[i].tid) { c0 = was[j].cpu_us; s0 = was[j].sys_us; break; }
        double ms = cur[i].cpu_us > c0 ? (double)(cur[i].cpu_us - c0) / 1e3 / secs : 0, sys = cur[i].sys_us > s0 ? (double)(cur[i].sys_us - s0) / 1e3 / secs : 0;
        all += ms;
        if (ms <= 0) continue;
        int at = n < ntop ? n++ : ntop - 1;
        if (at == ntop - 1 && n == ntop && top[at].s && top[at].ms >= ms) continue;   // full, and smaller than the last kept
        top[at] = (thr_rank){&cur[i], ms, sys};
        for (int j = at; j > 0 && top[j].ms > top[j - 1].ms; j--) { thr_rank t = top[j]; top[j] = top[j - 1]; top[j - 1] = t; }
    }
    *all_ms = all;
    return n;
}
static inline size_t thr_census_line(char *b, size_t sz, const thr_rank *top, int n, double all_ms, int nthreads) {
    size_t o = (size_t)snprintf(b, sz, "%.0f ms/s of CPU in %d threads; top: ", all_ms, nthreads);
    for (int i = 0; i < n && o < sz; i++) {
        const thr_sample *s = top[i].s;
        if (s->name[0]) o += (size_t)snprintf(b + o, sz - o, "%s'%s' %.0f ms/s, kernel %.0f", i ? "; " : "", s->name, top[i].ms, top[i].sys_ms);
        else o += (size_t)snprintf(b + o, sz - o, "%san unnamed %sthread (tid %llu, priority %d) %.0f ms/s, kernel %.0f", i ? "; " : "", s->gcd ? "GCD " : "", (unsigned long long)s->tid, s->pri, top[i].ms, top[i].sys_ms);
    }
    if (!n && o < sz) snprintf(b + o, sz - o, "none");
    return o < sz ? o : sz;
}
