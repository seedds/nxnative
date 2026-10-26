// Exercise the real Objective-C adapter with asset-free reflection objects.
// No MTLDevice or GPU is created. These are plain test objects, not game inputs.
#import <Foundation/Foundation.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static unsigned g_logged;
static int g_seen_writer, g_seen_atomic;
static void storage_access_test_log(const char *fmt, ...) {
    char line[512]; va_list args; va_start(args, fmt); vsnprintf(line, sizeof line, fmt, args); va_end(args);
    g_logged++;
    g_seen_writer |= strstr(line, "aid=7 ") && strstr(line, "access=1 rw=0->1 promoted=1");
    g_seen_atomic |= strstr(line, "aid=11 ") && strstr(line, "access=2 rw=0->1 promoted=1");
}
#define NVN_STORAGE_ACCESS_LOG(...) storage_access_test_log(__VA_ARGS__)
#include "../nvn/nvn_storage_access_metal.h"
#undef NVN_STORAGE_ACCESS_LOG
#include <assert.h>

@interface AccessPointer : NSObject
@property(nonatomic) NSUInteger access;
@property(nonatomic) BOOL elementIsArgumentBuffer;
@end
@implementation AccessPointer @end
@interface AccessMember : NSObject
@property(nonatomic) NSUInteger argumentIndex, offset, dataType;
@property(nonatomic, strong) AccessPointer *pointerType;
@end
@implementation AccessMember @end
@interface AccessStruct : NSObject
@property(nonatomic, strong) NSArray *members;
@end
@implementation AccessStruct @end
@interface AccessArgument : NSObject
@property(nonatomic) NSUInteger type, index, bufferDataType, access;
@property(nonatomic, strong) AccessStruct *bufferStructType;
@end
@implementation AccessArgument @end
@interface AccessFunction : NSObject
@property(nonatomic, strong) id encoder;
@property(nonatomic) NSUInteger fallbacks;
@end
@implementation AccessFunction
- (id)newArgumentEncoderWithBufferIndex:(NSUInteger)index reflection:(id __autoreleasing *)reflection {
    (void)index; if (reflection) *reflection = self; return nil;
}
- (id)newArgumentEncoderWithBufferIndex:(NSUInteger)index {
    (void)index; self.fallbacks++; return self.encoder;
}
@end

static AccessMember *member(NSUInteger aid, NSUInteger access) {
    AccessMember *m = [AccessMember new]; m.argumentIndex = aid; m.offset = aid * 8; m.dataType = MTLDataTypePointer;
    m.pointerType = [AccessPointer new]; m.pointerType.access = access; return m;
}
int main(void) { @autoreleasepool {
    setenv("DM_NVN_STORAGE_ACCESS_DIAG", "adapter", 1);
    AccessMember *reader = member(2, 0), *writer = member(7, 1), *atomic = member(11, 2);
    AccessArgument *a = [AccessArgument new]; a.type = 0; a.index = 0; a.bufferDataType = MTLDataTypeStruct;
    a.access = 0; // outer constant AB is read-only even when its pointees write
    a.bufferStructType = [AccessStruct new]; a.bufferStructType.members = @[atomic, reader, writer];
    nvn_refl rf = {.route_b = 1, .abi = 0, .nb = 5};
    rf.b[0] = (nvn_rbind){.kind = NVN_RB_SBUFP, .aid = 2, .reg = 0, .rw = 1};
    rf.b[1] = (nvn_rbind){.kind = NVN_RB_SBUFP, .aid = 7, .reg = 1};
    rf.b[2] = (nvn_rbind){.kind = NVN_RB_SBUFP, .aid = 11, .reg = 2};
    rf.b[3] = (nvn_rbind){.kind = NVN_RB_SBUFP, .aid = 15, .reg = 3};
    rf.b[4] = (nvn_rbind){.kind = NVN_RB_CBUF, .aid = 17};
    assert(nvn_storage_access_promote(&rf, (id)a, 18 * 8, "adapter", 5) == 2);
    assert(rf.b[0].rw && rf.b[1].rw && rf.b[2].rw && !rf.b[3].rw && !rf.b[4].rw);
    assert(g_seen_writer && g_seen_atomic);
    assert(!nvn_storage_access_promote(&rf, (id)a, 18 * 8, "adapter", 5));
    // Root access is not a proxy for nested pointer access in either direction.
    a.access = 1; reader.pointerType.access = 0; rf.b[0].rw = 0;
    assert(!nvn_storage_access_promote(&rf, (id)a, 18 * 8, "adapter", 5) && !rf.b[0].rw);
    writer.offset++; rf.b[1].rw = 0;
    assert(!nvn_storage_access_promote(&rf, (id)a, 18 * 8, "adapter", 5) && !rf.b[1].rw);
    writer.offset--; a.index = 1;
    assert(!nvn_storage_access_promote(&rf, (id)a, 18 * 8, "adapter", 5) && !rf.b[1].rw);
    a.index = 0; a.type = 2;
    assert(!nvn_storage_access_promote(&rf, (id)a, 18 * 8, "adapter", 5) && !rf.b[1].rw);
    a.type = 0; a.bufferDataType = MTLDataTypeFloat;
    assert(!nvn_storage_access_promote(&rf, (id)a, 18 * 8, "adapter", 5) && !rf.b[1].rw);
    a.bufferDataType = MTLDataTypeStruct; a.bufferStructType = nil;
    assert(!nvn_storage_access_promote(&rf, (id)a, 18 * 8, "adapter", 5) && !rf.b[1].rw);
    assert(!nvn_storage_access_promote(&rf, nil, 18 * 8, "adapter", 5) && rf.b[2].rw);
    // The per-filter budget stays bounded despite repeated missing reflection.
    for (int i = 0; i < 200; i++) nvn_storage_access_promote(&rf, nil, 18 * 8, "adapter", 5);
    assert(atomic_load(&g_storage_access_diag_notes) == 128 && g_logged == 128);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    AccessFunction *fn = [AccessFunction new]; fn.encoder = [NSObject new]; MTLArgument *unusable = (id)a;
    id result = nvn_storage_access_encoder((id)fn, 0, &unusable);
    assert(result == fn.encoder && fn.fallbacks == 1 && !unusable);
#pragma clang diagnostic pop
    puts("storage-access Objective-C adapter: outer-vs-pointee access, ID/offset, missing reflection and original true preservation PASS");
} return 0; }
