// System profile: DM_SYSPROF=<tsv path> (Mac; off by default, an empty value is off) writes what the system did with the game's GPU and process, sampled every 100 ms, so a session
// shows why a frame rate fell. Each row is "t_s, metric, value": t_s is the end of the sample's interval in seconds since the host's start (now_ns() - g_t0_ns), the clock of
// DM_NVN_TSV's @t_s rows and of the log, so the samples line up with the frame rows by time; dt_ms is the interval the sample covers. The metrics:
//   gpu_pstate: the mean performance state of the GPU's powered time (P1 = 1, the lowest clock), IOReport "GPU Stats" / "GPU Performance States" (GPUPH)
//   gpu_mhz: the mean clock of the powered time, when pmgr's voltage-states9 table has one frequency per powered state (else the clock is left out and gpu_pstate stays)
//   gpu_on: % of the interval the GPU was powered (not OFF): powered is not busy, the GPU stays powered between closely spaced submissions
//   gpu_w: GPU power, watts, IOReport "Energy Model" / "GPU Energy"
//   gpu_boost: the boost controller's requested state, mean P index (BSTGPUPH)
//   gpu_cltm, gpu_droop: % of the interval a thermal limit (GPU_CLTM other than NO_CLTM) and a voltage droop limit (GPUDVDH other than 100%) applied
//   role: the process's role (task_policy_get TASK_CATEGORY_POLICY: 1 foreground app, 2 background app, ...), thermal: NSProcessInfo's thermal state (0 nominal ... 3 critical),
//   low_power: Low Power Mode
// A reading the system does not give is left out on its own (no row), and the log says once which ones are missing: an M6 gave no clock (reported, not checked here), and an M5 Pro's frequency
// table lists 13 frequencies for its 15 powered states. A sample with the GPU never powered writes no clock or P state. A change of role is logged as it happens: macOS sets the performance
// controller's targets by it. IOReport is a private library the system tools use (powermetrics, Activity Monitor); it needs no root and is loaded with dlopen
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#include "sysprof.h"
#if TARGET_OS_OSX
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include "dm.h"

typedef struct IOReportSubscription *IOReportSubscriptionRef;
static CFMutableDictionaryRef (*ir_channels)(CFStringRef, CFStringRef, uint64_t, uint64_t, uint64_t);
static void (*ir_merge)(CFDictionaryRef, CFDictionaryRef, CFTypeRef);
static IOReportSubscriptionRef (*ir_subscribe)(void *, CFMutableDictionaryRef, CFMutableDictionaryRef *, uint64_t, CFTypeRef);
static CFDictionaryRef (*ir_samples)(IOReportSubscriptionRef, CFMutableDictionaryRef, CFTypeRef);
static CFDictionaryRef (*ir_delta)(CFDictionaryRef, CFDictionaryRef, CFTypeRef);
static CFStringRef (*ir_name)(CFDictionaryRef), (*ir_unit)(CFDictionaryRef);
static int (*ir_format)(CFDictionaryRef), (*ir_nstates)(CFDictionaryRef);
static CFStringRef (*ir_state_name)(CFDictionaryRef, int);
static int64_t (*ir_residency)(CFDictionaryRef, int), (*ir_value)(CFDictionaryRef, int32_t);

enum { CH_GPUPH, CH_BOOST, CH_CLTM, CH_DROOP, CH_ENERGY, CH_N };
// Each channel and where it is: its group and subgroup (a subgroup keeps the subscription to the channels used: "GPU Stats" has about 140, "Energy Model" about 360). GPU Energy has
// no subgroup, so its group's channels are filtered by name (ch_take)
static const struct { const char *name, *group, *sub; } k_ch[CH_N] = {
    [CH_GPUPH] = {"GPUPH", "GPU Stats", "GPU Performance States"},
    [CH_BOOST] = {"BSTGPUPH", "GPU Stats", "GPU Boost Controller Performance States"},
    [CH_CLTM] = {"GPU_CLTM", "GPU Stats", "CLTM-induced GPU Performance States"},
    [CH_DROOP] = {"GPUDVDH", "GPU Stats", "DVD Request States"},
    [CH_ENERGY] = {"GPU Energy", "Energy Model", NULL},
};

static double g_freq[32]; static int g_nfreq;

static int load_ioreport(void) {
    void *h = dlopen("/usr/lib/libIOReport.dylib", RTLD_LAZY);
    if (!h) return 0;
    ir_channels = dlsym(h, "IOReportCopyChannelsInGroup"); ir_merge = dlsym(h, "IOReportMergeChannels"); ir_subscribe = dlsym(h, "IOReportCreateSubscription");
    ir_samples = dlsym(h, "IOReportCreateSamples"); ir_delta = dlsym(h, "IOReportCreateSamplesDelta"); ir_name = dlsym(h, "IOReportChannelGetChannelName");
    ir_unit = dlsym(h, "IOReportChannelGetUnitLabel"); ir_format = dlsym(h, "IOReportChannelGetFormat"); ir_nstates = dlsym(h, "IOReportStateGetCount");
    ir_state_name = dlsym(h, "IOReportStateGetNameForIndex"); ir_residency = dlsym(h, "IOReportStateGetResidency"); ir_value = dlsym(h, "IOReportSimpleGetIntegerValue");
    return ir_channels && ir_merge && ir_subscribe && ir_samples && ir_delta && ir_name && ir_unit && ir_format && ir_nstates && ir_state_name && ir_residency && ir_value;
}

static int str_is(CFStringRef s, const char *c) { char b[64] = ""; if (s) CFStringGetCString(s, b, sizeof b, kCFStringEncodingUTF8); return !strcmp(b, c); }
static int ch_of(CFStringRef name) { for (int c = 0; c < CH_N; c++) if (str_is(name, k_ch[c].name)) return c; return -1; }   // the k_ch index of a channel, -1 if not one of them
// Appends s to the log text in b (size bytes, *w used), after sep unless it is the first; stops at the end of b
static void list_add(char *b, size_t size, int *w, const char *sep, const char *s) {
    if (*w < 0 || (size_t)*w >= size - 1) return;
    int k = snprintf(b + *w, size - (size_t)*w, "%s%s", *w ? sep : "", s);
    if (k > 0) *w = (size_t)*w + (size_t)k < size ? *w + k : (int)size - 1;
}

// The GPU's frequencies, lowest first: pmgr's voltage-states9, (frequency, voltage) pairs of 32-bit words; a zero frequency (the OFF entry) is skipped. Hz on newer chips, kHz on
// older ones: a table that does not come out ascending between 50 and 3,000 MHz either way is not used. Without a usable table the log lists pmgr's voltage-states* properties
// (names and lengths only), so a chip that keeps it elsewhere shows where to look
static void load_freqs(void) {
    io_iterator_t it; char names[512] = ""; int nw = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceNameMatching("pmgr"), &it)) return;
    for (io_object_t o; (o = IOIteratorNext(it));) {
        CFDataRef d = IORegistryEntryCreateCFProperty(o, CFSTR("voltage-states9"), NULL, 0);
        if (d && CFGetTypeID(d) == CFDataGetTypeID() && !g_nfreq) {
            const uint32_t *p = (const uint32_t *)CFDataGetBytePtr(d); int n = (int)(CFDataGetLength(d) / 8), ok = 1;
            for (int scale = 0; scale < 2 && !g_nfreq; scale++) {   // 0: Hz, 1: kHz
                g_nfreq = 0; ok = 1;
                for (int i = 0; i < n && g_nfreq < 32; i++) if (p[2 * i]) {
                    double mhz = p[2 * i] / (scale ? 1e3 : 1e6);
                    if (mhz < 50 || mhz > 3000 || (g_nfreq && mhz <= g_freq[g_nfreq - 1])) { ok = 0; break; }
                    g_freq[g_nfreq++] = mhz; }
                if (!ok) g_nfreq = 0;
            }
        }
        if (d) CFRelease(d);
        if (!g_nfreq) {
            CFMutableDictionaryRef props = NULL;
            if (IORegistryEntryCreateCFProperties(o, &props, kCFAllocatorDefault, 0) == KERN_SUCCESS && props) {
                for (NSString *k in (__bridge NSDictionary *)props) {
                    id v = ((__bridge NSDictionary *)props)[k];
                    if (![k hasPrefix:@"voltage-states"]) continue;
                    char e[96]; snprintf(e, sizeof e, "%s (%lu bytes)", k.UTF8String, [v isKindOfClass:NSData.class] ? (unsigned long)[(NSData *)v length] : 0ul);
                    list_add(names, sizeof names, &nw, ", ", e); }
                CFRelease(props); }
        }
        IOObjectRelease(o);
        if (g_nfreq) break;
    }
    IOObjectRelease(it);
    if (!g_nfreq) LOG("[sysprof] no usable GPU frequency table (pmgr voltage-states9): gpu_mhz is left out, gpu_pstate stays; pmgr has %s", nw ? names : "no voltage-states properties");
}

// The subscription's channels: each channel of k_ch from its group (and subgroup), merged into one; *have gets a bit per channel found
static CFMutableDictionaryRef ch_take(unsigned *have) CF_RETURNS_RETAINED;   // a Copy: the caller releases it
static CFMutableDictionaryRef ch_take(unsigned *have) {
    CFMutableDictionaryRef all = NULL; *have = 0;
    for (int c = 0; c < CH_N; c++) {
        CFStringRef g = CFStringCreateWithCString(NULL, k_ch[c].group, kCFStringEncodingUTF8), s = k_ch[c].sub ? CFStringCreateWithCString(NULL, k_ch[c].sub, kCFStringEncodingUTF8) : NULL;
        CFMutableDictionaryRef d = ir_channels(g, s, 0, 0, 0);
        CFRelease(g); if (s) CFRelease(s);
        if (!d) continue;
        CFArrayRef a = CFDictionaryGetValue(d, CFSTR("IOReportChannels")); CFMutableArrayRef keep = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
        for (CFIndex i = 0; a && i < CFArrayGetCount(a); i++) { CFDictionaryRef x = CFArrayGetValueAtIndex(a, i); if (str_is(ir_name(x), k_ch[c].name)) CFArrayAppendValue(keep, x); }
        if (CFArrayGetCount(keep)) {
            CFDictionarySetValue(d, CFSTR("IOReportChannels"), keep); *have |= 1u << c;
            if (!all) { all = d; d = NULL; } else ir_merge(all, d, NULL);
        }
        CFRelease(keep); if (d) CFRelease(d);
    }
    return all;
}

static int task_role(void) {
    task_category_policy_data_t p = {0}; mach_msg_type_number_t n = TASK_CATEGORY_POLICY_COUNT; boolean_t dflt = FALSE;
    return task_policy_get(mach_task_self(), TASK_CATEGORY_POLICY, (task_policy_t)&p, &n, &dflt) == KERN_SUCCESS ? (int)p.role : -1;
}
static const char *role_name(int r) {
    switch (r) {
        case TASK_UNSPECIFIED: return "unspecified"; case TASK_FOREGROUND_APPLICATION: return "foreground app"; case TASK_BACKGROUND_APPLICATION: return "background app";
        case TASK_CONTROL_APPLICATION: return "control app"; case TASK_GRAPHICS_SERVER: return "graphics server"; case TASK_THROTTLE_APPLICATION: return "throttled app";
        case TASK_NONUI_APPLICATION: return "non-UI app"; case TASK_DEFAULT_APPLICATION: return "default app"; case TASK_DARWINBG_APPLICATION: return "darwin background app";
        default: return "unknown";
    }
}

typedef struct { FILE *f; double t; unsigned seen; } out_t;   // seen: the channels that gave a value in a delta (logged once, after the first)
static void row(out_t *o, const char *name, double v) { fprintf(o->f, "%.3f\t%s\t%.3f\n", o->t, name, v); }

// One interval's channels: the GPU's state residencies and its energy
static void read_delta(CFDictionaryRef d, double secs, out_t *o) {
    CFArrayRef a = CFDictionaryGetValue(d, CFSTR("IOReportChannels"));
    for (CFIndex i = 0; a && i < CFArrayGetCount(a); i++) {
        CFDictionaryRef c = CFArrayGetValueAtIndex(a, i); int ch = ch_of(ir_name(c));
        if (ch < 0) continue;
        if (ch == CH_ENERGY) {
            if (ir_format(c) != 1) continue;   // a simple value: energy since the previous sample
            CFStringRef u = ir_unit(c);
            double v = (double)ir_value(c, 0), scale = str_is(u, "nJ") ? 1e-9 : str_is(u, "uJ") ? 1e-6 : str_is(u, "mJ") ? 1e-3 : 0;
            if (!scale) { static int noted; if (!noted) { noted = 1; char b[32] = "?"; if (u) CFStringGetCString(u, b, sizeof b, kCFStringEncodingUTF8); LOG("[sysprof] GPU Energy in unit %s, not nJ / uJ / mJ: gpu_w is left out", b); } continue; }
            if (secs > 0) { row(o, "gpu_w", v * scale / secs); o->seen |= 1u << CH_ENERGY; }
            continue;
        }
        int gpuph = ch == CH_GPUPH, boost = ch == CH_BOOST, cltm = ch == CH_CLTM;
        int n = ir_nstates(c); if (n <= 0) continue;
        double tot = 0, on = 0, mhz = 0, pw = 0, limited = 0; int p = 0;
        for (int k = 0; k < n; k++) {
            double r = (double)ir_residency(c, k); CFStringRef sn = ir_state_name(c, k); tot += r;
            if (gpuph || boost) {
                if (str_is(sn, "OFF") || str_is(sn, "IDLE") || str_is(sn, "DOWN")) continue;
                p++; on += r; pw += r * p; if (gpuph && p <= g_nfreq) mhz += r * g_freq[p - 1];
            } else if (cltm ? !str_is(sn, "NO_CLTM") : !str_is(sn, "100%")) limited += r;
        }
        if (tot <= 0) continue;
        if (gpuph) {
            row(o, "gpu_on", 100 * on / tot); o->seen |= 1u << CH_GPUPH;
            if (on > 0) { row(o, "gpu_pstate", pw / on); if (p == g_nfreq) row(o, "gpu_mhz", mhz / on); }   // a table of another length would bias the mean: left out
            static int noted; if (!noted && p != g_nfreq && g_nfreq) { noted = 1;
                LOG("[sysprof] GPUPH has %d powered states and pmgr's frequency table %d entries: gpu_mhz is left out, gpu_pstate stays", p, g_nfreq); }
        } else if (boost) { if (on > 0) row(o, "gpu_boost", pw / on); o->seen |= 1u << CH_BOOST; }
        else if (cltm) { row(o, "gpu_cltm", 100 * limited / tot); o->seen |= 1u << CH_CLTM; }
        else { row(o, "gpu_droop", 100 * limited / tot); o->seen |= 1u << CH_DROOP; }
    }
}

static void *sampler(void *arg) {
    char *path = arg; pthread_setname_np("dm-sysprof"); pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);   // utility QoS lets macOS coalesce its timer: wakes 20-75 ms late on an M5 Pro
    out_t o = {.f = fopen(path, "w")};
    if (!o.f) { LOG("[sysprof] cannot open %s", path); free(path); return NULL; }
    fprintf(o.f, "t_s\tmetric\tvalue\n"); fflush(o.f);
    IOReportSubscriptionRef sub = NULL; CFMutableDictionaryRef sc = NULL; CFDictionaryRef prev = NULL; unsigned have = 0;
    if (load_ioreport()) {
        load_freqs();
        CFMutableDictionaryRef ch = ch_take(&have);
        if (ch) { sub = ir_subscribe(NULL, ch, &sc, 0, NULL); CFRelease(ch); }
    }
    char fr[160] = "", miss[160] = ""; int w = 0, m = 0;
    for (int i = 0; i < g_nfreq; i++) { char e[16]; snprintf(e, sizeof e, "%.0f", g_freq[i]); list_add(fr, sizeof fr, &w, " ", e); }
    for (int c = 0; c < CH_N; c++) if (!(have >> c & 1)) list_add(miss, sizeof miss, &m, ", ", k_ch[c].name);
    if (!sub) LOG("[sysprof] no IOReport subscription: only the process role, thermal state and Low Power Mode are written");   // a limit of this system, not a fault
    else if (m) LOG("[sysprof] IOReport has no %s channel: its metric is left out", miss);
    int role0 = task_role(), first = 1;
    LOG("[sysprof] every 100 ms into %s; GPU frequencies %s MHz; process role %s", path, g_nfreq ? fr : "unknown", role_name(role0));
    if (sub) prev = ir_samples(sub, sc, NULL);
    uint64_t tprev = now_ns(), next = tprev;   // the time of the counters' last reading: each delta covers [tprev, t]
    for (;;) { @autoreleasepool {
        next += 100000000ull; uint64_t now = now_ns();   // a fixed 100 ms grid: a wake up to a period late keeps it; a later one restarts it from now
        if (next > now) { struct timespec ts = {(time_t)((next - now) / 1000000000ull), (long)((next - now) % 1000000000ull)}; nanosleep(&ts, NULL); }
        else next = now;
        CFDictionaryRef cur = sub ? ir_samples(sub, sc, NULL) : NULL;
        uint64_t t = now_ns(); o.t = (t - g_t0_ns) / 1e9;   // stamped when the counters were read
        row(&o, "dt_ms", (t - tprev) / 1e6);
        if (sub) {
            if (cur && prev) { CFDictionaryRef d = ir_delta(prev, cur, NULL); if (d) { read_delta(d, (t - tprev) / 1e9, &o); CFRelease(d); } }
            if (prev) CFRelease(prev);
            prev = cur;
            if (first && prev) { first = 0; unsigned gone = have & ~o.seen; char b[160] = ""; int k = 0;
                for (int c = 0; c < CH_N; c++) if (gone >> c & 1) list_add(b, sizeof b, &k, ", ", k_ch[c].name);
                if (k) LOG("[sysprof] subscribed but gave no value in the first sample: %s", b); }
        }
        tprev = t;
        int role = task_role(); if (role >= 0) row(&o, "role", role);
        NSProcessInfo *pi = NSProcessInfo.processInfo; row(&o, "thermal", (double)pi.thermalState); row(&o, "low_power", pi.lowPowerModeEnabled);
        if (role != role0) { LOG("[sysprof] process role %s -> %s", role_name(role0), role_name(role)); role0 = role; }
        fflush(o.f);   // the host is usually ended, not exited: each sample is on disk once written
    } }
    return NULL;
}

void dm_sysprof_start(void) {
    static atomic_int once; if (atomic_exchange(&once, 1)) return;
    const char *p = getenv("DM_SYSPROF"); if (!p || !*p) return;   // an empty value (set but empty) is off
    char *path = strdup(p); pthread_t t;
    if (!path || pthread_create(&t, NULL, sampler, path)) { LOG("[sysprof] cannot start its thread"); free(path); return; }
    pthread_detach(t);
}
#else
void dm_sysprof_start(void) {}
#endif
