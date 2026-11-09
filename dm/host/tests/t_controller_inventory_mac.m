// The editor and gameplay must exclude the same cached profiles once HID owns their model.
#import <Foundation/Foundation.h>
#include <assert.h>
#define LOG(...) log_discard(__VA_ARGS__)
static void log_discard(const char *format,...) { (void)format; }
static uint64_t now_ns(void) { return 0; }
#include "../pad_hid_mac.h"
static void hidpad_attach(hid_pad *d) { (void)d; }
static void hidpad_detach(hid_pad *d) { (void)d; }
static void hidpad_read(hid_pad *d) { (void)d; }
static void hidpad_motion(hid_pad *d,const pro_motion_sample *s,int n) { (void)d; (void)s; (void)n; }
static void hidpad_models_changed(void) {}

@interface InventoryDevice : NSObject
@property NSString *productCategory, *vendorName;
@end
@implementation InventoryDevice
- (id)extendedGamepad { return self; }
@end
int main(void) { @autoreleasepool {
    InventoryDevice *pro=[InventoryDevice new],*other=[InventoryDevice new];
    pro.productCategory=@"Switch Pro Controller"; other.productCategory=@"DualSense";
    NSArray *all=@[pro,other];
    assert([hidpad_gc_controllers(all) isEqual:all]);
    hid_pad raw={.vid=0x057e,.pid=0x2009,.twin=1,.twin_want=1};
    g_hidpads=&raw;
    assert(([hidpad_gc_controllers(all) isEqual:@[other]]));
    assert([hidpad_gc_controllers(@[(id)pro]) count]==0);
    g_hidpads=NULL; g_hidpads_gc=&raw;
    assert(([hidpad_gc_controllers(all) isEqual:@[other]]));
    raw.twin_want=0;
    assert([hidpad_gc_controllers(all) isEqual:all]);
    g_hidpads_gc=NULL;
    puts("controller inventory: cached HID-owned profiles excluded and native restoration preserved OK");
} return 0; }
