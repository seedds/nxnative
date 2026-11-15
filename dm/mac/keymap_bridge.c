// The keyboard map's model (keymap.h) for the settings window: Launcher.swift imports no C header, so it calls these by @_silgen_name. Its own file, linked beside
// launcher.c by pack_app.py, as save_sync.c is, so a test that builds the settings window without launcher.c's main (test_launcher_storage) links it too
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "keymap.h"

// keymap.h for the settings window (Launcher.swift imports no C header: @_silgen_name): the targets, the bindings' labels, and a map read and written as KM_SLOTS targets by binding
int dm_km_groups(void) { return KM_NGROUPS; }   // the sheet's groups, as the panel's and the keymap files' (km_groups)
const char *dm_km_group_title(int g) { return g >= 0 && g < KM_NGROUPS ? km_groups[g].title : ""; }
int dm_km_group_size(int g) { return g >= 0 && g < KM_NGROUPS ? km_groups[g].n : 0; }
int dm_km_group_target(int g, int i) { return g >= 0 && g < KM_NGROUPS && i >= 0 && i < km_groups[g].n ? km_groups[g].targets[i] : -1; }
int dm_km_slot(int code, int mods) { return km_layer(mods) * KM_KEYS + code; }   // a key pressed with mods (KM_MOD_*: 1 Shift, 2 Control, 4 Option) as a binding, as the panel binds it
const char *dm_km_target_label(int t) { return t >= 0 && t < KM_N ? km_targets[t].label : ""; }
int dm_km_slots(void) { return KM_SLOTS; }
int dm_km_keys(void) { return KM_KEYS; }
const char *dm_km_slot_label(int slot) { static char b[64]; return km_slot_label(slot, b, sizeof b); }
int dm_km_bindable(int slot) { return km_slot_bindable(slot); }
int dm_km_read(const char *map, uint8_t *key) { keymap m; int bad = km_parse(&m, map ? map : "", NULL, 0); memcpy(key, m.key, KM_SLOTS); return bad; }   // key: KM_SLOTS bytes
int dm_km_write(const uint8_t *key, char *out, size_t n) { keymap m; memcpy(m.key, key, KM_SLOTS); return km_format(&m, out, n); }   // the length: n or more, cut
int dm_km_format_max(void) { return KM_FORMAT_MAX; }
int dm_km_bind(uint8_t *key, int target, int slot, int add) {   // the binding presses target (slot -1: target loses all; add: it keeps its others); the target the binding had, -1 none
    keymap m; memcpy(m.key, key, KM_SLOTS); int was = slot >= 0 && slot < KM_SLOTS && m.key[slot] != KM_NONE && m.key[slot] != target ? m.key[slot] : -1;
    if (!add || slot < 0) km_bind(&m, target, -1);
    if (slot >= 0) km_bind(&m, target, slot);
    memcpy(key, m.key, KM_SLOTS); return was;
}
static km_preset g_presets[KM_MAX_PRESETS]; static int g_npresets;   // Default and the keymap files (keymap.h), as the last dm_km_presets_load found them
int dm_km_presets_load(void) {
    char err[2048]; g_npresets = km_presets_load(g_presets, KM_MAX_PRESETS, err, sizeof err);
    if (*err) fprintf(stderr, "[RED] launcher: keymap files: lines skipped: %s\n", err);
    return g_npresets;
}
const char *dm_km_preset_name(int i) { return i >= 0 && i < g_npresets ? g_presets[i].name : ""; }
const char *dm_km_preset_map(int i) { return i >= 0 && i < g_npresets ? g_presets[i].map : ""; }
int dm_km_preset_save(const uint8_t *key, const char *name, char *path, size_t n) { keymap m; memcpy(m.key, key, KM_SLOTS); return km_preset_save(&m, name, path, n); }
const char *dm_km_preset_dir(void) { static char d[1024]; return km_preset_dir(d, sizeof d) ? d : ""; }
