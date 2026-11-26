// Mac: wired Xbox One (GIP, pad_gip.h) and Xbox 360 (XUSB, pad_xusb.h) controllers over USB that macOS has no driver for, read through IOUSBHost on the main queue
// alongside front_mac.m's GameController players.
// front_mac.m defines the slot side (gippad_attach / gippad_detach / gippad_read): a pad is a player from its connection, as a GameController controller is; an Xbox One pad's
// Xbox button is Home (XUSB Guide, rumble and player lights are not read yet).
// Matched: a USB device of GIP's class (0xFF / 0x47 / 0xD0) or XUSB's class (0xFF / 0xFF / 0xFF with an 0xFF / 0x5D / 0x01 interface) that no driver has GIPPAD_WAIT seconds
// after it appeared (gip_claimed), so a pad macOS's Xbox driver takes (Microsoft's) stays with GameController and is never read twice. The device is configured without matching
// drivers to its interfaces (only when not configured yet and its configuration descriptor holds a controller interface), its controller interface opened, init packets sent for
// GIP (pad_gip_init), and its interrupt IN pipe read into a reusable DMA buffer; a GIP pad announcing itself again (a reset) clears its state and gets the init packets again.
// DM_GIP_PADS=0 turns GIP off, DM_GIP_LOG=1 logs non-input GIP packets; DM_XUSB=0 turns XUSB off, DM_XUSB_LOG=1 logs decoded XUSB state changes.
#pragma once
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <IOUSBHost/IOUSBHost.h>
#include "pad_gip.h"
#include "pad_xusb.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct gip_pad {
    pad_gip st; pad_xusb xst; int xusb;      // xusb: 1 = wired Xbox 360 (XUSB), 0 = Xbox One (GIP); st carries the decoded Switch state for front_mac.m
    char name[64]; int vid, pid; uint64_t entry; struct gip_pad *next;
    void *dev, *intf, *in, *out, *in_buf;   // IOUSBHostDevice, IOUSBHostInterface, its IOUSBHostPipe(s) and the reusable DMA read buffer, retained (CFBridgingRetain)
    int gone, pending;                      // gone: being removed, so a read that completes is not queued again; pending: requests not completed yet (freed at 0 once gone)
} gip_pad;
static gip_pad *g_gippads;
static IONotificationPortRef g_gip_port;
static io_iterator_t g_gip_iter, g_xusb_iter;
static int g_gip_log, g_xusb_log;
#define GIPPAD_WAIT 1.5
static void gippad_attach(gip_pad *d);
static void gippad_detach(gip_pad *d);   // d is already off g_gippads
static void gippad_read(gip_pad *d);

// A request completed: a removed pad is freed with its last one (the close aborts them, and each completion still comes, on the main queue)
static void gip_done(gip_pad *d) { if (--d->pending == 0 && d->gone) free(d); }
static void gip_free(gip_pad *d) { d->gone = 1; if (!d->pending) free(d); }
static void gip_send(gip_pad *d, const uint8_t *p, size_t n) {
    if (d->gone || !d->out) return;
    IOUSBHostPipe *out = (__bridge IOUSBHostPipe *)d->out; NSError *err = nil;
    NSMutableData *buf = [NSMutableData dataWithBytes:p length:n];
    if ([out enqueueIORequestWithData:buf completionTimeout:0 error:&err completionHandler:^(IOReturn st, NSUInteger done) { (void)buf; (void)done;
            if (st != kIOReturnSuccess && st != kIOReturnAborted && !d->gone) LOG("[RED] Xbox One pad '%s': a packet was not sent (IOReturn 0x%x)", d->name, st);
            gip_done(d); }]) d->pending++;
    else LOG("[RED] Xbox One pad '%s': cannot send a packet (%s)", d->name, err.localizedDescription.UTF8String);
}
static void gip_send_init(gip_pad *d) {
    size_t at = 0, n; uint8_t p[GIP_PACKET_MAX];
    while ((n = pad_gip_init(&d->st, d->vid, d->pid, &at, p))) gip_send(d, p, n);
}
static void gip_close(gip_pad *d) {
    d->gone = 1;
    IOUSBHostInterface *intf = d->intf ? CFBridgingRelease(d->intf) : nil; IOUSBHostDevice *dev = d->dev ? CFBridgingRelease(d->dev) : nil;
    if (d->in_buf) CFBridgingRelease(d->in_buf);
    if (d->in) CFBridgingRelease(d->in);
    if (d->out) CFBridgingRelease(d->out);
    d->dev = d->intf = d->in = d->out = d->in_buf = NULL;
    [intf destroy]; [dev destroy];
}
static void gip_remove(gip_pad *d) {
    gip_pad **link = &g_gippads; while (*link && *link != d) link = &(*link)->next;
    if (!*link) return;   // removed already
    *link = d->next;
    d->gone = 1; d->pending++;   // keep d alive if destroy completes pending requests synchronously
    gippad_detach(d);
    gip_close(d); gip_done(d);
}
static void gip_listen(gip_pad *d) {
    if (d->gone || !d->in || !d->in_buf) return;
    IOUSBHostPipe *in = (__bridge IOUSBHostPipe *)d->in; NSMutableData *buf = (__bridge NSMutableData *)d->in_buf; NSError *err = nil;
    if (![in enqueueIORequestWithData:buf completionTimeout:0 error:&err completionHandler:^(IOReturn st, NSUInteger n) {
            if (d->gone) { gip_done(d); return; }
            if (st != kIOReturnSuccess) { LOG("controller '%s' (USB, %s): reading stopped (IOReturn 0x%x)", d->name, d->xusb ? "Xbox 360" : "Xbox One", st); gip_remove(d); gip_done(d); return; }
            const uint8_t *r = buf.bytes;
            if (d->xusb) {
                if (pad_xusb_decode(&d->xst, r, n)) {
                    d->st.buttons = d->xst.buttons; memcpy(d->st.s, d->xst.s, sizeof d->st.s); d->st.heard = d->xst.heard;
                    if (g_xusb_log) LOG("[xusb] %s (%04x:%04x): buttons 0x%llx sticks (%d,%d) (%d,%d)", d->name, d->vid, d->pid,
                        (unsigned long long)d->st.buttons, d->st.s[0], d->st.s[1], d->st.s[2], d->st.s[3]);
                    gippad_read(d);
                }
            } else {
                uint8_t ack[GIP_PACKET_MAX]; size_t ack_len;
                if (g_gip_log && n && r[0] != GIP_CMD_INPUT) LOG("[gip] %s: packet 0x%02x, %lu bytes", d->name, r[0], (unsigned long)n);
                int act = pad_gip_decode(&d->st, r, n, ack, &ack_len);
                if (ack_len) gip_send(d, ack, ack_len);
                if (act == 2) { LOG("controller '%s' (USB, Xbox One) announced itself again: started again", d->name); pad_gip_reset(&d->st); gippad_read(d); gip_send_init(d); }
                if (act == 1) gippad_read(d);
            }
            gip_listen(d);   // before gip_done: the pad stays allocated while a read is queued
            gip_done(d); }]) {
        LOG("[RED] %s pad '%s': cannot read its reports (%s)", d->xusb ? "Xbox 360" : "Xbox One", d->name, err.localizedDescription.UTF8String);
        gip_remove(d); return;
    }
    d->pending++;
}
// Whether a driver has the device already. macOS's own Xbox driver puts a service of its own on the device (XboxUSBDevice) and its HID driver on the controller interface;
// interfaces with nothing but user clients on them are a configuration without a driver, as an earlier run of this host leaves the pad (it stays configured after the process ends)
static int gip_has_driver(io_registry_entry_t s, int depth) {
    io_iterator_t it; if (IORegistryEntryGetChildIterator(s, kIOServicePlane, &it) != KERN_SUCCESS) return 1;
    int claimed = 0; io_object_t c;
    while (!claimed && (c = IOIteratorNext(it))) {
        if (IOObjectConformsTo(c, "IOUserClient")) {}
        else if (!depth && IOObjectConformsTo(c, "IOUSBHostInterface")) claimed = gip_has_driver(c, 1);
        else claimed = 1;
        IOObjectRelease(c);
    }
    IOObjectRelease(it); return claimed;
}
static int gip_claimed(io_service_t s) { return gip_has_driver(s, 0); }
static int gip_int_prop(io_service_t s, CFStringRef key) { NSNumber *n = CFBridgingRelease(IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0)); return n.intValue; }
static int gip_if_class_ok(int xusb, int cls, int sub, int pro) {
    return xusb ? pad_xusb_interface_ok(cls, sub, pro) : (cls == GIP_IF_CLASS && sub == GIP_IF_SUBCLASS && pro == GIP_IF_PROTOCOL);
}
static int gip_config_has_interface(const IOUSBConfigurationDescriptor *cd, int xusb) {
    const IOUSBInterfaceDescriptor *id = NULL;
    while ((id = IOUSBGetNextInterfaceDescriptor(cd, (const IOUSBDescriptorHeader *)id)))
        if (gip_if_class_ok(xusb, id->bInterfaceClass, id->bInterfaceSubClass, id->bInterfaceProtocol)) return 1;
    return 0;
}
// Finds and opens the controller interface (GIP: ff/47/d0 with IN + OUT interrupt pipes; XUSB: ff/5d/01 with IN interrupt pipe). Returns its read buffer size, or 0 if none is ready.
static NSUInteger gip_open_interface(IOUSBHostDevice *dev, gip_pad *d, int *any_if, NSError **err_out) {
    *any_if = 0;
    io_iterator_t it; if (IORegistryEntryGetChildIterator(dev.ioService, kIOServicePlane, &it) != KERN_SUCCESS) return 0;
    NSUInteger in_cap = 0; io_object_t c;
    while (!in_cap && (c = IOIteratorNext(it))) {
        if (!IOObjectConformsTo(c, "IOUSBHostInterface")) { IOObjectRelease(c); continue; }
        (*any_if)++;
        int cls = gip_int_prop(c, CFSTR("bInterfaceClass")), sub = gip_int_prop(c, CFSTR("bInterfaceSubClass")), pro = gip_int_prop(c, CFSTR("bInterfaceProtocol"));
        if (!gip_if_class_ok(d->xusb, cls, sub, pro)) { IOObjectRelease(c); continue; }
        NSError *err = nil;
        IOUSBHostInterface *intf = [[IOUSBHostInterface alloc] initWithIOService:c options:IOUSBHostObjectInitOptionsNone queue:dispatch_get_main_queue() error:&err interestHandler:nil];
        IOObjectRelease(c);
        if (!intf) { if (err_out && !*err_out) *err_out = err; continue; }
        const IOUSBInterfaceDescriptor *id0 = intf.interfaceDescriptor;
        if (!id0 || !gip_if_class_ok(d->xusb, id0->bInterfaceClass, id0->bInterfaceSubClass, id0->bInterfaceProtocol)) { [intf destroy]; continue; }
        IOUSBHostPipe *in_p = nil, *out_p = nil; NSUInteger cap = 64;
        for (const IOUSBDescriptorHeader *h = NULL; (h = IOUSBGetNextAssociatedDescriptorWithType(intf.configurationDescriptor, (const IOUSBDescriptorHeader *)id0, h, kIOUSBDescriptorTypeEndpoint));) {
            const IOUSBEndpointDescriptor *ep = (const IOUSBEndpointDescriptor *)h;
            if ((ep->bmAttributes & 3) != kIOUSBEndpointTypeInterrupt) continue;
            int is_in = (ep->bEndpointAddress & 0x80) != 0;
            if ((is_in && in_p) || (!is_in && out_p)) continue;
            IOUSBHostPipe *p = [intf copyPipeWithAddress:ep->bEndpointAddress error:&err];
            if (p) {
                if (is_in) { in_p = p; NSUInteger mps = ep->wMaxPacketSize & 0x7ff; if (mps > cap) cap = mps; }
                else out_p = p;
            }
        }
        if (!in_p || (!d->xusb && !out_p)) {
            if (err_out && !*err_out) *err_out = err;
            [intf destroy]; continue;
        }
        d->intf = (void *)CFBridgingRetain(intf);
        d->in = (void *)CFBridgingRetain(in_p);
        if (out_p) d->out = (void *)CFBridgingRetain(out_p);
        in_cap = cap;
    }
    IOObjectRelease(it);
    return in_cap;
}
// The device, GIPPAD_WAIT after it appeared: opened, configured once if unconfigured and holding a matching interface, and started unless a driver has it
static void gip_open(uint64_t entry, int xusb, int tries, int configured) {
    for (gip_pad *o = g_gippads; o; o = o->next) if (o->entry == entry) return;
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IORegistryEntryIDMatching(entry)); if (!s) return;   // unplugged meanwhile
    if (gip_claimed(s)) { IOObjectRelease(s); return; }   // GameController reads it (macOS's Xbox driver)
    gip_pad *d = calloc(1, sizeof *d); if (!d) { IOObjectRelease(s); LOG("[RED] Xbox USB pad: cannot allocate its state"); return; }
    const char *kind = xusb ? "Xbox 360" : "Xbox One";
    d->xusb = xusb; d->entry = entry; d->vid = gip_int_prop(s, CFSTR("idVendor")); d->pid = gip_int_prop(s, CFSTR("idProduct"));
    NSString *name = CFBridgingRelease(IORegistryEntryCreateCFProperty(s, CFSTR("USB Product Name"), kCFAllocatorDefault, 0));
    snprintf(d->name, sizeof d->name, "%s", name.UTF8String ?: (xusb ? "Xbox 360 controller" : "Xbox One controller"));
    NSError *err = nil;
    IOUSBHostDevice *dev = [[IOUSBHostDevice alloc] initWithIOService:s options:IOUSBHostObjectInitOptionsNone queue:dispatch_get_main_queue() error:&err
        interestHandler:^(IOUSBHostObject *o, uint32_t type, void *arg) { (void)o; (void)arg;
            if (type == kIOMessageServiceIsTerminated) dispatch_async(dispatch_get_main_queue(), ^{   // by its registry entry: the pad may be freed already
                for (gip_pad *p = g_gippads; p; p = p->next) if (p->entry == entry) { LOG("controller '%s' (USB, %s) unplugged", p->name, p->xusb ? "Xbox 360" : "Xbox One"); gip_remove(p); return; } }); }];
    IOObjectRelease(s);
    if (!dev) { LOG("[RED] %s pad '%s' (%04x:%04x): cannot open it (%s)", kind, d->name, d->vid, d->pid, err.localizedDescription.UTF8String); free(d); return; }
    d->dev = (void *)CFBridgingRetain(dev);
    int any_if = 0; NSUInteger in_cap = gip_open_interface(dev, d, &any_if, &err);
    if (!any_if && !configured) {   // configure once; calling configureWithValue: again tears down interfaces still registering
        const IOUSBConfigurationDescriptor *cd = [dev configurationDescriptorWithIndex:0 error:nil];
        if (cd && !gip_config_has_interface(cd, xusb)) {
            if (xusb ? g_xusb_log : g_gip_log) LOG("controller '%s' (%04x:%04x): no %s interface in configuration, not configured", d->name, d->vid, d->pid, kind);
            gip_close(d); free(d); return;
        }
        NSUInteger cfg_val = cd ? cd->bConfigurationValue : 1;
        if (![dev configureWithValue:cfg_val matchInterfaces:NO error:&err]) {
            LOG("[RED] %s pad '%s': cannot configure it (%s)", kind, d->name, err.localizedDescription.UTF8String); gip_close(d); free(d); return;
        }
        configured = 1;
        in_cap = gip_open_interface(dev, d, &any_if, &err);
    }
    if (!in_cap) {
        if (any_if) {
            if (err) LOG("[RED] %s pad '%s': cannot open its interface (%s)", kind, d->name, err.localizedDescription.UTF8String);
            else if (!xusb || g_xusb_log) LOG("controller '%s' (%04x:%04x): no usable %s interface, not read", d->name, d->vid, d->pid, kind);
        }
        gip_close(d); free(d);
        if (!any_if) {   // the interface is registered a moment after the configuration
            if (tries > 1) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 10), dispatch_get_main_queue(), ^{ gip_open(entry, xusb, tries - 1, configured); });
            else LOG("[RED] %s pad: its interface did not appear after the configuration", kind);
        }
        return;
    }
    NSMutableData *in_buf = [( __bridge IOUSBHostInterface *)d->intf ioDataWithCapacity:in_cap error:&err];
    if (!in_buf) { LOG("[RED] %s pad '%s': cannot allocate its read buffer (%s)", kind, d->name, err.localizedDescription.UTF8String); gip_close(d); free(d); return; }
    d->in_buf = (void *)CFBridgingRetain(in_buf);
    d->next = g_gippads; g_gippads = d;
    LOG("controller '%s' (USB, %s, %04x:%04x): no macOS driver has it: read here", d->name, kind, d->vid, d->pid);
    if (!xusb) gip_send_init(d);
    gippad_attach(d);
    gip_listen(d);
}
static void gip_matched(void *ctx, io_iterator_t it) {
    int xusb = (int)(intptr_t)ctx; io_object_t s;
    while ((s = IOIteratorNext(it))) {
        uint64_t entry = 0; IORegistryEntryGetRegistryEntryID(s, &entry); IOObjectRelease(s);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(GIPPAD_WAIT * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ gip_open(entry, xusb, 10, 0); });
    }
}
static int gip_watch(int xusb, int cls, int sub, int pro, io_iterator_t *iter) {
    // By property match: +[IOUSBHostDevice createMatchingDictionary…] gives the keys a driver personality matches on, which IOServiceAddMatchingNotification does not find a device by
    NSDictionary *props = @{@"bDeviceClass": @(cls), @"bDeviceSubClass": @(sub), @"bDeviceProtocol": @(pro)};
    CFDictionaryRef match = (__bridge_retained CFDictionaryRef)@{@kIOProviderClassKey: @"IOUSBHostDevice", @kIOPropertyMatchKey: props};
    if (IOServiceAddMatchingNotification(g_gip_port, kIOFirstMatchNotification, match, gip_matched, (void *)(intptr_t)xusb, iter) != KERN_SUCCESS) return 0;
    gip_matched((void *)(intptr_t)xusb, *iter);   // the pads connected already, and arms the notification
    return 1;
}
static void gippad_start(void) {
    const char *e = getenv("DM_GIP_PADS"); int gip_on = !(e && !atoi(e));
    e = getenv("DM_XUSB"); int xusb_on = !(e && !atoi(e));
    e = getenv("DM_GIP_LOG"); g_gip_log = e && atoi(e);
    e = getenv("DM_XUSB_LOG"); g_xusb_log = e && atoi(e);
    if (!gip_on && !xusb_on) return;
    g_gip_port = IONotificationPortCreate(kIOMainPortDefault); if (!g_gip_port) return;
    IONotificationPortSetDispatchQueue(g_gip_port, dispatch_get_main_queue());
    int ok = 0;
    if (gip_on) {
        if (gip_watch(0, GIP_IF_CLASS, GIP_IF_SUBCLASS, GIP_IF_PROTOCOL, &g_gip_iter)) ok = 1;
        else LOG("[RED] Xbox One pads: cannot watch USB devices; only GameController controllers are read");
    }
    if (xusb_on) {
        if (gip_watch(1, XUSB_DEV_CLASS, XUSB_DEV_SUBCLASS, XUSB_DEV_PROTOCOL, &g_xusb_iter)) ok = 1;
        else LOG("[RED] Xbox 360 pads: cannot watch USB devices; only GameController controllers are read");
    }
    if (!ok) { IONotificationPortDestroy(g_gip_port); g_gip_port = NULL; }
}
