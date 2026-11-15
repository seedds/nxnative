#include "../../games/010019A01E2F2000/menu_visual.inc"
#include "../../host/runtime_patch.h"
#include <assert.h>
#include <pthread.h>
#include <sys/mman.h>
static unsigned checks,calls,last_event;static const void *last_node,*last_packet;static int available;static mp4_menu_binding binding;
#define CHECK(x) do{checks++;assert(x);}while(0)
static void callback(const void *node,void *packet,unsigned event){calls++;last_event=event;last_node=node;last_packet=packet;if(event==1)mp4_menu_unbind(&binding,node);else if(event==0)mp4_menu_visual(&binding,node,packet);else if(event==6)mp4_menu_compass_projection(&binding,packet);}
static void *lookup(const char *name){CHECK(!strcmp(name,"nxnativeMp4MenuVisualV2"));return available?(void*)callback:NULL;}
typedef struct {uint8_t q[32];uint64_t before,after,x19,x20,x0,x8,x9;} trace;
static void invoke(void *fn,void *c,void *data,const void *q,trace *t) {
    struct {trace *out;const void *input;} params={t,q};
    __asm__ volatile(
        "sub sp,sp,#0x400\nstr x29,[sp,#0x3f0]\nadd x29,%[data],#0xe8\n"
        "mov x16,%[fn]\nmov x22,%[params]\nldr x17,[x22,#8]\nldp q0,q1,[x17]\nldr x22,[x22]\nmov x21,%[c]\nmov x19,%[c]\nmov x20,%[data]\n"
        "mov x1,%[c]\nmov x4,%[c]\nmov x23,%[c]\nadd x24,%[data],#0x40\nmov x0,%[c]\nmov x2,%[data]\nmov x8,%[data]\n"
        "cmp xzr,xzr\nmrs x9,nzcv\nstr x9,[x22,#32]\nblr x16\n"
        "str x9,[x22,#80]\nstp q0,q1,[x22]\nmrs x9,nzcv\nstr x9,[x22,#40]\n"
        "str x19,[x22,#48]\nstr x20,[x22,#56]\nstr x0,[x22,#64]\nstr x8,[x22,#72]\n"
        "ldr x29,[sp,#0x3f0]\nadd sp,sp,#0x400\n"
        : : [fn]"r"(fn),[c]"r"(c),[data]"r"(data),[params]"r"(&params)
        : "x0","x1","x2","x3","x4","x5","x6","x7","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","x19","x20","x21","x22","x23","x24","x30","v0","v1","v2","v3","v4","v5","v6","v7","v16","v17","v18","v19","v20","v21","v22","v23","v24","v25","v26","v27","v28","v29","v30","v31","cc","memory");
}

int main(int argc,char **argv) {
    CHECK(argc==4);FILE *f=fopen(argv[1],"rb");CHECK(f);fseek(f,0,SEEK_END);size_t size=ftell(f);rewind(f);uint8_t *src=malloc(size);CHECK(src);CHECK(fread(src,1,size,f)==size);fclose(f);
    unsigned sites[34];f=fopen(argv[3],"r");CHECK(f);for(int i=0;i<34;i++)CHECK(fscanf(f,"%x",&sites[i])==1);fclose(f);
    uint8_t *jit=mmap(NULL,size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANON|MAP_JIT,-1,0);CHECK(jit!=MAP_FAILED);
    for(int index=0;index<34;index++)for(int mode=0;mode<6;mode++) {
        uint8_t frame[8]={0},other[8]={0},node[0x200]={0},packet[0x200],before[0x200],q[32];const void *p=mode==3?other:frame;
        memcpy(node+0x28,&p,8);p=packet;memcpy(node+0x50,&p,8);node[0x48]=mode==4?2:3;node[0x96]=0x5a;
        float matrix[12]={1,0,0,-8,0,1,0,4.5f,0,0,1,-9.99f};memcpy(node+0x60,matrix,48);memset(packet,0xa5,sizeof packet);memcpy(packet+8,matrix,48);memcpy(before,packet,sizeof packet);
        uint32_t proj[16]={0x3e000000,0,0,0,0,0x3e638e39,0,0x80000000,0,0,0xb9800000,0x80000000,0,0,0,0x3f800000};
        if(index>=32){memcpy(packet,proj,64);memcpy(before,packet,sizeof packet);}
        binding=(mp4_menu_binding){.camera=(uintptr_t)node,.frame=mode==5?0:(uintptr_t)frame};available=mode>=2;calls=0;
        for(int i=0;i<32;i++)q[i]=(uint8_t)(i*13+5);
        pthread_jit_write_protect_np(0);memcpy(jit,src,size);uint32_t jump[]={0x58000050,0xd61f0200};memcpy(jit+0x138df60,jump,8);p=(void*)lookup;memcpy(jit+0x138df68,&p,8);
        setenv("DM_MENU_FIT","1610",1);setenv("DM_SCREEN_RATIO",mode?"1610":"default",1);f=fopen(argv[2],"r");CHECK(f);unsigned applied;char error[256];CHECK(!runtime_patch_apply(f,"8275fbe1ec5191c279f3a943ad8524f4",0x138ee20,jit,size,error,sizeof error,&applied));fclose(f);CHECK(applied==(mode?35u:0u));
        __builtin___clear_cache((char*)jit,(char*)jit+size);pthread_jit_write_protect_np(1);
        trace t={0};invoke(jit+sites[index],node,packet,q,&t);
        int change=(index<13 || (index>=22 && index<27)) && mode==2;float want[12];memcpy(want,matrix,48);if(change)for(int i=0;i<8;i++)want[i]*=.9f;
        if(index>=32){float projection[16];memcpy(projection,proj,64);if(mode>=2 && mode!=5){projection[0]*=.9f;projection[5]*=.9f;}CHECK(!memcmp(packet,projection,64));}
        else {CHECK(!memcmp(packet+8,want,48));CHECK(!memcmp(node+0x60,matrix,48));CHECK(!memcmp(packet,before,8));CHECK(!memcmp(packet+56,before+56,8));}
        CHECK(t.x19==(uintptr_t)node);CHECK(t.x20==(uintptr_t)packet);if(index>=32)CHECK(t.x0==(uintptr_t)packet+0x40);else if(index!=17 && index!=19)CHECK(t.x0==(uintptr_t)node);CHECK(t.before==t.after);CHECK(!memcmp(t.q,q,32));CHECK(calls==(unsigned)(mode>=2));
        if(mode>=2){unsigned event=index>=32?6:(index<13 || (index>=22 && index<27))?0:index==13?1:index==20?4:(index==21 || index==27)?5:index>=28?(index%2?3:2):((index-14)%2?3:2);CHECK(last_event==event);CHECK(last_node==node);CHECK(last_packet==((event==0 || event==6)?packet:NULL));}
        if(index<13)CHECK(t.x8==0x5a);else if((index>=22 && index<27)){CHECK(t.x9==0x5a);CHECK(t.x8==(uintptr_t)packet);}
        else if(index==13)CHECK(binding.camera==(mode>=2?0:(uintptr_t)node));
        else if(index==15)CHECK(t.x8==0); // displaced LDR x8,[component+c0]
        else if(index==16)CHECK(t.x8==*(uint64_t*)(before+0x18)); // LDR x8,[original x8+18]
        else if(index==17 || index==19)CHECK(t.x0==0); // LDR x0,[component+148]
        else if(index==20){uint64_t pointer;memcpy(&pointer,node+0x28,8);CHECK(pointer==(uintptr_t)packet);}
    }
    munmap(jit,size);free(src);printf("PASS: %u native menu visual bridge checks; all 18 serializers, lazy creation, native cleanup and both compass projections\n",checks);
}
