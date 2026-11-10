// Private Metal buffers reject even querying contents under API validation.
// SourceSlice supplies only the descriptor fields that these CPU-copy fallbacks
// inspect. No invalid misaligned Metal texture is created or sent to the GPU.
#include "../nvn/nvn_mtl.m"
#include <unistd.h>
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
@interface SourceSlice : NSObject
@property(strong) id<MTLBuffer> buffer;
@property NSUInteger bufferOffset;
@end
@implementation SourceSlice
- (MTLTextureType)textureType { return MTLTextureTypeTextureBuffer; }
- (id<MTLTexture>)parentTexture { return nil; }
- (NSUInteger)bufferBytesPerRow { return 64; }
@end
static int checks, failed;
#define CHECK(c,label) do { checks++; if (!(c)) { failed++; fprintf(stderr,"FAIL: %s (%d)\n",label,__LINE__); } } while(0)
int main(int argc, char **argv) { @autoreleasepool {
    alarm(30);
    if (argc != 2 || (strcmp(argv[1], "shadow") && strcmp(argv[1], "rows"))) return 2;
    setenv("DM_PSO_ARCHIVE","off",1);setenv("DM_PSO_PREBUILD","0",1);setenv("DM_PSO_MANIFEST","off",1);setenv("DM_PSO_USER","/dev/null",1);
    mtl_init();
    id<MTLBuffer> src=[g_dev newBufferWithLength:4096 options:MTLResourceStorageModePrivate];
    CHECK(src != nil,"private source allocation");if(!src)return 2;
    CHECK(src.storageMode == MTLStorageModePrivate,"actual Metal buffer is private");
    SourceSlice *slice=[SourceSlice new];slice.buffer=src;slice.bufferOffset=4;
    uint64_t rid=0x1234, copies=g_tbs_copies, rows=g_tb2_copy;
    id<MTLTexture> result;
    if (!strcmp(argv[1], "shadow")) {
        result=tbuf_shadow((id)slice,&rid);
        CHECK(result == (id)slice,"unsupported shadow keeps its existing fallback");
    } else {
        NSUInteger al=[g_dev minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatR32Uint];
        MTLTextureDescriptor *d=[MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:16 resourceOptions:src.resourceOptions usage:MTLTextureUsageShaderRead];
        id<MTLTexture> source=[src newTextureWithDescriptor:d offset:al bytesPerRow:256];
        CHECK(source != nil,"real aligned private source view");if(!source)return 2;
        // A larger row-view alignment can require a copy even when the source
        // buffer texture itself is valid (also reproducible with DM_TBUF_ALIGN).
        result=tbuf_rows_copy(source,MTLPixelFormatR32Uint,4,1,256,2*al,&rid);
        CHECK(result == nil,"unsupported row copy keeps its existing failure result");
    }
    CHECK(rid == 0x1234,"no fabricated resource identity");
    CHECK(g_tbs_copies == copies && g_tb2_copy == rows,"no CPU snapshot or staging allocation");
    printf("t_tbuf_access %s: %d checks, %d failed\n",argv[1],checks,failed);
    return failed != 0;
} }
