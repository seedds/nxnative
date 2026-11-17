// Public command recording and real queue/program ownership. The marker is
// deliberately not an RT implementation; no pipeline or trace is submitted.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
static int test_rt_program_set(prog_t *,int,const NVNshaderData *,uint32_t,uint32_t,uint32_t);
static void test_rt_group_handle(const prog_t *,const int32_t[3],void *);
#define mtl_rt_program_set test_rt_program_set
#define mtl_rt_group_handle test_rt_group_handle
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
#undef mtl_rt_group_handle
#undef mtl_rt_program_set

// These APIs are outside the state test. Any accidental call is a test failure,
// rather than a successful substitute for production backend initialization.
static int test_rt_program_set(prog_t *p,int n,const NVNshaderData *s,uint32_t t,uint32_t c,uint32_t f) {
    (void)p;(void)n;(void)s;(void)t;(void)c;(void)f; abort();
}
static void test_rt_group_handle(const prog_t *p,const int32_t i[3],void *o) { (void)p;(void)i;(void)o; abort(); }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks,failed;
static _Atomic int markers_released;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr,"FAIL: %s\n",label); } } while (0)
@interface RtStateMarker : NSObject @end
@implementation RtStateMarker
- (void)dealloc { atomic_fetch_add(&markers_released,1); }
@end
static NVNcommandBuffer commands;

static prog_t program(int rt) {
    prog_t p={.magic=MAGIC('P','R','O','G'),.nstage=rt?0:1,.serial=nvn_new_serial()};
    NvProg *object=[NvProg new];
    if(rt) object->raytracing=(NvRayProgram *)[RtStateMarker new];
    serial_put(p.serial,object); return p;
}
static unsigned apply(NVNcommandHandle handle) {
    size_t length; uint8_t *bytes=nvn_rec_linear(handle,&length); unsigned records=0;
    for(size_t off=0;off<length;) {
        op_hdr h;memcpy(&h,bytes+off,sizeof h); if(h.op==OP_END)break;
        CHECK(h.size>=8&&h.size<=length-off,"expanded record is bounded");
        if(h.op==OP_PUSH_CONST) {
            const p_push_const *p=(const void *)(bytes+off+8);
            CHECK(p->offset<=sizeof S.push[0]&&p->size<=sizeof S.push[0]-p->offset,"push record is within its eight-bank group");
        }
        CHECK(state_op(h.op,bytes+off+8),"record replays through production state_op");
        off+=h.size;records++;
    }
    free(bytes);return records;
}
static void bind(const prog_t *p,int mask) {
    h_nvnCommandBufferBeginRecording(&commands);
    h_nvnCommandBufferBindProgram(&commands,(const NVNprogram *)p,mask);
    CHECK(apply(h_nvnCommandBufferEndRecording(&commands))==1,"public program bind records exactly once");
}
static void binding(void) {
    mtl_queue_activate(780101);
    prog_t graphics=program(0),compute=program(0),rt=program(1),other=program(1);
    bind(&graphics,3);bind(&compute,0x20);
    CHECK(S.prog==&graphics&&S.cprog==&compute&&!S.rprog,"graphics and compute have separate initial bindings");
    for(unsigned i=0;i<NVN_RESOURCE_STAGES;i++)g_args_wrote[i]=g_args_seq;
    uint64_t graphics_gen=g_args_gen[0],compute_gen=g_args_gen[5];
    bind(&rt,0x100);
    CHECK(S.rprog==&rt&&S.prog==&graphics&&S.cprog==&compute,"RT stage identity binds only ray tracing");
    CHECK(g_args_gen[0]==graphics_gen&&g_args_gen[5]==compute_gen,"RT bind leaves raster/compute argument generations alone");
    uint64_t rt_gen=g_args_gen[6];bind(&rt,0x100);
    CHECK(g_args_gen[6]==rt_gen,"same RT bind does not dirty arguments again");
    for(unsigned i=0;i<3;i++) {
        int mask=(int[]){0x20,3,0x1ff}[i];
        bind(&other,mask);
        CHECK(S.rprog==&other&&S.cprog==&compute&&S.prog==&graphics,"nonnull RT identity wins before caller stage-mask intersection");
        bind(&rt,0x100);
    }
    bind(&other,0);bind(NULL,0x100);
    CHECK(S.rprog==&rt&&S.cprog==&compute&&S.prog==&graphics,"zero mask and null RT mask leave RT state unchanged");
    prog_t missing={.serial=nvn_new_serial()},uninitialized={0},wrong=other;wrong.nstage=1;
    bind(&missing,0x100);bind(&uninitialized,0x100);bind(&wrong,0x100);
    CHECK(S.rprog==&rt,"missing serial or ordinary stage metadata cannot become RT identity");
    bind(&graphics,3);bind(&compute,0x20);
    CHECK(S.rprog==&rt,"ordinary program binding preserves the RT program");
    mtl_queue_forget(780101);
    for(unsigned i=0;i<4;i++)mtl_prog_release(&(prog_t[]){graphics,compute,rt,other}[i]);
}
static void ownership(void) {
    mtl_queue_activate(780102);
    prog_t p[7];uint64_t serial[7];
    for(int i=0;i<7;i++) {p[i]=program(i==6);serial[i]=p[i].serial;}
    for(int i=0;i<5;i++)S.bst[i]=&p[i];S.cprog=&p[5];S.rprog=&p[6];
    queue_program_preserve();queue_state *first=g_active_queue;
    CHECK(S.rprog!=&p[6]&&S.rprog->serial==serial[6],"queue owns an immutable RT descriptor after replay");
    CHECK(S.cprog->serial==serial[5],"seventh program slot does not displace compute");
    for(int i=0;i<7;i++) {
        CHECK(first->programs[i].serial==serial[i],"all seven distinct bound program serials are preserved");
        mtl_prog_release(&p[i]);memset(&p[i],0xa5,sizeof p[i]);
        CHECK(serial_get(serial[i])!=nil,"queue pin survives guest finalization and descriptor reuse");
    }
    prog_t second_rt=*S.rprog;
    queue_program_preserve(); // Already-owned descriptors must not add pins.
    mtl_queue_activate(780103);
    CHECK(!S.rprog&&!S.cprog&&!S.prog,"new queue starts with no RT/compute/raster program");
    bind(&second_rt,0x100);queue_program_preserve();
    CHECK(S.rprog!=&second_rt&&S.rprog->serial==serial[6],"second queue independently owns the same RT serial");
    mtl_queue_activate(780102);
    CHECK(S.rprog->serial==serial[6]&&S.cprog->serial==serial[5],"queue restore keeps RT and compute bindings distinct");
    mtl_queue_forget(780102);
    for(int i=0;i<6;i++)CHECK(serial_get(serial[i])==nil,"queue destruction releases every retired raster/compute pin");
    CHECK(serial_get(serial[6])!=nil,"other queue still pins retired RT program");
    mtl_queue_activate(780103);
    CHECK(S.rprog&&S.rprog->serial==serial[6],"remaining queue's RT snapshot survives original descriptor reuse");
    mtl_queue_forget(780103);
    CHECK(!S.rprog&&serial_get(serial[6])==nil,"last queue destruction releases the retired RT program");
}
static void forgotten(void) {
    prog_t rt=program(1),graphics=program(0),compute=program(0);
    mtl_queue_activate(780104);bind(&graphics,3);bind(&compute,0x20);bind(&rt,0x100);
    mtl_queue_activate(780105);bind(&graphics,3);bind(&compute,0x20);bind(&rt,0x100);
    h_nvnProgramFinalize((NVNprogram *)&rt);
    CHECK(!rt.magic,"public finalization invalidates the guest RT descriptor");
    CHECK(!S.rprog&&S.prog==&graphics&&S.cprog==&compute,"finalize forgets active RT binding without changing other scopes");
    mtl_queue_activate(780104);
    CHECK(!S.rprog&&S.prog==&graphics&&S.cprog==&compute,"finalize also forgets inactive queues' raw RT binding");
    mtl_queue_forget(780104);mtl_queue_forget(780105);
    mtl_prog_release(&graphics);mtl_prog_release(&compute);
}
static void pushes(int compact) {
    g_rec_compact=compact;mtl_queue_activate(780106);
    uint8_t expected[2][8*256]={0},data[256];
    CHECK(DMSH_PUSH_BLOCKS==8&&sizeof S.push==sizeof expected,"both push scopes own exactly eight 256-byte banks");
    h_nvnCommandBufferBeginRecording(&commands);
    for(int scope=0;scope<2;scope++)for(int block=0;block<8;block++) {
        for(int byte=0;byte<256;byte++)data[byte]=(uint8_t)(scope*91+block*17+byte);
        memcpy(expected[scope]+block*256,data,256);
        if(scope)h_nvnCommandBufferPushComputeConstants(&commands,block,0,256,data);
        else h_nvnCommandBufferPushGraphicsConstants(&commands,block,0,256,data);
        memset(data,0xed,sizeof data); // The immediate API owns its recorded bytes.
    }
    CHECK(apply(h_nvnCommandBufferEndRecording(&commands))==16,"all eight blocks in both scopes record");
    CHECK(!memcmp(S.push,expected,sizeof expected),"full eight-bank patterns replay without merging graphics and compute");
    h_nvnCommandBufferBeginRecording(&commands);
    for(int scope=0;scope<2;scope++) {
        void (*push)(NVNcommandBuffer *,uint64_t,uint64_t,uint64_t,const void *)=scope?h_nvnCommandBufferPushComputeConstants:h_nvnCommandBufferPushGraphicsConstants;
        push(&commands,8,0,4,data);push(&commands,UINT64_MAX,0,4,data);
        push(&commands,0,UINT64_MAX,1,data);push(&commands,0,1,UINT64_MAX,data);
        push(&commands,0,0,257,data);push(&commands,0,255,2,data);
        push(&commands,0,257,0,data);push(&commands,0,0,4,NULL);
    }
    CHECK(apply(h_nvnCommandBufferEndRecording(&commands))==0,"invalid banks, overflowing ranges and null data never record");
    CHECK(!memcmp(S.push,expected,sizeof expected),"rejected push ranges preserve both scopes");
    mtl_queue_activate(780107);
    uint8_t zero[sizeof S.push]={0};CHECK(!memcmp(S.push,zero,sizeof zero),"new queue has its own empty push scopes");
    uint32_t graphics=0x12345678,compute=0xfedcba98;
    h_nvnCommandBufferBeginRecording(&commands);
    h_nvnCommandBufferPushGraphicsConstants(&commands,7,252,4,&graphics);
    h_nvnCommandBufferPushComputeConstants(&commands,7,252,4,&compute);
    apply(h_nvnCommandBufferEndRecording(&commands));
    memcpy(zero+7*256+252,&graphics,4);memcpy(zero+8*256+7*256+252,&compute,4);
    CHECK(!memcmp(S.push,zero,sizeof zero),"last word of bank7 is valid and separate in each scope");
    mtl_queue_activate(780106);CHECK(!memcmp(S.push,expected,sizeof expected),"queue restore preserves every push bank");
    mtl_queue_activate(780107);CHECK(!memcmp(S.push,zero,sizeof zero),"second queue restores its own bank7 values");
    mtl_queue_forget(780106);mtl_queue_forget(780107);
}
int main(void) { @autoreleasepool {
    mtl_init();CHECK(h_nvnCommandBufferInitialize(&commands,NULL),"command buffer initializes");
    @autoreleasepool {binding();ownership();forgotten();pushes(0);pushes(1);}
    // prog_get keeps its last lookup strongly cached independently of serial
    // ownership. Replace that cache with an ordinary program before observing
    // marker destruction, so this checks queue/owner references alone.
    @autoreleasepool {prog_t plain=program(0);prog_get(&plain);mtl_prog_release(&plain);}
    // Production retirement also holds objects until a real submission fence.
    // No draw, dispatch or ray query is needed to complete that ownership path.
    mtl_sub_begin();uint64_t fence=mtl_sub_commit();
    CHECK(mtl_wait(fence,5000000000ull)&&!mtl_last_commit_error(),"retirement fence completes");mtl_drain();
    CHECK(atomic_load(&markers_released)==4,"RT marker objects die after the last owner releases their serial");
    h_nvnCommandBufferFinalize(&commands);
    printf("t_nvn_rt_state: %d checks, %d failed\n",checks,failed);return failed!=0;
} }
