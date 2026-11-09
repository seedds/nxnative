// Compile the production iOS read_pad callback on Mac; UIKit presentation is outside this test.
#import "controller_routes_ios.h"
#import "controller_join_apple.h"
#import "pad_apple.h"
#include <assert.h>
#include <stdatomic.h>
static dm_ios_routes g_routes;
static NSArray *g_sources[DM_PLAYERS], *connected;
static NSDictionary *g_overrides;
static BOOL g_routes_publishing;
static _Atomic int g_configuring;
static DmControllerJoin *g_join;
static int32_t axis(float value) { (void)value; return 0; }
static void front_ios_quit_poll(void) {}
static int front_ios_quit_blocks_input(void) { return 0; }
#define pad_map_apply(map,buttons) (buttons)
void dm_haptics_apple_controller(int p,GCController *c) { (void)p; (void)c; }
void dm_pad_set_player(int p,uint64_t b,int32_t x,int32_t y,int32_t rx,int32_t ry) { (void)p; (void)b; (void)x; (void)y; (void)rx; (void)ry; }
void dm_pad_set_side(int p,int role,uint64_t b,int32_t x,int32_t y) { (void)p; (void)role; (void)b; (void)x; (void)y; }
void dm_pad_set_pair_gamepad(int p,int role,uint64_t b,int32_t x,int32_t y) { dm_pad_set_side(p,role,b,x,y); }
@interface AdmissionView : NSObject
- (void)padsChanged:(id)notification;
@end
static AdmissionView *g_view;
@implementation AdmissionView
- (void)padsChanged:(id)notification {
    (void)notification;
    NSArray *rows=cs_update(connected);
    NSMutableArray *sources=[NSMutableArray new];
    for (int p=0;p<DM_PLAYERS;p++) [sources addObject:[NSMutableArray new]];
    g_routes=ios_plan_routes(g_routes.slots,rows,g_overrides,NO,sources);
    for (int p=0;p<DM_PLAYERS;p++) g_sources[p]=sources[p];
}
@end
#include "production_ios_read_pad.inc"
int main(void) { @autoreleasepool {
    NSMutableArray *pool=[NSMutableArray new];
    for (int i=0;i<9;i++) [pool addObject:[GCController controllerWithExtendedGamepad]];
    connected=pool; g_view=[AdmissionView new]; cs_update(connected); cs_press_to_join=YES;
    for (int i=0;i<8;i++) cs_admit(pool[i]);
    [g_view padsChanged:nil];
    GCController *waiting=pool[8];
    [waiting.extendedGamepad.buttonA setValue:1]; read_pad(waiting.extendedGamepad);
    assert(pad_slot_of(&g_routes.slots,(__bridge const void *)waiting)<0 && cs_waits_for_press(waiting,nil));
    [waiting.extendedGamepad.buttonA setValue:0]; read_pad(waiting.extendedGamepad);
    [pool removeObjectAtIndex:0]; [g_view padsChanged:nil];
    assert(pad_slot_of(&g_routes.slots,(__bridge const void *)waiting)<0);
    [waiting.extendedGamepad.buttonA setValue:1]; read_pad(waiting.extendedGamepad);
    assert(pad_slot_of(&g_routes.slots,(__bridge const void *)waiting)==0 && !cs_waits_for_press(waiting,nil));
    for (int i=0;i<7;i++) assert(g_routes.slots.owner[i+1]==(__bridge const void *)pool[i]);
    puts("iOS production callback: rejected joins wait for a fresh press without moving other players OK");
} return 0; }
