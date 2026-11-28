// Production catalog/program logic with a generated synthetic catalog and a
// real Metal compute pipeline. No native RT shader translation is claimed.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CommonCrypto/CommonDigest.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "nvn_obj.h"

static int checks, failures, pipeline_failure, missing_shader;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); } } while(0)
#define MLOG(...) do { fprintf(stderr,__VA_ARGS__);fputc('\n',stderr); } while(0)
#define MDIE(...) @throw [NSException exceptionWithName:@"ExpectedProgramFatal" reason:@"invalid group" userInfo:nil]
static id<MTLDevice> g_dev;
@interface NvShader : NSObject { @public id<MTLFunction> fn; NSDictionary *rtMetadata; } @end
@implementation NvShader @end
@class NvRayProgram;
@interface NvProg : NSObject { @public NvRayProgram *raytracing; } @end
@implementation NvProg @end
static NvShader *test_shader;
static NSMutableDictionary<NSNumber *, NvProg *> *serials;
static uint64_t next_serial;
static unsigned publications, resident_buffers, pipeline_calls;
static void shmap_load(void) {}
static NvShader *shader_get(NSString *key, int stage) {
    CHECK([key isEqual:@"rt_library_b"] && stage == NVN_RT_RESOURCE_STAGE);
    return missing_shader ? nil : test_shader;
}
static id<MTLComputePipelineState> cps_new(id<MTLFunction> fn, NSError **error) {
    pipeline_calls++; if (pipeline_failure) return nil;
    return [g_dev newComputePipelineStateWithFunction:fn error:error];
}
static void res_add(id<MTLResource> object) { CHECK(object != nil); resident_buffers++; }
uint64_t nvn_new_serial(void) { return ++next_serial; }
static void serial_put(uint64_t serial, NvProg *object) { CHECK(serial != 0 && object != nil); publications++; serials[@(serial)] = object; }
static NvProg *serial_get(uint64_t serial) { return serials[@(serial)]; }
static int mtl_addr_known(uint64_t a, uint64_t n) { (void)a; (void)n; return 1; }
#define DM_GAME_SHADER_KEY_SKIP_LEN 0
#define DM_GAME_SHADER_KEY_SKIP_OFF 0
/* PRODUCTION_SHADER_KEY */
#include "nvn_raytrace_program.inc"

static NSDictionary *base_metadata, *fixture;
static NSString *directory;
static id<MTLFunction> fixture_function;
static void load_catalog(NSDictionary *metadata) {
    test_shader = [NvShader new]; test_shader->fn = fixture_function; test_shader->rtMetadata = metadata;
}
static prog_t fresh(void) { prog_t p = {0}; p.magic = MAGIC('P','R','O','G'); return p; }
static NVNshaderData inputs[3];
static NSMutableArray<NSMutableData *> *controls, *codes;
static void load_inputs(void) {
    controls = [NSMutableArray array]; codes = [NSMutableArray array];
    for (unsigned i = 0; i < 3; i++) {
        [controls addObject:[[NSMutableData alloc] initWithContentsOfFile:[directory stringByAppendingPathComponent:[NSString stringWithFormat:@"module%u.ctl",i]]]];
        [codes addObject:[[NSMutableData alloc] initWithContentsOfFile:[directory stringByAppendingPathComponent:[NSString stringWithFormat:@"module%u.code",i]]]];
        inputs[i] = (NVNshaderData){(uintptr_t)codes[i].bytes, controls[i].bytes};
    }
}
static NSMutableDictionary *mutable_metadata(void) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:base_metadata options:0 error:nil];
    return [NSJSONSerialization JSONObjectWithData:data options:NSJSONReadingMutableContainers error:nil];
}
static void unchanged_failure(NVNshaderData *modules, int count, uint32_t trace, uint32_t callable, uint32_t flags) {
    prog_t p = fresh(); uint64_t serial = next_serial; unsigned n = publications;
    CHECK(!mtl_rt_program_set(&p,count,modules,trace,callable,flags));
    CHECK(p.serial == 0 && p.nstage == 0 && next_serial == serial && publications == n);
}
static void invalid_inputs(void) {
    unchanged_failure(NULL,3,1,0,0); unchanged_failure(inputs,0,1,0,0); unchanged_failure(inputs,-1,1,0,0);
    unchanged_failure(inputs,3,2,0,0); unchanged_failure(inputs,3,1,1,0); unchanged_failure(inputs,3,1,0,1);
    NVNshaderData bad[3]; memcpy(bad,inputs,sizeof bad); bad[0].control=(void *)1; unchanged_failure(bad,3,1,0,0);
    memcpy(bad,inputs,sizeof bad); bad[0].data=1; unchanged_failure(bad,3,1,0,0);
    uint8_t *control = controls[0].mutableBytes; uint8_t stage = control[0x56]; control[0x56]=5;
    unchanged_failure(inputs,3,1,0,0); control[0x56]=stage;
    uint32_t meta_offset;memcpy(&meta_offset,control+0x60,4);control[meta_offset+56]^=1;
    unchanged_failure(inputs,3,1,0,0);control[meta_offset+56]^=1;
    ((uint8_t *)codes[0].mutableBytes)[0]^=1;unchanged_failure(inputs,3,1,0,0);((uint8_t *)codes[0].mutableBytes)[0]^=1;
    uint32_t old_offset;memcpy(&old_offset,control+0x60,4);uint32_t overflow=UINT32_MAX;memcpy(control+0x60,&overflow,4);
    unchanged_failure(inputs,3,1,0,0);memcpy(control+0x60,&old_offset,4);
}
static void invalid_catalogs(void) {
    NSArray *paths = @[@"version",@"parameter_words",@"local_bytes",@"local_bias",@"core_offset",@"descriptor_offset",@"image_class_base",@"membership_base"];
    for (NSString *field in paths) { NSMutableDictionary *m=mutable_metadata();m[field]=@(-1);load_catalog(m);unchanged_failure(inputs,3,1,0,0); }
    for (NSString *field in @[@"catalog",@"module_entries",@"module_as_slots"]) {
        NSMutableDictionary *m=mutable_metadata();[m removeObjectForKey:field];load_catalog(m);unchanged_failure(inputs,3,1,0,0);
    }
    NSMutableDictionary *m=mutable_metadata();NSString *key=[fixture[@"module_keys"] firstObject];
    m[@"module_as_slots"][key]=@[@32];load_catalog(m);unchanged_failure(inputs,3,1,0,0);
    m=mutable_metadata();m[@"module_as_slots"][key]=@[@3,@4];load_catalog(m);unchanged_failure(inputs,3,1,0,0);
    m=mutable_metadata();for(NSString *k in [m[@"module_as_slots"] allKeys])m[@"module_as_slots"][k]=@[];
    load_catalog(m);unchanged_failure(inputs,3,1,0,0);
    load_catalog(base_metadata);pipeline_failure=1;unchanged_failure(inputs,3,1,0,0);pipeline_failure=0;
    missing_shader=1;unchanged_failure(inputs,3,1,0,0);missing_shader=0;
}
static void success_and_handles(void) {
    load_catalog(base_metadata);prog_t p=fresh();CHECK(mtl_rt_program_set(&p,3,inputs,1,0,0));
    CHECK(p.serial && p.nstage==0);NvRayProgram *rt=serial_get(p.serial)->raytracing;
    CHECK(rt->tag==p.serial && rt->asSlot==3 && rt->modules.count==3 && rt->library->pipeline!=nil);
    CHECK(rt->library->bank0.length==65536 && rt->membership.length==(rt->library->parameterWords-24)*4);
    const uint32_t *bank=rt->library->bank0.contents;for(unsigned i=0;i<65536/4;i++)CHECK(bank[i]==(i==10?rt->library->localBias+rt->library->localBytes:0));
    const uint32_t *mask=rt->membership.bytes;NSMutableSet *member=[NSMutableSet set];
    for(NSDictionary *module in rt->modules) {
        CHECK([module[@"metadata_sha256"] length]==64 && module[@"control"]==nil && module[@"code"]==nil);
        for(NSDictionary *entry in module[@"entries"]) [member addObject:entry[@"id"]];
    }
    for(unsigned id_=0;id_<rt->membership.length*8;id_++)CHECK(!!(mask[id_/32]&(1u<<(id_&31)))==[member containsObject:@(id_)]);
    prog_t copy=p;
    for(unsigned i=0;i<3;i++) {
        uint8_t token[32];memset(token,0x9d,sizeof token);int32_t indices[3]={(int32_t)i,-1,-1};
        mtl_rt_group_handle(&copy,indices,token);uint32_t words[4];memcpy(words,token,16);
        NSDictionary *primary=fixture[@"primary"][i];
        CHECK(words[0]==([primary[@"kind"] unsignedIntValue]<<16|[primary[@"id"] unsignedIntValue]));
        CHECK(words[1]==0 && words[2]==(uint32_t)p.serial && words[3]==(uint32_t)(p.serial>>32));
        for(unsigned j=16;j<32;j++)CHECK(token[j]==0x9d);
    }
    int32_t invalid[][3]={{-1,-1,-1},{3,-1,-1},{0,0,-1},{0,-1,0}};
    for(unsigned i=0;i<4;i++) { uint8_t token[32];memset(token,0x9d,32);int caught=0;
        @try {mtl_rt_group_handle(&p,invalid[i],token);}@catch(NSException *e){(void)e;caught=1;}
        CHECK(caught);for(unsigned j=0;j<32;j++)CHECK(token[j]==0x9d);
    }
    prog_t p2=fresh();CHECK(mtl_rt_program_set(&p2,1,&inputs[1],1,0,0));CHECK(p2.serial!=p.serial);
    NvRayProgram *subset=serial_get(p2.serial)->raytracing;CHECK(subset->asSlot==3 && subset->modules.count==1);
    unsigned primary=[fixture[@"primary"][1][@"id"] unsignedIntValue];const uint32_t *subset_mask=subset->membership.bytes;
    for(unsigned id_=0;id_<subset->membership.length*8;id_++)CHECK(!!(subset_mask[id_/32]&(1u<<(id_&31)))==(id_==primary));
    NSString *original=rt->modules[0][@"metadata_sha256"];memset(controls[0].mutableBytes,0,controls[0].length);memset(codes[0].mutableBytes,0,codes[0].length);
    uint32_t token[8]={0};int32_t first[3]={0,-1,-1};mtl_rt_group_handle(&p,first,token);
    CHECK(token[0]==([fixture[@"primary"][0][@"kind"] unsignedIntValue]<<16|[fixture[@"primary"][0][@"id"] unsignedIntValue]));
    CHECK([rt->modules[0][@"metadata_sha256"] isEqual:original]);
}
int main(int argc,char **argv) { @autoreleasepool {
    if(argc!=2)return 2;directory=@(argv[1]);g_dev=MTLCreateSystemDefaultDevice();if(!g_dev)return 77;
    NSError *error=nil;id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal; kernel void main0(uint tid [[thread_position_in_grid]]) {(void)tid;}" options:nil error:&error];
    fixture_function=[lib newFunctionWithName:@"main0"];if(!fixture_function){fprintf(stderr,"%s\n",error.localizedDescription.UTF8String);return 2;}
    fixture=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:@"fixture.json"]] options:0 error:nil];
    base_metadata=fixture[@"rt"];serials=[NSMutableDictionary dictionary];load_inputs();load_catalog(base_metadata);
    invalid_inputs();invalid_catalogs();success_and_handles();CHECK(resident_buffers>0 && pipeline_calls>0);
    printf("RT program catalog: %d checks, %d failures\n",checks,failures);return failures!=0;
} }
