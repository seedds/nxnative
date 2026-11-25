// HLE common: register host implementations by Switch mangled name.
// Note the differences between Apple AAPCS64 and Switch: Apple requires the caller to extend <32-bit arguments (bool/char/short), Switch code does not extend ⇒
// such parameters are always declared as int/unsigned and truncated by ourselves. Variadic arguments go on the stack on Apple and in registers on Switch ⇒ variadic functions cannot use C's "..." directly.
#pragma once
#include "dm.h"
#include "registration.h"
#include <fcntl.h>
#include <stddef.h>
#include <unistd.h>

// Read len bytes at off from the application's control.nacp (nn::ns::ApplicationControlProperty, 0x4000 bytes): DM_CONTROL_NACP, else control.nacp beside DM_ROMFS
// (the NSP script's extraction folder <version>/{RomFS,control.nacp}, the Mac app's Resources). Returns 0 on success, -1 if missing, -2 if too short.
static inline int dm_control_read(off_t off, void *out, size_t len, char *path_out, size_t path_cap) {
    char p[1100]; const char *e = getenv("DM_CONTROL_NACP");
    if (e && *e) snprintf(p, sizeof p, "%s", e);
    else {
        const char *rom = getenv("DM_ROMFS");
        if (rom) snprintf(p, sizeof p, "%s", rom);
        else snprintf(p, sizeof p, "%s%s/romfs", getenv("DM_ROOT") ?: (getenv("HOME") ?: ""), getenv("DM_ROOT") ? "" : "/dm-work");
        size_t k = strlen(p); while (k > 1 && p[k - 1] == '/') p[--k] = 0;
        char *sl = strrchr(p, '/'); if (sl) sl[1] = 0; else p[0] = 0;
        strlcat(p, "control.nacp", sizeof p);
    }
    if (path_out && path_cap) strlcpy(path_out, p, path_cap);
    int fd = open(p, O_RDONLY); ssize_t r = fd < 0 ? -1 : pread(fd, out, len, off);
    if (fd >= 0) close(fd);
    return r == (ssize_t)len ? 0 : (fd < 0 ? -1 : -2);
}
typedef struct { const char *name; void *fn; } hle_ent;
#define HLE(mangled, ret, cname, ...)                                                             \
    static ret cname(__VA_ARGS__);                                                                \
    DM_REGISTER("swhle", hle_ent, hle_ent_##cname, {mangled, (void *)cname}); \
    static ret cname(__VA_ARGS__)

typedef uint32_t Result;   // nn::Result: w0, 0 = success
#define RES_OK 0u
#define RES_FAIL 0x2A2u      // Generic failure (cut features such as networking uniformly return this)
enum { DM_APPLET_RESOURCE_USER_ID = 0x80 };   // identity of the game's applet in the single-process host

// Guest address → host object side table
void *objmap_get(const void *guest);
void objmap_put(const void *guest, void *host);
void *objmap_del(const void *guest);
uint64_t now_ns(void);
// A blocking native HLE wait releases the guest core. With the condition mutex
// held, enter with 1 before waiting and balance with 0 after reacquiring it.
void dm_thread_native_wait(int entering);
int dm_native_window_offscreen(const void *native);   // hle_misc.c: a native window no display shows (GameChat's virtual camera); hle_nvn.c drops the frames of its NVN windows
void dm_host_lock(pthread_mutex_t *m);  // contention on a host mutex releases the guest core
void dm_thread_service_connect(void);   // an HLE call that stands for connecting to a service on Horizon: threads started so far get to run (hle_thread.c)
void dm_thread_service_request(void);   // an HLE call that is a blocking service request on Horizon: the caller gives up its core (hle_thread.c)
uint64_t guest_ns(void);              // Guest clock (hle_os.c; does not advance while frozen, guest_clock_freeze)
int dm_detclock_enabled(void);
uint64_t dm_guest_frame(void);        // Incremented each time the host presents a frame; unaffected by machine load
void guest_clock_freeze(int on);
void dm_movie_active(int active);   // iOS movie decoder recovery waits for activation
void guest_clock_freeze_for(int reason, int on);   // Freeze by reason (0 tick hold, 1 background, 2 system keyboard, 3 NVN prepare, 4 NVN archive init); see hle_os.c
void guest_clock_read_wait(void);     // A guest read of the system counter: counts it in dm_clock_reads and waits while backgrounded or preparing (hle_os.c)
extern __thread uint64_t dm_clock_reads;   // the calling thread's guest reads of the system counter (hle_os.c; thr_yield tells clock waits by them)

// Forced takeover: even if the SDK module has a definition, all references to that symbol at load time (JUMP_SLOT/GLOB_DAT/ABS64) are resolved to the host implementation.
// Used only for the service-layer nn:: (service clients of fs/oe/aoc/pctl...); the SDK's libc++ and nn::os internals do not go into this table.
// Every entry must state the reason for the takeover; an entry in the table that no module references makes loading report a failure and abort (a wrong list must fail loudly).
// The implementation runs on this thread's big host stack (hle_bigstack.c), not on the stack of the game thread that calls it: a game thread's stack can be as small as 8 KB, and host
// code is written for host stacks (a path buffer sized by PATH_MAX, one log line's formatting, a file open, a WebKit or AudioUnit call). What is registered is a stub generated at
// compile time, local to its file as the implementation is (the implementation's address goes into x16, then a jump to dm_bigstack_enter); a call already off the guest stacks goes straight through. The register arguments
// and the first 64 bytes of stack arguments are carried over, so at most 16 parameters (8 in registers, 8 stack slots; a struct passed by value counts as the registers it takes);
// not for variadic functions. Each take-over moved here one by one after it crashed on a small stack (the file system on Super Mario 3D World's save thread, add-on content,
// AudioOut, the controller applet on Drag x Drive, Mario Kart 8 Deluxe's offline page) before this became the default (memory.md, Host calls on small guest stacks)
typedef struct { const char *name; void *fn; const char *why; const char *to; } hle_force_ent;   // to: a guest alias (HLE_FORCE_ALIAS below), fn NULL
#define HLE_NARG(...) HLE_NARG_(__VA_ARGS__, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define HLE_NARG_(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21, a22, a23, a24, n, ...) n
#define HLE_STR(x) HLE_STR_(x)
#define HLE_STR_(x) #x
// Past 8 parameters the stub enters at dm_bigstack_enter_n with x17 = the bytes of stack arguments the caller passes: 8 per parameter past the eighth, rounded up to 16 (hle_bigstack.c copies them to the host stack)
#define HLE_FORCE(mangled, why, ret, cname, ...)                                                   \
    _Static_assert(HLE_NARG(__VA_ARGS__) <= 16, #cname ": more parameters than the big-stack entry carries over (hle.h)"); \
    static ret cname(__VA_ARGS__) __attribute__((used));                                          \
    extern const char cname##_bs[];                                                               \
    __asm__(".text\n.p2align 2\n_" #cname "_bs:\n" \
            "    adrp x16, _" #cname "@PAGE\n    add x16, x16, _" #cname "@PAGEOFF\n"              \
            ".if " HLE_STR(HLE_NARG(__VA_ARGS__)) " > 8\n    mov x17, #(((" HLE_STR(HLE_NARG(__VA_ARGS__)) " - 8) * 8 + 15) & ~15)\n" \
            "    b _dm_bigstack_enter_n\n.else\n    b _dm_bigstack_enter\n.endif\n");           \
    DM_REGISTER("swforce", hle_force_ent, hle_force_##cname, {mangled, (void *)(uintptr_t)cname##_bs, why}); \
    static ret cname(__VA_ARGS__)
// Same as above, but the implementation runs on the caller's stack. Only for a take-over that runs the game's own code (an NRO's init and fini, a JIT plugin): that code's stack
// use is the game's, sized for the thread's own stack (which can be larger than the 512 KB host stack), and it may switch stacks itself (a coroutine), which would reuse the
// host stack the take-over is on. A short call into the SDK (fs::IsMounted, the audio renderer's command generation) is no reason. Say why beside it
#define HLE_FORCE_DIRECT(mangled, why, ret, cname, ...)                                            \
    static ret cname(__VA_ARGS__);                                                                \
    DM_REGISTER("swforce", hle_force_ent, hle_force_##cname, {mangled, (void *)cname, why}); \
    static ret cname(__VA_ARGS__)
// A guest alias: every reference to mangled binds to the loaded modules' own definition of target instead, as if the module had imported target. No host code runs and no
// signature is restated, so the alias holds for every SDK that defines both. For a hardware class whose constructor the host takes over to build the SDK's software class in the
// same memory: its members, called directly rather than through the vtable, go to the software class's members of the same signature (hle_codec.c). A target no loaded module
// defines leaves the reference bound as it would be without the entry, with a red line; a target that is itself taken over stops loading (force_init)
#define HLE_FORCE_ALIAS(mangled, target, why) HLE_FORCE_ALIAS_(mangled, target, why, __COUNTER__)
#define HLE_FORCE_ALIAS_(mangled, target, why, n) HLE_FORCE_ALIAS__(mangled, target, why, n)
#define HLE_FORCE_ALIAS__(mangled, target, why, n) \
    DM_REGISTER("swforce", hle_force_ent, hle_force_alias_##n, {mangled, NULL, why, target})
// Programs that do not link hle_bigstack.c (unit tests, whose stacks are host stacks) get this pass-through: the implementation runs on the caller's stack, as dm.h's weak dm_log
// writes there. The host's entry (hle_bigstack.c) replaces it at link time
__attribute__((weak, naked, visibility("hidden"))) void dm_bigstack_enter(void) { __asm__ volatile("br x16"); }
__attribute__((weak, naked, visibility("hidden"))) void dm_bigstack_enter_n(void) { __asm__ volatile("br x16"); }
// The game's call site inside a HLE_FORCE implementation: pass __builtin_return_address(0), which after a stack switch is the entry's own (hle_bigstack.c). The weak one is for the
// same programs as above
void *dm_bigstack_caller(void *ra);
__attribute__((weak, visibility("hidden"))) void *dm_bigstack_caller(void *ra) { return ra; }

// Host service sessions (hle_sf.c): a request on a session the host serves, as its service sees it, and the answer it fills in. The input is the raw CMIF data after the "SFCI"
// header; the buffers are the client's A (send), B (receive) and X (pointer) descriptors, read and written in place (one address space); the output is the raw data after "SFCO",
// zeroed beforehand. A service may copy one kernel handle to the client (an event's read end) and move one (a new session: an out object outside a domain)
typedef struct {
    unsigned cmd, object;                              // CMIF command; domain object (0 outside a domain)
    const uint8_t *in; size_t in_len;                  // raw input data
    uint32_t copy[4], move[4]; unsigned ncopy, nmove;  // handles the client sent
    struct { uint8_t *p; uint64_t size; } a[4], b[4], x[4]; unsigned na, nb, nx;
    uint8_t out[0xc0]; size_t out_len;                 // raw output data (zeroed; out_len is how much of it the service wrote)
    uint32_t out_copy, out_move;                       // handles for the client (0: none)
    void *ctx;                                         // the session's own state (sf_session)
} sf_req;
typedef Result (*sf_answer)(sf_req *r);
uint32_t sf_session(const char *name, sf_answer answer, void *ctx, void (*close)(void *ctx));   // a new host session (its handle), for a service to move to its client
// A service the host serves in its own file: SF_SERVICE("name", answer) registers it (the __DATA,swsf section), and connecting to that name gives a session answered by it
typedef struct { const char *name; sf_answer answer; } sf_service_ent;
#define SF_SERVICE(svc, fn) DM_REGISTER("swsf", sf_service_ent, sf_service_##fn, {svc, fn})
// Operation mode (0 handheld / 1 docked): owned by hle_oe.c, and the hid controller style follows it; the front end can switch it at runtime
int dm_opmode(void);
void dm_set_opmode(int m);          // On change, push notification 30 / 31 according to the switches the game has turned on
void dm_hid_opmode_changed(void);   // hle_hid.c: the npads follow the console mode; signals the style event of each npad whose style changed
// Focus (1 foreground / 2 focus lost / 3 background): called by the front end on lifecycle changes; a change pushes notification 15
void dm_focus_set(int st);
// Library applet stand-ins (controller, keyboard, error, Mii, account, shop): on Horizon the applet takes the application's focus until it closes, and the Show call returns after
// that. DM_APPLET("name"); first in the stand-in's body holds the game out of focus until the body returns, notifying 15 on the way in and out (hle_oe.c). Not for stand-ins that
// report the applet could not start (RES_FAIL cuts): those never had an applet open
void dm_applet_enter(const char *name);
void dm_applet_leave(void);
unsigned dm_account_pushed_users(void);   // hle_account.c: bitmask snapshotted by account::PushOpenUsers for oe::ExecuteProgram
static inline void dm_applet_leave_(const char *const *name) { (void)name; dm_applet_leave(); }
#define DM_APPLET(name) const char *const dm_applet_ __attribute__((cleanup(dm_applet_leave_))) = (name); dm_applet_enter(dm_applet_)
// Audio pause / resume following the lifecycle (called by the front end on going to background / returning to foreground)
extern uint8_t *dm_fps_state;   // frame step state page (loader.c install_fps_state): +0 float target fps, +4 movie flag (dm_movie_hz() != 0, written by the host at each present: Tears of the Kingdom's step stub caps its first, undecided steps by it), +0x10 float render scale, +0x18 double game seconds stepped; NULL when the build has none
extern int dm_fps_step_max;   // loader.c: the largest step the frame step stubs give the game (patch_nso.py ctx.step_max; 0 = FSTEP_MAX)
int dm_movie_hz(void);   // hle_movie.c: the picture rate of the movie playing (the fastest video decoder that handed out a picture in the last half second), 0 when none: the frame step follows it
void dm_movie_picture(const void *dec, const char *lib, int64_t ts_ns, int unknown_hz);   // hle_movie.c: video decoder dec (nn::movie's or nn::codec's) handed out a picture of this time; unknown_hz: its rate while the times give none
void dm_movie_stopped(const void *dec);   // hle_movie.c: dec was stopped or destroyed: its movie ends now
void dm_audio_suspend(int on);
uint64_t dm_audio_render_calls(void);   // Cumulative sound card callback count (the silent sink is not counted)
void dm_audio_reset(void);   // iOS media services reset: recreate the AudioUnits (hle_audio.c)
// nn::audio::AudioOutBuffer in the SDK's own layout, the audout service's buffer format (the same from SDK 8 to Switch 2's). The game fills and reads it through the SDK's own
// descriptor functions, which the host leaves to the sdk: SetAudioOutBufferInfo stores the data at +0x08, the buffer size at +0x10 and the data size at +0x18, the getters load the
// same offsets, and AudioBufferList links buffers through next (Hades's sdk+0x142ab0..0x142ae0). hle_audio.c's AudioOut queues and plays these; hle_audren.c hands it its rendered
// blocks in the same form. Until 2026-11 the host took three of those functions over to keep a private 24-byte layout, and the ones it left read it wrong (docs/reference/audio.md)
typedef struct { void *next; void *ptr; uint64_t cap, size, offset; } aout_buf;   // next, data, buffer_size, data_size, data_offset
_Static_assert(sizeof(aout_buf) == 40 && offsetof(aout_buf, ptr) == 0x08 && offsetof(aout_buf, cap) == 0x10 && offsetof(aout_buf, size) == 0x18 && offsetof(aout_buf, offset) == 0x20,
               "nn::audio::AudioOutBuffer layout");
void dm_audio_route_changed(void);   // the output route changed: update channel layout and watch for stalled callbacks (hle_audio.c)
int dm_audio_output_channels(void);   // the output route: 6 on a Mac device whose preferred speaker layout maps surround (5.0 / 5.1+) onto its outputs (iOS: six session channels), else 2 (hle_audio.c); reads the device on a host stack
int dm_audio_route(void);             // cached route, initialized on the calling host stack (hle_audio.c)
// In-process DeviceSink PCM: L R C LFE Ls Rs, followed by the stereo fold made before clamping/resampling.
// Both representations stay queued together; a device route change selects one without losing the original mix.
enum { DM_AUDIO_SINK_CHANNELS = 8 };
void *dm_pcmout_open_sink(void);
void dm_audio_device_changed(void);   // signal the game's bound output-device notifications (hle_audren.c)
