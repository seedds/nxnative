#include "front_screen_mode.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    front_screen_mode scaled = {66, 1800,1169,3600,2338,120,0,1};
    front_screen_mode modes[] = {
        {126,3024,1964,3024,1964,120,1,1}, // wrong Retina density
        {54,1512,982,3024,1964,120,1,1},
        {55,1512,982,3024,1964,60,1,1},
        {56,1512,982,3024,1964,59.94,1,1},
        {66,1800,1169,3600,2338,120,0,1},
        {0,1512,982,3024,1964,240,1,0},    // unusable mode
    };
    size_t n = sizeof modes / sizeof modes[0];
    assert(front_screen_native_pick(modes,n,scaled,scaled,1,1,120)==1);
    assert(front_screen_native_pick(modes,n,scaled,scaled,1,1,60)==2);
    assert(front_screen_native_pick(modes,n,modes[2],scaled,0,1,120)==1);
    assert(front_screen_native_pick(modes,n,modes[1],scaled,1,1,120)==1);
    assert(front_screen_native_pick(modes,n,modes[2],scaled,0,1,30)==2);
    front_screen_mode fixed = scaled; fixed.hz=60;
    assert(front_screen_native_pick(modes,n,fixed,fixed,0,0,60)==2);
    assert(front_screen_native_pick(modes,n,fixed,fixed,0,0,120)==1);
    assert(front_screen_native_pick(modes,n,scaled,scaled,1,1,144)==-1);
    assert(front_screen_native_pick(modes,0,scaled,scaled,1,1,120)==-1);
    assert(front_screen_native_pick(modes,n,scaled,scaled,1,1,0)==-1);
    front_screen_mode bad=scaled;bad.width=0;
    assert(front_screen_native_pick(modes,n,bad,scaled,1,1,120)==-1);
    assert(front_screen_hz_multiple(59.94,60));
    assert(!front_screen_hz_multiple(75,60));
    assert(!front_screen_hz_multiple(0,60));
    puts("PASS: native density,120Hz fallback, fixed60/30, rate changes, unusable/missing modes");
}
