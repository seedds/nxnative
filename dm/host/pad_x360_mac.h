// Mac: the Xbox 360 Wireless Gaming Receiver (045e:0291 / 0719 / 02a9) read over USB, its controllers played as raw pads (pad_hid_mac.h's hid_pad, one player slot each).
// The receiver is no HID device: each of its four controller slots is a vendor interface (class FF, subclass 5D, protocol 81) with an interrupt IN and OUT endpoint, which
// neither GameController nor HID lists, so a game saw no controller. macOS has no driver for those interfaces, so a user process opens them with IOUSBHost and no entitlement.
// The reports are decoded by pad_x360.h. OUT commands are 12 bytes: the LED (00 00 08 40+mode, 6..9 the quadrant of slot 1..4), the presence query (08 00 0F C0) and rumble
// (00 01 0F C0 00 <strong> <weak>), which haptics_apple.m's two-motor route sets (dm_haptics_apple_motors). The interfaces are opened exclusively, so another program holding
// them (a user-space driver such as x360w) keeps them, and one log line says so. All of it runs on the main queue, as the raw HID pads do. DM_X360=0 turns it off.
#pragma once
#import <IOUSBHost/IOUSBHost.h>
#include <pthread.h>
#include "haptics_apple.h"
#include "pad_x360.h"

static int hidpad_player(hid_pad *d);   // the player a raw pad holds, -1 none (front_mac.m)

@interface DmX360Slot : NSObject {
@public
    IOUSBHostInterface *iface;
    IOUSBHostPipe *in, *out;
    NSMutableData *buf;
    hid_pad *pad;   // on g_hidpads while a controller is present
    int rumble_p;   // the player its rumble route is set for, -1 none: with DM_PAD_PRESS_TO_JOIN a pad has no player until its first button
    int slot;       // 0..3, the receiver's slot (its LED quadrant)
    int gone;
}
@end
@implementation DmX360Slot
@end

static NSMutableArray<DmX360Slot *> *g_x360;
static IONotificationPortRef g_x360_port;
static io_iterator_t g_x360_iter;

static void x360_send(DmX360Slot *s, const uint8_t cmd[12]) {
    if (!s->out || s->gone) return;
    NSMutableData *d = [s->iface ioDataWithCapacity:12 error:nil];
    if (!d || d.length < 12) return;   // an IOUSBHost buffer's length is its capacity and cannot be changed
    memcpy(d.mutableBytes, cmd, 12);
    NSError *err = nil;
    if (![s->out enqueueIORequestWithData:d completionTimeout:0 error:&err completionHandler:^(IOReturn st, NSUInteger n) { (void)st; (void)n; (void)d; }])
        LOG("Xbox 360 receiver slot %d: command not sent (%s)", s->slot + 1, err.localizedDescription.UTF8String ?: "?");
}
static void x360_led(DmX360Slot *s, int on) { uint8_t c[12] = {0x00, 0x00, 0x08, (uint8_t)(0x40 + (on ? 6 + s->slot : 0))}; x360_send(s, c); }
static void x360_motors(DmX360Slot *s, uint8_t strong, uint8_t weak) { uint8_t c[12] = {0x00, 0x01, 0x0f, 0xc0, 0x00, strong, weak}; x360_send(s, c); }
// Rumble for player p (on the haptics queue, one call per change): to the slot whose controller holds p, on the main queue where the slots live
static void x360_rumble(int p, uint8_t strong, uint8_t weak) {
    dispatch_async(dispatch_get_main_queue(), ^{
        for (DmX360Slot *s in g_x360) if (s->pad && hidpad_player(s->pad) == p) x360_motors(s, strong, weak);
    });
}

static void x360_rumble_route(DmX360Slot *s) {   // the rumble route for the player its controller holds, once it holds one
    int p = hidpad_player(s->pad); if (p < 0) return;
    s->rumble_p = p; dm_haptics_apple_motors(p, x360_rumble);
}
static void x360_present(DmX360Slot *s) {
    if (s->pad) return;
    hid_pad *d = calloc(1, sizeof *d); if (!d) { LOG("[RED] Xbox 360 receiver: cannot allocate the controller's state"); return; }
    pad_x360_init(&d->st, getenv("DM_X360_DEAD"));
    snprintf(d->name, sizeof d->name, "Xbox 360 Wireless Controller %d", s->slot + 1);
    d->next = g_hidpads; g_hidpads = d; s->pad = d;
    hidpad_attach(d);
    x360_led(s, 1);
    s->rumble_p = -1; x360_rumble_route(s);
}
static void x360_absent(DmX360Slot *s) {
    hid_pad *d = s->pad; if (!d) return;
    if (s->rumble_p >= 0) dm_haptics_apple_motors(s->rumble_p, NULL);
    s->rumble_p = -1;
    x360_motors(s, 0, 0);
    s->pad = NULL;
    hid_pad **link = &g_hidpads;
    while (*link && *link != d) link = &(*link)->next;
    if (*link) *link = d->next;
    hidpad_detach(d);
    free(d);
}
static void x360_packet(DmX360Slot *s, const uint8_t *d, NSUInteger n) {
    pad_hid st = s->pad ? s->pad->st : (pad_hid){0};
    int r = pad_x360_report(&st, d, n);
    if (r & X360_ABSENT) { x360_absent(s); return; }
    if (r & (X360_PRESENT | X360_INPUT)) x360_present(s);   // input proves presence too (a missed presence report)
    if (!(r & X360_INPUT) || !s->pad) return;
    pad_hid *h = &s->pad->st;
    h->buttons = st.buttons; h->hat = st.hat; memcpy(h->axis, st.axis, sizeof h->axis);
    hidpad_read(s->pad);
    if (s->pad && s->rumble_p < 0) x360_rumble_route(s);   // it joined at this press (DM_PAD_PRESS_TO_JOIN)
}
static void x360_close(DmX360Slot *s) {
    if (s->gone) return;
    x360_absent(s);
    s->gone = 1;
    [s->iface destroy];
    [g_x360 removeObject:s];
}
static void x360_read(DmX360Slot *s) {
    if (s->gone) return;
    NSError *err = nil;
    BOOL ok = [s->in enqueueIORequestWithData:s->buf completionTimeout:0 error:&err completionHandler:^(IOReturn st, NSUInteger n) {
        if (st == kIOReturnSuccess) { x360_packet(s, s->buf.bytes, n); x360_read(s); return; }
        if (st != kIOReturnAborted) LOG("Xbox 360 receiver slot %d: read stopped (0x%x)", s->slot + 1, st);
        x360_close(s);
    }];
    if (!ok) { LOG("Xbox 360 receiver slot %d: read not queued (%s)", s->slot + 1, err.localizedDescription.UTF8String ?: "?"); x360_close(s); }
}
static void x360_open(io_service_t svc) {
    DmX360Slot *s = [DmX360Slot new];
    NSError *err = nil;
    __weak DmX360Slot *weak = s;
    s->iface = [[IOUSBHostInterface alloc] initWithIOService:svc options:IOUSBHostObjectInitOptionsNone queue:dispatch_get_main_queue() error:&err
                                             interestHandler:^(IOUSBHostObject *o, uint32_t type, void *arg) {
        (void)o; (void)arg;
        DmX360Slot *t = weak;
        if (t && type == kIOMessageServiceIsTerminated) x360_close(t);
    }];
    if (!s->iface) {
        static int said;
        if (!said++) LOG("Xbox 360 receiver: cannot open a controller slot (%s): another program holds it (quit it, such as a user-space driver, and start the game again)",
                         err.localizedDescription.UTF8String ?: "?");
        return;
    }
    const IOUSBConfigurationDescriptor *cfg = s->iface.configurationDescriptor; const IOUSBInterfaceDescriptor *ifd = s->iface.interfaceDescriptor;
    const IOUSBDescriptorHeader *cur = (const IOUSBDescriptorHeader *)ifd; const IOUSBEndpointDescriptor *ep;
    while (cfg && ifd && (ep = IOUSBGetNextEndpointDescriptor(cfg, ifd, cur))) {
        cur = (const IOUSBDescriptorHeader *)ep;
        if ((ep->bmAttributes & 3) != 3) continue;   // interrupt endpoints only
        IOUSBHostPipe *pipe = [s->iface copyPipeWithAddress:ep->bEndpointAddress error:&err];
        if (ep->bEndpointAddress & 0x80) s->in = pipe; else s->out = pipe;
    }
    s->buf = [s->iface ioDataWithCapacity:32 error:&err];
    if (!s->in || !s->out || !s->buf) { LOG("Xbox 360 receiver: a controller slot has no input or output pipe; not used"); [s->iface destroy]; return; }
    s->slot = ifd->bInterfaceNumber / 2 < 4 ? ifd->bInterfaceNumber / 2 : (int)g_x360.count;   // interfaces 0, 2, 4, 6 are slots 1..4 (1, 3, 5, 7: the headsets)
    [g_x360 addObject:s];
    if (g_x360.count == 1) LOG("Xbox 360 wireless receiver: reading its controllers (DM_X360=0 turns this off)");
    x360_read(s);
    uint8_t q[12] = {0x08, 0x00, 0x0f, 0xc0}; x360_send(s, q);   // the receiver reports which controllers are on
}
static void x360_matched(void *ctx, io_iterator_t it) {
    (void)ctx; io_service_t svc;
    while ((svc = IOIteratorNext(it))) { x360_open(svc); IOObjectRelease(svc); }
}
static void x360_quiet(void) {   // at exit: the receiver keeps a controller's last rumble after the program is gone
    if (!pthread_main_np()) return;
    for (DmX360Slot *s in g_x360) if (s->pad) x360_motors(s, 0, 0);
    usleep(20000);
}
static void x360_start(void) {
    const char *e = getenv("DM_X360"); if (e && !atoi(e)) return;
    g_x360 = [NSMutableArray new];
    atexit(x360_quiet);
    // IOServiceMatching (a Create function: owned, consumed below) rather than IOUSBHostInterface's createMatchingDictionary..., which hands over an owned dictionary from an
    // Objective-C method the static analyzer takes as not owned by its name
    CFMutableDictionaryRef m = IOServiceMatching(kIOUSBHostInterfaceClassName);
    if (m) {
        NSMutableDictionary *md = (__bridge NSMutableDictionary *)m;
        md[@kUSBHostMatchingPropertyVendorID] = @0x045e; md[@kUSBHostMatchingPropertyProductIDArray] = @[@0x0291, @0x0719, @0x02a9];
        md[@kUSBHostMatchingPropertyInterfaceClass] = @0xff; md[@kUSBHostMatchingPropertyInterfaceSubClass] = @0x5d; md[@kUSBHostMatchingPropertyInterfaceProtocol] = @0x81;
    }
    g_x360_port = IONotificationPortCreate(kIOMainPortDefault); if (!g_x360_port || !m) { if (m) CFRelease(m); return; }
    IONotificationPortSetDispatchQueue(g_x360_port, dispatch_get_main_queue());
    if (IOServiceAddMatchingNotification(g_x360_port, kIOFirstMatchNotification, m, x360_matched, NULL, &g_x360_iter) != KERN_SUCCESS) return;   // takes m
    x360_matched(NULL, g_x360_iter);   // the receivers already plugged in; this also arms the notification for later ones
}
