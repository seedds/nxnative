// Per-device gyro integration. Recenter establishes the identity orientation;
// no time is integrated across lost packets, suspend or a clock-format change.
#pragma once
#include "dm_input.h"
#include <math.h>
#include <string.h>
typedef struct { dm_motion value; float q[4]; uint32_t tick; unsigned tick_us; int have_tick; } pad_motion_state;
static inline void pad_motion_reset(pad_motion_state *m) { memset(m, 0, sizeof *m); }
static inline int pad_motion_update(pad_motion_state *m, uint32_t tick, unsigned tick_us, const float *acc, const float *gyro) {
    for (int i=0;i<3;i++) if (!isfinite(acc[i]) || !isfinite(gyro[i])) return 0;
    unsigned elapsed = tick - m->tick;
    if (tick_us == 32) elapsed &= 0xffff;
    if (m->have_tick && m->tick_us == tick_us && !elapsed) return 0;
    float dt = m->have_tick && m->tick_us == tick_us ? elapsed * tick_us / 1e6f : 0;
    if (!m->have_tick) m->q[0] = 1;
    m->have_tick=1; m->tick=tick; m->tick_us=tick_us;
    if (dt > 0.1f) dt = 0;
    memcpy(m->value.acc,acc,sizeof m->value.acc); memcpy(m->value.gyro,gyro,sizeof m->value.gyro);
    float speed=sqrtf(gyro[0]*gyro[0]+gyro[1]*gyro[1]+gyro[2]*gyro[2]);
    if (speed > 0 && dt > 0) {
        float half=speed*dt*(float)M_PI, k=sinf(half)/speed;
        float w=cosf(half), x=gyro[0]*k, y=gyro[1]*k, z=gyro[2]*k;
        float a=m->q[0], b=m->q[1], c=m->q[2], d=m->q[3];
        m->q[0]=a*w-b*x-c*y-d*z; m->q[1]=a*x+b*w+c*z-d*y;
        m->q[2]=a*y-b*z+c*w+d*x; m->q[3]=a*z+b*y-c*x+d*w;
        float norm=sqrtf(m->q[0]*m->q[0]+m->q[1]*m->q[1]+m->q[2]*m->q[2]+m->q[3]*m->q[3]);
        for(int i=0;i<4;i++) m->q[i]/=norm;
    }
    for(int i=0;i<3;i++) m->value.angle[i] += gyro[i]*dt;
    float w=m->q[0],x=m->q[1],y=m->q[2],z=m->q[3];
    float *d=m->value.dir;
    d[0]=1-2*(y*y+z*z); d[1]=2*(x*y+w*z); d[2]=2*(x*z-w*y);
    d[3]=2*(x*y-w*z); d[4]=1-2*(x*x+z*z); d[5]=2*(y*z+w*x);
    d[6]=2*(x*z+w*y); d[7]=2*(y*z-w*x); d[8]=1-2*(x*x+y*y);
    return 1;
}
