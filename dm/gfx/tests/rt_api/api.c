// Synthetic CPU routing/ABI fixture. The Python runner inserts the production
// HLE regions verbatim; only the recorder and backend are test-only spies.
#define mtl_rt_program_set spy_rt_program_set
#define mtl_rt_group_handle spy_rt_group_handle
#include "nvn_obj.h"
#include "nvn_capture.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <limits.h>
#include <pthread.h>
#include <errno.h>

static int checks, failures;
#define CHECK(test) do { checks++;if(!(test)){failures++;fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#test);} } while(0)
#define LOG(...) do { } while(0)
#define NVN(name,ret,...) static ret h_##name(__VA_ARGS__); \
    static const NVNT_##name signature_##name __attribute__((unused))=h_##name; \
    static ret h_##name(__VA_ARGS__)

static jmp_buf fatal;
static int expecting_fatal;
_Noreturn static void die(const char *format,...) {
    (void)format;
    if(expecting_fatal)longjmp(fatal,1);
    fprintf(stderr,"unexpected fatal\n");exit(90);
}

static int initialized, raster_calls, backend_calls, init_result=1, backend_result=1;
static int count_seen;
static uint32_t trace_seen, callable_seen, flags_seen;
static uint64_t serial_to_assign=123;
static const NVNshaderData *modules_seen;
static NVNdevice *device_seen;
static prog_t *program_seen;
static int32_t indices_seen[3];
static uint8_t handle_prefix[16]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
static pthread_mutex_t g_sub_mu=PTHREAD_MUTEX_INITIALIZER;
static void require_submit_lock(void) {
    int result=pthread_mutex_trylock(&g_sub_mu);CHECK(result==EBUSY);
    if(!result)pthread_mutex_unlock(&g_sub_mu);
}

static NVNboolean h_nvnProgramInitialize(NVNprogram *program,NVNdevice *device) {
    initialized++;device_seen=device;
    if(!init_result)return 0;
    memset(program,0,sizeof(prog_t));((prog_t *)program)->magic=MAGIC('P','R','O','G');return 1;
}
static NVNboolean h_nvnProgramSetShaders(NVNprogram *program,int n,const NVNshaderData *shaders) {
    (void)program;raster_calls++;count_seen=n;modules_seen=shaders;return 1;
}
int mtl_rt_program_set(prog_t *program,int n,const NVNshaderData *modules,uint32_t trace,uint32_t callable,uint32_t flags) {
    require_submit_lock();
    backend_calls++;program_seen=program;count_seen=n;modules_seen=modules;
    trace_seen=trace;callable_seen=callable;flags_seen=flags;
    if(backend_result)program->serial=serial_to_assign;
    return backend_result;
}
void mtl_rt_group_handle(const prog_t *program,const int32_t indices[3],void *out16) {
    require_submit_lock();
    program_seen=(prog_t *)program;memcpy(indices_seen,indices,sizeof indices_seen);
    memcpy(out16,handle_prefix,sizeof handle_prefix);
}

static p_rt_dispatch recorded[8];
static unsigned recorded_count, recorded_op;
static size_t recorded_bytes;
static void *rec(NVNcommandBuffer *command,uint32_t op,size_t bytes) {
    (void)command;
    if(recorded_count==8 || bytes!=sizeof(p_rt_dispatch))exit(91);
    recorded_op=op;recorded_bytes=bytes;return &recorded[recorded_count++];
}
#define REC(c,op,type) ((type *)rec(c,op,sizeof(type)))

/* PRODUCTION_HLE_RT */

static void builder(void) {
    union { max_align_t alignment;uint8_t bytes[0x48+16]; } storage;
    memset(&storage,0xa5,sizeof storage);
    NVNdevice device;NVNprogram program;
    void *b=storage.bytes;
    h_nvnProgramBuilderSetDevice(b,&device);
    h_nvnProgramBuilderSetDefaults(b);
    CHECK(h_nvnProgramBuilderGetDevice(b)==&device);
    CHECK(h_nvnProgramBuilderGetMaxTraceRecursionDepth(b)==2);
    CHECK(h_nvnProgramBuilderGetMaxCallableRecursionDepth(b)==2);
    CHECK(h_nvnProgramBuilderGetProgramRayTracingFlags(b)==0);
    CHECK(h_nvnProgramBuilderGetShaderCount(b)==0 && h_nvnProgramBuilderGetShaders(b)==NULL);
    CHECK(h_nvnProgramBuilderGetRayTracingModuleCount(b)==0 && h_nvnProgramBuilderGetRayTracingModuleData(b)==NULL);
    CHECK(sizeof(prog_t)==112 && sizeof(prog_builder_t)<=0x48);
    for(size_t i=sizeof(prog_builder_t);i<sizeof storage.bytes;i++)CHECK(storage.bytes[i]==0xa5);

    NVNshaderData raster[2]={{.data=1},{.data=2}},modules[3]={{.data=3},{.data=4},{.data=5}};
    h_nvnProgramBuilderSetShaders(b,2,raster);
    CHECK(h_nvnProgramBuilderGetShaders(b)==raster && h_nvnProgramBuilderGetShaderCount(b)==2);
    CHECK(h_nvnProgramInitializeFromBuilder(&program,b)==1 && raster_calls==1 && backend_calls==0);
    CHECK(initialized==1 && device_seen==&device && count_seen==2 && modules_seen==raster);

    h_nvnProgramBuilderSetShaders(b,0,NULL);
    h_nvnProgramBuilderSetRayTracingModules(b,3,modules);
    h_nvnProgramBuilderSetMaxTraceRecursionDepth(b,1);
    h_nvnProgramBuilderSetMaxCallableRecursionDepth(b,0);
    h_nvnProgramBuilderSetProgramRayTracingFlags(b,0x81000000u);
    modules[0].data=0x123456789; // The builder retained the caller's array, not an early snapshot.
    CHECK(h_nvnProgramBuilderGetRayTracingModuleData(b)==modules && h_nvnProgramBuilderGetRayTracingModuleCount(b)==3);
    CHECK(h_nvnProgramBuilderGetMaxTraceRecursionDepth(b)==1 && h_nvnProgramBuilderGetMaxCallableRecursionDepth(b)==0);
    CHECK(h_nvnProgramBuilderGetProgramRayTracingFlags(b)==0x81000000u);
    CHECK(h_nvnProgramInitializeFromBuilder(&program,b)==1 && raster_calls==1 && backend_calls==1);
    CHECK(program_seen==(prog_t *)&program && modules_seen==modules && modules_seen[0].data==0x123456789);
    CHECK(count_seen==3 && trace_seen==1 && callable_seen==0 && flags_seen==0x81000000u);
    CHECK(((prog_t *)&program)->serial==123);
    backend_result=0;
    CHECK(h_nvnProgramInitializeFromBuilder(&program,b)==0 && ((prog_t *)&program)->serial==0);
    backend_result=1;serial_to_assign=0;
    CHECK(h_nvnProgramInitializeFromBuilder(&program,b)==0); // No fake success from a backend without a serial.
    serial_to_assign=123;init_result=0;
    int before=backend_calls;
    CHECK(h_nvnProgramInitializeFromBuilder(&program,b)==0 && backend_calls==before);
    init_result=1;
    h_nvnProgramBuilderSetShaders(b,1,raster);
    CHECK(h_nvnProgramInitializeFromBuilder(&program,b)==0 && backend_calls==before);
    h_nvnProgramBuilderSetShaders(b,0,NULL);
    h_nvnProgramBuilderSetRayTracingModules(b,-1,modules);
    CHECK(h_nvnProgramInitializeFromBuilder(&program,b)==0 && backend_calls==before);
    h_nvnProgramBuilderSetRayTracingModules(b,1,NULL);
    CHECK(h_nvnProgramInitializeFromBuilder(&program,b)==0 && backend_calls==before);
}

static void group(void) {
    NVNprogram program={0};
    uint8_t output[48];memset(output,0xa7,sizeof output);
    NVNT_nvnProgramGetShaderGroupHandle volatile call=h_nvnProgramGetShaderGroupHandle;
    call(&program,(nvn_rt_group){{-7,101,INT32_MIN}},output+8);
    CHECK(program_seen==(prog_t *)&program);
    CHECK(indices_seen[0]==-7 && indices_seen[1]==101 && indices_seen[2]==INT32_MIN);
    CHECK(!memcmp(output+8,handle_prefix,16));
    for(unsigned i=0;i<sizeof output;i++)if(i<8 || i>=24)CHECK(output[i]==0xa7);
}

static void dispatch(void) {
    NVNcommandBuffer command;
    nvn_rt_table a={0x100000002,0x300000004,0x500000006},b={0x700000008,0x90000000a,0xb0000000c},c={0xd0000000e,0xf00000010,0x1100000012};
    nvn_rt_table a0=a,b0=b,c0=c;
    NVNT_nvnCommandBufferDispatchRays volatile call=h_nvnCommandBufferDispatchRays;
    call(&command,UINT64_C(0x12345678abcdef00),&a,&b,&c,17,9,5);
    memset(&a,0,sizeof a);memset(&b,0,sizeof b);memset(&c,0,sizeof c);
    CHECK(recorded_count==1 && recorded_op==OP_RT_DISPATCH && recorded_bytes==96);
    CHECK(recorded[0].raygen==UINT64_C(0x12345678abcdef00));
    CHECK(!memcmp(&recorded[0].miss,&a0,24) && !memcmp(&recorded[0].hit,&b0,24) && !memcmp(&recorded[0].callable,&c0,24));
    CHECK(recorded[0].width==17 && recorded[0].height==9 && recorded[0].depth==5 && recorded[0]._pad==0);
    call(&command,3,&a0,&b0,&c0,0,UINT32_MAX,UINT32_MAX);
    CHECK(recorded_count==2 && recorded[1].width==0 && recorded[1].height==UINT32_MAX);
    CHECK(!memcmp(&recorded[1].miss,&a0,24));
    call(&command,4,&a0,&b0,&c0,0x40000000,1,1);
    CHECK(recorded_count==3 && recorded[2].width==0x40000000);
    expecting_fatal=1;
    if(!setjmp(fatal)){call(&command,0,&a0,&b0,&c0,0x40000001,1,1);CHECK(0);}else CHECK(1);
    if(!setjmp(fatal)){call(&command,0,&a0,&b0,&c0,65536,65536,1);CHECK(0);}else CHECK(1);
    if(!setjmp(fatal)){call(&command,0,&a0,&b0,&c0,1,UINT32_MAX,UINT32_MAX);CHECK(0);}else CHECK(1);
    if(!setjmp(fatal)){call(&command,0,NULL,&b0,&c0,1,1,1);CHECK(0);}else CHECK(1);
    NVNT_nvnCommandBufferDispatchRaysIndirect volatile indirect=h_nvnCommandBufferDispatchRaysIndirect;
    if(!setjmp(fatal)){indirect(&command,0,&a0,&b0,&c0,UINT64_C(0x123456789));CHECK(0);}else CHECK(1);
    expecting_fatal=0;
    CHECK(recorded_count==3);
    CHECK(OP_BIND_AS==62 && OP_RT_DISPATCH==63 && OP_COUNT_==64);
    CHECK(cap_record_unsupported(OP_RT_DISPATCH)!=NULL);
}

int main(void) {
    builder();group();dispatch();
    printf("RT CPU API: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
