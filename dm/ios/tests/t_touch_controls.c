#include "../Sources/DmSwipeLook.h"
#include "../Sources/DmTouchControls.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static void sampling(int touchHz, int displayHz, double phase) {
    DmSwipeLook s; int32_t x,y; dm_swipe_begin(&s,0,0,0);
    int event = 0;
    for (int frame=1; frame<=displayHz; ++frame) {
        double now = (frame + phase)/displayHz;
        while ((event+1.0)/touchHz <= now) {
            ++event; double t = (double)event/touchHz;
            dm_swipe_move(&s,120*t,0,t);
        }
        dm_swipe_frame(&s,now,1,1,&x,&y);
        if (event) { assert(abs(x-21845)<=1); assert(y==0); }
    }
}
int main(void) {
    DmSwipeLook s; int32_t x,y; dm_swipe_begin(&s,100,200,1);
    dm_swipe_frame(&s,1,1,1,&x,&y); assert(x==0 && y==0);
    dm_swipe_move(&s,102,200,1+1.0/60);
    dm_swipe_frame(&s,1+1.0/60,1,1,&x,&y); assert(abs(x-21845)<=1 && y==0);
    dm_swipe_frame(&s,1+1.0/60+1.0/120,1,1,&x,&y); assert(abs(x-21845)<=1); // no alternating zero at faster display rate
    dm_swipe_frame(&s,1+1.0/60+0.041,1,1,&x,&y); assert(x==0 && y==0); // held stationary expires
    dm_swipe_move(&s,101,199,1+2.0/60); dm_swipe_frame(&s,1+2.0/60,1,1,&x,&y); assert(x<0 && y>0);
    dm_swipe_move(&s,1000,1000,1+3.0/60); dm_swipe_frame(&s,1+3.0/60,1,1,&x,&y); assert(hypot(x,y)<=32768);
    dm_swipe_move(&s,1100,1000,2); dm_swipe_frame(&s,2,1,1,&x,&y); assert(x==0 && y==0); // stall
    dm_swipe_begin(&s,0,0,3); dm_swipe_frame(&s,3,1,1,&x,&y); assert(x==0 && y==0);
    dm_swipe_move(&s,100,100,3); dm_swipe_frame(&s,3,1,1,&x,&y); assert(x==0 && y==0); // zero time ignored
    for (int t=30;t<=120;t*=2) for (int d=30;d<=120;d*=2) {
        sampling(t,d,0); sampling(t,d,0.3); sampling(t,d,0.7);
    }
    DmTouchControls defaults = dm_touch_controls(NULL,NULL,NULL);
    assert(!defaults.swipe && defaults.swipe_gain==1 && defaults.left_deadzone==0.1);
    DmTouchControls chosen=dm_touch_controls("1","2.5","0.25");
    assert(chosen.swipe && chosen.swipe_gain==2.5 && chosen.left_deadzone==0.25);
    const char *invalid[]={"", "nan", "inf", "1.0junk", "--1", "1e999"};
    for (unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        DmTouchControls bad=dm_touch_controls(invalid[i],invalid[i],invalid[i]);
        assert(!bad.swipe && bad.swipe_gain==1 && bad.left_deadzone==0.1);
    }
    DmTouchControls bounds=dm_touch_controls("0","100","-1");
    assert(!bounds.swipe && bounds.swipe_gain==4 && bounds.left_deadzone==0);
    bounds=dm_touch_controls(NULL,"-1","100");
    assert(bounds.swipe_gain==0.25 && bounds.left_deadzone==0.4);
    // Higher sensitivity increases tilt for the same physical finger speed.
    dm_swipe_begin(&s,0,0,4); dm_swipe_move(&s,1,0,4+1.0/60);
    dm_swipe_frame(&s,4+1.0/60,1,1,&x,&y); int32_t normal=x;
    dm_swipe_frame(&s,4+1.0/60,1,2,&x,&y); assert(abs(x-2*normal)<=1);
    // Upstream radial response at the default dead zone, across all directions.
    for (int dx=-100;dx<=100;dx+=5) for (int dy=-100;dy<=100;dy+=5) {
        double d=hypot(dx,dy), m=d<=7.7 ? 0 : fmin((d-7.7)/(77*0.8-7.7),1)/d;
        dm_touch_axes(dx,dy,77,defaults.left_deadzone,&x,&y);
        assert(abs(x-(int32_t)lrint(dx*m*32767))<=1);
        assert(abs(y-(int32_t)lrint(-dy*m*32767))<=1);
    }
    dm_touch_axes(10,0,77,0.1,&x,&y); assert(x>0);
    dm_touch_axes(10,0,77,0.25,&x,&y); assert(x==0 && y==0);
    dm_touch_axes(-60,0,77,0.25,&x,&y); assert(x<0 && y==0);
    dm_touch_axes(0,0,77,0,&x,&y); assert(x==0 && y==0);
    dm_touch_axes(60,-60,77,0.4,&x,&y); assert(x>0 && y>0 && hypot(x,y)<=32768);
    puts("touch controls: PASS (defaults, parsing, limits, sensitivity, expiry, mixed sample rates and unchanged legacy response)");
}
