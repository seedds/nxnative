// Controller motion from GameController (GCMotion) to the host's six-axis sensor (dm_motion_set, hle_hid.c). Shared by the Mac front end (front_mac.m) and the iOS gamepad
// (DmGamepad.m). A controller with a gyroscope (DualSense, DualShock 4, Switch Pro, Joy-Con) has its sensors switched on when it is given a player, and every motion update is
// passed on with the total acceleration (G, gravity direction as CoreMotion: (0, 0, -1) face up) and the rotation rate (rad/s); a controller without one does nothing.
// DM_MOTION=0 leaves the sensors off. GCMotion's acceleration / hasRotationRate / manual activation need macOS 11.3 / iOS 14.5, below both platforms' targets.
#pragma once
#import <GameController/GameController.h>
#include <stdlib.h>
#include "dm_input.h"

static inline void dm_motion_attach(GCController *c, int player) {
    const char *e = getenv("DM_MOTION"); if (e && !atoi(e)) return;
    dm_motion_clear(player);   // another controller takes the player: its stream and learned gyro offset start again (hle_hid.c)
    GCMotion *mo = c.motion;
    if (!mo || !mo.hasRotationRate) return;
    if (mo.sensorsRequireManualActivation) mo.sensorsActive = YES;   // DualSense / DualShock 4: off until asked for
    __block int live = 0, primed = 0; __block GCRotationRate first;
    mo.valueChangedHandler = ^(GCMotion *m) {
        float acc[3], rot[3];
        if (m.hasGravityAndUserAcceleration) {
            acc[0] = (float)(m.gravity.x + m.userAcceleration.x); acc[1] = (float)(m.gravity.y + m.userAcceleration.y); acc[2] = (float)(m.gravity.z + m.userAcceleration.z);
        } else { acc[0] = (float)m.acceleration.x; acc[1] = (float)m.acceleration.y; acc[2] = (float)m.acceleration.z; }
        rot[0] = (float)m.rotationRate.x; rot[1] = (float)m.rotationRate.y; rot[2] = (float)m.rotationRate.z;
        // A controller can advertise a gyroscope it does not have: some macOS drivers send only zeroes (#296), and the GameSir X5 Lite on an iPhone, a controller the phone
        // mounts in, is listed with a rotation rate and its reports kept the phone's gyro out (NX2Apple issue #971). So a controller is no source until its rotation rate
        // changes from its first report: a real gyroscope's noise changes it within a few reports. After that, a still or zero sample is real (a pad held still, free fall)
        if (!live) {
            if (!primed) { primed = 1; first = m.rotationRate; return; }
            if (m.rotationRate.x == first.x && m.rotationRate.y == first.y && m.rotationRate.z == first.z) return;
            live = 1;
        }
        dm_motion_set(player, acc, rot);
    };
}
// iOS turns a manual-activation controller's sensors off while the app is in the background (Control Center, the home screen) and leaves them off, so the gyro stayed dead until the
// controller reconnected (found by MacNativeGames on the iPad). Called when the app becomes active again: only controllers attached above (their handler is set)
static inline void dm_motion_rearm(GCController *c) {
    GCMotion *mo = c.motion;
    if (mo && mo.valueChangedHandler && mo.sensorsRequireManualActivation) mo.sensorsActive = YES;
}
static inline void dm_motion_detach(GCController *c, int player) {
    GCMotion *mo = c.motion;
    if (mo) { mo.valueChangedHandler = nil; if (mo.sensorsRequireManualActivation) mo.sensorsActive = NO; }
    dm_motion_clear(player);
}
