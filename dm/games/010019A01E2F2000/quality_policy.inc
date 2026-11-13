// Pure scene-size policy. All inputs are allocation extents, never HUD/window
// scaling. The game's native controller supplies the adaptive ratio; this code
// only maps that ratio to the selected maximum, keeping 4-pixel granularity.
#pragma once
#include <stdint.h>
static inline uint64_t mp4_extent(unsigned w,unsigned h) { return (uint64_t)h<<32|w; }
static inline int mp4_scene_policy(uint64_t native,uint64_t selected,uint64_t base,int dynamic,uint64_t *out) {
    uint32_t w=selected,h=selected>>32,nw=native,nh=native>>32,bw=base,bh=base>>32;
    if(!out || !w || !h || w>5120 || h>2880 || w%4 || h%4) return 0;
    if(!dynamic) { *out=selected;return 1; }
    if(!nw || !nh || !bw || !bh || nw>bw || nh>bh || bw>16384 || bh>16384) return 0;
    uint64_t x=((uint64_t)nw*w+bw-1)/bw,y=((uint64_t)nh*h+bh-1)/bh;
    x=(x+3)&~3ull;y=(y+3)&~3ull;
    *out=mp4_extent(x>w?w:(unsigned)x,y>h?h:(unsigned)y);return 1;
}
static inline int mp4_dynamic_extent(unsigned w,unsigned h,unsigned mw,unsigned mh,int dynamic) {
    return dynamic && w && h && mw && mh && w<=mw && h<=mh && !(w%4) && !(h%4);
}
