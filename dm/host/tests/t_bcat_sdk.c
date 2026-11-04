// Read-only delivery objects and real files, independent of a game or account.
#include "../hle_bcat.c"
#include <CommonCrypto/CommonDigest.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>
struct dm_flags g_flag;uint64_t g_t0_ns;uint64_t now_ns(void){return 0;}
static uintptr_t guest_begin,guest_end;static int guest_routes;
int dm_guest_as_contains(uintptr_t p){int guest=p>=guest_begin&&p<guest_end;if(guest)guest_routes++;return guest;}
extern void bcat_guest_digest_call(void *top,void *entry,void *object,uint64_t out[2],uintptr_t *returned_sp);
static char delivery[1024];static int available=1,phase=2,allowed=1;static unsigned outcome=1;static uint64_t transferred;static int observers;
int dm_bcat_copy_delivery(char *out,size_t n){if(!available)return 0;return strlcpy(out,delivery,n)<n;}
int dm_bcat_request_sync(void){return allowed;}
void *dm_bcat_sync_observe(void){if(!allowed)return NULL;observers++;return &available;}
void dm_bcat_sync_release(void *observer){if(observer)observers--;}
int dm_bcat_sync_observer_status(const void *observer,unsigned *result,uint64_t *bytes){(void)observer;*result=outcome;*bytes=transferred;return phase;}
int dm_bcat_sync_status(unsigned *result,uint64_t *bytes){*result=outcome;*bytes=transferred;return phase;}
void dm_bcat_digest(const void *bytes,size_t n,unsigned char out[16]){CC_MD5(bytes,(CC_LONG)n,out);}
static unsigned checks;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"BCAT SDK failed at %d: %s\n",__LINE__,#c);return 1;}}while(0)
static void expected_abort(int sig){(void)sig;_exit(97);}
static int unopened_getter_fails(int which){pid_t pid=fork();if(pid<0)return 0;if(!pid){signal(SIGABRT,expected_abort);uint64_t object=0;if(which==2){h_bcat_DirCtor(&object);(void)h_bcat_DirCount(&object);}else{h_bcat_FileCtor(&object);if(which==0)(void)h_bcat_FileSize(&object);else(void)h_bcat_FileDigest(&object);}_exit(0);}int status;return waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==97;}
int main(void){char temp[]="/tmp/nx-bcat-sdk-XXXXXX";CHECK(mkdtemp(temp));strlcpy(delivery,temp,sizeof delivery);char folder[1100],file[1200];snprintf(folder,sizeof folder,"%s/vsdata",temp);CHECK(!mkdir(folder,0700));snprintf(file,sizeof file,"%s/VSSetting_0.byaml",folder);FILE *f=fopen(file,"w");CHECK(f);fwrite("calendar",1,8,f);fclose(f);
    uint64_t object=123,dir=123,progress=123;h_bcat_FileCtor(&object);CHECK(object==0);
    available=0;CHECK(h_bcat_FileOpen(&object,"vsdata","VSSetting_0.byaml")!=0);available=1;
    CHECK(h_bcat_FileOpen(&object,"../escape","VSSetting_0.byaml")!=0);CHECK(h_bcat_FileOpen(&object,"vsdata","../escape")!=0);
    CHECK(!h_bcat_FileOpen(&object,"vsdata","VSSetting_0.byaml"));char buffer[16]={0};uint64_t count=123;
    CHECK(!h_bcat_FileRead(&object,&count,2,buffer,3)&&count==3&&!memcmp(buffer,"len",3));
    CHECK(!h_bcat_FileRead(&object,&count,8,buffer,3)&&count==0);CHECK(h_bcat_FileRead(&object,&count,-1,buffer,3)!=0);
    CHECK(h_bcat_FileSize(&object)==8);BcatDigest digest=h_bcat_FileDigest(&object);unsigned char expected[16]={0xa0,0xe7,0xb2,0xa5,0x65,0x11,0x9c,0x0a,0x7e,0xc3,0x12,0x6a,0x16,0x01,0x61,0x13};
    CHECK(!memcmp(&digest,expected,16));
    // A synthetic guest uses the real registered BIG entry, not the C function.
    // x1 starts as poison and must return the nonzero upper half of the digest.
    size_t page=(size_t)sysconf(_SC_PAGESIZE);void *mapping=mmap(NULL,page*4,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);CHECK(mapping!=MAP_FAILED);
    CHECK(!mprotect(mapping,page,PROT_NONE)&&!mprotect((char*)mapping+page*3,page,PROT_NONE));guest_begin=(uintptr_t)mapping+page;guest_end=(uintptr_t)mapping+page*3;
    uint64_t registers[2]={0};uintptr_t after_sp=0;bcat_guest_digest_call((void*)guest_end,hle_force_h_bcat_FileDigest.fn,&object,registers,&after_sp);
    CHECK(!memcmp(registers,expected,16)&&guest_routes==1&&after_sp==guest_end-16);guest_begin=guest_end=0;CHECK(!munmap(mapping,page*4));
    CHECK(unopened_getter_fails(0)&&unopened_getter_fails(1)&&unopened_getter_fails(2));
    h_bcat_DirCtor(&dir);BcatEntry unopened;int missing=123;CHECK(h_bcat_DirRead(&dir,&missing,&unopened,1)!=0);CHECK(!h_bcat_DirOpen(&dir,"vsdata"));BcatEntry entry;int n=123;
    CHECK(!h_bcat_DirRead(&dir,&n,NULL,0)&&n==0);CHECK(h_bcat_DirRead(&dir,&n,NULL,1)!=0);
    CHECK(!h_bcat_DirRead(&dir,&n,&entry,1024)&&n==1&&!strcmp(entry.name,"VSSetting_0.byaml")&&entry.size==8&&!memcmp(entry.digest,&digest,16));
    CHECK(h_bcat_DirCount(&dir)==1);CHECK(!h_bcat_DirRead(&dir,&n,&entry,1)&&n==0);CHECK(h_bcat_DirRead(&dir,&n,&entry,-1)!=0);
    char names[4][32];CHECK(!h_bcat_Enumerate(&n,names,4)&&n==1&&!strcmp(names[0],"vsdata"));
    h_bcat_ProgressCtor(&progress);CHECK(h_bcat_ProgressStatus(&progress)==0);allowed=0;CHECK(h_bcat_Sync(&progress)!=0);allowed=1;
    CHECK(!h_bcat_Sync(&progress)&&h_bcat_ProgressStatus(&progress)==2&&h_bcat_ProgressResult(&progress)==0);
    phase=9;outcome=1;transferred=0;h_bcat_ProgressUpdate(&progress);CHECK(h_bcat_ProgressStatus(&progress)==9&&h_bcat_ProgressResult(&progress)!=0&&h_bcat_ProgressDownloaded(&progress)==0);
    outcome=0;transferred=8;h_bcat_ProgressUpdate(&progress);CHECK(h_bcat_ProgressResult(&progress)==0&&h_bcat_ProgressDownloaded(&progress)==8);
    phase=2;outcome=1;transferred=0;CHECK(!h_bcat_Sync(&progress)&&observers==1&&h_bcat_ProgressStatus(&progress)==2&&h_bcat_ProgressDownloaded(&progress)==0);
    allowed=0;CHECK(h_bcat_Sync(&progress)!=0&&observers==1&&h_bcat_ProgressStatus(&progress)==2);allowed=1;
    h_bcat_FileClose(&object);CHECK(h_bcat_FileRead(&object,&count,0,buffer,1)!=0);CHECK(!h_bcat_FileOpen(&object,"vsdata","VSSetting_0.byaml"));
    h_bcat_DirClose(&dir);CHECK(h_bcat_DirRead(&dir,&n,&entry,1)!=0);
    h_bcat_FileDtor(&object);CHECK(h_bcat_FileRead(&object,&count,0,buffer,1)!=0);h_bcat_DirDtor(&dir);h_bcat_ProgressDtor(&progress);CHECK(observers==0);
    CHECK(!unlink(file));CHECK(!rmdir(folder));CHECK(!rmdir(temp));printf("BCAT SDK real file/lifecycle/progress: %u checks passed\n",checks);return 0;
}
