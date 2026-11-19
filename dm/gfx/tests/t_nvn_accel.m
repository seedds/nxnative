// NVN2 acceleration structures through real recording/replay and Metal intersections.
// Synthetic geometry only: sizing, deferred descriptors, ordered inline writes, BLAS/TLAS
// build/refit, packed instance transforms/IDs/masks, copy, and GPU-reported compaction.
#import <Foundation/Foundation.h>
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
#include "../nvn/nvn_accel.h"
#include <spawn.h>
#include <sys/wait.h>

uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
void crash_install(void);
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr,"FAIL: %s\n",label); } } while (0)

static uint8_t *memory;
static const size_t memory_size = 32u << 20;
static size_t memory_used = 65536;
static pool_t pool;
static NVNbuffer buffers[64];
static unsigned buffer_count;
static NVNcommandBuffer commands;
static void (*calculate)(const NVNdevice *,const nvn_as_build *,uint64_t *,uint64_t *,uint64_t *);
static void (*build_as)(NVNcommandBuffer *,uint64_t,const nvn_as_build *,uint64_t,const nvn_as_post *);
static void (*copy_as)(NVNcommandBuffer *,uint64_t,uint64_t);
static void (*compact_as)(NVNcommandBuffer *,uint64_t,uint64_t);
static void (*bind_structure)(NVNcommandBuffer *,int,int,uint64_t);
static void *as_proc(const char *name) {
    void *p=(void *)h_nvnDeviceGetProcAddress(NULL,name);
    CHECK(p!=NULL,"NVN2 AS entry point resolves through GetProcAddress");
    if (!p) exit(2);
    return p;
}

static uint64_t guest_buffer(size_t n) {
    n = (n + 255) & ~(size_t)255;
    if (!n) n = 256;
    if (memory_used + n > memory_size || buffer_count == 64) { fprintf(stderr,"test storage exhausted\n"); exit(2); }
    NVNbufferBuilder b = {0};
    h_nvnBufferBuilderSetDefaults(&b);
    h_nvnBufferBuilderSetStorage(&b,(NVNmemoryPool *)&pool,(ptrdiff_t)memory_used,n);
    NVNbuffer *object = &buffers[buffer_count++];
    CHECK(h_nvnBufferInitialize(object,&b),"guest NVN buffer initializes");
    uint64_t address = h_nvnBufferGetAddress(object);
    CHECK(address == (uint64_t)(uintptr_t)(memory + memory_used),"AS storage uses the guest buffer address");
    memory_used += n;
    return address;
}

static void begin(void) { h_nvnCommandBufferBeginRecording(&commands); }
static NVNcommandHandle end(void) { return h_nvnCommandBufferEndRecording(&commands); }
static void replay(NVNcommandHandle handle) {
    size_t size;
    uint8_t *records = nvn_rec_linear(handle,&size);
    CHECK(records && size >= sizeof(op_hdr),"NVN recording flattens for replay");
    if (!records) exit(2);
    mtl_sub_begin(); mtl_exec_rest(NULL,0); mtl_exec(records); mtl_exec_rest(NULL,0);
    free(records);
}
static void finish(void) {
    mtl_sub_commit(); mtl_finish();
    CHECK(!mtl_last_commit_error(),"AS work completes without a Metal error");
}

typedef struct { float rows[12]; uint32_t id_mask, sbt_flags; uint64_t child; } packed_instance;
_Static_assert(sizeof(packed_instance) == 64,"NVN packed instance format");
static packed_instance instance(float x,float z,unsigned id,unsigned mask,uint64_t child) {
    packed_instance p = {.rows={1,0,0,x, 0,1,0,0, 0,0,1,z},
                         .id_mask=(id & 0xffffffu)|(mask << 24), .child=child};
    return p;
}
typedef struct { float origin[4], direction[4]; uint32_t options[4]; } ray_input;
typedef struct { float distance; uint32_t hit, instance, user, primitive, geometry; } ray_output;
static id<MTLComputePipelineState> ray_pipeline;
static NSString *const ray_source = @"#include <metal_stdlib>\n#include <metal_raytracing>\n"
    "using namespace metal; using namespace metal::raytracing;\n"
    "struct RayInput { float4 origin; float4 direction; uint4 options; };\n"
    "struct HitOutput { float distance; uint hit, instance, user, primitive, geometry; };\n"
    "kernel void probe(instance_acceleration_structure scene [[buffer(0)]], device const RayInput *inputs [[buffer(1)]], device HitOutput *out [[buffer(2)]], uint i [[thread_position_in_grid]]) {\n"
    " ray r; r.origin=inputs[i].origin.xyz; r.direction=inputs[i].direction.xyz; r.min_distance=0.001f; r.max_distance=1000.0f;\n"
    " intersector<triangle_data,instancing> q; q.assume_geometry_type(geometry_type::triangle);\n"
    " auto h=q.intersect(r,scene,inputs[i].options.x);\n"
    " out[i]=h.type==intersection_type::none?HitOutput{-1,0,~0u,~0u,~0u,~0u}:HitOutput{h.distance,1,h.instance_id,h.user_instance_id,h.primitive_id,h.geometry_id};\n"
    "}\n";

static ray_input ray_at(float x,unsigned mask) {
    ray_input r = {.origin={x,0,10,0},.direction={0,0,-1,0},.options={mask,0,0,0}};
    return r;
}
// Appended to the same submission as the replayed AS operations. It verifies GPU
// ordering as well as AS contents, rather than waiting before adding the query.
static void query(unsigned stage,unsigned slot,const ray_input *rays,const ray_output *expected,unsigned n) {
    uint64_t address=mtl_as_bound_address(stage,slot), rid=mtl_as_resource_id(address);
    CHECK(address && rid,"bound AS resolves to a Metal resource");
    if (!rid) { finish(); return; }
    NSError *error=nil;
    if (!ray_pipeline) {
        MTLCompileOptions *options=[MTLCompileOptions new]; options.languageVersion=MTLLanguageVersion4_0;
        id<MTLLibrary> lib=[g_dev newLibraryWithSource:ray_source options:options error:&error];
        id<MTLFunction> f=[lib newFunctionWithName:@"probe"];
        ray_pipeline=f?[g_dev newComputePipelineStateWithFunction:f error:&error]:nil;
        if (!ray_pipeline) fprintf(stderr,"ray fixture: %s\n",error.localizedDescription.UTF8String);
        CHECK(ray_pipeline!=nil,"actual ray-intersection fixture compiles");
        if (!ray_pipeline) { finish(); return; }
    }
    id<MTLBuffer> input=[g_dev newBufferWithBytes:rays length:n*sizeof *rays options:MTLResourceStorageModeShared];
    id<MTLBuffer> output=[g_dev newBufferWithLength:n*sizeof(ray_output) options:MTLResourceStorageModeShared];
    memset(output.contents,0xcd,output.length); res_add(input); res_add(output);
    MTL4ArgumentTableDescriptor *d=[MTL4ArgumentTableDescriptor new]; d.maxBufferBindCount=3;
    id<MTL4ArgumentTable> table=rec_table([g_dev newArgumentTableWithDescriptor:d error:&error]);
    [table setResource:(MTLResourceID){rid} atBufferIndex:0];
    [table setAddress:input.gpuAddress atIndex:1]; [table setAddress:output.gpuAddress atIndex:2];
    id<MTL4ComputeCommandEncoder> encoder=dispatch_enc();
    [encoder setArgumentTable:table]; [encoder setComputePipelineState:ray_pipeline];
    [encoder dispatchThreadgroups:MTLSizeMake(n,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
    g_blit_ct=0; args_dirty_all(); finish();
    ray_output *got=output.contents;
    for (unsigned i=0;i<n;i++) {
        CHECK(got[i].hit==expected[i].hit,"ray hit or miss follows geometry and visibility mask");
        if (!expected[i].hit) continue;
        if (fabsf(got[i].distance-expected[i].distance)>0.0001f)
            fprintf(stderr,"ray %u distance %g, expected %g\n",i,got[i].distance,expected[i].distance);
        CHECK(fabsf(got[i].distance-expected[i].distance)<0.0001f,"ray distance includes the child transform");
        CHECK(got[i].user==expected[i].user,"packed user instance ID reaches the intersection");
        CHECK(got[i].instance==expected[i].instance,"intersection identifies the correct instance");
        CHECK(got[i].primitive==0 && got[i].geometry==0,"intersection identifies the synthetic triangle");
    }
    res_remove(input); res_remove(output);
}

static nvn_as_build storage(nvn_as_geometry *g,uint32_t flags) {
    nvn_as_build b={.flags=flags,.geometries=g,.count=1};
    uint64_t bytes=UINT64_MAX,scratch=UINT64_MAX,update=UINT64_MAX;
    // This HLE call is the real missing-output contract that caused Welcome Tour's
    // allocation abort; all three old words must be replaced, even in a size query.
    calculate(NULL,&b,&bytes,&scratch,&update);
    CHECK(bytes && bytes!=UINT64_MAX,"AS query replaces the stale storage-size word");
    CHECK(scratch!=UINT64_MAX && update!=UINT64_MAX,"AS query writes both scratch-size outputs");
    CHECK(bytes < memory_size/4 && scratch < memory_size/4 && update < memory_size/4,"small synthetic geometry has bounded actual Metal requirements");
    if (!bytes || bytes>=memory_size/4 || scratch>=memory_size/4 || update>=memory_size/4) exit(2);
    b.destination=guest_buffer(bytes); b.destination_size=bytes;
    b.scratch_size=scratch>update?scratch:update; b.scratch=guest_buffer(b.scratch_size);
    return b;
}
static void record_build(const nvn_as_build *b,const nvn_as_post *post) {
    build_as(&commands,1,b,post?1:0,post);
}
static void bind_as(unsigned stage,unsigned slot,uint64_t address) {
    bind_structure(&commands,(int)stage,(int)slot,address);
}
static void expect_two(unsigned stage,unsigned slot,float x0,float d0,float d1) {
    ray_input rays[]={ray_at(x0,1),ray_at(4,2),ray_at(x0,2),ray_at(4,1),ray_at(8,3)};
    ray_output hits[]={{.distance=d0,.hit=1,.instance=0,.user=17},
                       {.distance=d1,.hit=1,.instance=1,.user=29},{0},{0},{0}};
    query(stage,slot,rays,hits,5);
}

static int run(void) { @autoreleasepool {
    crash_install(); mtl_init();
    CHECK(g_dev.supportsRaytracing,"Metal device supports ray tracing");
    if (!g_dev.supportsRaytracing) return 1;
    calculate=as_proc("nvnDeviceCalculateAccelerationStructureMemoryRequirements");
    build_as=as_proc("nvnCommandBufferBuildAccelerationStructures");
    copy_as=as_proc("nvnCommandBufferCopyAccelerationStructure");
    compact_as=as_proc("nvnCommandBufferCompactAccelerationStructure");
    bind_structure=as_proc("nvnCommandBufferBindAccelerationStructure");
    if (posix_memalign((void **)&memory,(size_t)getpagesize(),memory_size)) return 2;
    memset(memory,0,memory_size);
    pool=(pool_t){.magic=MAGIC('P','O','O','L'),.mem=memory,.size=memory_size};
    mtl_pool_add(memory,memory_size); nvn_dirty_add_range(memory,memory_size);
    CHECK(h_nvnCommandBufferInitialize(&commands,NULL),"AS command buffer initializes");
    h_nvnCommandBufferAddCommandMemory(&commands,(NVNmemoryPool *)&pool,0,65536);
    mtl_queue_activate(910001);

    // GPU addresses may be null during a metadata-only query. No fake input
    // buffers are necessary to ask how much storage a three-vertex BLAS needs.
    nvn_as_geometry triangles={.type=1,.triangles={.vertex_count=3,.vertex_format=NVN_FORMAT_RGB32F,.vertex_stride=12,
                                                .index_count=3,.index_type=2,.flags=1}};
    nvn_as_build blas=storage(&triangles,NVN_AS_ALLOW_UPDATE|NVN_AS_ALLOW_COMPACTION);
    uint64_t vertices=guest_buffer(36),indices=guest_buffer(12),instances=guest_buffer(128),report=guest_buffer(8);
    triangles.triangles.vertices=vertices; triangles.triangles.indices=indices;
    triangles.triangles.index_type=2; triangles.triangles.index_count=3;
    uint32_t ix[]={0,1,2}; memcpy((void *)(uintptr_t)indices,ix,sizeof ix);
    // Begin with an off-screen degenerate triangle. Only the earlier queued
    // inline write supplies the actual triangle consumed by the AS build.
    float points[]={-1,-1,0, 1,-1,0, 0,1,0};
    float wrong[]={20,20,20, 20,20,20, 20,20,20}; memcpy((void *)(uintptr_t)vertices,wrong,sizeof wrong);
    packed_instance inst[]={instance(0,2,17,1,blas.destination),instance(4,5,29,2,blas.destination)};
    // Exercise the observed packed value5. Its NVN meaning is a separately
    // documented cross-API inference; this synthetic hit verifies the host's
    // conversion/build path, not native NVN flag semantics.
    inst[0].sbt_flags=5u << 24;
    memcpy((void *)(uintptr_t)instances,inst,sizeof inst);
    nvn_as_geometry top_geometry={.type=3,.instances={.count=2}};
    nvn_as_build top=storage(&top_geometry,NVN_AS_ALLOW_UPDATE);
    top_geometry.instances.address=instances;
    nvn_as_post post={.type=0,.address=report}; *(uint64_t *)(uintptr_t)report=UINT64_MAX;

    begin();
    h_nvnCommandBufferUpdateUniformBuffer(&commands,vertices,sizeof points,0,sizeof points,points);
    record_build(&blas,&post); record_build(&top,NULL); bind_as(8,3,top.destination);
    NVNcommandHandle first=end();
    replay(first); expect_two(8,3,0,8,5);
    uint64_t compacted=*(uint64_t *)(uintptr_t)report;
    CHECK(compacted && compacted!=UINT64_MAX && compacted<=blas.destination_size,"GPU writes a real compacted-size report");

    // Descriptor mutation after recording must affect the submission. Record an
    // off-screen degenerate triangle, then select the real vertices only after
    // EndRecording. An eager descriptor snapshot produces misses on both rays.
    // This BLAS uses 16-bit indices; the original one uses 32-bit indices.
    nvn_as_build copied=storage(&triangles,NVN_AS_ALLOW_UPDATE|NVN_AS_ALLOW_COMPACTION);
    uint64_t bad_vertices=guest_buffer(sizeof wrong),indices16=guest_buffer(6);
    uint16_t ix16[]={0,1,2}; memcpy((void *)(uintptr_t)indices16,ix16,sizeof ix16);
    memcpy((void *)(uintptr_t)bad_vertices,wrong,sizeof wrong);
    nvn_as_geometry deferred=triangles; deferred.triangles.vertices=bad_vertices;
    deferred.triangles.indices=indices16; deferred.triangles.index_type=1;
    copied.geometries=&deferred;
    nvn_as_build late_top=storage(&top_geometry,NVN_AS_ALLOW_UPDATE);
    packed_instance late_inst[]={instance(0,2,17,1,copied.destination),instance(4,5,29,2,copied.destination)};
    begin();
    h_nvnCommandBufferUpdateUniformBuffer(&commands,instances,sizeof late_inst,0,sizeof late_inst,late_inst);
    record_build(&copied,NULL); record_build(&late_top,NULL); bind_as(8,6,late_top.destination);
    NVNcommandHandle late=end(); deferred.triangles.vertices=vertices;
    replay(late); expect_two(8,6,0,8,5);

    // Metal has no 8-bit AS index format. Exercise the GPU expansion with an
    // inline write immediately before the build, then trace through that child.
    // Float3 vertices have a padded16-byte stride, including final-record padding.
    nvn_as_geometry triangles8=triangles; triangles8.triangles.index_type=0;
    triangles8.triangles.indices=guest_buffer(4);
    triangles8.triangles.vertices=guest_buffer(48); triangles8.triangles.vertex_stride=16;
    float padded_points[]={-1,-1,0,99, 1,-1,0,99, 0,1,0,99};
    uint8_t ix8[]={0,1,2,0};
    memset((void *)(uintptr_t)triangles8.triangles.indices,0,sizeof ix8);
    nvn_as_build blas8=storage(&triangles8,0),top8=storage(&top_geometry,0);
    packed_instance inst8[]={instance(0,2,17,1,blas8.destination),instance(4,5,29,2,blas8.destination)};
    begin();
    h_nvnCommandBufferUpdateUniformBuffer(&commands,triangles8.triangles.vertices,sizeof padded_points,0,sizeof padded_points,padded_points);
    h_nvnCommandBufferUpdateUniformBuffer(&commands,triangles8.triangles.indices,sizeof ix8,0,sizeof ix8,ix8);
    h_nvnCommandBufferUpdateUniformBuffer(&commands,instances,sizeof inst8,0,sizeof inst8,inst8);
    record_build(&blas8,NULL); record_build(&top8,NULL); bind_as(8,8,top8.destination);
    replay(end()); expect_two(8,8,0,8,5);

    // AABB descriptors also support metadata-only sizing. A padded stride and
    // ordered GPU upload exercise the build, followed by a real GPU size query.
    nvn_as_geometry boxes={.type=2,.aabbs={.count=2,.stride=32,.flags=1}};
    nvn_as_build box_as=storage(&boxes,NVN_AS_ALLOW_COMPACTION);
    boxes.aabbs.boxes=guest_buffer(64);
    float box_data[]={-1,-1,-1,1,1,1,0,0, 3,-1,-1,5,1,1,0,0};
    uint64_t box_report=guest_buffer(8); *(uint64_t *)(uintptr_t)box_report=UINT64_MAX;
    nvn_as_post box_post={.type=0,.address=box_report};
    begin();
    h_nvnCommandBufferUpdateUniformBuffer(&commands,boxes.aabbs.boxes,sizeof box_data,0,sizeof box_data,box_data);
    record_build(&box_as,&box_post); replay(end()); finish();
    CHECK(mtl_as_resource_id(box_as.destination)!=0,"AABB build publishes a real Metal acceleration structure");
    CHECK(*(uint64_t *)(uintptr_t)box_report>0 && *(uint64_t *)(uintptr_t)box_report<=box_as.destination_size,
          "AABB build writes a valid GPU compacted-size report");

    // Refit both levels, with preceding GPU writes to geometry and packed
    // instances. Updated hit distances and IDs exercise the entire dependency.
    for (unsigned i=2;i<9;i+=3) points[i]=1;
    inst[0]=instance(1,3,17,1,blas.destination); inst[1]=instance(4,6,29,2,blas.destination);
    nvn_as_build refit_blas=blas,refit_top=top;
    refit_blas.operation=1; refit_blas.source=blas.destination;
    refit_top.operation=1; refit_top.source=top.destination;
    uint64_t update_bytes=UINT64_MAX;
    mtl_as_sizes(&refit_blas,NULL,NULL,&update_bytes);
    if (!update_bytes) refit_blas.scratch=refit_blas.scratch_size=0;
    mtl_as_sizes(&refit_top,NULL,NULL,&update_bytes);
    if (!update_bytes) refit_top.scratch=refit_top.scratch_size=0;
    begin();
    h_nvnCommandBufferUpdateUniformBuffer(&commands,vertices,sizeof points,0,sizeof points,points);
    h_nvnCommandBufferUpdateUniformBuffer(&commands,instances,sizeof inst,0,sizeof inst,inst);
    record_build(&refit_blas,NULL); record_build(&refit_top,NULL); bind_as(5,2,top.destination);
    replay(end()); expect_two(5,2,1,6,3);
    CHECK(mtl_as_bound_address(8,3)==top.destination,"compute binding does not replace the ray-stage binding");

    // A fresh top-level structure references a copied or compacted child. This
    // distinguishes a real child-address mapping from an address-only stub.
    uint64_t copy=guest_buffer(blas.destination_size);
    begin(); copy_as(&commands,blas.destination,copy);
    replay(end()); finish();
    inst[0].child=inst[1].child=copy; memcpy((void *)(uintptr_t)instances,inst,sizeof inst);
    begin(); record_build(&top,NULL); bind_as(8,4,top.destination);
    replay(end()); expect_two(8,4,1,6,3);

    // Read the size again after refit; only that GPU result determines the
    // compact destination's guest capacity.
    begin(); record_build(&blas,&post); replay(end()); finish();
    compacted=*(uint64_t *)(uintptr_t)report;
    CHECK(compacted && compacted<=blas.destination_size,"refitted geometry has a valid GPU compacted size");
    uint64_t compact=guest_buffer(compacted);
    begin(); compact_as(&commands,blas.destination,compact);
    replay(end()); finish();
    inst[0].child=inst[1].child=compact; memcpy((void *)(uintptr_t)instances,inst,sizeof inst);
    begin(); record_build(&top,NULL); bind_as(8,5,top.destination); replay(end()); expect_two(8,5,1,6,3);

    // A compacted child remains refittable using its actual compact capacity,
    // even when the original build query required more storage. Update both
    // levels so the TLAS bounds follow the moved triangle, then restore them.
    nvn_as_build compact_refit=refit_blas;
    compact_refit.source=compact_refit.destination=compact; compact_refit.destination_size=compacted;
    for(unsigned pass=0;pass<2;pass++) {
        float z=pass?1:2; for(unsigned i=2;i<9;i+=3)points[i]=z;
        begin();
        h_nvnCommandBufferUpdateUniformBuffer(&commands,vertices,sizeof points,0,sizeof points,points);
        record_build(&compact_refit,NULL); record_build(&refit_top,NULL);
        replay(end()); expect_two(8,5,1,7-z,4-z);
    }

    // A compact immediately following its build has no completed CPU size yet.
    // The backend must obtain the real GPU size, then resume the same NVN list;
    // it cannot fabricate a report or silently omit the remaining TLAS/query.
    nvn_as_build immediate=storage(&triangles,NVN_AS_ALLOW_COMPACTION);
    uint64_t immediate_compact=guest_buffer(immediate.destination_size);
    nvn_as_build immediate_top=storage(&top_geometry,0);
    packed_instance immediate_inst[]={instance(1,3,17,1,immediate_compact),instance(4,6,29,2,immediate_compact)};
    begin();
    h_nvnCommandBufferUpdateUniformBuffer(&commands,instances,sizeof immediate_inst,0,sizeof immediate_inst,immediate_inst);
    record_build(&immediate,NULL);
    compact_as(&commands,immediate.destination,immediate_compact);
    record_build(&immediate_top,NULL); bind_as(8,7,immediate_top.destination);
    replay(end()); expect_two(8,7,1,6,3);

    // QueueSubmit consumes the CPU arrays even if replay cannot start yet.
    // Reuse every descriptor layer after a held submission; only the GPU input
    // buffers retain their lifetime. The resulting hits and report destination
    // distinguish submission snapshots from later worker-side pointer reads.
    NVNqueue queued; NVNqueueBuilder qb={0}; h_nvnQueueBuilderSetDefaults(&qb);
    CHECK(h_nvnQueueInitialize(&queued,&qb),"logical queue initializes for AS snapshot test");
    nvn_as_geometry submitted_geometry[]={triangles,top_geometry};
    nvn_as_build submitted[]={storage(&submitted_geometry[0],NVN_AS_ALLOW_COMPACTION),
                              storage(&submitted_geometry[1],NVN_AS_ALLOW_COMPACTION)};
    uint64_t submitted_instances=guest_buffer(128),bad_instances=guest_buffer(128);
    packed_instance submitted_inst[]={instance(1,3,17,1,submitted[0].destination),instance(4,6,29,2,submitted[0].destination)};
    packed_instance bad_inst[]={instance(20,3,17,1,submitted[0].destination),instance(24,6,29,2,submitted[0].destination)};
    memcpy((void *)(uintptr_t)bad_instances,bad_inst,sizeof bad_inst);
    submitted_geometry[1].instances.address=submitted_instances;
    uint64_t submitted_report=guest_buffer(16),unused_report=guest_buffer(16);
    memset((void *)(uintptr_t)submitted_report,0xff,16); memset((void *)(uintptr_t)unused_report,0xff,16);
    nvn_as_post submitted_post={.type=0,.address=submitted_report};
    begin();
    h_nvnCommandBufferUpdateUniformBuffer(&commands,submitted_instances,sizeof submitted_inst,0,sizeof submitted_inst,submitted_inst);
    build_as(&commands,2,submitted,1,&submitted_post); bind_as(8,10,submitted[1].destination);
    NVNcommandHandle submitted_handle=end();
    nvn_queue_test_hold(1);
    h_nvnQueueSubmitCommands(&queued,1,&submitted_handle);
    nvn_as_geometry changed_geometry=submitted_geometry[0]; changed_geometry.triangles.vertices=bad_vertices;
    submitted[0].geometries=&changed_geometry;
    submitted_geometry[0].triangles.vertices=bad_vertices;
    submitted_geometry[1].instances.address=bad_instances;
    submitted_post.address=unused_report;
    nvn_queue_test_hold(0); h_nvnQueueFinish(&queued);
    CHECK(!mtl_last_commit_error(),"held logical-queue AS work completes without a Metal error");
    for(unsigned i=0;i<2;i++) {
        uint64_t got=((uint64_t *)(uintptr_t)submitted_report)[i];
        CHECK(got && got!=UINT64_MAX && got<=submitted[i].destination_size,"submitted postbuild array keeps its original GPU report destination");
        CHECK(((uint64_t *)(uintptr_t)unused_report)[i]==UINT64_MAX,"post-submit descriptor reuse does not redirect a GPU report");
    }
    mtl_sub_begin(); expect_two(8,10,1,6,3);
    h_nvnQueueFinalize(&queued); mtl_queue_activate(910001);

    // Persistent bindings belong to a logical queue. Alternate distinct AS
    // bindings and verify the restored queues with actual intersections.
    mtl_queue_activate(910002);
    CHECK(!mtl_as_bound_address(5,2) && !mtl_as_bound_address(8,3),"a new queue starts without AS bindings");
    begin(); bind_as(5,2,late_top.destination); bind_as(8,3,late_top.destination); replay(end());
    expect_two(5,2,0,8,5);
    mtl_queue_activate(910001);
    CHECK(mtl_as_bound_address(5,2)==top.destination && mtl_as_bound_address(8,3)==top.destination,
          "switching queues restores compute and ray-stage AS bindings");
    mtl_sub_begin(); expect_two(5,2,1,6,3);
    mtl_queue_activate(910002);
    CHECK(mtl_as_bound_address(5,2)==late_top.destination && mtl_as_bound_address(8,3)==late_top.destination,
          "the second queue retains its own compute and ray-stage AS bindings");
    mtl_sub_begin(); expect_two(8,3,0,8,5);
    mtl_queue_activate(910001);

    // NVNbuffers are views; finalizing an alias preserves the pool's AS data.
    // Retiring the child pool removes its address, while existing TLAS objects
    // keep the Metal children they were built with.
    nvn_as_geometry lifetime_geometry=triangles;
    lifetime_geometry.triangles.vertices=guest_buffer(36);
    nvn_as_build lifetime=storage(&lifetime_geometry,NVN_AS_ALLOW_UPDATE);
    uint8_t *child_memory=NULL;
    CHECK(!posix_memalign((void **)&child_memory,getpagesize(),65536),"child AS pool storage allocates");
    if(!child_memory)exit(2); memset(child_memory,0,65536);
    NVNmemoryPool child_pool; NVNmemoryPoolBuilder child_pb={0};
    h_nvnMemoryPoolBuilderSetDefaults(&child_pb); h_nvnMemoryPoolBuilderSetStorage(&child_pb,child_memory,65536);
    h_nvnMemoryPoolBuilderSetFlags(&child_pb,0x24);
    CHECK(h_nvnMemoryPoolInitialize(&child_pool,&child_pb),"separate child AS memory pool initializes");
    NVNbuffer child_buffer,child_alias; NVNbufferBuilder child_bb={0};
    h_nvnBufferBuilderSetDefaults(&child_bb); h_nvnBufferBuilderSetStorage(&child_bb,&child_pool,0,lifetime.destination_size);
    CHECK(h_nvnBufferInitialize(&child_buffer,&child_bb) && h_nvnBufferInitialize(&child_alias,&child_bb),"two NVNbuffer views share child AS storage");
    lifetime.destination=h_nvnBufferGetAddress(&child_buffer);
    uint64_t lifetime_instances=guest_buffer(128);
    packed_instance lifetime_inst[]={instance(0,2,17,1,lifetime.destination),instance(4,5,29,2,lifetime.destination)};
    memcpy((void *)(uintptr_t)lifetime_instances,lifetime_inst,sizeof lifetime_inst);
    nvn_as_geometry lifetime_top_geometry={.type=3,.instances={.address=lifetime_instances,.count=2}};
    nvn_as_build lifetime_top=storage(&lifetime_top_geometry,0);
    float lifetime_points[]={-1,-1,0, 1,-1,0, 0,1,1};
    memcpy((void *)(uintptr_t)lifetime_geometry.triangles.vertices,lifetime_points,sizeof lifetime_points);
    begin(); record_build(&lifetime,NULL); record_build(&lifetime_top,NULL); bind_as(8,11,lifetime_top.destination);
    replay(end()); expect_two(8,11,0,7.5f,4.5f);
    uint64_t lifetime_id=mtl_as_resource_id(lifetime.destination),lifetime_top_id=mtl_as_resource_id(lifetime_top.destination);
    h_nvnBufferFinalize(&child_alias);
    CHECK(as_find(lifetime.destination)!=nil && mtl_as_resource_id(lifetime.destination)==lifetime_id,
          "finalizing a buffer alias preserves the pool's AS mapping");
    mtl_sub_begin(); expect_two(8,11,0,7.5f,4.5f);

    // Both triangles have the same bounding box, so rebuilding the child does
    // not require updating the old TLAS bounds. Changed distances prove that
    // its instance references the rebuilt child rather than an orphaned copy.
    lifetime_points[5]=1; lifetime_points[8]=0;
    begin();
    h_nvnCommandBufferUpdateUniformBuffer(&commands,lifetime_geometry.triangles.vertices,sizeof lifetime_points,0,sizeof lifetime_points,lifetime_points);
    record_build(&lifetime,NULL); replay(end());
    CHECK(mtl_as_resource_id(lifetime.destination)==lifetime_id,"same-address BLAS rebuild preserves its Metal resource identity");
    CHECK(mtl_as_resource_id(lifetime_top.destination)==lifetime_top_id,"child rebuild preserves the existing TLAS identity");
    expect_two(8,11,0,7.75f,4.75f);

    // Copy into that same live child address. The source's flat z=1 triangle
    // fits inside the old TLAS bounds, and changed distances verify the copy.
    begin(); copy_as(&commands,blas.destination,lifetime.destination); replay(end());
    CHECK(mtl_as_resource_id(lifetime.destination)==lifetime_id,"copy into a live BLAS address preserves its Metal resource identity");
    CHECK(mtl_as_resource_id(lifetime_top.destination)==lifetime_top_id,"copy into a child preserves the existing TLAS identity");
    expect_two(8,11,0,7,4);

    h_nvnBufferFinalize(&child_buffer); h_nvnMemoryPoolFinalize(&child_pool);
    CHECK(as_find(lifetime.destination)==nil,"finalizing the child pool retires its AS guest address");
    mtl_sub_begin(); expect_two(8,11,0,7,4);

    CHECK(h_nvnMemoryPoolInitialize(&child_pool,&child_pb) && h_nvnBufferInitialize(&child_buffer,&child_bb),
          "retired pool and buffer storage initialize again at the same address");
    CHECK(h_nvnBufferGetAddress(&child_buffer)==lifetime.destination && as_find(lifetime.destination)==nil,
          "reinitialized pool storage does not recover the retired AS mapping");
    nvn_as_build replacement_top=storage(&lifetime_top_geometry,0);
    lifetime_points[5]=0;
    begin();
    h_nvnCommandBufferUpdateUniformBuffer(&commands,lifetime_geometry.triangles.vertices,sizeof lifetime_points,0,sizeof lifetime_points,lifetime_points);
    record_build(&lifetime,NULL); record_build(&replacement_top,NULL); bind_as(8,12,replacement_top.destination);
    replay(end());
    CHECK(mtl_as_resource_id(lifetime.destination)!=lifetime_id,"a new AS at retired guest storage gets a distinct live Metal resource");
    expect_two(8,12,0,8,5);
    mtl_sub_begin(); expect_two(8,11,0,7,4);
    h_nvnBufferFinalize(&child_buffer); h_nvnMemoryPoolFinalize(&child_pool); free(child_memory);

    begin(); bind_as(5,2,0); bind_as(8,3,0); replay(end()); finish();
    CHECK(!mtl_as_bound_address(5,2) && !mtl_as_bound_address(8,3),"AS bindings can be explicitly cleared");
    mtl_queue_forget(910001); mtl_queue_forget(910002);
    for (unsigned i=0;i<buffer_count;i++) h_nvnBufferFinalize(&buffers[i]);
    h_nvnCommandBufferFinalize(&commands); mtl_pool_del(memory); nvn_dirty_remove_range(memory); free(memory);
    printf("t_nvn_accel: %d checks, %d failed (encoder thread %s)\n",checks,failed,getenv("DM_NVN_ENC_THREAD"));
    return failed!=0;
} }

int main(int argc,char **argv) {
    if (argc>1) { setenv("DM_NVN_ENC_THREAD",argv[1],1); return run(); }
    extern char **environ;
    int result=0;
    for (int mode=0;mode<2;mode++) {
        char *args[]={argv[0],mode?"1":"0",NULL}; pid_t pid; int status=0;
        if (posix_spawn(&pid,argv[0],NULL,NULL,args,environ) || waitpid(pid,&status,0)<0 || !WIFEXITED(status) || WEXITSTATUS(status)) result=1;
    }
    return result;
}
