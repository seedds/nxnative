// Mac: wired Xbox 360 (XUSB) and Xbox One (GIP) pads that macOS has no driver for, read directly over USB from the host's own process, with no driver, no kernel extension and no
// entitlement (pad_xusb.h decodes the reports). GameController lists only the pads Apple supports and these pads have no HID interface, so neither GameController nor pad_hid_mac.h sees them.
// front_mac.m owns slots, mappings and focus (xusbpad_attach / xusbpad_detach / xusbpad_read), as for a generic HID pad.
// Only a device whose class is vendor-specific at the device level is opened (ff/ff/ff for an Xbox 360 pad, ff/47/d0 for an Xbox One pad), and then only to claim its controller interface
// (ff/5d/01, or the ff/47/d0 interface that has an interrupt IN and an interrupt OUT endpoint); a hub, a disk or a keyboard is never touched. A pad that connects without an active
// configuration (the ones macOS leaves unconfigured) is given its first one. An Xbox One pad is sent the power-on command once, as every driver does. The pad is read with one pending interrupt
// read on the main run loop, so a USB device that another process has claimed is skipped with a log line and a pad can be read by one process only.
// Not read: the Xbox 360 wireless receiver (another protocol), a pad that needs the Xbox One authentication handshake, rumble, the player light, the Guide button.
// DM_XUSB=0 turns this off, DM_XUSB_LOG=1 logs every report that changes.
#pragma once
#import <CoreFoundation/CoreFoundation.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/IOCFPlugIn.h>
#import <IOKit/IOMessage.h>
#import <IOKit/usb/IOUSBLib.h>
#include "pad_xusb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct xusb_pad {
    IOUSBDeviceInterface942 **dev; IOUSBInterfaceInterface942 **intf; CFRunLoopSourceRef src;
    UInt8 pipe, outpipe; UInt32 maxp; uint8_t report[64]; int gip;   // gip: an Xbox One pad (the power-on command, the GIP report layout), else an Xbox 360 pad
    pad_xusb st; char name[64]; int vid, pid; uint64_t entry; io_object_t interest; struct xusb_pad *next;
} xusb_pad;

static xusb_pad *g_xusb_pads;
static IONotificationPortRef g_xusb_port;
static int g_xusb_log;
static void xusbpad_attach(xusb_pad *d);
static void xusbpad_detach(xusb_pad *d);
static void xusbpad_read(xusb_pad *d);

static int xusb_prop_int(io_service_t s, CFStringRef key, int fallback) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0); int n = fallback;
    if (v) { if (CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &n); CFRelease(v); }
    return n;
}
static void xusb_prop_str(io_service_t s, CFStringRef key, char *out, size_t n) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0);
    if (v) { if (CFGetTypeID(v) == CFStringGetTypeID()) CFStringGetCString((CFStringRef)v, out, (CFIndex)n, kCFStringEncodingUTF8); CFRelease(v); }
}

// A pad gone (unplugged, or a read that failed): the controller leaves its slot first, then the USB objects are released. The run loop source goes first, so no completion of
// the aborted read can reach a pad that is being freed.
static void xusb_drop(xusb_pad *d) {
    for (xusb_pad **p = &g_xusb_pads; *p; p = &(*p)->next) if (*p == d) { *p = d->next; break; }
    xusbpad_detach(d);   // d is already off g_xusb_pads
    if (d->src) CFRunLoopRemoveSource(CFRunLoopGetMain(), d->src, kCFRunLoopCommonModes);
    if (d->intf) { (*d->intf)->AbortPipe(d->intf, d->pipe); (*d->intf)->USBInterfaceClose(d->intf); (*d->intf)->Release(d->intf); }
    if (d->dev) { (*d->dev)->USBDeviceClose(d->dev); (*d->dev)->Release(d->dev); }
    if (d->interest) IOObjectRelease(d->interest);
    fprintf(stderr, "xusb: pad '%s' (%04x:%04x) gone\n", d->name, d->vid, d->pid);
    free(d);
}

static void xusb_arm(xusb_pad *d);
static void xusb_done(void *ref, IOReturn result, void *arg0) {
    xusb_pad *d = ref; UInt32 n = (UInt32)(uintptr_t)arg0;
    if (result != kIOReturnSuccess) { xusb_drop(d); return; }   // an unplugged pad completes its pending read with an error
    pad_xusb s;
    if (d->gip ? pad_gip_parse(d->report, n, &s) : pad_xusb_parse(d->report, n, &s)) {
        if (g_xusb_log && memcmp(&s, &d->st, sizeof s)) fprintf(stderr, "xusb: %04x:%04x b1 %02x b2 %02x lt %u rt %u l(%d,%d) r(%d,%d)\n", d->vid, d->pid, s.b1, s.b2, s.lt, s.rt, s.lx, s.ly, s.rx, s.ry);
        d->st = s; xusbpad_read(d);
    }
    xusb_arm(d);
}
static void xusb_arm(xusb_pad *d) {
    IOReturn r = (*d->intf)->ReadPipeAsync(d->intf, d->pipe, d->report, d->maxp < sizeof d->report ? d->maxp : sizeof d->report, xusb_done, d);
    if (r != kIOReturnSuccess) { fprintf(stderr, "[RED] xusb: cannot read '%s' (0x%x)\n", d->name, r); xusb_drop(d); }
}

static void xusb_interest(void *ref, io_service_t svc, natural_t msg, void *arg) {
    (void)svc; (void)arg;
    if (msg == kIOMessageServiceIsTerminated) xusb_drop((xusb_pad *)ref);
}

// The interface of an open device that is the pad's controller interface: the class says so, and it has an interrupt IN endpoint (an Xbox One pad's audio interfaces have none at their
// first setting) and, for an Xbox One pad, an interrupt OUT endpoint too, to send the power-on command to. On success it is open and its pipes are set; otherwise it is left closed.
static int xusb_claim(xusb_pad *d, IOUSBInterfaceInterface942 **c, int cls, int sub, int pro) {
    if (!(d->gip ? pad_gip_interface_ok(cls, sub, pro) : pad_xusb_interface_ok(cls, sub, pro))) return 0;
    if ((*c)->USBInterfaceOpen(c) != kIOReturnSuccess) {
        fprintf(stderr, "xusb: '%s' (%04x:%04x): the controller interface is in use by another process\n", d->name, d->vid, d->pid);
        return 0;
    }
    UInt8 neps = 0, in = 0, out = 0; UInt16 mp = 0; (*c)->GetNumEndpoints(c, &neps);
    for (UInt8 p = 1; p <= neps; p++) {
        UInt8 dir, num, tt, iv; UInt16 m;
        if ((*c)->GetPipeProperties(c, p, &dir, &num, &tt, &m, &iv) != kIOReturnSuccess || tt != kUSBInterrupt) continue;
        if (dir == kUSBIn && !in) { in = p; mp = m; } else if (dir == kUSBOut && !out) out = p;
    }
    if (!in || (d->gip && !out)) { (*c)->USBInterfaceClose(c); return 0; }
    d->pipe = in; d->outpipe = out; d->maxp = mp;
    return 1;
}

// One USB device the registry reports: a pad's USB device is opened only after its class says it may hold one of this protocol, and kept only when it has the controller interface
static void xusb_open(io_service_t svc, int gip) {
    uint64_t entry = 0; IORegistryEntryGetRegistryEntryID(svc, &entry);
    for (xusb_pad *p = g_xusb_pads; p; p = p->next) if (p->entry == entry) return;
    int dc = xusb_prop_int(svc, CFSTR("bDeviceClass"), -1), ds = xusb_prop_int(svc, CFSTR("bDeviceSubClass"), -1), dp = xusb_prop_int(svc, CFSTR("bDeviceProtocol"), -1);
    if (!(gip ? pad_gip_device_ok(dc, ds, dp) : pad_xusb_device_ok(dc, ds, dp))) return;
    xusb_pad *d = calloc(1, sizeof *d); if (!d) return;
    d->gip = gip; d->entry = entry; d->vid = xusb_prop_int(svc, CFSTR("idVendor"), 0); d->pid = xusb_prop_int(svc, CFSTR("idProduct"), 0);
    xusb_prop_str(svc, CFSTR("USB Product Name"), d->name, sizeof d->name);
    if (!d->name[0]) snprintf(d->name, sizeof d->name, "USB pad %04x:%04x", d->vid, d->pid);
    IOCFPlugInInterface **plug = NULL; SInt32 score;
    kern_return_t kr = IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    if (kr != KERN_SUCCESS || !plug) { fprintf(stderr, "xusb: '%s' (%04x:%04x): no USB plug-in (0x%x)\n", d->name, d->vid, d->pid, kr); free(d); return; }
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID942), (LPVOID *)&d->dev); (*plug)->Release(plug);
    if (!d->dev) { fprintf(stderr, "xusb: '%s' (%04x:%04x): no USB device interface\n", d->name, d->vid, d->pid); free(d); return; }
    IOReturn r = (*d->dev)->USBDeviceOpen(d->dev);
    if (r != kIOReturnSuccess) { fprintf(stderr, "xusb: '%s' (%04x:%04x) cannot be opened (0x%x): another process may hold it\n", d->name, d->vid, d->pid, r); (*d->dev)->Release(d->dev); free(d); return; }
    UInt8 cfg = 0; (*d->dev)->GetConfiguration(d->dev, &cfg);
    if (!cfg) {
        IOUSBConfigurationDescriptorPtr cd = NULL;
        if ((*d->dev)->GetConfigurationDescriptorPtr(d->dev, 0, &cd) == kIOReturnSuccess && cd) (*d->dev)->SetConfiguration(d->dev, cd->bConfigurationValue);
    }
    IOUSBFindInterfaceRequest req = { kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare };
    io_iterator_t ifs = 0; io_service_t isvc;
    if ((*d->dev)->CreateInterfaceIterator(d->dev, &req, &ifs) == kIOReturnSuccess) {
        while (!d->intf && (isvc = IOIteratorNext(ifs))) {
            IOCFPlugInInterface **ip = NULL; IOUSBInterfaceInterface942 **c = NULL;
            if (IOCreatePlugInInterfaceForService(isvc, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &ip, &score) == KERN_SUCCESS && ip) {
                (*ip)->QueryInterface(ip, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID942), (LPVOID *)&c); (*ip)->Release(ip);
                UInt8 cls = 0, sub = 0, pro = 0;
                if (c) { (*c)->GetInterfaceClass(c, &cls); (*c)->GetInterfaceSubClass(c, &sub); (*c)->GetInterfaceProtocol(c, &pro); }
                if (c && xusb_claim(d, c, cls, sub, pro)) d->intf = c; else if (c) (*c)->Release(c);
            }
            IOObjectRelease(isvc);
        }
        IOObjectRelease(ifs);
    }
    if (!d->intf) { if (g_xusb_log) fprintf(stderr, "xusb: '%s' (%04x:%04x) has no usable controller interface\n", d->name, d->vid, d->pid); (*d->dev)->USBDeviceClose(d->dev); (*d->dev)->Release(d->dev); free(d); return; }   // a vendor-specific device that is no pad of this protocol: left as it was found
    if ((*d->intf)->CreateInterfaceAsyncEventSource(d->intf, &d->src) != kIOReturnSuccess || !d->src) {
        fprintf(stderr, "[RED] xusb: '%s' (%04x:%04x) has no run loop source to read with\n", d->name, d->vid, d->pid);
        (*d->intf)->USBInterfaceClose(d->intf); (*d->intf)->Release(d->intf); (*d->dev)->USBDeviceClose(d->dev); (*d->dev)->Release(d->dev); free(d); return;
    }
    CFRunLoopAddSource(CFRunLoopGetMain(), d->src, kCFRunLoopCommonModes);
    d->next = g_xusb_pads; g_xusb_pads = d;
    IOServiceAddInterestNotification(g_xusb_port, svc, kIOGeneralInterest, xusb_interest, d, &d->interest);
    fprintf(stderr, "xusb: %s pad '%s' (%04x:%04x) is read directly over USB\n", gip ? "Xbox One" : "Xbox 360", d->name, d->vid, d->pid);
    if (gip) {   // an Xbox One pad sends nothing until it is told to power on
        uint8_t on[sizeof pad_gip_power_on]; memcpy(on, pad_gip_power_on, sizeof on);
        IOReturn w = (*d->intf)->WritePipe(d->intf, d->outpipe, on, sizeof on);
        if (w != kIOReturnSuccess) fprintf(stderr, "[RED] xusb: '%s': the power-on command failed (0x%x)\n", d->name, w);
    }
    xusbpad_attach(d);
    xusb_arm(d);
}

static void xusb_matched(void *ref, io_iterator_t it) {
    int gip = (int)(intptr_t)ref; io_service_t svc;
    while ((svc = IOIteratorNext(it))) { if (g_xusb_log) fprintf(stderr, "xusb: USB device %llx matched\n", ({ uint64_t id = 0; IORegistryEntryGetRegistryEntryID(svc, &id); id; })); xusb_open(svc, gip); IOObjectRelease(svc); }
}

// One watch per protocol. The registry matches the device's class too, so a hub or a disk never reaches xusb_open. Properties to match go under IOPropertyMatch
static void xusb_watch(int gip) {
    CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostDevice"); if (!m) return;
    int cls = 0xff, sub = gip ? 0x47 : 0xff, pro = gip ? 0xd0 : 0xff;
    CFNumberRef nc = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &cls), ns = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &sub), np = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pro);
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(props, CFSTR("bDeviceClass"), nc); CFDictionarySetValue(props, CFSTR("bDeviceSubClass"), ns); CFDictionarySetValue(props, CFSTR("bDeviceProtocol"), np);
    CFDictionarySetValue(m, CFSTR(kIOPropertyMatchKey), props); CFRelease(props); CFRelease(nc); CFRelease(ns); CFRelease(np);
    io_iterator_t it = 0;   // the matching dictionary is consumed
    if (IOServiceAddMatchingNotification(g_xusb_port, kIOFirstMatchNotification, m, xusb_matched, (void *)(intptr_t)gip, &it) != KERN_SUCCESS) return;
    xusb_matched((void *)(intptr_t)gip, it);   // the pads already connected; a notification's iterator is armed once it is drained
}

static void xusb_start(void) {
    const char *e = getenv("DM_XUSB"); if (e && !atoi(e)) return;
    e = getenv("DM_XUSB_LOG"); g_xusb_log = e && atoi(e);
    g_xusb_port = IONotificationPortCreate(kIOMainPortDefault); if (!g_xusb_port) return;
    CFRunLoopAddSource(CFRunLoopGetMain(), IONotificationPortGetRunLoopSource(g_xusb_port), kCFRunLoopCommonModes);
    xusb_watch(0);
    xusb_watch(1);
}
