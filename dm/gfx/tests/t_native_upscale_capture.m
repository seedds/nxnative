// Literal GPU test of the before/after capture and restoration of native state.
#define main existing_store_main
#include "t_unwritten_store.m"
#undef main
#include <unistd.h>
int main(void){@autoreleasepool{
    alarm(35);setbuf(stdout,NULL);char arm[256],out[256];snprintf(arm,sizeof arm,"/tmp/native-upscale-%d.arm",getpid());snprintf(out,sizeof out,"/tmp/native-upscale-%d",getpid());unlink(arm);
    setenv("DM_NVN_NATIVE_CAPTURE_ARM",arm,1);setenv("DM_NVN_NATIVE_CAPTURE_OUT",out,1);setenv("DM_NVN_NATIVE_CAPTURE_KEYS","0123456789abcdef",1);setenv("DM_NVN_NATIVE_CAPTURE_FRAMES","1",1);
    mtl_init();mtl_set_frame(10);CHECK(g_ncap_first==UINT64_MAX,"unarmed capture leaves rendering unchanged");
    FILE *f=fopen(arm,"w");fclose(f);mtl_set_frame(11);
    NSError *error=nil;NSString *code=@"#include <metal_stdlib>\nusing namespace metal;\nstruct A{device uint* values[[id(0)]];constant uint* delta[[id(1)]];texture2d<uint,access::read> input[[id(2)]];texture2d<uint,access::write> output[[id(3)]];};\nkernel void main0(constant A&a[[buffer(0)]],uint i[[thread_position_in_grid]]){if(i<16){a.values[i]+=a.delta[0];uint2 p(i&3,i>>2);a.output.write(a.input.read(p)+uint4(7,0,0,0),p);}}";
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:code options:nil error:&error];id<MTLFunction> fn=[lib newFunctionWithName:@"main0"];id<MTLComputePipelineState> pipeline=[g_dev newComputePipelineStateWithFunction:fn error:&error];CHECK(pipeline!=nil,"native fixture shader compiles");if(!pipeline)return 2;
    uint32_t initial[16],delta[64]={3};for(int i=0;i<16;i++)initial[i]=10+i;
    id<MTLBuffer> values=[g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared],seed=[g_dev newBufferWithBytes:initial length:64 options:MTLResourceStorageModeShared],parameters=[g_dev newBufferWithBytes:delta length:256 options:MTLResourceStorageModeShared];res_add(values);res_add(seed);res_add(parameters);
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:4 height:4 mipmapped:NO];td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
    id<MTLTexture> source=[g_dev newTextureWithDescriptor:td],destination=[g_dev newTextureWithDescriptor:td];res_add(source);res_add(destination);uint32_t pixels[16];for(int i=0;i<16;i++)pixels[i]=40;[source replaceRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0 withBytes:pixels bytesPerRow:16];for(int i=0;i<16;i++)pixels[i]=5;[destination replaceRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0 withBytes:pixels bytesPerRow:16];
    NvShader *shader=[NvShader new];strcpy(shader->key,"0123456789abcdef_b");shader->stage=5;shader->rf.nb=4;shader->rf.tg[0]=16;shader->rf.tg[1]=shader->rf.tg[2]=1;
    shader->rf.b[0]=(nvn_rbind){.kind=NVN_RB_SBUFP,.reg=2,.aid=0,.rw=1};shader->rf.b[1]=(nvn_rbind){.kind=NVN_RB_PUSH,.aid=1};shader->rf.b[2]=(nvn_rbind){.kind=NVN_RB_TEX,.reg=0,.aid=2};shader->rf.b[3]=(nvn_rbind){.kind=NVN_RB_IMG,.reg=1,.aid=3,.rw=1};
    shader->ae=[fn newArgumentEncoderWithBufferIndex:0];shader->arg_len=shader->ae.encodedLength;shader->ab_buf=[g_dev newBufferWithLength:shader->arg_len options:MTLResourceStorageModeShared];res_add(shader->ab_buf);[shader->ae setArgumentBuffer:shader->ab_buf offset:0];[shader->ae setBuffer:values offset:0 atIndex:0];[shader->ae setBuffer:parameters offset:0 atIndex:1];[shader->ae setTexture:source atIndex:2];[shader->ae setTexture:destination atIndex:3];
    S.ssbo[5][2]=(p_bind_buf){.addr=(uint64_t)values.contents,.size=64};ncap_begin_args(shader,5);ncap_texture(shader,5,2,0,source);ncap_texture(shader,5,3,0,destination);
    mtl_sub_begin();enc_b_copy_buffer(blit(),seed,0,values,0,64);CHECK(((uint32_t *)values.contents)[0]==0,"source still has stale CPU bytes before GPU execution");
    id<MTL4ComputeCommandEncoder> encoder=dispatch_enc();enc_c_pso(encoder,pipeline);enc_c_args(encoder,shader->ab_buf.gpuAddress,0);p_dispatch dispatch={.x=1,.y=1,.z=1};
    ncap_dispatch(shader,&dispatch,0,encoder,pipeline,0);enc_c_dispatch(encoder,MTLSizeMake(1,1,1),MTLSizeMake(16,1,1));ncap_dispatch(shader,&dispatch,0,encoder,pipeline,1);
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"native dispatch and captures complete");CHECK(!mtl_last_commit_error(),"GPU reports no error");mtl_set_frame(12);
    NSData *raw=[NSData dataWithContentsOfFile:[@(out) stringByAppendingPathComponent:@"receipt.json"]];NSDictionary *receipt=[NSJSONSerialization JSONObjectWithData:raw options:0 error:nil];CHECK([receipt[@"complete"] boolValue]&&![receipt[@"failed"] boolValue]&&![receipt[@"truncated"] boolValue]&&[receipt[@"io_ok"] boolValue],"receipt is complete and all bindings were captured");NSArray *records=receipt[@"records"];CHECK(records.count==2,"both ordered capture points are present");
    for(int phase=0;phase<2;phase++){
        NSDictionary *record=records[phase];CHECK([record[@"phase"] isEqual:phase?@"after":@"before"],"capture phase is explicit");
        for(NSDictionary *resource in record[@"resources"]){NSData *data=[NSData dataWithContentsOfFile:[@(out) stringByAppendingPathComponent:resource[@"file"]]];unsigned argument=[resource[@"argument"] unsignedIntValue];
            if(argument==0){int ok=data.length==64;for(int i=0;i<16&&ok;i++)ok=((const uint32_t *)data.bytes)[i]==(uint32_t)(10+i+(phase?3:0));CHECK(ok,phase?"post-dispatch buffer reflects native writes":"pre-dispatch snapshot sees the preceding GPU copy");}
            if(argument==1){CHECK(!phase&&data.length==256&&((const uint32_t *)data.bytes)[0]==3,"effective constants are captured exactly");}
            if(argument==2){int ok=data.length==64;for(int i=0;i<16&&ok;i++)ok=((const uint32_t *)data.bytes)[i]==40;CHECK(ok,"actual input texels and tight row pitch are preserved");}
            if(argument==3){int ok=data.length==64;for(int i=0;i<16&&ok;i++)ok=((const uint32_t *)data.bytes)[i]==(uint32_t)(phase?47:5);CHECK(ok,phase?"output texture reflects the native kernel":"read-write output state is captured before execution");}
        }
    }
    CHECK(((uint32_t *)values.contents)[0]==13,"capture restored the native argument table and pipeline");CHECK(g_ncap_done,"capture stops at its frame bound");unlink(arm);printf("NATIVE_CAPTURE %d checks, %d failed; %s\n",checks,failed,out);return failed!=0;
}}
