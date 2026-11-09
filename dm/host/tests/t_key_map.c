// key_map.h (DM_KEY_MAP, the Mac keyboard's map to the game's controls): names, parsing, the table, and what a wrong map leaves.
#include "key_map.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    key_map m; char err[160];
    // The issue's map, in any letter case and with either separator, and a key given twice (the last entry wins)
    assert(key_map_parse(&m, "space:R, lshift:L,Left:rleft up:RUP right:rright,down:rdown,minus:-,equal:+,j:none,j:A", err, sizeof err) == 9 && !*err);
    assert(key_map_act(&m, 49) == 7 && key_map_act(&m, 56) == 6 && key_map_act(&m, 123) == KM_RS_LEFT && key_map_act(&m, 126) == KM_RS_UP);
    assert(key_map_act(&m, 124) == KM_RS_RIGHT && key_map_act(&m, 125) == KM_RS_DOWN && key_map_act(&m, 27) == 11 && key_map_act(&m, 24) == 10 && key_map_act(&m, 38) == 0);
    assert(key_map_act(&m, 14) == KM_DEFAULT && key_map_act(&m, 200) == KM_DEFAULT);   // E keeps the layout's R; a code past the table is the layout's too
    assert(key_map_parse(&m, "k:none", err, sizeof err) == 1 && key_map_act(&m, 40) == KM_NONE);
    // Every name once: keys and controls resolve, the stick and shake controls by their codes
    assert(km_key("ENTER", 5) == 36 && km_key("esc", 3) == 53 && km_key("f12", 3) == 111 && km_key("rcmd", 4) == 54 && km_key("fn", 2) == 63 && km_key("x", 1) == 7);
    assert(km_control("shake", 5) == KM_SHAKE && km_control("lup", 3) == KM_LS_UP && km_control("ldown", 5) == KM_LS_DOWN && km_control("zr", 2) == 9 && km_control("ls", 2) == 4);
    for (size_t i = 0; i < sizeof KM_KEY_NAMES / sizeof *KM_KEY_NAMES; i++) assert(KM_KEY_NAMES[i].code < KM_KEYS && km_key(KM_KEY_NAMES[i].name, strlen(KM_KEY_NAMES[i].name)) == KM_KEY_NAMES[i].code);
    for (size_t i = 0; i < sizeof KM_CONTROL_NAMES / sizeof *KM_CONTROL_NAMES; i++) assert(km_control(KM_CONTROL_NAMES[i].name, strlen(KM_CONTROL_NAMES[i].name)) == KM_CONTROL_NAMES[i].act);
    // An empty map names nothing
    assert(key_map_parse(&m, "", err, sizeof err) == 0 && key_map_parse(&m, NULL, err, sizeof err) == 0 && key_map_parse(&m, " , ", err, sizeof err) == 0 && key_map_act(&m, 49) == KM_DEFAULT);
    // A wrong entry refuses the whole map, with the entry in the reason, and leaves no key mapped
    assert(key_map_parse(&m, "space:r,spacebar:l", err, sizeof err) == -1 && strstr(err, "no such key") && strstr(err, "\"spacebar:l\"") && key_map_act(&m, 49) == KM_DEFAULT);
    assert(key_map_parse(&m, "space:drift", err, sizeof err) == -1 && strstr(err, "no such control") && strstr(err, "\"space:drift\""));
    assert(key_map_parse(&m, "space", err, sizeof err) == -1 && strstr(err, "no colon") && strstr(err, "\"space\""));
    assert(key_map_parse(&m, "space:", err, sizeof err) == -1 && strstr(err, "no such control"));
    assert(key_map_parse(&m, ":r", err, sizeof err) == -1 && strstr(err, "no such key"));
    puts("t_key_map: ok");
    return 0;
}
