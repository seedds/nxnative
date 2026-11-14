// Mac: Xbox One controllers over USB that macOS has no driver for (pad_gip.h), read through IOUSBHost on the main queue alongside front_mac.m's GameController players.
// front_mac.m defines the slot side (gippad_attach / gippad_detach / gippad_read): a pad is a player from its connection, as a GameController controller is; its Xbox button is Home.
// Matched: a USB device of GIP's class (0xFF / 0x47 / 0xD0) that no driver has GIPPAD_WAIT seconds after it appeared (gip_claimed), so a pad macOS's Xbox driver takes
// (Microsoft's) stays with GameController and is never read twice. The device is configured without matching drivers to its interfaces, its interface 0 opened, the init
// packets sent (pad_gip_init) and its interrupt IN pipe read; the pad announcing itself again (a reset) gets them again. No rumble and no player light: GIP's rumble packet
// is not sent yet. DM_GIP_PADS=0 turns this off; DM_GIP_LOG=1 logs each packet that is not an input report.
#pragma once
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <IOUSBHost/IOUSBHost.h>
#include "pad_gip.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct gip_pad {
    pad_gip st; char name[64]; int vid, pid; uint64_t entry; struct gip_pad *next;
    void *dev, *intf, *in, *out;   // IOUSBHostDevice, IOUSBHostInterface and its two IOUSBHostPipes, retained (CFBridgingRetain)
    int gone, pending;             // gone: being removed, so a read that completes is not queued again; pending: requests not completed yet (freed at 0 once gone)
} gip_pad;
static gip_pad *g_gippads;
static IONotificationPortRef g_gip_port;
static int g_gip_log;
#define GIPPAD_WAIT 1.5
static void gippad_attach(gip_pad *d);
static void gippad_detach(gip_pad *d);   // d is already off g_gippads
static void gippad_read(gip_pad *d);

// A request completed: a removed pad is freed with its last one (the close aborts them, and each completion still comes, on the main queue)
static void gip_done(gip_pad *d) { if (--d->pending == 0 && d->gone) free(d); }
static void gip_free(gip_pad *d) { d->gone = 1; if (!d->pending) free(d); }
static void gip_send(gip_pad *d, const uint8_t *p, size_t n) {
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
    if (d->in) CFBridgingRelease(d->in);
    if (d->out) CFBridgingRelease(d->out);
    d->dev = d->intf = d->in = d->out = NULL;
    [intf destroy]; [dev destroy];
}
static void gip_remove(gip_pad *d) {
    gip_pad **link = &g_gippads; while (*link && *link != d) link = &(*link)->next;
    if (!*link) return;   // removed already
    *link = d->next;
    gippad_detach(d);
    gip_close(d); gip_free(d);
}
static void gip_listen(gip_pad *d) {
    IOUSBHostInterface *intf = (__bridge IOUSBHostInterface *)d->intf; IOUSBHostPipe *in = (__bridge IOUSBHostPipe *)d->in; NSError *err = nil;
    NSMutableData *buf = [intf ioDataWithCapacity:64 error:&err];
    if (!buf || ![in enqueueIORequestWithData:buf completionTimeout:0 error:&err completionHandler:^(IOReturn st, NSUInteger n) {
            if (d->gone) { gip_done(d); return; }
            if (st != kIOReturnSuccess) { LOG("controller '%s' (USB, Xbox One): reading stopped (IOReturn 0x%x)", d->name, st); gip_remove(d); gip_done(d); return; }
            uint8_t ack[GIP_PACKET_MAX]; size_t ack_len; const uint8_t *r = buf.bytes;
            if (g_gip_log && n && r[0] != GIP_CMD_INPUT) LOG("[gip] %s: packet 0x%02x, %lu bytes", d->name, r[0], (unsigned long)n);
            int act = pad_gip_decode(&d->st, r, n, ack, &ack_len);
            if (ack_len) gip_send(d, ack, ack_len);
            if (act == 2) { LOG("controller '%s' (USB, Xbox One) announced itself again: started again", d->name); gip_send_init(d); }
            if (act == 1) gippad_read(d);
            gip_listen(d);   // before gip_done: the pad stays allocated while a read is queued
            gip_done(d); }]) {
        LOG("[RED] Xbox One pad '%s': cannot read its reports (%s)", d->name, err.localizedDescription.UTF8String);
        gip_remove(d); return;
    }
    d->pending++;
}
// Whether a driver has the device already. macOS's own Xbox driver puts a service of its own on the device (XboxUSBDevice) and its HID driver on interface 0; interfaces with
// nothing but user clients on them are a configuration without a driver, as an earlier run of this host leaves the pad (it stays configured after the process ends)
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
static io_service_t gip_interface0(io_service_t s) {   // the configured device's interface 0
    io_iterator_t it; if (IORegistryEntryGetChildIterator(s, kIOServicePlane, &it) != KERN_SUCCESS) return 0;
    io_service_t found = 0; io_object_t c;
    while (!found && (c = IOIteratorNext(it))) {
        NSNumber *num = CFBridgingRelease(IORegistryEntryCreateCFProperty(c, CFSTR("bInterfaceNumber"), kCFAllocatorDefault, 0));
        if (IOObjectConformsTo(c, "IOUSBHostInterface") && num && !num.intValue) found = c; else IOObjectRelease(c);
    }
    IOObjectRelease(it); return found;
}
static int gip_int_prop(io_service_t s, CFStringRef key) { NSNumber *n = CFBridgingRelease(IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0)); return n.intValue; }
// The device, GIPPAD_WAIT after it appeared: opened, configured and started unless a driver has it
static void gip_open(uint64_t entry, int tries) {
    for (gip_pad *o = g_gippads; o; o = o->next) if (o->entry == entry) return;
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IORegistryEntryIDMatching(entry)); if (!s) return;   // unplugged meanwhile
    if (gip_claimed(s)) { IOObjectRelease(s); return; }   // GameController reads it (macOS's Xbox driver)
    gip_pad *d = calloc(1, sizeof *d); if (!d) { IOObjectRelease(s); LOG("[RED] Xbox One pad: cannot allocate its state"); return; }
    d->entry = entry; d->vid = gip_int_prop(s, CFSTR("idVendor")); d->pid = gip_int_prop(s, CFSTR("idProduct"));
    NSString *name = CFBridgingRelease(IORegistryEntryCreateCFProperty(s, CFSTR("USB Product Name"), kCFAllocatorDefault, 0));
    snprintf(d->name, sizeof d->name, "%s", name.UTF8String ?: "Xbox One controller");
    NSError *err = nil;
    IOUSBHostDevice *dev = [[IOUSBHostDevice alloc] initWithIOService:s options:IOUSBHostObjectInitOptionsNone queue:dispatch_get_main_queue() error:&err
        interestHandler:^(IOUSBHostObject *o, uint32_t type, void *arg) { (void)o; (void)arg;
            if (type == kIOMessageServiceIsTerminated) dispatch_async(dispatch_get_main_queue(), ^{   // by its registry entry: the pad may be freed already
                for (gip_pad *p = g_gippads; p; p = p->next) if (p->entry == entry) { LOG("controller '%s' (USB, Xbox One) unplugged", p->name); gip_remove(p); return; } }); }];
    IOObjectRelease(s);
    if (!dev) { LOG("[RED] Xbox One pad '%s' (%04x:%04x): cannot open it (%s)", d->name, d->vid, d->pid, err.localizedDescription.UTF8String); free(d); return; }
    d->dev = (void *)CFBridgingRetain(dev);
    if (![dev configureWithValue:1 matchInterfaces:NO error:&err]) {
        LOG("[RED] Xbox One pad '%s': cannot configure it (%s)", d->name, err.localizedDescription.UTF8String); gip_close(d); free(d); return;
    }
    io_service_t is = gip_interface0(dev.ioService);
    if (!is) {   // the interface is registered a moment after the configuration
        gip_close(d); free(d);
        if (tries > 1) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 10), dispatch_get_main_queue(), ^{ gip_open(entry, tries - 1); });
        else LOG("[RED] Xbox One pad: its interface did not appear after the configuration");
        return;
    }
    IOUSBHostInterface *intf = [[IOUSBHostInterface alloc] initWithIOService:is options:IOUSBHostObjectInitOptionsNone queue:dispatch_get_main_queue() error:&err interestHandler:nil];
    IOObjectRelease(is);
    if (!intf) { LOG("[RED] Xbox One pad '%s': cannot open its interface (%s)", d->name, err.localizedDescription.UTF8String); gip_close(d); free(d); return; }
    d->intf = (void *)CFBridgingRetain(intf);
    const IOUSBInterfaceDescriptor *id0 = intf.interfaceDescriptor;
    if (!id0 || id0->bInterfaceClass != GIP_IF_CLASS || id0->bInterfaceSubClass != GIP_IF_SUBCLASS || id0->bInterfaceProtocol != GIP_IF_PROTOCOL) {
        LOG("controller '%s' (%04x:%04x): its interface 0 is not GIP's, not read", d->name, d->vid, d->pid); gip_close(d); free(d); return;
    }
    for (const IOUSBDescriptorHeader *h = NULL; (h = IOUSBGetNextAssociatedDescriptorWithType(intf.configurationDescriptor, (const IOUSBDescriptorHeader *)id0, h, kIOUSBDescriptorTypeEndpoint));) {
        const IOUSBEndpointDescriptor *ep = (const IOUSBEndpointDescriptor *)h;
        if ((ep->bmAttributes & 3) != kIOUSBEndpointTypeInterrupt) continue;
        void **slot = ep->bEndpointAddress & 0x80 ? &d->in : &d->out; if (*slot) continue;
        IOUSBHostPipe *p = [intf copyPipeWithAddress:ep->bEndpointAddress error:&err]; if (p) *slot = (void *)CFBridgingRetain(p);
    }
    if (!d->in || !d->out) { LOG("[RED] Xbox One pad '%s': no interrupt IN and OUT pipes", d->name); gip_close(d); free(d); return; }
    d->next = g_gippads; g_gippads = d;
    LOG("controller '%s' (USB, Xbox One, %04x:%04x): no macOS driver has it: read here", d->name, d->vid, d->pid);
    gip_send_init(d);
    gippad_attach(d);
    gip_listen(d);
}
static void gip_matched(void *ctx, io_iterator_t it) {
    (void)ctx; io_object_t s;
    while ((s = IOIteratorNext(it))) {
        uint64_t entry = 0; IORegistryEntryGetRegistryEntryID(s, &entry); IOObjectRelease(s);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(GIPPAD_WAIT * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ gip_open(entry, 10); });
    }
}
static void gippad_start(void) {
    const char *e = getenv("DM_GIP_PADS"); if (e && !atoi(e)) return;
    e = getenv("DM_GIP_LOG"); g_gip_log = e && atoi(e);
    g_gip_port = IONotificationPortCreate(kIOMainPortDefault); if (!g_gip_port) return;
    IONotificationPortSetDispatchQueue(g_gip_port, dispatch_get_main_queue());
    // By property match: +[IOUSBHostDevice createMatchingDictionary…] gives the keys a driver personality matches on, which IOServiceAddMatchingNotification does not find a device by
    NSDictionary *props = @{@"bDeviceClass": @(GIP_IF_CLASS), @"bDeviceSubClass": @(GIP_IF_SUBCLASS), @"bDeviceProtocol": @(GIP_IF_PROTOCOL)};
    CFDictionaryRef match = (__bridge_retained CFDictionaryRef)@{@kIOProviderClassKey: @"IOUSBHostDevice", @kIOPropertyMatchKey: props};
    io_iterator_t it;
    if (IOServiceAddMatchingNotification(g_gip_port, kIOFirstMatchNotification, match, gip_matched, NULL, &it) != KERN_SUCCESS) {   // consumes match
        LOG("[RED] Xbox One pads: cannot watch USB devices; only GameController controllers are read"); return;
    }
    gip_matched(NULL, it);   // the pads connected already, and arms the notification
}
