// The player's controls file (controls.h): the built-in keyboard map, pad lines (a button sent as others, back to the identity, rails, swap_sticks), key lines (clear, none,
// stick directions, Shake), the mouse lines, lines not understood, a later file changing only what it names, and the stick values of directions
#include "../controls.h"
#include <stdio.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
static int line(controls *c, const char *s) { char b[256]; snprintf(b, sizeof b, "%s", s); return ctl_line(c, b); }
static int nbad; static void bad(const char *p, int n, const char *t) { (void)p; (void)n; (void)t; nbad++; }

int main(void) {
    controls c; ctl_defaults(&c);
    CHECK(c.key[38].btn == DM_BTN_A && c.key[13].dirs == CTL_LUP && c.key[49].btn == DM_BTN_SHAKE, "built in: J is A, W the left stick up, Space shakes");
    CHECK(ctl_key_is_default(&c, 123), "built in: the left arrow is the D-pad");
    CHECK(ctl_pad_map(&c, DM_BTN_A | DM_BTN_SHAKE) == (DM_BTN_A | DM_BTN_SHAKE), "no pad lines: buttons pass");

    CHECK(line(&c, "pad A=B") && line(&c, "pad B = A"), "pad: A and B swapped");
    CHECK(ctl_pad_map(&c, DM_BTN_A) == DM_BTN_B && ctl_pad_map(&c, DM_BTN_B) == DM_BTN_A, "A sends B, B sends A");
    CHECK(ctl_pad_map(&c, DM_BTN_A | DM_BTN_X | DM_BTN_SHAKE) == (DM_BTN_B | DM_BTN_X | DM_BTN_SHAKE), "the others and Shake pass");
    CHECK(line(&c, "pad ZR=ZR+R") && ctl_pad_map(&c, DM_BTN_ZR) == (DM_BTN_ZR | DM_BTN_R), "one button sends two");
    CHECK(line(&c, "pad minus=none") && ctl_pad_map(&c, DM_BTN_MINUS) == 0, "none: the button does nothing");
    CHECK(line(&c, "pad LSL=L") && ctl_pad_map(&c, DM_BTN_LSL) == DM_BTN_L, "a rail button");
    CHECK(line(&c, "pad A=A") && line(&c, "pad B=B") && line(&c, "pad ZR=ZR") && line(&c, "pad Minus=Minus") && line(&c, "pad LSL=LSL") && !c.pad_mapped, "back to the identity");
    CHECK(!line(&c, "pad A=LUp") && !line(&c, "pad Shake=A") && !line(&c, "pad Q=A") && !line(&c, "pad A=") && !line(&c, "pad A=B+"), "pad: a direction, Shake, an unknown name, nothing");
    CHECK(line(&c, "pad swap_sticks=1") && c.swap_sticks, "swap_sticks");

    CHECK(line(&c, "key 38=B") && c.key[38].btn == DM_BTN_B, "J sends B");
    CHECK(line(&c, "key 123=RLeft") && c.key[123].dirs == CTL_RLEFT && !ctl_key_is_default(&c, 123), "the left arrow is the right stick: no longer built in");
    CHECK(line(&c, "key 49=none") && !c.key[49].btn && !c.key[49].dirs, "Space unbound");
    CHECK(line(&c, "key 18=A+Shake") && c.key[18].btn == (DM_BTN_A | DM_BTN_SHAKE), "two actions");
    CHECK(!line(&c, "key 128=A") && !line(&c, "key x=A") && !line(&c, "key 5=Jump"), "key: past 127, not a number, an unknown action");
    CHECK(line(&c, "key clear") && !c.key[13].dirs && !c.key[18].btn, "key clear");
    CHECK(line(&c, "  # a comment") && line(&c, "") && line(&c, "key 0=A # J"), "comments and empty lines");

    CHECK(line(&c, "mouse=right") && c.mouse == CTL_MOUSE_RIGHT && line(&c, "mouse = left") && c.mouse == CTL_MOUSE_LEFT && line(&c, "mouse=off") && !c.mouse, "mouse");
    CHECK(!line(&c, "mouse=up"), "mouse: an unknown stick");
    CHECK(line(&c, "mouse_speed=2.5") && c.mouse_speed == 2.5f && !line(&c, "mouse_speed=0") && !line(&c, "mouse_speed=50"), "mouse_speed and its range");
    CHECK(line(&c, "mouse_invert_y=1") && c.mouse_invert_y, "mouse_invert_y");
    CHECK(line(&c, "mouse_button right=L+ZL") && c.mouse_btn[1].btn == (DM_BTN_L | DM_BTN_ZL) && !line(&c, "mouse_button side=A"), "mouse_button");
    CHECK(!line(&c, "joystick A=B") && !line(&c, "pad"), "an unknown word, a word alone");

    // Two files: the game's, read after the global one, changes only what it names
    char g[] = "/tmp/t_controls_gXXXXXX", p[] = "/tmp/t_controls_pXXXXXX";
    int fg = mkstemp(g), fp = mkstemp(p); FILE *f;
    f = fdopen(fg, "w"); fputs("pad A=B\nkey 38=X\nmouse=right\nbogus line\n", f); fclose(f);
    f = fdopen(fp, "w"); fputs("key 38=Y\r\nmouse_speed=3\n", f); fclose(f);
    ctl_defaults(&c); nbad = 0;
    CHECK(ctl_load(&c, g, bad) == 1 && nbad == 1, "the global file: one line not understood");
    CHECK(ctl_load(&c, p, bad) == 0, "the game's file");
    CHECK(c.key[38].btn == DM_BTN_Y && ctl_pad_map(&c, DM_BTN_A) == DM_BTN_B && c.mouse == CTL_MOUSE_RIGHT && c.mouse_speed == 3.f, "later lines win; the rest stays");
    CHECK(ctl_load(&c, "/nonexistent/controls.txt", bad) == -1, "a missing file");
    unlink(g); unlink(p);

    int32_t lx, ly, rx, ry; ctl_dirs_sticks(CTL_LUP | CTL_LLEFT | CTL_RDOWN, &lx, &ly, &rx, &ry);
    CHECK(lx == -DM_STICK_MAX && ly == DM_STICK_MAX && rx == 0 && ry == -DM_STICK_MAX, "directions to sticks");
    ctl_dirs_sticks(CTL_LLEFT | CTL_LRIGHT, &lx, &ly, &rx, &ry);
    CHECK(lx == 0, "opposite directions cancel");

    printf("t_controls: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
