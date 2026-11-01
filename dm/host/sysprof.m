// System profile for the per-frame timing rows (DM_NVN_TSV, Mac): what the system did with the game's GPU and process while the frames ran, so one session shows why a frame rate
// fell. A thread samples every 100 ms (dm_sysprof_start) and hle_nvn.c writes the latest sample into each frame's rows (@gpu_mhz and the rest, dm_sysprof_get):
//   GPU clock and powered time: IOReport "GPU Stats" / "GPU Performance States" (GPUPH): residency of each performance state over the interval; the clock is the mean frequency of the
//     powered states (pmgr's voltage-states9 table), the powered share is the time not OFF. Powered is not busy: the GPU stays powered between closely spaced submissions
//   GPU power: IOReport "Energy Model" / "GPU Energy" over the interval
//   What chose the clock: the boost controller's requested state (BSTGPUPH, mean P index), the share of time a thermal limit applied (GPU_CLTM other than NO_CLTM) and a voltage
//     droop limit applied (GPUDVDH other than 100%)
//   The process's role (task_policy_get TASK_CATEGORY_POLICY: 1 foreground app, 2 background app, ...) and the system's thermal state and Low Power Mode
// IOReport is a private library the system tools use (powermetrics, Activity Monitor); it needs no root. It is loaded with dlopen, so a system without it only leaves the GPU columns out.
// A change of role is logged as it happens: macOS sets the performance controller's targets by it
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#include "sysprof.h"
#if TARGET_OS_OSX
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
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

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static dm_sysprof g_last = {.gpu_mhz = -1, .gpu_on = -1, .gpu_w = -1, .boost = -1, .cltm = -1, .droop = -1, .role = -1, .thermal = -1, .low_power = -1};
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

static void load_freqs(void) {   // the GPU's frequencies, lowest first: pmgr's voltage-states9, (frequency, voltage) pairs of 32-bit words
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceNameMatching("pmgr"), &it)) return;
    for (io_object_t o; (o = IOIteratorNext(it));) {
        CFDataRef d = IORegistryEntryCreateCFProperty(o, CFSTR("voltage-states9"), NULL, 0);
        if (d) {
            const uint32_t *p = (const uint32_t *)CFDataGetBytePtr(d); int n = (int)(CFDataGetLength(d) / 8);
            for (int i = 0; i < n && g_nfreq < 32; i++) if (p[2 * i]) g_freq[g_nfreq++] = p[2 * i] / (p[2 * i] > 100000000u ? 1e6 : 1e3);   // Hz on newer chips, kHz on older ones
            CFRelease(d);
        }
        IOObjectRelease(o);
        if (g_nfreq) break;
    }
    IOObjectRelease(it);
}

static int str_is(CFStringRef s, const char *c) { char b[64] = ""; if (s) CFStringGetCString(s, b, sizeof b, kCFStringEncodingUTF8); return !strcmp(b, c); }

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

// One interval's channels: the GPU's state residencies and its energy
static void read_delta(CFDictionaryRef d, double secs, dm_sysprof *s) {
    CFArrayRef a = CFDictionaryGetValue(d, CFSTR("IOReportChannels"));
    for (CFIndex i = 0; a && i < CFArrayGetCount(a); i++) {
        CFDictionaryRef c = CFArrayGetValueAtIndex(a, i); CFStringRef name = ir_name(c);
        if (ir_format(c) == 1) {   // a simple value: energy since the previous sample
            if (!str_is(name, "GPU Energy")) continue;
            double v = (double)ir_value(c, 0), scale = str_is(ir_unit(c), "nJ") ? 1e-9 : str_is(ir_unit(c), "uJ") ? 1e-6 : str_is(ir_unit(c), "mJ") ? 1e-3 : 0;
            if (scale && secs > 0) s->gpu_w = v * scale / secs;
            continue;
        }
        int n = ir_nstates(c); if (n <= 0) continue;
        double tot = 0, on = 0, mhz = 0, pw = 0, limited = 0; int p = 0;
        int gpuph = str_is(name, "GPUPH"), boost = str_is(name, "BSTGPUPH"), cltm = str_is(name, "GPU_CLTM"), droop = str_is(name, "GPUDVDH");
        if (!gpuph && !boost && !cltm && !droop) continue;
        for (int k = 0; k < n; k++) {
            double r = (double)ir_residency(c, k); CFStringRef sn = ir_state_name(c, k); tot += r;
            if (gpuph || boost) {
                if (str_is(sn, "OFF") || str_is(sn, "IDLE") || str_is(sn, "DOWN")) continue;
                on += r; pw += r * ++p; if (gpuph && p - 1 < g_nfreq) mhz += r * g_freq[p - 1];
            } else if (cltm ? !str_is(sn, "NO_CLTM") : !str_is(sn, "100%")) limited += r;
        }
        if (tot <= 0) continue;
        if (gpuph) { s->gpu_on = 100 * on / tot; s->gpu_mhz = on > 0 ? mhz / on : 0; }
        else if (boost) s->boost = on > 0 ? pw / on : 0;
        else if (cltm) s->cltm = 100 * limited / tot;
        else s->droop = 100 * limited / tot;
    }
}

static void *sampler(void *arg) {
    (void)arg; pthread_setname_np("dm-sysprof"); pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    IOReportSubscriptionRef sub = NULL; CFMutableDictionaryRef sc = NULL; CFDictionaryRef prev = NULL; uint64_t tprev = 0;
    if (load_ioreport()) {
        load_freqs();
        CFMutableDictionaryRef ch = ir_channels(CFSTR("GPU Stats"), NULL, 0, 0, 0), em = ir_channels(CFSTR("Energy Model"), NULL, 0, 0, 0);
        if (ch && em) ir_merge(ch, em, NULL);
        if (ch) sub = ir_subscribe(NULL, ch, &sc, 0, NULL);
        if (sub) { prev = ir_samples(sub, sc, NULL); tprev = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
    }
    char fr[160] = ""; int w = 0;
    for (int i = 0; i < g_nfreq && w < (int)sizeof fr - 8; i++) w += snprintf(fr + w, sizeof fr - w, "%s%.0f", i ? " " : "", g_freq[i]);
    if (!sub) LOG("[RED] [sysprof] no IOReport subscription: the GPU clock, power and limit columns are left out of the frame rows");
    LOG("[sysprof] every 100 ms into the frame rows: %s; GPU frequencies %s MHz; process role %s", sub ? "GPU clock, powered time, power, boost state and limits (IOReport)" : "process role only",
        g_nfreq ? fr : "unknown", role_name(task_role()));
    int role0 = task_role();
    for (;;) {
        usleep(100000);
        dm_sysprof s = {.gpu_mhz = -1, .gpu_on = -1, .gpu_w = -1, .boost = -1, .cltm = -1, .droop = -1};
        if (sub) {
            CFDictionaryRef cur = ir_samples(sub, sc, NULL); uint64_t t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            if (cur && prev) { CFDictionaryRef d = ir_delta(prev, cur, NULL); if (d) { read_delta(d, (t - tprev) / 1e9, &s); CFRelease(d); } }
            if (prev) CFRelease(prev);
            prev = cur; tprev = t;
        }
        s.role = task_role();
        NSProcessInfo *pi = NSProcessInfo.processInfo; s.thermal = (int)pi.thermalState; s.low_power = pi.lowPowerModeEnabled;
        if (s.role != role0) { LOG("[sysprof] process role %s → %s", role_name(role0), role_name(s.role)); role0 = s.role; }
        pthread_mutex_lock(&g_mu); g_last = s; pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}

static void start_once(void) { pthread_t t; if (!pthread_create(&t, NULL, sampler, NULL)) pthread_detach(t); }
void dm_sysprof_start(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, start_once); }
int dm_sysprof_get(dm_sysprof *out) { pthread_mutex_lock(&g_mu); *out = g_last; pthread_mutex_unlock(&g_mu); return out->role >= 0; }
#else
void dm_sysprof_start(void) {}
int dm_sysprof_get(dm_sysprof *out) { (void)out; return 0; }
#endif
