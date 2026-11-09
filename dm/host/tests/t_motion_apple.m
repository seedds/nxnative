// Exercise the shared GameController adapter with fake sensors; no physical controller is needed.
#import "../motion_apple.h"
#include <assert.h>
#include <stdio.h>

@interface TestMotion : NSObject
@property BOOL hasRotationRate, hasGravityAndUserAcceleration, sensorsRequireManualActivation, sensorsActive;
@property GCAcceleration acceleration, gravity, userAcceleration;
@property GCRotationRate rotationRate;
@property(copy) GCMotionValueChangedHandler valueChangedHandler;
@end
@implementation TestMotion
@end
@interface TestController : NSObject
@property TestMotion *motion;
@end
@implementation TestController
@end

static int samples, cleared = -1, player, sampledRole, clearedRole;
static uint64_t generation;
uint64_t dm_pad_generation(int p) { (void)p; return generation; }
static float acc[3], rot[3];
void dm_motion_set(int p, const float a[3], const float w[3]) {
    samples++; player = p;
    for (int i = 0; i < 3; i++) { acc[i] = a[i]; rot[i] = w[i]; }
}
void dm_motion_set_generation(int p, uint64_t token, const float a[3], const float w[3]) {
    if (token == generation) dm_motion_set(p, a, w);
}
void dm_motion_clear(int p) { cleared = p; }

void dm_motion_set_side_generation(int p,int role,uint64_t generation,const float a[3],const float r[3]) { sampledRole=role; dm_motion_set_generation(p,generation,a,r); }
void dm_motion_clear_side(int p,int role) { clearedRole=role; dm_motion_clear(p); }

int main(void) { @autoreleasepool {
    TestController *c = [TestController new]; TestMotion *m = [TestMotion new]; c.motion = m;
    m.hasRotationRate = YES; m.sensorsRequireManualActivation = YES;
    setenv("DM_MOTION", "0", 1); dm_motion_attach((id)c, 2);
    assert(!m.sensorsActive && !m.valueChangedHandler);
    unsetenv("DM_MOTION"); dm_motion_attach((id)c, 2);
    assert(m.sensorsActive && m.valueChangedHandler);
    m.valueChangedHandler((id)m);
    assert(!samples);   // An advertised sensor with zero-only reports must not suppress the phone or the flat pose.
    m.hasGravityAndUserAcceleration = YES;
    m.gravity = (GCAcceleration){0, 0, -1}; m.userAcceleration = (GCAcceleration){0.25, 0, 0};
    m.rotationRate = (GCRotationRate){0, 1, 0}; m.valueChangedHandler((id)m);
    assert(samples == 1 && player == 2 && acc[0] == 0.25f && acc[2] == -1 && rot[1] == 1);
    m.hasGravityAndUserAcceleration = NO; m.rotationRate = (GCRotationRate){0};
    m.valueChangedHandler((id)m);
    assert(samples == 2 && acc[2] == 0);   // Once real data arrives, a zero acceleration can be free fall.
    GCMotionValueChangedHandler old = m.valueChangedHandler;
    generation++; old((id)m);
    assert(samples == 2);   // a queued callback from before a live reassignment cannot write to the old slot
    dm_motion_detach((id)c, 2);
    assert(!m.sensorsActive && !m.valueChangedHandler && cleared == 2);
    m.hasGravityAndUserAcceleration=YES;
    dm_motion_attach_role((id)c,0,1); m.valueChangedHandler((id)m);
    assert(samples==3 && player==0 && sampledRole==1);
    GCMotionValueChangedHandler leftCallback=m.valueChangedHandler;
    generation++; dm_motion_detach_role((id)c,0,1);
    assert(clearedRole==1 && !m.valueChangedHandler);
    dm_motion_attach_role((id)c,0,2); m.valueChangedHandler((id)m);
    assert(samples==4 && sampledRole==2);
    leftCallback((id)m); assert(samples==4);
    dm_motion_detach_role((id)c,0,2); assert(clearedRole==2);
    puts("t_motion_apple: passed");
    return 0;
} }
