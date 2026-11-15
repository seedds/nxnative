// Execute only the emitted HUD bridge with mock lookup/constructor; never gameplay.
#include "../../games/010019A01E2F2000/hud_layout.inc"
#include "../../host/runtime_patch.h"
#include <assert.h>
#include <pthread.h>
#include <sys/mman.h>
static unsigned checks,lookups,calls;
#define CHECK(x) do {checks++;assert(x);}while(0)
typedef struct {uint64_t in[18],out[18];uint8_t qin[512],qout[512];uint64_t before,after,record;} trace;
extern void hud_invoke(void *,void *,float *,trace *);
static const void *expected_properties;static int enabled;
static void apply(const void *p,void *m) {CHECK(p==expected_properties);CHECK(mp4_hud_apply(p,m));calls++;}
static void *lookup(const char *name) {CHECK(!strcmp(name,"nxnativeMp4HudLayoutV1"));lookups++;return enabled?(void *)apply:NULL;}
__asm__(
    ".text\n"
    ".p2align 2\n"
    ".globl _hud_invoke\n"
    "_hud_invoke:\n"
    "stp x29,x30,[sp,#-128]!\n"
    "stp x19,x20,[sp,#16]\n"
    "stp x21,x22,[sp,#32]\n"
    "stp x23,x24,[sp,#48]\n"
    "stp d8,d9,[sp,#64]\n"
    "stp d10,d11,[sp,#80]\n"
    "stp d12,d13,[sp,#96]\n"
    "stp d14,d15,[sp,#112]\n"
    "mov x19,x1\n"
    "mov x20,x0\n"
    "mov x21,x2\n"
    "mov x22,x3\n"
    "sub sp,sp,#80\n"
    "add x23,sp,#4\n"
    "ldr q0,[x21,#0]\n"
    "str q0,[x23,#0]\n"
    "ldr q0,[x21,#16]\n"
    "str q0,[x23,#16]\n"
    "ldr q0,[x21,#32]\n"
    "str q0,[x23,#32]\n"
    "ldr q0,[x21,#48]\n"
    "str q0,[x23,#48]\n"
    "ldr x0,[x21,#64]\n"
    "str x0,[x23,#64]\n"
    "ldr w0,[x21,#72]\n"
    "str w0,[x23,#72]\n"
    "add x23,x22,#288\n"
    "ldr q0,[x23,#0]\n"
    "ldr q1,[x23,#16]\n"
    "ldr q2,[x23,#32]\n"
    "ldr q3,[x23,#48]\n"
    "ldr q4,[x23,#64]\n"
    "ldr q5,[x23,#80]\n"
    "ldr q6,[x23,#96]\n"
    "ldr q7,[x23,#112]\n"
    "ldr q8,[x23,#128]\n"
    "ldr q9,[x23,#144]\n"
    "ldr q10,[x23,#160]\n"
    "ldr q11,[x23,#176]\n"
    "ldr q12,[x23,#192]\n"
    "ldr q13,[x23,#208]\n"
    "ldr q14,[x23,#224]\n"
    "ldr q15,[x23,#240]\n"
    "ldr q16,[x23,#256]\n"
    "ldr q17,[x23,#272]\n"
    "ldr q18,[x23,#288]\n"
    "ldr q19,[x23,#304]\n"
    "ldr q20,[x23,#320]\n"
    "ldr q21,[x23,#336]\n"
    "ldr q22,[x23,#352]\n"
    "ldr q23,[x23,#368]\n"
    "ldr q24,[x23,#384]\n"
    "ldr q25,[x23,#400]\n"
    "ldr q26,[x23,#416]\n"
    "ldr q27,[x23,#432]\n"
    "ldr q28,[x23,#448]\n"
    "ldr q29,[x23,#464]\n"
    "ldr q30,[x23,#480]\n"
    "ldr q31,[x23,#496]\n"
    "ldp x0,x1,[x22,#0]\n"
    "ldp x2,x3,[x22,#16]\n"
    "ldp x4,x5,[x22,#32]\n"
    "ldp x6,x7,[x22,#48]\n"
    "ldp x8,x9,[x22,#64]\n"
    "ldp x10,x11,[x22,#80]\n"
    "ldp x12,x13,[x22,#96]\n"
    "ldp x14,x15,[x22,#112]\n"
    "ldp x16,x17,[x22,#128]\n"
    "cmp xzr,xzr\n"
    "mrs x24,nzcv\n"
    "str x24,[x22,#1312]\n"
    "blr x20\n"
    "add x23,x22,#144\n"
    "stp x0,x1,[x23,#0]\n"
    "stp x2,x3,[x23,#16]\n"
    "stp x4,x5,[x23,#32]\n"
    "stp x6,x7,[x23,#48]\n"
    "stp x8,x9,[x23,#64]\n"
    "stp x10,x11,[x23,#80]\n"
    "stp x12,x13,[x23,#96]\n"
    "stp x14,x15,[x23,#112]\n"
    "stp x16,x17,[x23,#128]\n"
    "add x23,x22,#800\n"
    "str q0,[x23,#0]\n"
    "str q1,[x23,#16]\n"
    "str q2,[x23,#32]\n"
    "str q3,[x23,#48]\n"
    "str q4,[x23,#64]\n"
    "str q5,[x23,#80]\n"
    "str q6,[x23,#96]\n"
    "str q7,[x23,#112]\n"
    "str q8,[x23,#128]\n"
    "str q9,[x23,#144]\n"
    "str q10,[x23,#160]\n"
    "str q11,[x23,#176]\n"
    "str q12,[x23,#192]\n"
    "str q13,[x23,#208]\n"
    "str q14,[x23,#224]\n"
    "str q15,[x23,#240]\n"
    "str q16,[x23,#256]\n"
    "str q17,[x23,#272]\n"
    "str q18,[x23,#288]\n"
    "str q19,[x23,#304]\n"
    "str q20,[x23,#320]\n"
    "str q21,[x23,#336]\n"
    "str q22,[x23,#352]\n"
    "str q23,[x23,#368]\n"
    "str q24,[x23,#384]\n"
    "str q25,[x23,#400]\n"
    "str q26,[x23,#416]\n"
    "str q27,[x23,#432]\n"
    "str q28,[x23,#448]\n"
    "str q29,[x23,#464]\n"
    "str q30,[x23,#480]\n"
    "str q31,[x23,#496]\n"
    "mrs x24,nzcv\n"
    "str x24,[x22,#1320]\n"
    "add x23,sp,#4\n"
    "str x23,[x22,#1328]\n"
    "ldr q0,[x23,#0]\n"
    "str q0,[x21,#0]\n"
    "ldr q0,[x23,#16]\n"
    "str q0,[x21,#16]\n"
    "ldr q0,[x23,#32]\n"
    "str q0,[x21,#32]\n"
    "ldr q0,[x23,#48]\n"
    "str q0,[x21,#48]\n"
    "ldr x0,[x23,#64]\n"
    "str x0,[x21,#64]\n"
    "ldr w0,[x23,#72]\n"
    "str w0,[x21,#72]\n"
    "add sp,sp,#80\n"
    "ldp d8,d9,[sp,#64]\n"
    "ldp d10,d11,[sp,#80]\n"
    "ldp d12,d13,[sp,#96]\n"
    "ldp d14,d15,[sp,#112]\n"
    "ldp x19,x20,[sp,#16]\n"
    "ldp x21,x22,[sp,#32]\n"
    "ldp x23,x24,[sp,#48]\n"
    "ldp x29,x30,[sp],#128\n"
    "ret\n"
);

int main(int argc,char **argv) {
    CHECK(argc==3);FILE*f=fopen(argv[1],"rb");CHECK(f);fseek(f,0,SEEK_END);size_t size=ftell(f);rewind(f);
    uint8_t *source=malloc(size);CHECK(source);CHECK(fread(source,1,size,f)==size);fclose(f);
    uint8_t *jit=mmap(NULL,size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANON|MAP_JIT,-1,0);CHECK(jit!=MAP_FAILED);
    uint8_t props[0x60]={0},desc[0x60]={0},id[16]={0};
    memcpy(props+0xc,mp4_hud_groups[0].template_id,16);int one=1;memcpy(props+0x50,&one,4);
    const void *ptr=id;memcpy(props+0x58,&ptr,8);ptr=props;memcpy(desc+0x58,&ptr,8);expected_properties=props;
    for(int mode=0;mode<3;mode++) {
        enabled=mode==2;lookups=calls=0;
        pthread_jit_write_protect_np(0);memcpy(jit,source,size);
        uint32_t jump[]={0x58000050,0xd61f0200};memcpy(jit+0x138df60,jump,8);ptr=(void *)lookup;memcpy(jit+0x138df68,&ptr,8);
        setenv("DM_HUD_SCALE",mode?"85":"default",1);f=fopen(argv[2],"r");CHECK(f);
        unsigned applied;char error[256];int rc=runtime_patch_apply(f,"8275fbe1ec5191c279f3a943ad8524f4",0x138ee20,jit,size,error,sizeof error,&applied);fclose(f);
        CHECK(!rc);CHECK(applied==(unsigned)(mode!=0));
        __builtin___clear_cache((char *)jit,(char *)jit+size);pthread_jit_write_protect_np(1);
        trace t={0};for(int i=0;i<18;i++) t.in[i]=0x12340000+i;for(int i=0;i<512;i++)t.qin[i]=(uint8_t)(i*13+5);
        float m[19]={1,0,0,-10,0,1,0,5,0,0,1,-6,1,1,1,-10,5,-6,0},original[19],want[19];memcpy(original,m,sizeof m);memcpy(want,m,sizeof m);
        if(enabled)CHECK(mp4_hud_matrix(want,-1,1,.85f));
        hud_invoke(jit+0x231240,desc,m,&t);
        for(int i=0;i<18;i++)CHECK(t.out[i]==(i==2?t.record:t.in[i]));
        CHECK(!memcmp(t.qin,t.qout,sizeof t.qin));CHECK(t.before==t.after);
        CHECK(!memcmp(m,enabled?want:original,sizeof m));
        CHECK(lookups==(unsigned)(mode!=0));CHECK(calls==(unsigned)enabled);
    }
    munmap(jit,size);free(source);printf("PASS: %u native HUD bridge checks; default, missing callback, enabled, registers/SIMD/NZCV, original stack ABI\n",checks);
}
