// The Mac keyboard map (keymap.h): the default map, DM_KEYMAP over it (keys moved, a target left with none, errors skipped), the full form the settings window saves read back
// as the same map, chords (Shift + arrows the right stick by default, any modifier either side, a plain key's own binding while no chord is bound), several keys on
// one target, a key released as it went down, repeats
#include "../keymap.h"
#include <unistd.h>
#include <stdio.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

static uint64_t press(const keymap *m, km_state *st, const char *key, int mods) { km_key(m, st, km_key_code(key), 1, mods); return km_buttons(st); }
static uint64_t lift(const keymap *m, km_state *st, const char *key) { km_key(m, st, km_key_code(key), 0, 0); return km_buttons(st); }

int main(void) {
    keymap m; km_state st = {0}; int32_t lx, ly, rx, ry; char err[256], out[1024];

    km_default(&m);
    CHECK(m.key[km_key_code("j")] == KM_A && m.key[km_key_code("k")] == KM_B && m.key[km_key_code("z")] == KM_ZL && m.key[km_key_code("space")] == KM_SHAKE, "default: J=A K=B Z=ZL Space=shake");
    CHECK(m.key[13] == KM_LUP && m.key[123] == KM_DLEFT && m.key[36] == KM_PLUS && m.key[48] == KM_MINUS, "default: W left stick up, ← D-pad left, Return +, Tab −");
    CHECK(m.key[KM_SHIFT * KM_KEYS + 126] == KM_RUP && m.key[KM_SHIFT * KM_KEYS + 123] == KM_RLEFT && km_key_code("shift+up") == KM_SHIFT * KM_KEYS + 126, "default: Shift+↑ right stick up, Shift+← left");
    CHECK(press(&m, &st, "j", 0) == 1u, "J presses A");
    CHECK(lift(&m, &st, "j") == 0, "and lets it go");
    press(&m, &st, "w", 0); press(&m, &st, "d", 0); km_sticks(&st, &lx, &ly, &rx, &ry);
    CHECK(lx == 32767 && ly == 32767 && !rx && !ry, "W+D: the left stick up and right");
    press(&m, &st, "a", 0); km_sticks(&st, &lx, &ly, &rx, &ry);
    CHECK(lx == 0, "A+D cancel");
    km_release_all(&st); km_sticks(&st, &lx, &ly, &rx, &ry);
    CHECK(!lx && !ly && !km_buttons(&st), "release all: nothing held");

    CHECK(press(&m, &st, "left", KM_MOD_SHIFT) == 0, "Shift+←: not the D-pad");
    km_sticks(&st, &lx, &ly, &rx, &ry); CHECK(rx == -32767, "Shift+←: the right stick left");
    lift(&m, &st, "left"); km_sticks(&st, &lx, &ly, &rx, &ry); CHECK(rx == 0, "← up without Shift: the stick it pressed goes back");
    press(&m, &st, "up", 0); CHECK(km_buttons(&st) == 1u << KM_DUP, "↑ alone: D-pad up");
    lift(&m, &st, "up");
    CHECK(press(&m, &st, "j", 0) == 1u && press(&m, &st, "j", 0) == 1u && lift(&m, &st, "j") == 0, "a repeat counts once");
    CHECK(!km_key(&m, &st, km_key_code("h"), 1, 0), "H is no game key by default");
    CHECK(!km_bindable(55) && km_key_code("#55") < 0 && km_key_code("#56") == 56 && km_key_code("nope") < 0, "⌘ never bound, #<code> for other keys");

    // the Twilight Princess-like layout: camera on the arrows, D-pad on 1-4
    const char *tp = "RLEFT=left RUP=up RRIGHT=right RDOWN=down DUP=1 DDOWN=2 DLEFT=3 DRIGHT=4 A=space B=j X=k Y=l ZL=i SHAKE=";
    CHECK(km_parse(&m, tp, err, sizeof err) == 0 && !*err, "a valid map: no errors");
    CHECK(m.key[123] == KM_RLEFT && m.key[126] == KM_RUP && m.key[18] == KM_DUP && m.key[49] == KM_A && m.key[34] == KM_ZL, "arrows the right stick, 1 D-pad up, Space A, I ZL");
    CHECK(m.key[38] == KM_B && m.key[40] == KM_X && m.key[37] == KM_Y, "J K L moved to B X Y");
    CHECK(m.key[6] == KM_NONE && m.key[32] == KM_NONE, "Z (ZL's) and U (X's) lost their targets");
    { int any = 0; for (int c = 0; c < KM_KEYS; c++) any |= m.key[c] == KM_SHAKE; CHECK(!any, "SHAKE=: no key shakes"); }
    km_release_all(&st);
    press(&m, &st, "up", 0); km_sticks(&st, &lx, &ly, &rx, &ry); CHECK(ry == 32767 && !km_buttons(&st), "↑ is the right stick up");
    km_release_all(&st);

    // saved in full and read back: the same map
    CHECK(km_format(&m, out, sizeof out) < (int)sizeof out, "the full map fits");
    keymap m2; CHECK(km_parse(&m2, out, err, sizeof err) == 0 && !memcmp(m.key, m2.key, sizeof m.key), "the full form reads back as the same map");
    CHECK(strstr(out, "A=space ") && strstr(out, "SHAKE= ") && strstr(out, "RUP=up"), "the full form names every target");
    keymap d; km_default(&d); km_format(&d, out, sizeof out); km_parse(&m2, out, NULL, 0);
    CHECK(!memcmp(d.key, m2.key, sizeof d.key), "the default's full form is the default");

    // several keys on one target, held together
    km_parse(&m, "A=j,space", NULL, 0);
    press(&m, &st, "j", 0); press(&m, &st, "space", 0);
    CHECK(lift(&m, &st, "j") == 1u, "J up, Space still holds A");
    CHECK(lift(&m, &st, "space") == 0, "both up: A up");

    // a bound Shift is a button and still starts chords: with Shift's chords bound it does both; a key with no chord is its own (ZL + D-pad)
    km_parse(&m, "B=lshift", NULL, 0);
    km_key(&m, &st, 56, 1, 0); CHECK(km_buttons(&st) == 1u << KM_B, "Left Shift presses B");
    press(&m, &st, "left", 0); km_sticks(&st, &lx, &ly, &rx, &ry); CHECK(km_buttons(&st) == 1u << KM_B && rx == -32767, "Left Shift seen down, then ←: B and the right stick left");
    km_release_all(&st);
    km_parse(&m, "ZL=lshift DUP=1", NULL, 0);
    km_key(&m, &st, 56, 1, 0); press(&m, &st, "1", KM_MOD_SHIFT);
    CHECK(km_buttons(&st) == (1u << KM_ZL | 1u << KM_DUP), "ZL on Shift, D-pad up on 1: Shift+1 (no chord) is ZL and D-pad up");
    km_key(&m, &st, 56, 0, 0); CHECK(km_buttons(&st) == 1u << KM_DUP, "Shift up: 1 still holds D-pad up");
    km_release_all(&st);

    // errors: skipped, named, the rest applied
    CHECK(km_parse(&m, "A=space FOO=j B=nokey,k nonsense", err, sizeof err) == 3, "three errors");
    CHECK(strstr(err, "FOO") && strstr(err, "nokey") && strstr(err, "nonsense"), "each named");
    CHECK(m.key[49] == KM_A && m.key[40] == KM_B, "the good entries still apply");
    CHECK(!strcmp(km_key_label(123), "\xE2\x86\x90") && !strcmp(km_targets[KM_PLUS].label, "+"), "labels");

    // keymap files: a short hand-written one over the default, the full one km_ini_write makes read back as the same map, wrong lines skipped and named
    char name[128];
    const char *ini = "; mine\n[Keymap]\nName = WASD + IJKL\n\nA = l, space\nRightStickUp = up\nDpadUp = 1\nShake =\nzl=q\n";
    CHECK(km_ini_read(ini, name, sizeof name, &m, err, sizeof err) == 0 && !*err, "a hand-written file: no errors");
    CHECK(!strcmp(name, "WASD + IJKL"), "its name");
    CHECK(m.key[37] == KM_A && m.key[49] == KM_A && m.key[126] == KM_RUP && m.key[18] == KM_DUP && m.key[12] == KM_ZL, "its keys (ini and DM_KEYMAP names, any case)");
    CHECK(m.key[38] == KM_NONE && m.key[13] == KM_LUP && m.key[40] == KM_B, "J lost A, the rest the default's");
    { int any = 0; for (int c = 0; c < KM_KEYS; c++) any |= m.key[c] == KM_SHAKE; CHECK(!any, "Shake = : no key"); }
    char file[8192]; km_ini_write(&m, "Mine", file, sizeof file);
    CHECK(strstr(file, "Name = Mine\n") && strstr(file, "A = l, space\n") && strstr(file, "Shake =\n") && strstr(file, "; Right stick\n"), "the written file");
    keymap m3; CHECK(km_ini_read(file, name, sizeof name, &m3, err, sizeof err) == 0 && !strcmp(name, "Mine") && !memcmp(m.key, m3.key, sizeof m.key), "it reads back as the same map");
    CHECK(km_ini_read("A = space\nFoo = j\nB = nokey\njunk\n", name, sizeof name, &m3, err, sizeof err) == 3 && !*name, "three wrong lines, no name");
    CHECK(strstr(err, "Foo") && strstr(err, "nokey") && strstr(err, "junk") && m3.key[49] == KM_A, "each named, the good line applied");

    // the presets folder: Default first, then files by name (from Name =, else the file's), a saved preset replaces one of its name
    char tmp[] = "/tmp/t_keymap_XXXXXX"; CHECK(mkdtemp(tmp) != NULL, "a temporary HOME");
    setenv("HOME", tmp, 1);
    static km_preset list[KM_MAX_PRESETS];
    CHECK(km_presets_load(list, KM_MAX_PRESETS, err, sizeof err) == 1 && !strcmp(list[0].name, "Default"), "no folder: Default only");
    char path[1024];
    CHECK(km_preset_save(&m, "WASD + IJKL", path, sizeof path) == 0 && strstr(path, "/Library/Application Support/nxnative/keymaps/wasd-ijkl.ini"), "saved as wasd-ijkl.ini");
    keymap d2; km_default(&d2); CHECK(km_preset_save(&d2, "Another", path, sizeof path) == 0, "another saved");
    { char dir[1024]; km_preset_dir(dir, sizeof dir); char f2[1100]; snprintf(f2, sizeof f2, "%s/zz.ini", dir); FILE *f = fopen(f2, "w"); fputs("B = space\n", f); fclose(f); }
    int n = km_presets_load(list, KM_MAX_PRESETS, err, sizeof err);
    CHECK(n == 4 && !strcmp(list[1].name, "Another") && !strcmp(list[2].name, "WASD + IJKL") && !strcmp(list[3].name, "zz") && !*err, "Default, Another, WASD + IJKL, zz (no Name: the file's)");
    km_parse(&m3, list[2].map, NULL, 0); CHECK(!memcmp(m3.key, m.key, sizeof m.key), "the preset's map is the saved one");
    km_bind(&m, KM_Y, 4); CHECK(km_preset_save(&m, "wasd + ijkl", path, sizeof path) == 0 && km_presets_load(list, KM_MAX_PRESETS, err, sizeof err) == 4, "the same name again: replaced, not added");

    // PR #386's form (NX2Apple ships it): entries separated by ',', keys by their codes, its names; none is no key
    CHECK(km_parse(&m, "A=40,B=38", err, sizeof err) == 0 && m.key[40] == KM_A && m.key[38] == KM_B, "A=40,B=38: A and B swapped (K and J)");
    CHECK(km_parse(&m, "Up=13,Down=1,Left=0,Right=2,LUp=126,Plus=49,Shake=none", err, sizeof err) == 0 && m.key[13] == KM_DUP && m.key[1] == KM_DDOWN && m.key[0] == KM_DLEFT &&
          m.key[2] == KM_DRIGHT && m.key[126] == KM_LUP && m.key[49] == KM_PLUS, "its D-pad names, LUp, Plus; codes below 10 are codes there");
    { int any = 0; for (int c = 0; c < KM_KEYS; c++) any |= m.key[c] == KM_SHAKE; CHECK(!any, "Shake=none: no key shakes"); }
    CHECK(km_parse(&m, "LDown=1", NULL, 0) == 0 && m.key[18] == KM_LDOWN, "one entry alone is ours: 1 is the 1 key");
    CHECK(km_parse(&m, "A=40,B=55,Foo=3", err, sizeof err) == 2 && strstr(err, "55") && strstr(err, "Foo") && m.key[40] == KM_A, "a code never bound (Command) and a wrong name named, the rest applied");

    // chords: either side, Control and Option, labels and names, the full form and keymap files, a modifier takes no chord, the key alone stays its own
    km_parse(&m, "RLEFT= RUP= RRIGHT= RDOWN=", NULL, 0); CHECK(press(&m, &st, "left", KM_MOD_SHIFT) == 1u << KM_DLEFT, "Shift's chords cleared: Shift+← is the D-pad");
    km_release_all(&st);
    CHECK(km_parse(&m, "Y=ctrl+j X=option+j LS=opt+#4", err, sizeof err) == 0, "ctrl+, option+, opt+#<code>");
    km_key(&m, &st, 62, 1, 0); CHECK(press(&m, &st, "j", 0) == 1u << KM_Y, "Right Control seen down, then J: Y");
    lift(&m, &st, "j"); km_key(&m, &st, 62, 0, 0); CHECK(press(&m, &st, "j", KM_MOD_OPT) == 1u << KM_X && lift(&m, &st, "j") == 0, "Option (the event's flag) + J: X, and back");
    CHECK(press(&m, &st, "j", 0) == 1u << KM_A, "J alone: A");
    km_release_all(&st);
    CHECK(km_key_code("shift+lshift") < 0 && km_key_code("shift+") < 0 && km_key_code("cmd+j") < 0 && km_key_code("shift+#55") < 0, "no chord of a modifier, nothing, ⌘");
    char lb[64]; CHECK(!strcmp(km_slot_label(km_key_code("shift+up"), lb, sizeof lb), "\xE2\x87\xA7\xE2\x86\x91") && !strcmp(km_slot_label(km_key_code("opt+#4"), lb, sizeof lb), "\xE2\x8C\xA5H"), "labels: ⇧↑, ⌥H");
    km_format(&m, out, sizeof out); CHECK(strstr(out, "Y=ctrl+j") && strstr(out, "X=opt+j") && strstr(out, "LS=opt+h") && strstr(out, "RUP=shift+up ") && km_parse(&m2, out, NULL, 0) == 0 && km_same(&m, &m2), "in the full form and back");
    km_ini_write(&m, "C", file, sizeof file); CHECK(strstr(file, "Y = ctrl+j\n") && km_ini_read(file, name, sizeof name, &m3, NULL, 0) == 0 && km_same(&m, &m3), "in a keymap file and back");
    CHECK(km_ini_read("RightStickUp = shift+w\n", name, sizeof name, &m3, NULL, 0) == 0 && m3.key[KM_SHIFT * KM_KEYS + 13] == KM_RUP && m3.key[13] == KM_LUP, "a hand-written chord; W alone stays the left stick");
    km_default(&d); km_key(&d, &st, 126, 1, KM_MOD_SHIFT); km_key(&d, &st, 60, 0, 0); km_sticks(&st, &lx, &ly, &rx, &ry); CHECK(ry == 32767, "Shift let go before ↑: the right stick held");
    km_key(&d, &st, 126, 0, 0); km_sticks(&st, &lx, &ly, &rx, &ry); CHECK(ry == 0 && !km_buttons(&st), "↑ up: the right stick back, no D-pad");

    // the largest map (every binding on a target) fits the buffers sized for it; a save that fails before its path is known leaves the path empty
    { static keymap big; km_default(&big); for (int c = 0; c < KM_SLOTS; c++) if (km_slot_bindable(c)) big.key[c] = (uint8_t)(c % KM_N);
      static char f1[KM_FORMAT_MAX], f2[KM_INI_MAX];
      CHECK(km_format(&big, f1, sizeof f1) < (int)sizeof f1 && km_ini_write(&big, "Every key", f2, sizeof f2) < (int)sizeof f2, "every binding: the full form and the file fit");
      keymap back; CHECK(km_parse(&back, f1, NULL, 0) == 0 && km_same(&big, &back), "and read back"); }
    unsetenv("HOME"); path[0] = 'x'; CHECK(km_preset_save(&m, "No home", path, sizeof path) == -1 && !path[0], "no HOME: -1, no path");
    printf("t_keymap: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
