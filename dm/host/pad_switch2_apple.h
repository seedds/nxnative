// Nintendo Switch 2 controllers (Pro Controller 2, Joy-Con 2) over Bluetooth LE, shared by the Mac and iOS front ends as haptics_apple.m is: the CoreBluetooth transport
// (pad_switch2_apple.m) turns each controller's reports into a pad, and the front end gives a pad a player slot through dm_switch2_front. switch2_proto.h is the protocol.
//
// Off unless DM_SW2=1 (the "Switch 2 controllers" setting): with it off no Bluetooth client is created, so no permission prompt appears. The app's Info.plist needs
// NSBluetoothAlwaysUsageDescription (dm/mac/pack_app.py writes it); without it macOS ends the process at the first scan, so this stays off without it.
//
// Pairing happens in the game: hold Sync on a controller until its player lights sweep. On the Mac the host then bonds it to this Mac (switch2_proto.h, Bonding), and from then on
// a press of any button wakes it toward this Mac and it connects without Sync. A controller woken for another host (a nearby Switch 2) is left alone and nothing is written to it.
// Where this host's address cannot be known (iOS) a controller in pairing mode is used without bonding, and needs Sync again next time.
// The feedback is the controller's player LEDs and a log line.
//
// A pad is one player: a Pro Controller 2, a left and a right Joy-Con 2 together (one Pro Controller), or one Joy-Con 2 held sideways. Motion comes from the Pro Controller 2's
// sensor, the right Joy-Con's of a pair (DM_SW2_MOTION_SIDE=L for the left) or the single Joy-Con's, as the game's six-axis sensor of that player (dm_motion_set_hid).
// DM_SW2_HOST=aa:bb:cc:dd:ee:ff names this Mac's Bluetooth address when it cannot be read; DM_SW2_VERBOSE=1 logs the first report of each second; DM_SW2_MAP=a,b,c,d,e,f and
// DM_SW2_GYRO_DPS=<degrees per second per count> adjust the motion axes and scale (switch2_proto.h s2_motion). The Pro Controller 2 is untested on hardware.
#pragma once
#include "switch2_proto.h"
#include "hle_haptics.h"

typedef struct s2_pad { s2_pad_state st; char name[48]; struct s2_pad *next; } s2_pad;

// The front end's side. All calls come on the main queue.
typedef struct {
    void (*attach)(s2_pad *p);   // a new pad: take it a player slot (player() then names it, -1 for none)
    void (*detach)(s2_pad *p);   // the pad's last controller went: it is already off the list and is freed after the call
    void (*read)(s2_pad *p);     // the pad's state changed
    int (*player)(s2_pad *p);    // 0-based player, -1 without one
} dm_switch2_front;

int dm_switch2_start(const dm_switch2_front *front);   // 1: started; 0: off (DM_SW2 is not 1, or the Info.plist has no Bluetooth usage description)
void dm_switch2_active(int on);                         // focus lost (0): the sensors stop driving the game's; back (1): they continue
s2_pad *dm_switch2_pads(void);                          // the pads, a list through next
// Rumble: the sink for a player whose pad is a Switch 2 controller (dm_haptics_apple_set_sink(player, dm_switch2_rumble)). The game's two full-key motors, left and right, reach the
// pad's controllers as s2_rumble_pick says: every value the game sets plays once, in its own 5 ms sample (switch2_proto.h, Rumble timing), three to a packet every 15 ms while they
// buzz, then a few packets of silence (a write without response can be lost).
void dm_switch2_rumble(const dm_haptic_state *state);
