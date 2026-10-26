// Trinity timing on Pokemon Legends: Z-A 2.0.2. All addresses and original
// instructions are asserted offline by this build's OBS table in patches.py.
// Default: callbacks do nothing. DM_ZA_TIMING=1 enables clock diagnostics only.
// DM_ZA_FPS_MODE=static60 reproduces the original author's single-instruction
// experiment at main+0x1b296d8, without modifying guest instructions at runtime.
// DM_ZA_FPS_MODE=adaptive60 additionally selects the controller's measured code
// even when the native adaptive flag is off. This is an experiment, not a claim
// of correct speed: the base-clock/wall ratio below must establish that first.
// dynamic60 uses whole-frame active guest time and two verified duration data
// slots. It retains the native channel pause/zero handling and never edits code.
// Use DM_PRESENT_INTERVAL=game. Raising display frequency alone is insufficient.
// Reference: Fl4sh9174/Switch-Emulator-Ultrawide-FPS-Mods, this game's v2.0.2 pack.
#include "hle.h"
#include <mach/mach.h>
#include <pthread.h>
#include <limits.h>
#include <stdatomic.h>
#include "za_frame_delta.h"

enum { ZA_GAME, ZA_STATIC60, ZA_ADAPTIVE60, ZA_DYNAMIC60 };
static int g_za_diag, g_za_valid;
static _Atomic int g_za_mode, g_za_live_allowed;
static _Atomic uint64_t g_za_epoch;
static pthread_once_t g_za_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_za_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_za_interval_seen, g_za_warmup;
static za_frame_delta_state g_za_frame;
static za_frame_delta_result g_za_step;
static uint64_t g_za_exception_logs;
static uint64_t *g_za_table, g_za_table_owned[2];
static const uint64_t k_za_duration[5] = {16666667, 33333334, 50000000, 66666667, 83333334};
#define ZA_DURATION_OFFSET 0x3393e20ull
static struct {
    uint64_t calls, start_wall, start_guest, sum_ns, invalid, zero;
    uint64_t min_ns, max_ns, interval_calls, codes[6];
    uint64_t report_wall, report_guest, report_sum;
    int64_t last_delta, last_elapsed;
    int configured, adaptive, forwarded, selected;
} g_za;

// The original signed comparisons at main+0x1b29684..0x1b296d0. These codes
// are the native controller's values, not a guessed seconds-per-frame formula.
static int za_elapsed_code(int64_t ns) {
    if (ns > 66666667) return 4;
    if (ns > 50000000) return 3;
    if (ns > 33333334) return 2;
    return ns > 16666667;
}
static int za_read(uint64_t at, void *dst, size_t len) {
    vm_size_t got = 0;
    return at >= 0x1000 && vm_read_overwrite(mach_task_self(), (vm_address_t)at,
        len, (vm_address_t)dst, &got) == KERN_SUCCESS && got == len;
}
static const char *za_mode_name(int mode) {
    return mode == ZA_STATIC60 ? "static60" : mode == ZA_ADAPTIVE60 ? "adaptive60" : mode == ZA_DYNAMIC60 ? "dynamic60" : "game";
}
// The loader intentionally maps NSO ro/data as RW, with text separately RX.
// Never change a mapping's protection to make this optional experiment work.
static int za_table_rw_nonexec(const void *p, size_t len) {
    vm_address_t target = (vm_address_t)p, at = target;
    vm_size_t size = 0; natural_t depth = 0;
    for (int i = 0; i < 8; i++) {
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(mach_task_self(), &at, &size, &depth,
                (vm_region_recurse_info_t)&info, &count) != KERN_SUCCESS) return 0;
        if (info.is_submap) { depth++; continue; }
        return at <= target && size >= len && target - at <= size - len
            && (info.protection & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)) == (VM_PROT_READ | VM_PROT_WRITE);
    }
    return 0;
}
// All table writers hold g_za_mu. Aligned atomic words also prevent tearing for
// original guest LDR readers. No code/cave page or third duration slot is used.
static int za_table_arm(void) {
    module_t *m = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    const size_t bytes = sizeof k_za_duration;
    if (!m || !m->base || strcmp(m->build_id, "b1f12fd919eae86ab8a978317677e64b")
        || m->text_end != 0x3163f70 || m->ro_off != 0x3164000 || m->data_off != 0x3bc2000
        || m->size < ZA_DURATION_OFFSET + bytes || ZA_DURATION_OFFSET < m->ro_off
        || ZA_DURATION_OFFSET + bytes > m->data_off || dm_detclock_enabled()) {
        LOG("[RED] Z-A dynamic60: module layout/build or active-clock prerequisite failed; table unchanged"); return 0;
    }
    uint64_t *p = (uint64_t *)(m->base + ZA_DURATION_OFFSET), original[5];
    if (((uintptr_t)p & 7) || !za_table_rw_nonexec(p, bytes)
        || !za_read((uint64_t)(uintptr_t)p, original, bytes) || memcmp(original, k_za_duration, bytes)) {
        LOG("[RED] Z-A dynamic60: duration table is not original aligned RW/non-X data; table unchanged"); return 0;
    }
    g_za_table = p; memcpy(g_za_table_owned, k_za_duration, sizeof g_za_table_owned);
    za_frame_delta_reset(&g_za_frame); memset(&g_za_step, 0, sizeof g_za_step);
    g_za_exception_logs = 0;
    LOG("Z-A dynamic60: verified main+%#llx duration data; graphics code=0, clock code=0/1; active frame range=(0,50] ms, phase bound=2 ms; long active stalls are explicitly limited",
        ZA_DURATION_OFFSET);
    return 1;
}
static void za_table_restore(void) {
    if (!g_za_table) return;
    int conflicts = 0;
    // Restore only values still owned by us. Never overwrite another writer.
    // Restoring slot 0 first preserves the native table's monotonic order.
    for (int i = 0; i < 2; i++) {
        uint64_t expected = g_za_table_owned[i];
        if (!__atomic_compare_exchange_n(&g_za_table[i], &expected, k_za_duration[i], 0,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) conflicts++;
    }
    LOG("Z-A dynamic60: duration ownership released; restore_conflicts=%d", conflicts);
    g_za_table = NULL;
}
static int za_table_publish(uint64_t step, int code) {
    if (!g_za_table || code < 0 || code > 1 || !step || step > ZA_FRAME_MAX_NS) return 0;
    uint64_t desired[2] = {k_za_duration[0], k_za_duration[1]}; desired[code] = step;
    if (desired[0] > desired[1] || desired[1] > k_za_duration[2]) return 0;
    for (int i = 0; i < 5; i++) {
        uint64_t expected = i < 2 ? g_za_table_owned[i] : k_za_duration[i];
        if (__atomic_load_n(&g_za_table[i], __ATOMIC_ACQUIRE) != expected) return 0;
    }
    // The two aligned stores need not be transactional, but intermediate values
    // must remain ordered for original ratio/quality callers on other threads.
    int first = desired[0] > g_za_table_owned[1] ? 1 : 0;
    for (int n = 0; n < 2; n++) {
        int i = n ? 1 - first : first;
        uint64_t expected = g_za_table_owned[i];
        if (!__atomic_compare_exchange_n(&g_za_table[i], &expected, desired[i], 0,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
        g_za_table_owned[i] = desired[i];
    }
    return 1;
}
// Caller holds g_za_mu. The first new-mode interval must run before we discard
// two clock samples: one old-mode frame may already be in flight at the boundary.
// Reissuing the current mode also resets the epoch, useful after a pause/loading
// screen without hiding stalls automatically from the timing measurements.
static int za_reset_locked(int mode, const char *why) {
    int old = atomic_load_explicit(&g_za_mode, memory_order_acquire);
    if (mode == ZA_DYNAMIC60 && old != ZA_DYNAMIC60 && !za_table_arm()) return 0;
    if (old == ZA_DYNAMIC60 && mode != ZA_DYNAMIC60) za_table_restore();
    // A diagnostic epoch in the same dynamic mode must not reseed the actual
    // game controller. Dropping two observed samples cannot change engine dt.
    memset(&g_za, 0, sizeof g_za); g_za.min_ns = UINT64_MAX;
    g_za_interval_seen = 0; g_za_warmup = 2;
    atomic_store_explicit(&g_za_mode, mode, memory_order_release);
    uint64_t epoch = atomic_fetch_add_explicit(&g_za_epoch, 1, memory_order_acq_rel) + 1;
    LOG("Z-A timing: epoch=%llu mode=%s boundary=%s; counters reset, awaiting new-mode interval and two clock samples",
        (unsigned long long)epoch, za_mode_name(mode), why);
    return 1;
}
static void za_init(void) {
    const char *trace = getenv("DM_ZA_TIMING"), *mode = getenv("DM_ZA_FPS_MODE");
    int live_optin = trace && !strcmp(trace, "1");
    g_za_diag = trace && strcmp(trace, "0");
    int selected_mode = ZA_GAME;
    if (mode && *mode && strcmp(mode, "game")) {
        if (!strcmp(mode, "static60")) selected_mode = ZA_STATIC60;
        else if (!strcmp(mode, "adaptive60")) selected_mode = ZA_ADAPTIVE60;
        else if (!strcmp(mode, "dynamic60")) selected_mode = ZA_DYNAMIC60;
        else LOG("[RED] DM_ZA_FPS_MODE=%s: expected game, static60, adaptive60 or dynamic60; leaving timing unchanged", mode);
    }
    if (!g_za_diag && !selected_mode) return;
    if (strcmp(dm_main_build(), "b1f12fd919eae86ab8a978317677e64b")) {
        LOG("[RED] Z-A timing: unsupported main build; diagnostics and experiment disabled");
        return;
    }
    if (!gate_obs_present(OBS_ZA_CLOCK_DELTA) || !gate_obs_present(OBS_ZA_INTERVAL)) {
        LOG("[RED] Z-A timing: rewritten main lacks the required clock/interval observation gates");
        return;
    }
    g_za_valid = 1;
    pthread_mutex_lock(&g_za_mu);
    if (!za_reset_locked(selected_mode, "startup")) za_reset_locked(ZA_GAME, "dynamic prerequisites rejected");
    selected_mode = atomic_load(&g_za_mode);
    pthread_mutex_unlock(&g_za_mu);
    // Published only after the startup opt-in, exact build and both gates passed.
    // A DM_CTL change never initializes or arms timing on a previously off run.
    atomic_store_explicit(&g_za_live_allowed, live_optin, memory_order_release);
    LOG("Z-A timing: mode=%s, base-clock diagnostics enabled; original instructions are retained, no runtime code patch; live_switch=%d", za_mode_name(selected_mode), live_optin);
}

void dm_za_timing_env_changed(const char *key, const char *value) {
    if (!key || strcmp(key, "DM_ZA_FPS_MODE")) return;
    if (!atomic_load_explicit(&g_za_live_allowed, memory_order_acquire)) {
        LOG("Z-A timing: live mode change ignored; startup DM_ZA_TIMING=1 and validated gates are required");
        return;
    }
    int mode;
    if (value && !strcmp(value, "game")) mode = ZA_GAME;
    else if (value && !strcmp(value, "static60")) mode = ZA_STATIC60;
    else if (value && !strcmp(value, "adaptive60")) mode = ZA_ADAPTIVE60;
    else if (value && !strcmp(value, "dynamic60")) mode = ZA_DYNAMIC60;
    else { LOG("[RED] Z-A timing: invalid live mode %s; current epoch unchanged", value ?: "(null)"); return; }
    pthread_mutex_lock(&g_za_mu);
    za_reset_locked(mode, "DM_CTL");
    pthread_mutex_unlock(&g_za_mu);
}

// main+0x1b292dc: x0 is the returned clock record. The original ldr d0,[x0,#8]
// follows this callback, then delta_ns / 1e9 is passed to the engine as a float.
static void za_clock_delta(uint64_t *regs, int param) {
    (void)param;
    pthread_once(&g_za_once, za_init);
    if (!g_za_valid) return;
    uint64_t epoch = atomic_load_explicit(&g_za_epoch, memory_order_acquire);
    int64_t delta = 0;
    if (!za_read(regs[0] + 8, &delta, sizeof delta)) {
        static unsigned failures;
        if (failures++ < 8) LOG("[RED] Z-A timing: unreadable base-clock record %p", (void *)(uintptr_t)regs[0]);
        return;
    }
    uint64_t wall = now_ns(), guest = guest_ns();
    pthread_mutex_lock(&g_za_mu);
    if (epoch != atomic_load_explicit(&g_za_epoch, memory_order_acquire) || !g_za_interval_seen) { pthread_mutex_unlock(&g_za_mu); return; }
    int mode = atomic_load_explicit(&g_za_mode, memory_order_acquire);
    if (g_za_warmup) {
        if (!--g_za_warmup) LOG("Z-A timing: epoch=%llu mode=%s ready; next clock sample starts a fresh measurement",
            (unsigned long long)epoch, za_mode_name(mode));
        pthread_mutex_unlock(&g_za_mu); return;
    }
    uint64_t call = ++g_za.calls; g_za.last_delta = delta;
    if (!g_za.start_wall) { g_za.start_wall = wall; g_za.start_guest = guest; }
    else if (delta >= 0 && delta <= 1000000000ll) {
        g_za.sum_ns += (uint64_t)delta;
        if (!delta) g_za.zero++;
        if ((uint64_t)delta < g_za.min_ns) g_za.min_ns = (uint64_t)delta;
        if ((uint64_t)delta > g_za.max_ns) g_za.max_ns = (uint64_t)delta;
    } else g_za.invalid++;
    if (call <= 8 || !(call % 300)) {
        double real = (wall - g_za.start_wall) / 1e9, active = (guest - g_za.start_guest) / 1e9;
        double window = g_za.report_wall ? (wall - g_za.report_wall) / 1e9 : 0;
        double window_active = g_za.report_guest ? (guest - g_za.report_guest) / 1e9 : 0;
        double stepped = (g_za.sum_ns - g_za.report_sum) / 1e9;
        LOG("Z-A timing: epoch=%llu mode=%s frames=%llu delta=%.6f ms sum=%.6f s wall=%.6f s active=%.6f s ratio_wall=%.6f ratio_active=%.6f window=%.6f s window_ratio_wall=%.6f window_ratio_active=%.6f min/max=%.6f/%.6f ms zero=%llu invalid=%llu interval_calls=%llu configured=%d native_adaptive=%d elapsed=%.6f ms forwarded=%d selected=%d codes[0..4/other]=%llu/%llu/%llu/%llu/%llu/%llu",
            (unsigned long long)epoch, za_mode_name(mode), (unsigned long long)call, delta / 1e6, g_za.sum_ns / 1e9,
            real, active, real > 0 ? (g_za.sum_ns / 1e9) / real : 0,
            active > 0 ? (g_za.sum_ns / 1e9) / active : 0,
            window, window > 0 ? stepped / window : 0,
            window_active > 0 ? stepped / window_active : 0,
            g_za.min_ns == UINT64_MAX ? 0 : g_za.min_ns / 1e6, g_za.max_ns / 1e6,
            (unsigned long long)g_za.zero, (unsigned long long)g_za.invalid,
            (unsigned long long)g_za.interval_calls, g_za.configured, g_za.adaptive,
            g_za.last_elapsed / 1e6, g_za.forwarded, g_za.selected,
            (unsigned long long)g_za.codes[0], (unsigned long long)g_za.codes[1],
            (unsigned long long)g_za.codes[2], (unsigned long long)g_za.codes[3],
            (unsigned long long)g_za.codes[4], (unsigned long long)g_za.codes[5]);
        g_za.report_wall = wall; g_za.report_guest = guest; g_za.report_sum = g_za.sum_ns;
        if (mode == ZA_DYNAMIC60) LOG("Z-A dynamic60: epoch=%llu raw_frame=%.6f ms step=%.6f ms flags=%#x samples=%llu active_sum=%.6f s measured_issued=%.6f s phase=%lld ns limited=%.6f s seeds=%llu frozen=%llu resumed=%llu clock_resets=%llu; measured totals exclude explicit seed steps and are independent of diagnostic warmup",
            (unsigned long long)epoch, g_za_step.raw_ns / 1e6, g_za_step.step_ns / 1e6, g_za_step.flags,
            (unsigned long long)g_za_frame.samples, g_za_frame.raw_sum / 1e9, g_za_frame.step_sum / 1e9,
            (long long)g_za_frame.debt, g_za_frame.limited_ns / 1e9, (unsigned long long)g_za_frame.seeds,
            (unsigned long long)g_za_frame.frozen, (unsigned long long)g_za_frame.resumed,
            (unsigned long long)g_za_frame.resets);
    }
    pthread_mutex_unlock(&g_za_mu);
}

// main+0x1b296f0 is immediately after bl main+0x113488, at mov w1,w20.
// w20 already holds the configured code, w22 the measured/fixed candidate.
// Reproducing mov w20,#0 at the earlier load also requires w22=0 when the
// native adaptive flag is off; otherwise its original csel had copied w20.
static void za_interval(uint64_t *regs, int param) {
    (void)param;
    pthread_once(&g_za_once, za_init);
    if (!g_za_valid) return;
    uint64_t config = 0; uint8_t native[2]; int64_t elapsed = 0;
    if (!za_read(regs[19] + 0x40, &config, sizeof config)
        || !za_read(config + 0x20, native, sizeof native)
        || !za_read(regs[21] + 8, &elapsed, sizeof elapsed)) {
        static unsigned failures;
        if (failures++ < 8) LOG("[RED] Z-A timing: interval inputs unreadable; leaving saved registers unchanged");
        return;
    }
    pthread_mutex_lock(&g_za_mu);
    int mode = atomic_load_explicit(&g_za_mode, memory_order_acquire);
    if (mode == ZA_DYNAMIC60) {
        if (native[1] && elapsed > 1000000000ll) {
            // This guard runs after our observation point. Respect its native
            // code-4 behavior instead of silently supplying the wrong table.
            LOG("[RED] Z-A dynamic60: native long-work guard reached; returning to game timing");
            za_reset_locked(ZA_GAME, "native long-work guard");
        } else {
            g_za_step = za_frame_delta_advance(&g_za_frame, guest_ns(), now_ns());
            if (!(g_za_step.flags & ZA_FRAME_FROZEN)) {
                if (za_table_publish(g_za_step.step_ns, g_za_step.code)) {
                    regs[20] = 0; regs[22] = (uint32_t)g_za_step.code;
                } else {
                    LOG("[RED] Z-A dynamic60: duration ownership or bounds changed; returning to game timing");
                    za_reset_locked(ZA_GAME, "duration ownership failure");
                }
            }
            if ((g_za_step.flags & (ZA_FRAME_LIMITED | ZA_FRAME_CLOCK_RESET)) &&
                    (g_za_exception_logs++ < 8 || !(g_za_frame.samples % 300)))
                LOG("Z-A dynamic60: exceptional frame raw=%.6f ms step=%.6f ms flags=%#x lost_active=%.6f s",
                    g_za_step.raw_ns / 1e6, g_za_step.step_ns / 1e6, g_za_step.flags, g_za_frame.limited_ns / 1e9);
        }
    } else if (mode) {
        regs[20] = 0;
        if (mode == ZA_ADAPTIVE60) regs[22] = (uint32_t)za_elapsed_code(elapsed);
        else if (!native[1]) regs[22] = 0;
    }
    int forwarded = (int32_t)regs[20], candidate = (int32_t)regs[22];
    int selected = candidate > forwarded ? candidate : forwarded;
    // The original guard after the max also fixes code 4 above one second when
    // native adaptive mode is on. Normally the elapsed comparisons chose 4 first.
    if (native[1] && elapsed > 1000000000ll) selected = 4;
    g_za_interval_seen = 1;
    g_za.interval_calls++; g_za.configured = (int8_t)native[0]; g_za.adaptive = native[1] != 0;
    g_za.last_elapsed = elapsed; g_za.forwarded = forwarded; g_za.selected = selected;
    g_za.codes[selected >= 0 && selected <= 4 ? selected : 5]++;
    pthread_mutex_unlock(&g_za_mu);
}
// Observe the actual move layout, without changing guest registers or HID.
// Ordinary ZL, menus, and absent targets do not enable virtual skill layout.
static pthread_once_t g_skill_once = PTHREAD_ONCE_INIT;
static int g_skill_valid, g_skill_virtual_layout, g_skill_diag;
static uint64_t g_skill_vtable;
static _Atomic uint64_t g_skill_layout;
static _Atomic int g_skill_visible;
static void za_skill_init(void) {
    const char *layout = getenv("DM_ZA_SKILL_LAYOUT"), *diag = getenv("DM_ZA_SKILL_DIAG");
    g_skill_virtual_layout = layout && atoi(layout);
    g_skill_diag = diag && atoi(diag);
    if (!g_skill_virtual_layout && !g_skill_diag) return;
    for (int i = 0; i < g_nmod; i++) {
        module_t *m = &g_mod[i];
        if (!strcmp(m->name, "main") && !strcmp(m->build_id, "b1f12fd919eae86ab8a978317677e64b")
            && m->text_end == 0x3163f70 && m->data_off == 0x3bc2000) {
            g_skill_vtable = (uint64_t)(uintptr_t)m->base + 0x3dbdc88;
            g_skill_valid = 1;
        }
    }
    LOG("Z-A skill context: exact build verified=%d; virtual skill layout option=%d; normal face functions preserved",
        g_skill_valid, g_skill_virtual_layout);
}
int dm_za_skill_layout_active(void) {
    // Do not initialize from UIKit before the guest module metadata is ready.
    return atomic_load_explicit(&g_skill_visible, memory_order_acquire);
}
static void za_skill_visibility(uint64_t *regs, int param) {
    (void)param;
    pthread_once(&g_skill_once, za_skill_init);
    if (!g_skill_valid) return;
    uint64_t vtable;
    // x0 is the live object the original method is about to dereference.
    memcpy(&vtable, (const void *)(uintptr_t)regs[0], sizeof vtable);
    if (vtable != g_skill_vtable) return;
    int visible = regs[1] & 1;
    atomic_store_explicit(&g_skill_layout, regs[0], memory_order_relaxed);
    atomic_store_explicit(&g_skill_visible, visible && g_skill_virtual_layout, memory_order_release);
    static unsigned reports;
    if (g_skill_diag && reports++ < 128) LOG("Z-A skill context: layout=%#llx visible=%d active=%d",
        (unsigned long long)regs[0], visible, visible && g_skill_virtual_layout);
}
static void za_skill_destroy(uint64_t *regs, int param) {
    (void)param;
    if (regs[0] != atomic_load_explicit(&g_skill_layout, memory_order_relaxed)) return;
    atomic_store_explicit(&g_skill_visible, 0, memory_order_release);
    atomic_store_explicit(&g_skill_layout, 0, memory_order_relaxed);
    if (g_skill_diag) LOG("Z-A skill context: move layout destroyed; ordinary face functions active");
}
__attribute__((constructor)) static void za_timing_gates(void) {
    gate_obs_set(OBS_ZA_CLOCK_DELTA, za_clock_delta);
    gate_obs_set(OBS_ZA_INTERVAL, za_interval);
    gate_obs_set(OBS_ZA_SKILL_VISIBILITY, za_skill_visibility);
    gate_obs_set(OBS_ZA_SKILL_DESTROY, za_skill_destroy);
}
