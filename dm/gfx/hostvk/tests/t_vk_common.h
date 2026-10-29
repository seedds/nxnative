// What the hostvk tests share: the check counter, the front end's stub, and the rule that decides between running, skipping and failing when there is no Vulkan.
//   - No loader or no physical device: "note: no Vulkan device: skipped" and exit 0 (the Windows CI runner has no Vulkan driver), unless VK_ICD_FILENAMES, VK_DRIVER_FILES or
//     VK_LOADER_DRIVERS_SELECT names a driver: then the driver was chosen on purpose (dm/host/CMakeLists.txt sets lavapipe for CTest on Linux) and its absence is a failure.
//   - Validation: always asked for (hostvk_opts.validation); when VK_INSTANCE_LAYERS names VK_LAYER_KHRONOS_validation (CTest on Linux) it must be on, else a missing layer is
//     only a note. Every validation-layer warning or error fails the test (hostvk_dev.val_errors / val_warnings, counted by the debug-utils messenger)
#pragma once
#include "vk_device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail, total;
static void t(const char *name, int ok) {
    total++; fail += !ok;
    printf("%-96s %s\n", name, ok ? "\xE2\x9C\x93" : "\xE2\x9C\x97");
}
static int finish(const char *test) {
    printf("== %s: %d checks, %d failed ==\n", test, total, fail);
    return fail != 0;
}

// front.h's front_vk_instance_extensions (front_sdl.c in the host): what a window would ask for. The tests set it; 0 extensions = no Vulkan window
static const char *g_front_exts[4];
static unsigned g_front_n;
int front_vk_instance_extensions(const char **names, unsigned *n) {
    unsigned k = 0;
    if (names && n) for (; k < *n && k < g_front_n; k++) names[k] = g_front_exts[k];
    if (n) *n = k;
    return (int)g_front_n;
}

static int env_set(const char *v) { const char *e = getenv(v); return e && *e; }
static int vk_required(void) { return env_set("VK_ICD_FILENAMES") || env_set("VK_DRIVER_FILES") || env_set("VK_LOADER_DRIVERS_SELECT"); }
static int validation_required(void) { const char *e = getenv("VK_INSTANCE_LAYERS"); return e && strstr(e, "VK_LAYER_KHRONOS_validation"); }

// hostvk_init with o; on no Vulkan at all, skip (exit 0) or fail as above. Returns hostvk_init's result
static int init_or_skip(const char *test, const hostvk_opts *o, hostvk_dev *d, char *err, size_t n) {
    int rc = hostvk_init(o, d, err, n);
    if (rc == HOSTVK_E_NO_VULKAN || rc == HOSTVK_E_NO_DEVICE) {
        if (!vk_required()) {
            printf("note: no Vulkan device: skipped (%s)\n", err);
            printf("== %s: 0 checks, 0 failed (skipped: no Vulkan device) ==\n", test);
            exit(0);
        }
        printf("VK_ICD_FILENAMES / VK_DRIVER_FILES / VK_LOADER_DRIVERS_SELECT names a driver, but hostvk_init says: %s\n", err);
    }
    return rc;
}
static void check_validation(const hostvk_dev *d, const char *when) {
    char name[160];
    if (!d->validation) {
        snprintf(name, sizeof name, "validation layer on (%s)", when);
        if (validation_required()) t(name, 0);
        else printf("note: VK_LAYER_KHRONOS_validation is not installed: validation not checked (%s)\n", when);
        return;
    }
    snprintf(name, sizeof name, "no validation message (%s): %u errors, %u warnings", when, (unsigned)d->val_errors, (unsigned)d->val_warnings);
    t(name, d->val_errors == 0 && d->val_warnings == 0);
}
