// Remote state is independent of UIKit so cancellation and mode transitions
// can be checked without running the guest or selecting a menu item.
#ifndef DM_TV_REMOTE_H
#define DM_TV_REMOTE_H
#include <stdint.h>
#include "dm_input.h"
enum { TV_REMOTE_MOVE, TV_REMOTE_CAMERA, TV_REMOTE_DPAD };
enum { TV_REMOTE_LEFT=1, TV_REMOTE_RIGHT=2, TV_REMOTE_UP=4, TV_REMOTE_DOWN=8 };
typedef struct {
    int mode, directions;
    int16_t x, y;
    uint64_t pressed, held, pulse;
} TVRemoteState;
typedef struct { uint64_t buttons; int16_t lx, ly, rx, ry; } TVRemoteOutput;
static inline void tv_remote_cancel(TVRemoteState *s, int release_held) {
    s->directions = 0; s->x = s->y = 0; s->pressed = s->pulse = 0;
    if (release_held) s->held = 0;
}
static inline void tv_remote_mode(TVRemoteState *s, int mode) {
    tv_remote_cancel(s, 0); s->mode = mode;
}
static inline TVRemoteOutput tv_remote_output(const TVRemoteState *s) {
    TVRemoteOutput o = { .buttons = s->pressed | s->held | s->pulse };
    int16_t x = s->directions & (TV_REMOTE_LEFT|TV_REMOTE_RIGHT)
        ? ((!!(s->directions & TV_REMOTE_RIGHT) - !!(s->directions & TV_REMOTE_LEFT)) * 32767) : s->x;
    int16_t y = s->directions & (TV_REMOTE_UP|TV_REMOTE_DOWN)
        ? ((!!(s->directions & TV_REMOTE_UP) - !!(s->directions & TV_REMOTE_DOWN)) * 32767) : s->y;
    if (s->mode == TV_REMOTE_MOVE) { o.lx=x; o.ly=y; }
    else if (s->mode == TV_REMOTE_CAMERA) { o.rx=x; o.ry=y; }
    else {
        if (x < -8192) o.buttons |= DM_BTN_LEFT;
        if (x > 8192) o.buttons |= DM_BTN_RIGHT;
        if (y < -8192) o.buttons |= DM_BTN_DOWN;
        if (y > 8192) o.buttons |= DM_BTN_UP;
    }
    return o;
}
#endif
