#include <assert.h>
#include <stdio.h>
#include "../Sources/tv_remote.h"
int main(void) {
    TVRemoteState s = {0}; TVRemoteOutput o;
    s.directions = TV_REMOTE_UP|TV_REMOTE_RIGHT; s.pressed = DM_BTN_A;
    o=tv_remote_output(&s); assert(o.lx==32767 && o.ly==32767 && o.rx==0 && o.buttons==DM_BTN_A);
    s.directions |= TV_REMOTE_LEFT; o=tv_remote_output(&s); assert(o.lx==0 && o.ly==32767);
    s.held=DM_BTN_R; tv_remote_mode(&s, TV_REMOTE_CAMERA);
    o=tv_remote_output(&s); assert(o.buttons==DM_BTN_R && !o.lx && !o.ly && !o.rx && !o.ry);
    s.x=-20000; s.y=12000; s.pressed=DM_BTN_B; s.pulse=DM_BTN_X;
    o=tv_remote_output(&s); assert(o.rx==-20000 && o.ry==12000 && !o.lx && !o.ly);
    assert(o.buttons==(DM_BTN_R|DM_BTN_B|DM_BTN_X));
    tv_remote_mode(&s, TV_REMOTE_DPAD); s.x=-20000; s.y=12000;
    o=tv_remote_output(&s); assert(o.buttons==(DM_BTN_R|DM_BTN_LEFT|DM_BTN_UP));
    assert(!o.lx && !o.ly && !o.rx && !o.ry);
    tv_remote_cancel(&s, 1); o=tv_remote_output(&s);
    assert(!o.buttons && !o.lx && !o.ly && !o.rx && !o.ry);
    puts("Remote transitions, cancellation, simultaneous actions and stick/D-pad modes PASS");
}
