// Mac controller input: keep the existing eight-player slot policy, with
// shared button choices and per-model extras for GameController and Triton HID.
#import "controllers_mac.h"
#import <GameController/GameController.h>
#import <IOKit/hid/IOHIDManager.h>
#import "haptics_apple.h"
#include "dm.h"
#include "motion_apple.h"
#include "motion_pro_mac.h"
#include "pad_slots.h"
#include "pad_apple.h"
#include "pad_map.h"
#include "input_preferences_mac.h"
#include "steam_input.h"
#include "pad_motion.h"
#include <math.h>

enum { PAD_OFF, PAD_DPAD, PAD_LEFT_STICK, PAD_RIGHT_STICK, PAD_TOUCH };

@interface DMController : NSObject {
@public
    int slot, map[DM_PAD_BUTTONS];
    steam_state input;
    IOHIDDeviceRef device;
    unsigned char report[512];
    BOOL feature_warning, rumble_warning, gyro_enabled, connected;
    uint64_t registry_id;
    int pad_mode[2], touch_id[2];
    pad_motion_state motion;
    NSTimeInterval motion_at, gc_not_before;
}
@property NSString *name;
@property NSString *model;
@property GCController *controller;
@end
@implementation DMController
- (instancetype)init { if ((self = [super init])) { slot = -1; touch_id[0]=touch_id[1]=-1; gyro_enabled=YES; pad_physical_default(map, 0); } return self; }
- (void)dealloc { if (device) CFRelease(device); }
@end

static NSMutableArray<DMController *> *g_devices;
static NSMutableDictionary *g_maps; // session-only model choices when saving is disabled
static NSString *g_controls_domain;
static NSDictionary *g_legacy_maps; // read-only import of the old controllers.plist
static pad_map g_button_map;
static BOOL g_map_fixed, g_maps_loaded, g_maps_saved;
static pad_slots g_slots;
static int g_merge, g_focus=1, g_app_focus=1, g_window_focus=1, g_modal;
static __weak DMController *g_motion_source;
static __weak DMController *g_merge_haptics_source;
static NSTimeInterval controller_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static dm_touch g_mouse; static int g_mouse_down;
static BOOL g_touches_published;
static void steam_keepalive(DMController *d);
static void controllers_changed(GCController *gone);
static BOOL steam_discover(void);
static void steam_native_admission(void);
static void controller_panel_refresh(void);
static void controller_read_gc(DMController *d, id<GCDevicePhysicalInputState> state);
static void controller_reset_motion(DMController *d);
static void controller_attach_motion(DMController *d);

static NSString *controller_map_key(DMController *d) { return [@"controller." stringByAppendingString:d.model]; }
static void controller_load_map(DMController *d) {
    pad_physical_default(d->map, 0);
    for(int i=0;i<IM_BUTTONS;i++) d->map[i]=g_button_map.to[i]; // NOLINT(bugprone-signed-char-misuse): -1 is the unbound mapping, not a character.
    d->pad_mode[0]=[d.model isEqual:@"Valve Triton"] ? PAD_DPAD : PAD_OFF;
    d->pad_mode[1]=[d.model isEqual:@"Valve Triton"] ? PAD_TOUCH : PAD_OFF;
    d->gyro_enabled=YES;
    id a=input_preferences_enabled() ? input_preference(g_controls_domain,controller_map_key(d)) : g_maps[d.model];
    if(![a isKindOfClass:NSDictionary.class]) return;
    id pads=a[@"pads"], gyro=a[@"gyro"], extra=a[@"buttons"];
    if([pads isKindOfClass:NSArray.class] && [pads count]==2)
        for(int i=0;i<2;i++) if([pads[i] isKindOfClass:NSNumber.class] && [pads[i] doubleValue]==[pads[i] intValue] && [pads[i] intValue]>=PAD_OFF && [pads[i] intValue]<=PAD_TOUCH) d->pad_mode[i]=[pads[i] intValue];
    if([gyro isKindOfClass:NSNumber.class] && ([gyro doubleValue]==0 || [gyro doubleValue]==1)) d->gyro_enabled=[gyro boolValue];
    // Only extra physical sources live here. Standard buttons have one shared
    // map for the game, never a hidden per-model override of DM_PAD_MAP.
    if([extra isKindOfClass:NSDictionary.class]) for(int i=IM_BUTTONS;i<DM_PAD_BUTTONS;i++) {
        id v=extra[@(i).stringValue]; if(![v isKindOfClass:NSNumber.class]) continue;
        int n=[v intValue]; if([v doubleValue]==n && (n==-1 || (n>=0 && n<16) || n==63)) d->map[i]=n;
    }
}
static void controller_reload_maps(void) {
    BOOL saved=input_preferences_enabled();
    if(g_maps_loaded && !saved && !g_maps_saved) return; // keep this session's edits
    if(g_maps_saved!=saved) [g_maps removeAllObjects];
    g_maps_loaded=YES; g_maps_saved=saved;
    if(g_controls_domain) CFPreferencesAppSynchronize((__bridge CFStringRef)g_controls_domain);
    g_map_fixed=input_map_fixed("DM_PAD_MAP");
    pad_map_default(&g_button_map);
    NSString *spec=input_preference_map(g_controls_domain,"DM_PAD_MAP",1);
    if(spec) pad_map_parse(&g_button_map,spec.UTF8String);
    for(DMController *d in g_devices) {
        BOOL gyro=d->gyro_enabled;
        controller_load_map(d);
        if(gyro!=d->gyro_enabled) { controller_reset_motion(d); controller_attach_motion(d); steam_keepalive(d); }
    }
}
static BOOL controller_save_buttons(void) {
    NSMutableArray *entries=[NSMutableArray array];
    for(int i=0;i<IM_BUTTONS;i++) if(g_button_map.to[i]!=i) {
        int to=g_button_map.to[i]; // NOLINT(bugprone-signed-char-misuse): preserve the -1 unbound mapping.
        NSString *name=to==63 ? @"Shake" : to<0 ? @"none" : @(im_names[to]);
        [entries addObject:[NSString stringWithFormat:@"%s=%@",im_names[i],name]];
    }
    BOOL ok=input_preference_write(g_controls_domain,@"launcher.DM_PAD_MAP",entries.count ? [entries componentsJoinedByString:@","] : nil);
    for(DMController *d in g_devices) for(int i=0;i<IM_BUTTONS;i++) d->map[i]=g_button_map.to[i]; // NOLINT(bugprone-signed-char-misuse): -1 is the unbound mapping, not a character.
    return ok;
}
static BOOL controller_set_button(int source, int target) {
    if(g_map_fixed || source<0 || source>=IM_BUTTONS || !(target==-1 || (target>=0 && target<IM_BUTTONS) || target==63)) return NO;
    g_button_map.to[source]=(int8_t)target;
    return controller_save_buttons();
}
static BOOL controller_save_map(DMController *d) {
    NSMutableDictionary *extra=[NSMutableDictionary dictionary], *value=[NSMutableDictionary dictionary];
    for(int i=IM_BUTTONS;i<DM_PAD_BUTTONS;i++) if(d->map[i]!=-1) extra[@(i).stringValue]=@(d->map[i]);
    if(extra.count) value[@"buttons"]=extra;
    BOOL triton=[d.model isEqual:@"Valve Triton"];
    if(d->pad_mode[0]!=(triton ? PAD_DPAD : PAD_OFF) || d->pad_mode[1]!=(triton ? PAD_TOUCH : PAD_OFF)) value[@"pads"]=@[@(d->pad_mode[0]),@(d->pad_mode[1])];
    if(!d->gyro_enabled) value[@"gyro"]=@NO;
    g_maps[d.model]=value;
    BOOL ok=input_preference_write(g_controls_domain,controller_map_key(d),value.count ? value : nil);
    // Apply even when synchronization failed: the panel reports session-only use.
    for(DMController *other in g_devices) if(other!=d && [other.model isEqual:d.model]) {
        for(int i=IM_BUTTONS;i<DM_PAD_BUTTONS;i++) other->map[i]=d->map[i];
        memcpy(other->pad_mode,d->pad_mode,sizeof d->pad_mode); other->gyro_enabled=d->gyro_enabled;
    }
    return ok;
}
// An old per-model standard map may conflict with another model or a launcher
// choice. Import only on the player's explicit action, keeping the file intact.
static BOOL controller_import_map(DMController *d) {
    if(g_map_fixed) return NO;
    id value=g_legacy_maps[d.model], buttons=value;
    if([value isKindOfClass:NSDictionary.class]) buttons=value[@"buttons"];
    if(![buttons isKindOfClass:NSArray.class] || [buttons count]!=DM_PAD_BUTTONS) return NO;
    for(int i=0;i<DM_PAD_BUTTONS;i++) {
        id v=buttons[i]; if(![v isKindOfClass:NSNumber.class]) continue;
        int n=[v intValue]; if([v doubleValue]!=n || !(n==-1 || (n>=0 && n<16) || n==63)) continue;
        if(i<IM_BUTTONS) g_button_map.to[i]=(int8_t)n; else d->map[i]=n;
    }
    if([value isKindOfClass:NSDictionary.class]) {
        id pads=value[@"pads"],gyro=value[@"gyro"];
        if([pads isKindOfClass:NSArray.class] && [pads count]==2) for(int i=0;i<2;i++) {
            id v=pads[i]; if([v isKindOfClass:NSNumber.class] && [v doubleValue]==[v intValue] && [v intValue]>=PAD_OFF && [v intValue]<=PAD_TOUCH) d->pad_mode[i]=[v intValue];
        }
        if([gyro isKindOfClass:NSNumber.class] && ([gyro doubleValue]==0 || [gyro doubleValue]==1)) d->gyro_enabled=[gyro boolValue];
    }
    BOOL buttons_ok=controller_save_buttons(); return controller_save_map(d) && buttons_ok;
}
static int32_t stronger(int32_t a, int32_t b) { return abs(a) >= abs(b) ? a : b; }
// IOHID output runs on the haptics queue; removal synchronously stops that route
// before closing the device. Kept as a seam for packet/lifetime regression tests.
static IOReturn (*steam_write)(IOHIDDeviceRef,IOHIDReportType,CFIndex,const uint8_t *,CFIndex)=IOHIDDeviceSetReport;
static void controller_route_haptics(DMController *d, int player) {
    if (d && d->device) dm_haptics_apple_output(player,d,^(dm_haptic_state state,float gain) {
        uint8_t bytes[10];
        float left=dm_haptics_apple_controller_intensity(dm_haptics_level(&state.motor[0],1,gain).intensity);
        float right=dm_haptics_apple_controller_intensity(dm_haptics_level(&state.motor[1],1,gain).intensity);
        steam_rumble_report(bytes,(uint16_t)(left*65535),(uint16_t)(right*65535));
        IOReturn r=steam_write(d->device,kIOHIDReportTypeOutput,0x80,bytes,sizeof bytes);
        if(r!=kIOReturnSuccess && !d->rumble_warning) {
            fprintf(stderr,"[RED] Steam Controller rumble failed (0x%x)\n",r); d->rumble_warning=YES;
        }
    });
    else dm_haptics_apple_controller(player,d.controller);
}
static void controller_merge_haptics(void) {
    DMController *current=g_merge_haptics_source;
    // A newly connected pad must not steal an ongoing merged-player effect.
    if(current && current->slot>=0 && (current->device || current.controller.haptics.supportedLocalities.count)) return;
    DMController *route=nil;
    for (DMController *pad in g_devices) if (pad->slot>=0) {
        if (!route && pad.controller) route=pad;
        if (pad->device || pad.controller.haptics.supportedLocalities.count) { route=pad; break; }
    }
    if(route!=current) { g_merge_haptics_source=route; controller_route_haptics(route,0); }
}
static void controller_touches(void) {
    dm_touch touches[16]; int n=0; unsigned used=1;
    // Reserve every held ID before allocating a newly pressed pad's finger.
    for(DMController *d in g_devices) for(int i=0;i<2;i++) {
        if(!g_focus || d->slot<0 || d->pad_mode[i]!=PAD_TOUCH || !d->input.pad[i].down) d->touch_id[i]=-1;
        if(d->touch_id[i]>=0) used|=1u<<d->touch_id[i];
    }
    if(g_focus && g_mouse_down) touches[n++]=g_mouse;
    for(DMController *d in g_devices) for(int i=0;i<2;i++) {
        if(!g_focus || d->slot<0 || d->pad_mode[i]!=PAD_TOUCH || !d->input.pad[i].down) continue;
        if(d->touch_id[i]<0) for(int j=1;j<16;j++) if(!(used & (1u<<j))) { d->touch_id[i]=j; used|=1u<<j; break; }
        if(d->touch_id[i]<0) continue;
        steam_pad pad=d->input.pad[i];
        touches[n++]=(dm_touch){(uint32_t)d->touch_id[i], fminf(.999f,(pad.x+32767)/65534.0f), fminf(.999f,(32767-pad.y)/65534.0f)};
    }
    // DM_CTL can hold a scripted touch too. Release only a finger this front
    // end published, rather than clearing scripted input on every pad event.
    if(n || g_touches_published) dm_touch_set(touches,n);
    g_touches_published=n>0;
}
void front_controllers_mouse(const dm_touch *touch) {
    g_mouse_down=g_focus && touch!=NULL; if(touch) { g_mouse=*touch; g_mouse.id=0; } controller_touches();
}
static steam_state controller_effective(DMController *d) {
    if(!g_focus) return (steam_state){0};
    steam_state s=d->input;
    for(int i=0;i<2;i++) if(s.pad[i].down) {
        int32_t x=s.pad[i].x,y=s.pad[i].y;
        if(d->pad_mode[i]==PAD_DPAD) {
            if(x < -8192) s.buttons|=DM_BTN_LEFT; if(x > 8192) s.buttons|=DM_BTN_RIGHT;
            if(y < -8192) s.buttons|=DM_BTN_DOWN; if(y > 8192) s.buttons|=DM_BTN_UP;
        } else if(d->pad_mode[i]==PAD_LEFT_STICK || d->pad_mode[i]==PAD_RIGHT_STICK) {
            if(abs(x)<4096 && abs(y)<4096) x=y=0;
            if(d->pad_mode[i]==PAD_LEFT_STICK) { s.lx=stronger(s.lx,x); s.ly=stronger(s.ly,y); }
            else { s.rx=stronger(s.rx,x); s.ry=stronger(s.ry,y); }
        }
    }
    return s;
}
static void controller_publish(DMController *d) {
    if (g_merge) {
        uint64_t buttons = 0; int32_t lx=0, ly=0, rx=0, ry=0;
        for (DMController *pad in g_devices) if (pad->slot >= 0) {
            steam_state s=controller_effective(pad);
            buttons |= pad_map_buttons(s.buttons, pad->map);
            lx=stronger(lx,s.lx); ly=stronger(ly,s.ly); rx=stronger(rx,s.rx); ry=stronger(ry,s.ry);
        }
        dm_pad_set_player(0, buttons, lx, ly, rx, ry);
    } else if (d->slot >= 0) {
        steam_state s=controller_effective(d);
        dm_pad_set_player(d->slot, pad_map_buttons(s.buttons, d->map), s.lx,s.ly,s.rx,s.ry);
    }
    controller_touches();
}
// Keep upstream's native and passive Pro motion attached to the same admitted slots as buttons.
static void controller_pro_changed(void) {
    int count = 0, player = -1;
    for (DMController *d in g_devices) if (d.controller && [d.controller.productCategory isEqualToString:@"Switch Pro Controller"]) {
        count++; player = d->slot < 0 ? -1 : g_merge ? 0 : d->slot;
    }
    dm_motion_pro_players(count, player);
}
static void controller_attach_motion(DMController *d) {
    if (d.controller && d->slot >= 0 && g_focus && d->gyro_enabled) dm_motion_attach(d.controller, g_merge ? 0 : d->slot);
}
static DMController *controller_motion_source(void) {
    for(DMController *d in g_devices) if(d->slot>=0 && d->gyro_enabled && d->motion.have_tick && controller_now()-d->motion_at<.25) return d;
    return nil;
}
static void controller_publish_motion(DMController *d) {
    if(!g_focus || d->slot<0 || !d->gyro_enabled || !d->motion.have_tick) return;
    if(g_merge) {
        if(controller_motion_source()!=d) return;
        if(g_motion_source!=d) { dm_pad_motion(0,NULL); g_motion_source=d; }
    }
    dm_pad_motion(g_merge ? 0 : d->slot,&d->motion.value);
}
static void controller_reset_motion(DMController *d) {
    BOOL source=!g_merge || g_motion_source==d;
    if(g_motion_source==d) g_motion_source=nil;
    pad_motion_reset(&d->motion);
    if (d.controller && d->slot >= 0) dm_motion_detach(d.controller, g_merge ? 0 : d->slot);
    else if(d->slot>=0 && source) dm_pad_motion(g_merge ? 0 : d->slot,NULL);
}
static void controller_update_focus(void) {
    int was_focused=g_focus;
    g_focus=g_app_focus && g_window_focus && !g_modal;
    dm_haptics_apple_focus(g_focus);
    dm_motion_pro_active(g_focus);
    if(!g_focus) {
        g_mouse_down=0;
        for(DMController *d in g_devices) { controller_reset_motion(d); d->input=(steam_state){0}; controller_publish(d); }
        controller_touches();
    } else if(!was_focused) {
        // Native buttons held through a focus change need no new physical
        // event. Only restore already admitted controllers, never cached peers.
        for(DMController *d in g_devices) if(d.controller && d->slot>=0) { controller_attach_motion(d); controller_read_gc(d,[d.controller.input capture]); }
    }
}
void front_controllers_focus(int active) { g_app_focus=active; controller_update_focus(); }
void front_controllers_window(int active) { g_window_focus=active; controller_update_focus(); }
void front_controllers_modal(int active) { g_modal=active; controller_update_focus(); }
static void controller_connect(DMController *d) {
    if (d->slot >= 0) return;
    d->slot = pad_slot_take(&g_slots, (__bridge const void *)d, d.name.UTF8String);
    if (d->slot < 0) { fprintf(stderr, "controller '%s': all %d players are taken\n", d.name.UTF8String, DM_PLAYERS); return; }
    controller_attach_motion(d);
    controller_pro_changed();
    if (g_merge) controller_merge_haptics(); else controller_route_haptics(d, d->slot);
    dm_pad_players(g_merge ? 1 : pad_slot_mask(&g_slots));
    fprintf(stderr, "controller '%s': player %d\n", d.name.UTF8String, g_merge ? 1 : d->slot + 1);
    controller_panel_refresh();
}
static void controller_disconnect(DMController *d) {
    if (d->slot < 0) return;
    controller_reset_motion(d);
    int slot = d->slot;
    pad_slot_free(&g_slots, (__bridge const void *)d); d->slot = -1;
    controller_pro_changed();
    d->input = (steam_state){0};
    if (g_merge) controller_merge_haptics(); else dm_haptics_apple_controller(slot, nil);
    if (g_merge) controller_publish(d); else dm_pad_set_player(slot, 0, 0, 0, 0, 0);
    dm_pad_players(g_merge ? (pad_slot_mask(&g_slots) ? 1 : 0) : pad_slot_mask(&g_slots));
    controller_touches();
    controller_panel_refresh();
}
static int32_t controller_axis(float v) { return (int32_t)(fmaxf(-1, fminf(1, v)) * DM_STICK_MAX); }
static void controller_read_gc(DMController *d, id<GCDevicePhysicalInputState> state) {
    // A native event's source HID is already published. Scan at admission,
    // after that event exists, so notification/enumeration order cannot let a
    // Triton profile take a native slot before its interface is claimed.
    if(d->slot<0 && !steam_discover()) return;
    // Seizing a HID stops new native events, but macOS can retain its profile
    // and cached buttons. Never allocate a player from that cached snapshot.
    if (state.lastEventTimestamp <= d->gc_not_before) return;
    uint32_t b = 0;
    #define K(name, n) if (state.buttons[name].pressedInput.pressed) b |= 1u << n
    // Normalize Nintendo labels before the shared button map, as on iOS.
    if (dm_pad_labelled(d.controller)) { K(GCInputButtonA,0); K(GCInputButtonB,1); K(GCInputButtonX,2); K(GCInputButtonY,3); }
    else { K(GCInputButtonB,0); K(GCInputButtonA,1); K(GCInputButtonY,2); K(GCInputButtonX,3); }
    K(GCInputLeftThumbstickButton,4); K(GCInputRightThumbstickButton,5);
    K(GCInputLeftShoulder,6); K(GCInputRightShoulder,7); K(GCInputLeftTrigger,8); K(GCInputRightTrigger,9);
    K(GCInputButtonMenu,10); K(GCInputButtonOptions,11);
    id<GCDirectionPadElement> pad=state.dpads[GCInputDirectionPad];
    if(pad.left.pressed) b|=DM_BTN_LEFT; if(pad.up.pressed) b|=DM_BTN_UP;
    if(pad.right.pressed) b|=DM_BTN_RIGHT; if(pad.down.pressed) b|=DM_BTN_DOWN;
    K(GCInputXboxPaddleOne,16); K(GCInputXboxPaddleTwo,17);
    K(GCInputXboxPaddleThree,18); K(GCInputXboxPaddleFour,19);
    K(GCInputBackLeftButton(0),16); K(GCInputBackRightButton(0),17);
    K(GCInputBackLeftButton(1),18); K(GCInputBackRightButton(1),19);
    K(GCInputLeftPaddle,16); K(GCInputRightPaddle,17);
    K(GCInputDualShockTouchpadButton,21);
    K(GCInputButtonHome,22); K(GCInputButtonShare,23);
    #undef K
    id<GCDirectionPadElement> left=state.dpads[GCInputLeftThumbstick], right=state.dpads[GCInputRightThumbstick];
    d->input = (steam_state){.buttons=b, .lx=controller_axis(left.xAxis.value), .ly=controller_axis(left.yAxis.value),
                              .rx=controller_axis(right.xAxis.value), .ry=controller_axis(right.yAxis.value)};
    if(d->slot<0) {
        if(!b && abs(d->input.lx)<4096 && abs(d->input.ly)<4096 && abs(d->input.rx)<4096 && abs(d->input.ry)<4096) return;
        controller_connect(d);
        if(d->slot>=0) steam_native_admission();
    }
    if(!g_focus) d->input=(steam_state){0}; // joining in the mapping panel is allowed
    controller_publish(d);
}
static void controller_gate_gc(DMController *d) {
    // Use GameController's own clock; its epoch is deliberately unspecified.
    id<GCDevicePhysicalInputState> initial=[d.controller.input capture];
    d->gc_not_before=initial.lastEventTimestamp+initial.lastEventLatency;
}
static DMController *controller_add_gc(GCController *c) {
    DMController *d = [DMController new]; d.controller = c;
    d.name = c.vendorName ?: @"Game Controller";
    d.model = [NSString stringWithFormat:@"%@ / %@", d.name, c.productCategory ?: @"Extended Gamepad"];
    controller_load_map(d); [g_devices addObject:d];
    for (NSString *button in @[GCInputButtonHome, GCInputButtonShare])
        c.physicalInputProfile.buttons[button].preferredSystemGestureState = GCSystemGestureStateDisabled;
    c.input.queue = dispatch_get_main_queue();
    controller_gate_gc(d);
    __weak DMController *weak = d;
    c.input.elementValueDidChangeHandler = ^(id<GCDevicePhysicalInput> input, id<GCPhysicalInputElement> el) {
        (void)el; DMController *pad=weak; if (pad) controller_read_gc(pad,[input capture]);
    };
    return d;
}
static void controllers_reconcile(NSArray<GCController *> *now) {
    for (DMController *d in g_devices.copy) if (d.controller && ![now containsObject:d.controller]) {
        d.controller.input.elementValueDidChangeHandler = nil; controller_disconnect(d); [g_devices removeObject:d];
    }
    for(DMController *d in g_devices) if(d->device) {
        if(d->connected) { BOOL opening=d->slot<0; controller_connect(d); if(opening) steam_keepalive(d); }
        else controller_disconnect(d);
    }
    for (GCController *c in now) if (c.extendedGamepad) {
        BOOL found=NO; for (DMController *d in g_devices) if (d.controller == c) found=YES;
        if (!found) controller_add_gc(c);
    }
    controller_pro_changed();
    controller_panel_refresh();
}
static void controllers_changed(GCController *gone) {
    // Native notifications can precede the manager's asynchronous HID callback.
    // Claim new interfaces synchronously before arming ANY new native profile.
    steam_discover();
    NSMutableArray<GCController *> *now=[GCController.controllers mutableCopy]; if(gone) [now removeObject:gone];
    controllers_reconcile(now);
}

// SDL's Triton protocol, using macOS IOHID directly: no external driver/library.
// Prefer direct Triton input so its touchpads/IMU are available even when the
// GameController profile exposes only standard buttons. Exclusive ownership
// prevents duplicate native events without guessing identity from model names.
static IOHIDManagerRef g_hid;
// A native collection and the vendor-report collection can be siblings. Their
// opens commit together; suppression handles never receive protocol callbacks.
@interface DMSteamHID : NSObject {
@public
    IOHIDDeviceRef device;
    uint64_t registry_id;
    int product;
    BOOL protocol, native, opened;
}
@property NSString *group;
@property DMController *raw;
@end
@implementation DMSteamHID
- (void)dealloc { if(device) CFRelease(device); }
@end
static NSMutableDictionary<NSNumber *,DMSteamHID *> *g_steam_hids;
static NSMutableSet<NSString *> *g_steam_fallback;
static NSMutableSet<NSNumber *> *g_steam_attempted;
static long hid_number(IOHIDDeviceRef device, CFStringRef key) {
    CFTypeRef v = IOHIDDeviceGetProperty(device, key); long n = -1;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(v, kCFNumberLongType, &n);
    return n;
}
static void steam_keepalive(DMController *d) {
    if (!d->device || d->slot < 0) return;
    // Temporary lizard-mode off, refreshed every three seconds. Firmware's
    // watchdog restores desktop behavior after this process exits (as in SDL).
    uint8_t feature[64] = {1, 0x87, 3, 9, 0, 0};
    IOReturn r = steam_write(d->device, kIOHIDReportTypeFeature, 1, feature, sizeof feature);
    uint8_t imu[64]; steam_imu_report(imu,d->gyro_enabled);
    IOReturn motion=steam_write(d->device,kIOHIDReportTypeFeature,1,imu,sizeof imu);
    if(r==kIOReturnSuccess) r=motion;
    if (r != kIOReturnSuccess && !d->feature_warning) {
        fprintf(stderr, "[RED] Steam Controller feature report failed (0x%x); quit Steam if it owns this device\n", r); d->feature_warning=YES;
    }
}
static void steam_report(void *context, IOReturn result, void *sender, IOHIDReportType type, uint32_t reportID, uint8_t *bytes, CFIndex length) {
    (void)sender; (void)type; (void)reportID;
    DMController *d = (__bridge DMController *)context;
    if (result != kIOReturnSuccess) { d->connected=NO; controller_disconnect(d); controllers_changed(nil); return; }
    if (length < 0) return;
    steam_state state; int event = steam_decode(bytes, (size_t)length, &state);
    if (event == -1) { d->connected=NO; controller_disconnect(d); controllers_changed(nil); }
    else if (event > 0) {
        BOOL newConnection=!d->connected; d->connected=YES;
        if(newConnection) controllers_changed(nil);
        if(d->slot<0) return;
        if (event == 1) {
            if(g_focus) {
                d->input = state;
                if(d->gyro_enabled && pad_motion_update(&d->motion,state.tick,state.tick_us,state.acc,state.gyro)) { d->motion_at=controller_now(); controller_publish_motion(d); }
            }
            controller_publish(d);
        }
    }
}
static uint64_t steam_registry_id(IOHIDDeviceRef device) {
    uint64_t id=0; IORegistryEntryGetRegistryEntryID(IOHIDDeviceGetService(device),&id); return id;
}
static NSString *steam_group(IOHIDDeviceRef device, int pid, long interface) {
    // USB's registry topology distinguishes controllers without model names or
    // serial guesses. Puck ports 2..5 are independent controllers on one USB
    // device, so use their interface ancestor, not the receiver's identity.
    BOOL puck=pid==0x1304 || pid==0x1305;
    io_registry_entry_t entry=IOHIDDeviceGetService(device);
    IOObjectRetain(entry);
    for(int depth=0; entry && depth<64; depth++) {
        if(IOObjectConformsTo(entry,puck ? "IOUSBHostInterface" : "IOUSBHostDevice")) {
            uint64_t id=0; IORegistryEntryGetRegistryEntryID(entry,&id); IOObjectRelease(entry);
            return id ? [NSString stringWithFormat:@"usb:%llu",(unsigned long long)id] : nil;
        }
        io_registry_entry_t parent=IO_OBJECT_NULL;
        IORegistryEntryGetParentEntry(entry,kIOServicePlane,&parent);
        IOObjectRelease(entry); entry=parent;
    }
    if(entry) IOObjectRelease(entry);
    // Bluetooth has no USB ancestor. Only use the OS's explicit physical ID;
    // a generic parent can be shared by unrelated devices or split collections.
    CFTypeRef value=IOHIDDeviceGetProperty(device,CFSTR(kIOHIDPhysicalDeviceUniqueIDKey));
    if(value && CFGetTypeID(value)==CFStringGetTypeID() && CFStringGetLength(value))
        return [NSString stringWithFormat:@"hid:%d:%@:%ld",pid,(__bridge NSString *)value,puck ? interface : -1];
    return nil;
}
static BOOL steam_protocol(IOHIDDeviceRef device, BOOL *complete) {
    CFArrayRef elements=IOHIDDeviceCopyMatchingElements(device,NULL,kIOHIDOptionsTypeNone);
    if(!elements) { *complete=NO; return NO; }
    uint64_t bits[3]={0};
    for(id value in (__bridge NSArray *)elements) {
        IOHIDElementRef element=(__bridge IOHIDElementRef)value;
        IOHIDElementType type=IOHIDElementGetType(element);
        if(type<kIOHIDElementTypeInput_Misc || type>kIOHIDElementTypeInput_ScanCodes) continue;
        uint32_t report=IOHIDElementGetReportID(element);
        int i=report==0x42 ? 0 : report==0x45 ? 1 : report==0x47 ? 2 : -1;
        if(i>=0) bits[i]+=(uint64_t)IOHIDElementGetReportSize(element)*(IOHIDElementIsArray(element) ? IOHIDElementGetReportCount(element) : 1);
    }
    CFRelease(elements);
    // The decoder needs 46 bytes including the report ID. A standard gamepad
    // collection is suppression-only unless it actually advertises that state.
    return bits[0]>=45*8 || bits[1]>=45*8 || bits[2]>=45*8;
}
static DMSteamHID *steam_identify(IOHIDDeviceRef device, BOOL *complete) {
    int vid=(int)hid_number(device,CFSTR(kIOHIDVendorIDKey)), pid=(int)hid_number(device,CFSTR(kIOHIDProductIDKey));
    if(vid<0 || pid<0) { *complete=NO; return nil; }
    if(vid!=0x28de || (pid!=0x1302 && pid!=0x1303 && pid!=0x1304 && pid!=0x1305)) return nil;
    uint64_t id=steam_registry_id(device);
    if(!id) { *complete=NO; return nil; }
    if(g_steam_hids[@(id)]) return g_steam_hids[@(id)];
    long interface=-1;
    CFTypeRef property=IORegistryEntrySearchCFProperty(IOHIDDeviceGetService(device),kIOServicePlane,CFSTR("bInterfaceNumber"),kCFAllocatorDefault,kIORegistryIterateRecursively|kIORegistryIterateParents);
    if(property) { if(CFGetTypeID(property)==CFNumberGetTypeID()) CFNumberGetValue(property,kCFNumberLongType,&interface); CFRelease(property); }
    long page=hid_number(device,CFSTR(kIOHIDPrimaryUsagePageKey)), usage=hid_number(device,CFSTR(kIOHIDPrimaryUsageKey));
    if(page<0 || usage<0 || (interface<0 && (pid==0x1304 || pid==0x1305))) { *complete=NO; return nil; }
    BOOL native=[GCController supportsHIDDevice:device];
    BOOL protocol=steam_device(vid,pid,(int)interface) && steam_protocol(device,complete);
    if(!protocol && !native) return nil;
    DMSteamHID *hid=[DMSteamHID new]; hid->device=(IOHIDDeviceRef)CFRetain(device);
    hid->registry_id=id; hid->product=pid; hid->protocol=protocol; hid->native=native;
    hid.group=steam_group(device,pid,interface);
    if(!hid.group) fprintf(stderr,"[RED] Steam Controller HID has no stable physical identity; retaining native input.\n");
    return hid;
}
static void steam_close(DMSteamHID *hid) {
    if(!hid->opened) return;
    DMController *d=hid.raw;
    if(d) {
        IOHIDDeviceRegisterInputReportCallback(hid->device,d->report,sizeof d->report,NULL,NULL);
        IOHIDDeviceUnscheduleFromRunLoop(hid->device,CFRunLoopGetMain(),kCFRunLoopCommonModes);
        controller_disconnect(d); [g_devices removeObject:d]; hid.raw=nil;
    }
    IOHIDDeviceClose(hid->device,kIOHIDOptionsTypeNone); hid->opened=NO;
}
static NSDictionary<NSString *,NSMutableArray<DMSteamHID *> *> *steam_groups(void) {
    NSMutableDictionary *groups=[NSMutableDictionary dictionary];
    for(DMSteamHID *hid in g_steam_hids.allValues) if(hid.group) {
        if(!groups[hid.group]) groups[hid.group]=[NSMutableArray array];
        [groups[hid.group] addObject:hid];
    }
    return groups;
}
static void steam_claim(NSString *key, NSArray<DMSteamHID *> *group) {
    if([g_steam_fallback containsObject:key]) return;
    DMSteamHID *protocol=nil; int nprotocol=0;
    for(DMSteamHID *hid in group) if(hid->protocol) { protocol=hid; ++nprotocol; }
    // Wait for a raw sibling during enumeration. If a native event is admitted
    // first, steam_native_admission freezes that group in native fallback.
    if(!nprotocol) return;
    // Unknown multiple protocol endpoints cannot safely select an output route.
    BOOL failed=nprotocol!=1, claimed=NO;
    for(DMSteamHID *hid in group) if(!failed && !hid->opened) {
        [g_steam_attempted addObject:@(hid->registry_id)];
        IOReturn r=IOHIDDeviceOpen(hid->device,kIOHIDOptionsTypeSeizeDevice);
        if(r!=kIOReturnSuccess) {
            fprintf(stderr,"[RED] cannot exclusively open Steam Controller (0x%x); native fallback retained. Quit Steam, check Input Monitoring permissions, then reconnect.\n",r);
            failed=YES;
        } else { hid->opened=YES; claimed=YES; }
    }
    if(failed) {
        // Roll back only this controller's group, never another Puck port.
        for(DMSteamHID *hid in group) steam_close(hid);
        [g_steam_fallback addObject:key]; return;
    }
    if(!claimed) return;
    // Established routes passed their own admission scan. Preserve their slot,
    // held input and rumble; gate only queued input in unadmitted profiles.
    for(DMController *pad in g_devices) if(pad.controller && pad->slot<0) controller_gate_gc(pad);
    if(protocol.raw) return;
    DMController *d=[DMController new]; d->device=(IOHIDDeviceRef)CFRetain(protocol->device); d->registry_id=protocol->registry_id;
    d.name=@"Steam Controller (2026)"; d.model=@"Valve Triton";
    controller_load_map(d); [g_devices addObject:d]; protocol.raw=d;
    IOHIDDeviceRegisterInputReportCallback(d->device,d->report,sizeof d->report,steam_report,(__bridge void *)d);
    IOHIDDeviceScheduleWithRunLoop(d->device,CFRunLoopGetMain(),kCFRunLoopCommonModes);
}
static void steam_native_admission(void) {
    for(NSString *key in steam_groups()) {
        BOOL raw=NO;
        for(DMSteamHID *hid in g_steam_hids.allValues) if([hid.group isEqual:key] && hid.raw) raw=YES;
        if(!raw) [g_steam_fallback addObject:key];
    }
}
static BOOL steam_discover(void) {
    if(!g_hid) return YES; // DM_STEAM_HID=0 leaves GameController in charge.
    if(!g_steam_hids) g_steam_hids=[NSMutableDictionary dictionary];
    if(!g_steam_fallback) g_steam_fallback=[NSMutableSet set];
    CFMutableDictionaryRef match=IOServiceMatching("IOHIDDevice");
    if(!match) return NO;
    ((__bridge NSMutableDictionary *)match)[@kIOHIDVendorIDKey]=@0x28de;
    io_iterator_t iterator=IO_OBJECT_NULL;
    if(IOServiceGetMatchingServices(kIOMainPortDefault,match,&iterator)!=KERN_SUCCESS) return NO;
    if(!iterator) return YES;
    BOOL complete=YES;
    NSMutableArray<DMSteamHID *> *found=[NSMutableArray array];
    io_service_t service;
    while((service=IOIteratorNext(iterator))) {
        IOHIDDeviceRef device=IOHIDDeviceCreate(kCFAllocatorDefault,service);
        if(device) { DMSteamHID *hid=steam_identify(device,&complete); if(hid) [found addObject:hid]; CFRelease(device); }
        else complete=NO;
        IOObjectRelease(service);
    }
    if(!IOIteratorIsValid(iterator)) complete=NO;
    IOObjectRelease(iterator);
    if(!complete) return NO; // no partial ownership transaction
    for(DMSteamHID *hid in found) g_steam_hids[@(hid->registry_id)]=hid;
    NSMutableSet *unknown=[NSMutableSet set];
    for(DMSteamHID *hid in g_steam_hids.allValues) if(!hid.group && hid->native) [unknown addObject:@(hid->product)];
    NSDictionary *groups=steam_groups();
    BOOL native_safe=YES;
    for(NSString *key in groups) {
        NSArray<DMSteamHID *> *group=groups[key];
        if([unknown containsObject:@(group.firstObject->product)]) {
            BOOL opened=NO; for(DMSteamHID *hid in group) if(hid->opened) opened=YES;
            // Missing identity may describe a sibling with asymmetric metadata.
            // Never claim another compatible group behind native fallback. If
            // it appeared after a raw claim, preserve active players and defer
            // new native admission until the ambiguity is gone.
            if(opened) native_safe=NO; else [g_steam_fallback addObject:key];
        } else steam_claim(key,group);
    }
    return native_safe;
}
static void steam_added(void *context, IOReturn result, void *sender, IOHIDDeviceRef device) {
    (void)context; (void)sender; (void)device; if(result!=kIOReturnSuccess) return;
    controllers_changed(nil);
}
static void steam_removed(void *context, IOReturn result, void *sender, IOHIDDeviceRef device) {
    (void)context; (void)result; (void)sender;
    uint64_t id=steam_registry_id(device);
    DMSteamHID *removed=g_steam_hids[@(id)];
    if(removed) {
        NSString *key=removed.group;
        steam_close(removed); [g_steam_hids removeObjectForKey:@(id)];
        // Losing either half invalidates this raw route. Do not seize a late
        // replacement behind native input; wait for the whole group to unplug.
        if(key) {
            BOOL remains=NO;
            for(DMSteamHID *hid in g_steam_hids.allValues) if([hid.group isEqual:key]) { steam_close(hid); remains=YES; }
            if(remains) [g_steam_fallback addObject:key]; else [g_steam_fallback removeObject:key];
        }
    }
    [g_steam_attempted removeObject:@(id)];
    controllers_changed(nil); controller_panel_refresh();
}

// Shared standard buttons; model-specific extra inputs. Physical labels avoid label ambiguity.
static NSArray<NSString *> *source_names(void) {
    return @[@"East (right face)", @"South (bottom face)", @"North (top face)", @"West (left face)",
             @"Left stick press", @"Right stick press", @"Left shoulder", @"Right shoulder", @"Left trigger", @"Right trigger",
             @"Menu / Start", @"View / Select", @"D-pad left", @"D-pad up", @"D-pad right", @"D-pad down",
             @"L4 / Paddle 1", @"R4 / Paddle 2", @"L5 / Paddle 3", @"R5 / Paddle 4", @"Left pad click", @"Right pad click", @"Guide / Home", @"Quick Access / Share"];
}
@interface DMControllerPanel : NSObject
@property NSPanel *panel;
@property NSPopUpButton *devices;
@property NSMutableArray<NSPopUpButton *> *buttons;
@property NSArray<DMController *> *choices;
@property NSMutableArray<NSPopUpButton *> *pads;
@property NSButton *gyro;
@property NSButton *recenter;
@property NSButton *importLegacy;
@property NSTextField *note;
@property NSMutableArray<NSButton *> *presets;
@property DMController *selected;
- (void)refresh;
@end
static DMControllerPanel *g_panel;
@implementation DMControllerPanel
- (void)show:(id)sender {
    (void)sender;
    controller_reload_maps();
    for(DMController *d in g_devices) controller_publish(d);
    if (!self.panel) {
        self.panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0,0,520,770) styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
        self.panel.title = @"Controller Mapping"; self.panel.releasedWhenClosed = NO;
        NSView *v = self.panel.contentView;
        self.devices = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(20,720,480,30)];
        self.devices.target=self; self.devices.action=@selector(selectDevice:); [v addSubview:self.devices];
        self.note=[NSTextField wrappingLabelWithString:@""];
        self.note.frame=NSMakeRect(20,657,480,57); [v addSubview:self.note];
        self.importLegacy=[NSButton buttonWithTitle:@"Import previous mapping for this model" target:self action:@selector(importPrevious:)];
        self.importLegacy.frame=NSMakeRect(20,620,480,30); [v addSubview:self.importLegacy];
        self.pads=[NSMutableArray array];
        for(int i=0;i<2;i++) {
            NSTextField *label=[NSTextField labelWithString:i ? @"Right trackpad" : @"Left trackpad"];
            label.frame=NSMakeRect(20,586-i*32,180,24); [v addSubview:label];
            NSPopUpButton *mode=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(255,582-i*32,235,28)];
            [mode addItemsWithTitles:@[@"Off",@"D-pad",@"Left stick",@"Right stick",@"Switch touchscreen"]];
            mode.tag=i; mode.target=self; mode.action=@selector(mapPad:); [v addSubview:mode]; [self.pads addObject:mode];
        }
        self.gyro=[NSButton checkboxWithTitle:@"Motion controls" target:self action:@selector(mapGyro:)];
        self.gyro.frame=NSMakeRect(20,510,220,28); [v addSubview:self.gyro];
        self.recenter=[NSButton buttonWithTitle:@"Recenter gyro" target:self action:@selector(recenterGyro:)];
        self.recenter.frame=NSMakeRect(255,510,235,28); [v addSubview:self.recenter];
        NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(20,65,480,425)]; scroll.hasVerticalScroller=YES;
        NSView *rows = [[NSView alloc] initWithFrame:NSMakeRect(0,0,455,DM_PAD_BUTTONS*32)];
        self.buttons = [NSMutableArray array];
        NSArray *targets = @[@"Unassigned", @"A", @"B", @"X", @"Y", @"Left stick press", @"Right stick press", @"L", @"R", @"ZL", @"ZR", @"+", @"-", @"D-pad left", @"D-pad up", @"D-pad right", @"D-pad down", @"Shake"];
        for (int i=0; i<DM_PAD_BUTTONS; i++) {
            CGFloat y=(DM_PAD_BUTTONS-i-1)*32;
            NSTextField *label=[NSTextField labelWithString:source_names()[i]]; label.frame=NSMakeRect(0,y+4,230,24); [rows addSubview:label];
            NSPopUpButton *button=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(235,y,210,28)];
            [button addItemsWithTitles:targets]; button.tag=i; button.target=self; button.action=@selector(mapButton:);
            [rows addSubview:button]; [self.buttons addObject:button];
        }
        scroll.documentView=rows; [v addSubview:scroll];
        [scroll.contentView scrollToPoint:NSMakePoint(0, rows.frame.size.height-scroll.contentSize.height)];
        self.presets=[NSMutableArray array];
        for (int i=0; i<2; i++) {
            NSButton *preset=[NSButton buttonWithTitle:i ? @"Xbox Labels" : @"Nintendo Positions" target:self action:@selector(preset:)];
            preset.frame=NSMakeRect(20+i*245,18,235,32); preset.tag=i; [v addSubview:preset]; [self.presets addObject:preset];
        }
        [self.panel center];
    }
    [self refresh]; [self.panel makeKeyAndOrderFront:nil];
}
- (void)refresh {
    if (!self.panel) return;
    NSMutableArray *choices=[NSMutableArray array]; for (DMController *d in g_devices) if (d->slot>=0) [choices addObject:d];
    self.choices=choices;
    if (![choices containsObject:self.selected]) self.selected=choices.firstObject;
    [self.devices removeAllItems];
    for (DMController *d in choices) [self.devices addItemWithTitle:[NSString stringWithFormat:@"Player %d: %@", g_merge ? 1 : d->slot+1, d.name]];
    if (!choices.count) [self.devices addItemWithTitle:@"Connect a controller and press a button to configure it"];
    else [self.devices selectItemAtIndex:[choices indexOfObject:self.selected]];
    self.note.stringValue=g_map_fixed ? @"Standard buttons are fixed by the launch environment or env.txt for this run. Extra inputs remain configurable for this model." : input_preferences_enabled() ? @"Standard button choices apply to every controller for this game and are shared with launchers. Extra inputs, trackpads and motion are saved per model." : @"Saved choices are disabled. Changes apply only to this session. Standard buttons apply to every controller; extra inputs apply to this model.";
    self.importLegacy.hidden=!g_legacy_maps[self.selected.model ?: @""];
    self.importLegacy.enabled=self.selected && !g_map_fixed;
    for(NSButton *preset in self.presets) preset.enabled=self.selected && !g_map_fixed;
    BOOL triton=[self.selected.model isEqual:@"Valve Triton"];
    for(int i=0;i<2;i++) { [self.pads[i] selectItemAtIndex:self.selected ? self.selected->pad_mode[i] : PAD_OFF]; self.pads[i].enabled=triton; }
    self.gyro.enabled=triton; self.gyro.state=self.selected && self.selected->gyro_enabled ? NSControlStateValueOn : NSControlStateValueOff;
    self.recenter.enabled=triton && self.selected->gyro_enabled;
    for (int i=0; i<DM_PAD_BUTTONS; i++) {
        int n=self.selected ? self.selected->map[i] : -1;
        [self.buttons[i] selectItemAtIndex:n==63 ? 17 : n+1]; self.buttons[i].enabled=self.selected && (i>=IM_BUTTONS || !g_map_fixed);
    }
}
- (void)selectDevice:(id)sender { (void)sender; NSInteger n=self.devices.indexOfSelectedItem; if(n>=0 && n<(NSInteger)self.choices.count) self.selected=self.choices[n]; [self refresh]; }
- (void)apply:(BOOL)saved {
    if (!saved) {
        NSAlert *alert=[NSAlert new]; alert.messageText=@"Controller mapping could not be saved"; alert.informativeText=@"The mapping works for this session. Check the app log and available disk space."; [alert runModal];
    }
    for (DMController *d in g_devices) {
        controller_publish(d);
    }
    [self refresh];
}
- (void)mapButton:(NSPopUpButton *)sender {
    if (!self.selected || self.selected->slot<0) return;
    NSInteger n=sender.indexOfSelectedItem; int target=n==17 ? 63 : (int)n-1;
    if(sender.tag<IM_BUTTONS) [self apply:controller_set_button((int)sender.tag,target)];
    else { self.selected->map[sender.tag]=target; [self apply:controller_save_map(self.selected)]; }
}
- (void)mapPad:(NSPopUpButton *)sender { if(self.selected) { self.selected->pad_mode[sender.tag]=(int)sender.indexOfSelectedItem; [self apply:controller_save_map(self.selected)]; } }
- (void)mapGyro:(NSButton *)sender {
    if(!self.selected) return;
    self.selected->gyro_enabled=sender.state==NSControlStateValueOn;
    BOOL ok=controller_save_map(self.selected);
    for(DMController *d in g_devices) if([d.model isEqual:self.selected.model]) { controller_reset_motion(d); controller_attach_motion(d); steam_keepalive(d); }
    [self apply:ok];
}
- (void)recenterGyro:(id)sender { (void)sender; if(self.selected) { controller_reset_motion(self.selected); controller_attach_motion(self.selected); } }
- (void)preset:(NSButton *)sender {
    if(!self.selected || g_map_fixed) return;
    int map[DM_PAD_BUTTONS]; pad_physical_default(map,(int)sender.tag);
    for(int i=0;i<IM_BUTTONS;i++) g_button_map.to[i]=(int8_t)map[i];
    [self apply:controller_save_buttons()];
}
- (void)importPrevious:(id)sender {
    (void)sender; if(!self.selected || g_map_fixed) return;
    BOOL ok=controller_import_map(self.selected);
    for(DMController *d in g_devices) if([d.model isEqual:self.selected.model]) { controller_reset_motion(d); controller_attach_motion(d); steam_keepalive(d); }
    [self apply:ok];
}
@end
static void controller_panel_refresh(void) { [g_panel refresh]; }
NSMenu *front_controllers_menu(void) {
    if (!g_panel) g_panel=[DMControllerPanel new];
    NSMenu *menu=[[NSMenu alloc] initWithTitle:@"Controllers"];
    NSMenuItem *item=[menu addItemWithTitle:@"Button Mapping…" action:@selector(show:) keyEquivalent:@""];
    item.target=g_panel; return menu;
}
void front_controllers_start(void) {
    g_devices=[NSMutableArray array]; g_maps=[NSMutableDictionary dictionary];
    g_controls_domain=input_preferences_domain(); controller_reload_maps();
    NSString *root=getenv("DM_ROOT") ? @(getenv("DM_ROOT")) : [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES)[0] stringByAppendingPathComponent:@"org.nxnative.controllers"];
    g_legacy_maps=[NSDictionary dictionaryWithContentsOfFile:[root stringByAppendingPathComponent:@"controllers.plist"]];
    g_merge=getenv("DM_PAD_MERGE") && atoi(getenv("DM_PAD_MERGE"));
    static const char keyboard=0;
    const char *keyboard_player=getenv("DM_KBD_PLAYER");
    if (!g_merge && keyboard_player && !strcmp(keyboard_player,"own")) pad_slot_take(&g_slots,&keyboard,"keyboard");
    // Independent devices are essential: the manager must not pre-open an
    // interface non-exclusively before steam_attach claims it exclusively.
    const char *steam_hid=getenv("DM_STEAM_HID");
    if (!(steam_hid && !strcmp(steam_hid,"0"))) {
        g_steam_attempted=[NSMutableSet set];
        g_hid=IOHIDManagerCreate(kCFAllocatorDefault,kIOHIDManagerOptionIndependentDevices);
        NSMutableArray *matches=[NSMutableArray array];
        for (NSNumber *pid in @[@0x1302,@0x1303,@0x1304,@0x1305]) [matches addObject:@{@kIOHIDVendorIDKey:@0x28de,@kIOHIDProductIDKey:pid}];
        IOHIDManagerSetDeviceMatchingMultiple(g_hid,(__bridge CFArrayRef)matches);
        IOHIDManagerRegisterDeviceMatchingCallback(g_hid,steam_added,NULL); IOHIDManagerRegisterDeviceRemovalCallback(g_hid,steam_removed,NULL);
        IOHIDManagerScheduleWithRunLoop(g_hid,CFRunLoopGetMain(),kCFRunLoopCommonModes);
        IOReturn r=IOHIDManagerOpen(g_hid,kIOHIDOptionsTypeNone);
        if(r!=kIOReturnSuccess) fprintf(stderr,"[RED] Steam Controller HID access unavailable (0x%x); check macOS Input Monitoring permissions\n",r);
        NSTimer *timer=[NSTimer timerWithTimeInterval:3 repeats:YES block:^(NSTimer *t) { (void)t; for (DMController *d in g_devices) steam_keepalive(d); }];
        [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
    }
    for (NSNotificationName name in @[GCControllerDidConnectNotification,GCControllerDidDisconnectNotification])
        [NSNotificationCenter.defaultCenter addObserverForName:name object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { controllers_changed([n.name isEqual:GCControllerDidDisconnectNotification] ? n.object : nil); }];
    controllers_changed(nil);
    dm_motion_pro_start(controller_pro_changed);
    dm_motion_pro_active(g_focus);
    for(NSNotificationName name in @[NSApplicationDidResignActiveNotification,NSApplicationDidBecomeActiveNotification])
        [NSNotificationCenter.defaultCenter addObserverForName:name object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { front_controllers_focus([n.name isEqual:NSApplicationDidBecomeActiveNotification]); }];
}
