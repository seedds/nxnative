// Real AS queries select synthetic 32-byte records through GPU-consumed SBT
// contributions. This tests the binding contract, not a native RT shader ABI.
#define main nvn_accel_suite_main
#include "t_nvn_accel.m"
#undef main
#include <simd/simd.h>

typedef struct {
    nvn_as_binding binding;
    uint32_t hit, ordinal, user, offset, record0, record7, geometry, primitive;
    float distance, normal[3], inverse[12];
} sbt_output;
_Static_assert(sizeof(sbt_output)==120,"SBT query fixture ABI");
static NSString *const sbt_source=@"#include <metal_stdlib>\n#include <metal_raytracing>\n"
"using namespace metal;using namespace metal::raytracing;\n"
"struct Binding {ulong rid,offsets;uint count,type;};struct Ray {float4 origin,direction;uint4 options;};\n"
"struct Output {Binding binding;uint hit,ordinal,user,offset,record0,record7,geometry,primitive;float distance,normal[3],inverse[12];};\n"
"static_assert(sizeof(Binding)==24&&sizeof(Output)==120);\n"
"kernel void probe(instance_acceleration_structure scene [[buffer(0)]],constant Binding &binding [[buffer(1)]],device const Ray *rays [[buffer(2)]],device const uint *records [[buffer(3)]],device Output *out [[buffer(4)]],uint i [[thread_position_in_grid]]) {\n"
" ray r;r.origin=rays[i].origin.xyz;r.direction=rays[i].direction.xyz;r.min_distance=.001f;r.max_distance=1000;\n"
" intersector<triangle_data,instancing,world_space_data> q;q.assume_geometry_type(geometry_type::triangle);auto h=q.intersect(r,scene,rays[i].options.x);\n"
" Output o={};o.binding=binding;o.hit=h.type!=intersection_type::none;uint ordinal=o.hit?h.instance_id:rays[i].options.y;o.offset=~0u;\n"
" if(ordinal<binding.count)o.offset=((device const uint *)binding.offsets)[ordinal];\n"
" if(o.hit) {o.ordinal=h.instance_id;o.user=h.user_instance_id;o.geometry=h.geometry_id;o.primitive=h.primitive_id;o.distance=h.distance;\n"
" if(o.offset<64) {o.record0=records[8*o.offset];o.record7=records[8*o.offset+7];}\n"
" for(uint c=0;c<4;c++)for(uint row=0;row<3;row++)o.inverse[4*row+c]=h.world_to_object_transform[c][row];\n"
" float3x3 inverse(h.world_to_object_transform[0],h.world_to_object_transform[1],h.world_to_object_transform[2]);float3 normal=normalize(transpose(inverse)*normalize(float3(0,-1,1)));for(uint j=0;j<3;j++)o.normal[j]=normal[j];}\n"
" out[i]=o;}\n";
typedef struct {nvn_as_binding binding;packed_instance instances[4];} sbt_expected;
static sbt_expected expected[16];
static NSMutableArray<id<MTLBuffer>> *outputs;
static id<MTLComputePipelineState> sbt_pipeline;
static id<MTLBuffer> sbt_records;
static simd_float4x4 matrices[4];
static simd_float3 point(simd_float4x4 m,simd_float3 p) {return simd_mul(m,(simd_float4){p.x,p.y,p.z,1}).xyz;}
static simd_float3 normal_for(simd_float4x4 m) {
    simd_float3 a=point(m,(simd_float3){-1,-1,-1}),b=point(m,(simd_float3){1,-1,-1}),c=point(m,(simd_float3){0,1,1});
    return simd_normalize(simd_cross(b-a,c-a));
}
static packed_instance packed(unsigned ordinal,unsigned id,unsigned offset,uint64_t child) {
    packed_instance p={.id_mask=(id&0xffffffu)|((ordinal<3?1u<<ordinal:0)<<24),.sbt_flags=(5u<<24)|(offset&0xffffffu),.child=child};
    for(unsigned r=0;r<3;r++)for(unsigned c=0;c<4;c++)p.rows[4*r+c]=matrices[ordinal].columns[c][r];return p;
}
static void append_recording(void) {
    size_t n;uint8_t *records=nvn_rec_linear(end(),&n);CHECK(records&&n>=sizeof(op_hdr),"SBT recording flattens");
    if(!records)exit(2);mtl_exec_rest(NULL,0);mtl_exec(records);mtl_exec_rest(NULL,0);free(records);
}
static nvn_as_binding acquire(uint64_t address) {
    nvn_as_binding binding;CHECK(mtl_as_acquire(address,&binding),"built AS acquires a dispatch binding");return binding;
}
static nvn_as_build storage_n(nvn_as_geometry *g,unsigned count,unsigned flags) {
    nvn_as_build b={.flags=flags,.geometries=g,.count=count};uint64_t bytes,scratch,update;calculate(NULL,&b,&bytes,&scratch,&update);
    CHECK(bytes&&bytes<memory_size/4,"fixture has bounded real AS storage");
    b.destination=guest_buffer(bytes);b.destination_size=bytes;b.scratch_size=MAX(scratch,update);b.scratch=guest_buffer(b.scratch_size);return b;
}
static void probe(nvn_as_binding binding,const packed_instance instances[4]) {
    if(!outputs)outputs=[NSMutableArray array];unsigned slot=(unsigned)outputs.count;if(slot>=16)exit(2);
    expected[slot].binding=binding;memcpy(expected[slot].instances,instances,sizeof expected[slot].instances);
    ray_input rays[13]={};
    for(unsigned i=0;i<3;i++) {
        simd_float3 n=normal_for(matrices[i]);
        for(unsigned j=0;j<3;j++) {
            simd_float3 p=point(matrices[i],(simd_float3){4.f*j,-1.f/3,-1.f/3});simd_float3 origin=p+5*n,direction=-n;
            rays[3*i+j]=(ray_input){.origin={origin.x,origin.y,origin.z,0},.direction={direction.x,direction.y,direction.z,0},.options={1u<<i,i}};
        }
        rays[9+i]=rays[3*i];rays[9+i].options[0]=1u<<((i+1)%3);
    }
    rays[12]=rays[0];rays[12].options[0]=0;rays[12].options[1]=3;
    NSError *error=nil;
    if(!sbt_pipeline) {
        MTLCompileOptions *options=[MTLCompileOptions new];options.languageVersion=MTLLanguageVersion4_0;
        id<MTLLibrary> lib=[g_dev newLibraryWithSource:sbt_source options:options error:&error];id<MTLFunction> f=[lib newFunctionWithName:@"probe"];
        sbt_pipeline=f?[g_dev newComputePipelineStateWithFunction:f error:&error]:nil;
        CHECK(sbt_pipeline!=nil,"real query/SBT record fixture compiles");if(!sbt_pipeline){fprintf(stderr,"%s\n",error.localizedDescription.UTF8String);exit(2);}
    }
    id<MTLBuffer> input;size_t off=arena_alloc(sizeof binding+sizeof rays,&input);
    memcpy((uint8_t *)input.contents+off,&binding,sizeof binding);memcpy((uint8_t *)input.contents+off+sizeof binding,rays,sizeof rays);
    id<MTLBuffer> output=[g_dev newBufferWithLength:13*sizeof(sbt_output) options:MTLResourceStorageModeShared];memset(output.contents,0xcd,output.length);res_add(output);[outputs addObject:output];
    MTL4ArgumentTableDescriptor *d=[MTL4ArgumentTableDescriptor new];d.maxBufferBindCount=5;
    id<MTL4ArgumentTable> table=rec_table([g_dev newArgumentTableWithDescriptor:d error:&error]);
    defer_obj_at(table,1,atomic_load(&g_sub_open)); // queued probes outlive this stack frame
    [table setResource:(MTLResourceID){binding.resource_id} atBufferIndex:0];[table setAddress:input.gpuAddress+off atIndex:1];
    [table setAddress:input.gpuAddress+off+sizeof binding atIndex:2];[table setAddress:sbt_records.gpuAddress atIndex:3];[table setAddress:output.gpuAddress atIndex:4];
    id<MTL4ComputeCommandEncoder> e=dispatch_enc();[e setArgumentTable:table];[e setComputePipelineState:sbt_pipeline];
    [e dispatchThreadgroups:MTLSizeMake(13,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];g_blit_ct=0;args_dirty_all();
}
static void check_probes(void) {
    for(unsigned p=0;p<outputs.count;p++) {
        const sbt_output *got=outputs[p].contents;const sbt_expected *e=&expected[p];
        for(unsigned ray=0;ray<13;ray++) {
            unsigned i=ray<9?ray/3:ray<12?ray-9:3,j=ray%3;unsigned offset=e->instances[i].sbt_flags&0xffffffu;
            CHECK(got[ray].hit==(ray<9),"actual AS hit and mask behavior");
            CHECK(!memcmp(&got[ray].binding,&e->binding,sizeof e->binding),"GPU receives the acquired binding snapshot");
            CHECK(got[ray].offset==offset,"GPU snapshot preserves all24 SBT bits without flag contamination");
            if(ray>=9)continue;
            CHECK(got[ray].ordinal==i&&got[ray].user==(e->instances[i].id_mask&0xffffffu),"query ordinal stays distinct from custom ID");
            CHECK(got[ray].record0==(0x12340000u+offset*17)&&got[ray].record7==~offset,"hit selects both ends of the correct32-byte GPU record");
            CHECK(got[ray].geometry==(j?1u:0u)&&got[ray].primitive==(j?j-1:0),"query preserves geometry-local primitive IDs");
            CHECK(fabsf(got[ray].distance-5)<.0001f,"real transformed hit distance");
            simd_float4x4 inverse=simd_inverse(matrices[i]);simd_float3 normal=normal_for(matrices[i]);
            for(unsigned row=0;row<3;row++)for(unsigned col=0;col<4;col++)CHECK(fabsf(got[ray].inverse[4*row+col]-inverse.columns[col][row])<.0001f,"query supplies the actual inverse without a sidecar");
            for(unsigned axis=0;axis<3;axis++)CHECK(fabsf(got[ray].normal[axis]-normal[axis])<.0001f,"query inverse-transpose matches independently transformed edges");
        }
        res_remove(outputs[p]);
    }
    outputs=nil;
}

static int sbt_run(void) { @autoreleasepool {
    crash_install();mtl_init();CHECK(g_dev.supportsRaytracing,"device supports actual ray queries");if(!g_dev.supportsRaytracing)return 1;
    calculate=as_proc("nvnDeviceCalculateAccelerationStructureMemoryRequirements");build_as=as_proc("nvnCommandBufferBuildAccelerationStructures");
    copy_as=as_proc("nvnCommandBufferCopyAccelerationStructure");compact_as=as_proc("nvnCommandBufferCompactAccelerationStructure");
    if(posix_memalign((void **)&memory,getpagesize(),memory_size))return 2;memset(memory,0,memory_size);
    pool=(pool_t){.magic=MAGIC('P','O','O','L'),.mem=memory,.size=memory_size};mtl_pool_add(memory,memory_size);nvn_dirty_add_range(memory,memory_size);
    CHECK(h_nvnCommandBufferInitialize(&commands,NULL),"SBT command buffer initializes");h_nvnCommandBufferAddCommandMemory(&commands,(NVNmemoryPool *)&pool,0,65536);mtl_queue_activate(910004);
    uint32_t records[64*8]={};for(unsigned i=0;i<64;i++){records[8*i]=0x12340000u+17*i;records[8*i+7]=~i;}
    sbt_records=[g_dev newBufferWithBytes:records length:sizeof records options:MTLResourceStorageModeShared];res_add(sbt_records);
    float xyz[]={-1,-1,-1,1,-1,-1,0,1,1, 3,-1,-1,5,-1,-1,4,1,1, 7,-1,-1,9,-1,-1,8,1,1};
    uint64_t vertices=guest_buffer(sizeof xyz);memcpy((void *)(uintptr_t)vertices,xyz,sizeof xyz);
    nvn_as_geometry g[2]={{.type=1,.triangles={.vertices=vertices,.vertex_count=3,.vertex_format=NVN_FORMAT_RGB32F,.vertex_stride=12,.flags=1}},
                         {.type=1,.triangles={.vertices=vertices+36,.vertex_count=6,.vertex_format=NVN_FORMAT_RGB32F,.vertex_stride=12,.flags=1}}};
    nvn_as_build blas=storage_n(g,2,0);
    const float s=.7071067811865475f;
    matrices[0]=matrix_identity_float4x4;matrices[0].columns[3].x=-25;
    matrices[1]=(simd_float4x4){.columns={{s,s,0,0},{-s,s,0,0},{0,0,1,0},{0,0,0,1}}};
    matrices[2]=(simd_float4x4){.columns={{2*s,2*s,0,0},{-.5f*s,.5f*s,0,0},{0,0,3,0},{25,0,0,1}}};
    matrices[3]=matrix_identity_float4x4;matrices[3].columns[3].x=1000;
    unsigned offsets[3][4]={{7,13,29,0xffffff},{29,7,13,0x654321},{3,9,17,0xabcdef}};
    packed_instance input[3][4],wrong[4];
    for(unsigned set=0;set<3;set++)for(unsigned i=0;i<4;i++)input[set][i]=packed(i,(set?29:17)+i*12,offsets[set][i],blas.destination);
    for(unsigned i=0;i<4;i++)wrong[i]=instance(1000,0,1,0,blas.destination);
    uint64_t instances=guest_buffer(sizeof wrong);memcpy((void *)(uintptr_t)instances,wrong,sizeof wrong);
    nvn_as_geometry tg={.type=3,.instances={.address=instances,.count=4}};
    nvn_as_build top=storage_n(&tg,1,NVN_AS_ALLOW_UPDATE|NVN_AS_ALLOW_COMPACTION);
    uint8_t *top_memory=NULL;if(posix_memalign((void **)&top_memory,getpagesize(),65536))return 2;memset(top_memory,0,65536);
    CHECK(top.destination_size<=65536,"fixture TLAS fits its separate pool");
    NVNmemoryPool top_pool;NVNmemoryPoolBuilder pb={0};h_nvnMemoryPoolBuilderSetDefaults(&pb);h_nvnMemoryPoolBuilderSetStorage(&pb,top_memory,65536);h_nvnMemoryPoolBuilderSetFlags(&pb,0x24);
    CHECK(h_nvnMemoryPoolInitialize(&top_pool,&pb),"separate TLAS pool initializes");NVNbuffer top_buffer;NVNbufferBuilder bb={0};h_nvnBufferBuilderSetDefaults(&bb);h_nvnBufferBuilderSetStorage(&bb,&top_pool,0,top.destination_size);
    CHECK(h_nvnBufferInitialize(&top_buffer,&bb),"TLAS storage view initializes");top.destination=h_nvnBufferGetAddress(&top_buffer);

    begin();h_nvnCommandBufferUpdateUniformBuffer(&commands,instances,sizeof input[0],0,sizeof input[0],input[0]);record_build(&blas,NULL);record_build(&top,NULL);replay(end());
    nvn_as_binding empty={.resource_id=~0ull};CHECK(!mtl_as_acquire(0,&empty)&&!memcmp(&empty,&(nvn_as_binding){0},sizeof empty),"null AS acquisition is explicitly empty");
    nvn_as_binding bottom=acquire(blas.destination);CHECK(bottom.type==NVN_AS_BINDING_BLAS&&!bottom.instance_count&&!bottom.instance_sbt_offsets,"BLAS binding needs no instance snapshot");
    nvn_as_binding first=acquire(top.destination);CHECK(first.type==NVN_AS_BINDING_TLAS&&first.instance_count==4&&first.instance_sbt_offsets,"TLAS binding has one GPU u32 per source instance");probe(first,input[0]);
    begin();h_nvnCommandBufferUpdateUniformBuffer(&commands,instances,sizeof wrong,0,sizeof wrong,wrong);append_recording();blit_raw();
    nvn_as_binding reused=acquire(top.destination);CHECK(reused.instance_sbt_offsets==first.instance_sbt_offsets,"reusing input bytes leaves the built snapshot unchanged");probe(reused,input[0]);

    nvn_as_build refit=top;refit.operation=1;refit.source=top.destination;
    begin();h_nvnCommandBufferUpdateUniformBuffer(&commands,instances,sizeof input[1],0,sizeof input[1],input[1]);record_build(&refit,NULL);append_recording();
    nvn_as_binding second=acquire(top.destination);CHECK(second.resource_id==first.resource_id&&second.instance_sbt_offsets!=first.instance_sbt_offsets,"in-place refit versions only immutable SBT ownership");probe(second,input[1]);
    begin();h_nvnCommandBufferUpdateUniformBuffer(&commands,instances,sizeof input[2],0,sizeof input[2],input[2]);record_build(&top,NULL);append_recording();
    nvn_as_binding third=acquire(top.destination);CHECK(third.resource_id==first.resource_id&&third.instance_sbt_offsets!=second.instance_sbt_offsets,"compatible rebuild preserves actual AS identity with a new snapshot");probe(third,input[2]);

    uint64_t copied=guest_buffer(top.destination_size),compacted=guest_buffer(top.destination_size);
    begin();h_nvnCommandBufferUpdateUniformBuffer(&commands,instances,sizeof wrong,0,sizeof wrong,wrong);copy_as(&commands,top.destination,copied);compact_as(&commands,top.destination,compacted);append_recording();
    nvn_as_binding copy=acquire(copied),compact=acquire(compacted);CHECK(copy.instance_sbt_offsets==third.instance_sbt_offsets&&compact.instance_sbt_offsets==third.instance_sbt_offsets,"copy/compaction share the actual immutable source snapshot");
    probe(copy,input[2]);probe(compact,input[2]);
    begin();h_nvnCommandBufferUpdateUniformBuffer(&commands,instances,sizeof input[0],0,sizeof input[0],input[0]);record_build(&refit,NULL);append_recording();nvn_as_binding fourth=acquire(top.destination);
    CHECK(fourth.instance_sbt_offsets!=third.instance_sbt_offsets,"refitting original does not overwrite copy/compact snapshots");probe(fourth,input[0]);probe(acquire(copied),input[2]);

    h_nvnBufferFinalize(&top_buffer);h_nvnMemoryPoolFinalize(&top_pool);CHECK(as_find(top.destination)==nil,"pool retirement removes the guest mapping");probe(fourth,input[0]);
    CHECK(h_nvnMemoryPoolInitialize(&top_pool,&pb)&&h_nvnBufferInitialize(&top_buffer,&bb),"same guest storage reinitializes");
    begin();h_nvnCommandBufferUpdateUniformBuffer(&commands,instances,sizeof input[1],0,sizeof input[1],input[1]);record_build(&top,NULL);append_recording();nvn_as_binding replacement=acquire(top.destination);
    CHECK(replacement.resource_id!=fourth.resource_id&&replacement.instance_sbt_offsets!=fourth.instance_sbt_offsets,"address reuse gets an independent AS and SBT owner");probe(replacement,input[1]);probe(fourth,input[0]);
    finish();check_probes();
    // Persistent snapshot remains usable in a later submission, after the arena
    // conversion output and caller input have both been reused.
    mtl_sub_begin();begin();h_nvnCommandBufferUpdateUniformBuffer(&commands,instances,sizeof wrong,0,sizeof wrong,wrong);append_recording();blit_raw();probe(acquire(copied),input[2]);probe(acquire(compacted),input[2]);finish();check_probes();

    h_nvnBufferFinalize(&top_buffer);h_nvnMemoryPoolFinalize(&top_pool);free(top_memory);res_remove(sbt_records);sbt_records=nil;
    mtl_queue_forget(910004);for(unsigned i=0;i<buffer_count;i++)h_nvnBufferFinalize(&buffers[i]);h_nvnCommandBufferFinalize(&commands);mtl_pool_del(memory);nvn_dirty_remove_range(memory);free(memory);
    printf("t_nvn_accel_sbt: %d checks, %d failed (encoder thread %s)\n",checks,failed,getenv("DM_NVN_ENC_THREAD"));return failed!=0;
} }
int main(int argc,char **argv) {
    if(argc>1){setenv("DM_NVN_ENC_THREAD",argv[1],1);return sbt_run();}
    extern char **environ;int result=0;
    for(int mode=0;mode<2;mode++){char *args[]={argv[0],mode?"1":"0",NULL};pid_t pid;int status=0;if(posix_spawn(&pid,argv[0],NULL,NULL,args,environ)||waitpid(pid,&status,0)<0||!WIFEXITED(status)||WEXITSTATUS(status))result=1;}return result;
}
