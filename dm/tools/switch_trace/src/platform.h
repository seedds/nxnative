// What nvn_trace.cpp needs from the mod loader and the SDK, in one place. Not yet built for a console: the names below are those of the usual code-injection mod frameworks for the
// Switch (an exlaunch-style module: a trampoline hook on an imported function, linked against the SDK's nn:: headers). Check each against the framework and SDK you
// build with; nothing else in nvn_trace.cpp depends on them.
#pragma once
#include <stdint.h>
#include <stddef.h>

// ---- hooking: one trampoline hook on the game's import of nvnBootstrapLoader ----
// st_hook_bootstrap installs `replacement` in place of the SDK's nvnBootstrapLoader and returns the original (callable) function.
typedef void *(*st_bootstrap_fn)(const char *name);
st_bootstrap_fn st_hook_bootstrap(st_bootstrap_fn replacement);

// ---- files on the SD card (nn::fs) ----
int st_fs_mount(void);                                       // mounts the SD card as "sd:" (nn::fs::MountSdCardForDebug("sd") in exlaunch-style mods); 0 = ok
int st_fs_exists(const char *path);                          // nn::fs::GetEntryType
int st_fs_read_small(const char *path, char *buf, size_t n); // whole small file into buf (NUL-terminated); bytes read, -1 if absent
int st_fs_append(const char *path, const void *data, size_t n);   // creates the file if needed and appends; 0 = ok
int st_fs_delete(const char *path);

// ---- input (optional): the trigger combo, nn::hid ----
// The buttons held now on the handheld controller or player 1's, as a mask of nn::hid::NpadButton bit numbers 0-15 (A B X Y LS RS L R ZL ZR Plus Minus Left Up Right Down).
// Called once a frame while no capture runs. Build with ST_NO_HID to leave it out (0: the combo never fires; the file and start_frame triggers still work).
uint32_t st_buttons(void);

// ---- time and log ----
uint64_t st_cpu_ticks(void);                   // nn::os::GetSystemTick(); only for the hook's own overhead figure
uint64_t st_cpu_tick_hz(void);                 // nn::os::GetSystemTickFrequency()
void st_log(const char *fmt, ...);             // the framework's log (or nothing)

#if defined(ST_EXLAUNCH) && defined(ST_MOCK)
#error "ST_EXLAUNCH and ST_MOCK are exclusive"
#endif
#ifdef ST_TITLE_ID
#define ST_LOG_PATH "sd:/switch-trace/" ST_TITLE_ID "/trace.log"
#else
#define ST_LOG_PATH "sd:/switch-trace/trace.log"
#endif
#ifdef ST_EXLAUNCH
// ---- an adapter for exlaunch-style modules (C++), compiled only with ST_EXLAUNCH ----
// Kept here so the core stays plain. Untested: adjust to the framework's current macro names.
#include "lib.hpp"
#include <nn/fs.h>
#include <nn/hid.h>
#include <nn/os.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static st_bootstrap_fn g_st_orig_bootstrap, g_st_repl_bootstrap;
extern "C" void *nvnBootstrapLoader(const char *name);
HOOK_DEFINE_TRAMPOLINE(StBootstrap) { static void *Callback(const char *name) { return g_st_repl_bootstrap(name); } };
static void *st_call_orig(const char *name) { return StBootstrap::Orig(name); }
st_bootstrap_fn st_hook_bootstrap(st_bootstrap_fn replacement) {
    g_st_repl_bootstrap = replacement; StBootstrap::InstallAtFuncPtr(nvnBootstrapLoader); g_st_orig_bootstrap = st_call_orig; return g_st_orig_bootstrap;
}
int st_fs_mount(void) { return nn::fs::MountSdCardForDebug("sd").IsSuccess() ? 0 : -1; }
int st_fs_exists(const char *path) { nn::fs::DirectoryEntryType t; return nn::fs::GetEntryType(&t, path).IsSuccess(); }
int st_fs_read_small(const char *path, char *buf, size_t n) {
    nn::fs::FileHandle h; if (!nn::fs::OpenFile(&h, path, nn::fs::OpenMode_Read).IsSuccess()) return -1;
    int64_t sz = 0; nn::fs::GetFileSize(&sz, h); if ((size_t)sz >= n) sz = (int64_t)n - 1;
    nn::fs::ReadFile(h, 0, buf, (size_t)sz); nn::fs::CloseFile(h); buf[sz] = 0; return (int)sz;
}
int st_fs_append(const char *path, const void *data, size_t n) {
    if (!st_fs_exists(path)) nn::fs::CreateFile(path, 0);
    nn::fs::FileHandle h; if (!nn::fs::OpenFile(&h, path, nn::fs::OpenMode_Write | nn::fs::OpenMode_AllowAppend).IsSuccess()) return -1;
    int64_t off = 0; nn::fs::GetFileSize(&off, h);
    bool ok = nn::fs::WriteFile(h, off, data, n, nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush)).IsSuccess();
    nn::fs::CloseFile(h); return ok ? 0 : -1;
}
int st_fs_delete(const char *path) { return nn::fs::DeleteFile(path).IsSuccess() ? 0 : -1; }
#ifndef ST_NO_HID
// Classic states first; with ST_LARK also the Switch 2 ones (OoT's sdk exports GetNpadState for NpadHandheldLarkState and NpadLarkState). Whether a Switch 2 in handheld
// reports through NpadHandheldState at all is unknown, and the Lark structs are in no public header: ST_LARK needs headers that declare them, with the button set's
// offset taken from the SDK headers or the game's own call site, not guessed. The sd:/switch-trace/go file and start_frame triggers work without any of this.
uint32_t st_buttons(void) {
    uint32_t m = 0;
    auto add = [&m](const auto &b) { for (int i = 0; i < 16; i++) if (b.Test(i)) m |= 1u << i; };
    { nn::hid::NpadHandheldState s = {}; nn::hid::GetNpadState(&s, nn::hid::NpadId::Handheld); add(s.mButtons); }
    { nn::hid::NpadFullKeyState s = {}; nn::hid::GetNpadState(&s, nn::hid::NpadId::No1); add(s.mButtons); }
    { nn::hid::NpadJoyDualState s = {}; nn::hid::GetNpadState(&s, nn::hid::NpadId::No1); add(s.mButtons); }
#ifdef ST_LARK
    { nn::hid::NpadHandheldLarkState s = {}; nn::hid::GetNpadState(&s, nn::hid::NpadId::Handheld); add(s.mButtons); }
    { nn::hid::NpadLarkState s = {}; nn::hid::GetNpadState(&s, nn::hid::NpadId::No1); add(s.mButtons); }
#endif
    return m;
}
#else
uint32_t st_buttons(void) { return 0; }
#endif
uint64_t st_cpu_ticks(void) { return nn::os::GetSystemTick().GetInt64Value(); }
uint64_t st_cpu_tick_hz(void) { return nn::os::GetSystemTickFrequency(); }
void st_log(const char *fmt, ...) { char b[512]; va_list v; va_start(v, fmt); vsnprintf(b, sizeof b, fmt, v); va_end(v); st_fs_append(ST_LOG_PATH, b, strlen(b)); st_fs_append(ST_LOG_PATH, "\n", 1); }
#endif
