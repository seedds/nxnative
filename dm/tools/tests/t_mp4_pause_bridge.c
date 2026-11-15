// Execute emitted camera bridges only, with a mock continuation/lookup.
#include "../../games/010019A01E2F2000/pause_ui.inc"
#include "../../host/runtime_patch.h"
#include "../../games/010019A01E2F2000/menu_trace.inc"
#include <assert.h>
#include <pthread.h>
#include <sys/mman.h>
static unsigned checks,calls,snapshots;static int available;
#define CHECK(x) do {checks++;assert(x);}while(0)
static void callback(const void *c,void *data,unsigned event){calls++;if(event==2){mp4_menu_snapshot s;snapshots+=mp4_menu_snapshot_read(c,data,&s);}else mp4_pause_apply(c,data,event);}
static void *lookup(const char *s){CHECK(!strcmp(s,"nxnativeMp4PauseUiV2"));return available?(void *)callback:NULL;}
typedef struct {uint8_t q[32];uint64_t before,after,x19,x20,x0,x8;} trace;
static void invoke(void *fn,void *c,void *data,const void *q,trace *t) {
    __asm__ volatile(
        "sub sp,sp,#0x400\nstr x29,[sp,#0x3f0]\nadd x29,%[data],#0xe8\n"
        "mov x16,%[fn]\nmov x22,%[trace]\nmov x21,%[c]\nmov x19,%[c]\nmov x20,#0x123\n"
        "mov x0,%[c]\nmov x2,%[data]\nmov x8,%[data]\nldp q0,q1,[%[q]]\n"
        "cmp xzr,xzr\nmrs x9,nzcv\nstr x9,[x22,#32]\nblr x16\n"
        "stp q0,q1,[x22]\nmrs x9,nzcv\nstr x9,[x22,#40]\n"
        "str x19,[x22,#48]\nstr x20,[x22,#56]\nstr x0,[x22,#64]\nstr x8,[x22,#72]\n"
        "ldr x29,[sp,#0x3f0]\nadd sp,sp,#0x400\n"
        : : [fn]"r"(fn),[c]"r"(c),[data]"r"(data),[q]"r"(q),[trace]"r"(t)
        : "x0","x1","x2","x3","x4","x5","x6","x7","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","x19","x20","x21","x22","x30","v0","v1","v2","v3","v4","v5","v6","v7","v16","v17","v18","v19","v20","v21","v22","v23","v24","v25","v26","v27","v28","v29","v30","v31","cc","memory");
}
int main(int argc,char **argv) {
    CHECK(argc==4);FILE*f=fopen(argv[1],"rb");CHECK(f);fseek(f,0,SEEK_END);size_t size=ftell(f);rewind(f);uint8_t *src=malloc(size);CHECK(src);CHECK(fread(src,1,size,f)==size);fclose(f);
    uint8_t *jit=mmap(NULL,size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANON|MAP_JIT,-1,0);CHECK(jit!=MAP_FAILED);
    uint8_t c[0x340]={0},owner[0x40]={0},props[0x60]={0};float world[15]={0};const void *p=props;memcpy(owner+0x30,&p,8);p=owner;memcpy(c+0x20,&p,8);
    const uint8_t id[]={0x68,0xd8,0x47,0xf0,0x68,0x20,0x88,0x4a,0x8f,0x84,0x6c,0x55,0xb0,0x2f,0x73,0x9d};
    for(int kind=0;kind<3;kind+=2)for(int mode=0;mode<5;mode++) {
        available=mode>=2;calls=0;snapshots=0;memcpy(props+12,id,16);if(kind==2){const uint8_t inventory[]={0xeb,0x1c,0x03,0x70,0xbd,0xd0,0x24,0x4f,0x8e,0x46,0x1a,0x34,0xa4,0x16,0x17,0x08};memcpy(props+12,inventory,16);p=world;memcpy(owner+0x38,&p,8);}if(mode==3)props[12]^=1;c[0x1c]=kind==2?0xd8:0;c[0x2fb]=0x5a; p=mode==4?NULL:owner;memcpy(c+0x20,&p,8);
        uint8_t data[0x140],before[0x140],want[0x140],q[32],expected_q[32];memset(data,0xa5,sizeof data);
        uint32_t ortho=2;float bounds[]={8.f/.9f,-8.f/.9f,-5,5},fov=55;
        memcpy(data,&ortho,4);memcpy(data+12,bounds,16);memcpy(data+8,&fov,4);memcpy(before,data,sizeof data);memcpy(want,data,sizeof data);
        for(int i=0;i<32;i++)q[i]=(uint8_t)(i*13+5);memcpy(expected_q,q,32);
        int change=kind!=2 && mode>=2 && mode!=3 && mode!=4;
        if(change)CHECK(kind?mp4_pause_fov(want):mp4_pause_bounds(want));
        pthread_jit_write_protect_np(0);memcpy(jit,src,size);uint32_t jump[]={0x58000050,0xd61f0200};memcpy(jit+0x138df60,jump,8);p=(void*)lookup;memcpy(jit+0x138df68,&p,8);
        setenv("DM_MENU_FIT",mode?"1610":"default",1);setenv("DM_MENU_TRACE",mode?"1":"default",1);f=fopen(argv[kind==2?3:2],"r");CHECK(f);unsigned applied;char error[256];CHECK(!runtime_patch_apply(f,"8275fbe1ec5191c279f3a943ad8524f4",0x138ee20,jit,size,error,sizeof error,&applied));fclose(f);CHECK(applied==(mode?(kind==2?1u:35u):0u));
        __builtin___clear_cache((char*)jit,(char*)jit+size);pthread_jit_write_protect_np(1);
        trace t={0};invoke(jit+(kind==2?0x101fc14:kind?0x3fd4c:0x916c0c),c,data,q,&t);
        CHECK(!memcmp(data,change?want:before,sizeof data));CHECK(t.x19==(uintptr_t)c);CHECK(t.x20==0x123);CHECK(t.x0==(uintptr_t)(kind?c:c+0x8c));
        if(kind==2){CHECK(t.x8==0x5a);CHECK(snapshots==(unsigned)(mode==2));}CHECK(t.before==t.after);CHECK(!memcmp(t.q,expected_q,32));CHECK(calls==(unsigned)(mode>=2));
    }
    munmap(jit,size);free(src);printf("PASS: %u native pause bridge checks; default, missing helper, matched/mismatched template, missing owner\n",checks);
}
