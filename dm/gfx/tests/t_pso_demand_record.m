// Real pso_get hybrid misses must record demand before their queued compiles run.
// Synthetic functions and cached stand-ins need no translated/game libraries.
#include "../nvn/nvn_mtl.m"
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(name, cond) do { checks++; if (!(cond)) { failed++; fprintf(stderr,"FAIL: %s (%d)\n",name,__LINE__); } } while (0)
static const char *vkey="1111111111111111_b", *fkey="2222222222222222_b";
static NSArray<NSString *> *rows(const char *path) {
    NSString *text=[NSString stringWithContentsOfFile:@(path) encoding:NSUTF8StringEncoding error:nil];
    CHECK("manifest is independently readable",text != nil);
    NSMutableArray<NSString *> *r=[NSMutableArray array];
    for(NSString *line in [text componentsSeparatedByString:@"\n"]) if([line hasPrefix:@"R\t"]) [r addObject:line];
    return r;
}
static void check_rows(NSArray<NSString *> *r, unsigned n) {
    CHECK("exact number of canonical render rows",r.count == n);
    for(NSUInteger i=0;i<r.count;i++) {
        pso_skey k; int ok=!pso_skey_parse([r[i] substringFromIndex:2].UTF8String,&k);
        CHECK("canonical key parses",ok);if(!ok)continue;
        CHECK("synthetic shader names and real stages survive serialization",!strcmp(k.vs,vkey)&&!strcmp(k.fs,fkey)&&k.vs_stage==0&&k.fs_stage==1);
        CHECK("ordinary key contains no process pointers or merged state",!k.k.vs&&!k.k.fs&&!k.k.fsfn&&!k.merged&&!k.fz);
        CHECK("pipeline output state survives serialization",k.k.cfmt[0]==MTLPixelFormatRGBA8Unorm&&k.k.samples==1&&k.k.chmask==(i?7u:15u));
    }
}
static int reload(const char *path, unsigned n) {
    NSArray<NSString *> *r=rows(path);check_rows(r,n);
    g_shmap=[@{@"1111111111111111":@"vertex",@"2222222222222222":@"pixel"} mutableCopy];
    long lines=-1;NSArray<WarmTask *> *tasks=warm_parse(@[@(path)],&lines);
    CHECK("fresh process accepts recorded rows as prewarm tasks",lines==n&&tasks.count==(n?1u:0u));
    if(tasks.count) {
        WarmTask *t=tasks[0];CHECK("rows group into the correct render pair",t->kind==1&&[t->vkey isEqualToString:@(vkey)]&&[t->fkey isEqualToString:@(fkey)]&&t->rs.count==n);
        for(NSUInteger i=0;i<t->rs.count&&i<r.count;i++) {
            pso_skey key;int ok=!pso_skey_parse([r[i] substringFromIndex:2].UTF8String,&key);
            CHECK("prewarm retains the complete canonical key",ok&&[t->rs[i] isEqualToData:[NSData dataWithBytes:&key length:sizeof key]]);
        }
    }
    printf("reload: %d checks, %d failed\n",checks,failed);return failed!=0;
}
static void child_reload(const char *exe,const char *path,unsigned n) {
    char count[16];snprintf(count,sizeof count,"%u",n);
    char *args[]={(char *)exe,"--reload",(char *)path,count,NULL};pid_t pid;int status=0;
    int error=posix_spawn(&pid,exe,NULL,NULL,args,environ);CHECK("fresh process starts",!error);
    if(!error){CHECK("fresh process exits normally",waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&!WEXITSTATUS(status));}
}
static int pending(const pso_key *key) {
    pthread_mutex_lock(&g_pp_mu);int i=pso_pend_find(key);int yes=i>=0&&!g_pp[i].done&&!g_pp[i].run&&g_pp[i].d!=nil;pthread_mutex_unlock(&g_pp_mu);return yes;
}
int main(int argc,char **argv) { @autoreleasepool {
    alarm(90);
    if(argc==4&&!strcmp(argv[1],"--reload"))return reload(argv[2],(unsigned)strtoul(argv[3],NULL,10));
    int off=argc==2&&!strcmp(argv[1],"--off"),cap=argc==2&&!strcmp(argv[1],"--cap");
    if(argc>2||(argc==2&&!off&&!cap))return 2;
    char path[]="/tmp/t_pso_demand_record.XXXXXX";int fd=mkstemp(path);if(fd<0)return 2;close(fd);
    setenv("DM_PSO_USER",path,1);setenv("DM_PSO_ARCHIVE","off",1);setenv("DM_PSO_MANIFEST","off",1);setenv("DM_PSO_PREBUILD","0",1);
    setenv("DM_PSO_DEMAND_RECORD",off?"0":"1",1);setenv("DM_PSO_HYBRID","1",1);setenv("DM_NVN_PREDICT","0",1);unsetenv("DM_SAVE");
    mtl_init();CHECK("hybrid path is available",hybrid_on());if(!hybrid_on())return 2;
    NSError *error=nil;
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nvertex float4 v(uint i[[vertex_id]]){return float4(i==1?3.0:-1.0,i==2?3.0:-1.0,0,1);}\nfragment float4 f(){return float4(0,1,0,1);}\n" options:nil error:&error];
    CHECK("synthetic library compiles",lib!=nil);if(!lib)return 2;
    NvShader *vs=[NvShader new],*fs=[NvShader new];vs->stage=0;fs->stage=1;vs->has_pull=1;fs->rf.color_known=1;fs->rf.color_mask=1;
    strlcpy(vs->key,vkey,sizeof vs->key);strlcpy(fs->key,fkey,sizeof fs->key);
    vs->fn=fn4_note([lib newFunctionWithName:@"v"],lib,@"v",nil);fs->fn=fn4_note([lib newFunctionWithName:@"f"],lib,@"f",nil);
    CHECK("synthetic function entries exist",vs->fn&&fs->fn);if(!vs->fn||!fs->fn)return 2;
    NvProg *pr=[NvProg new];pr->sh[0]=vs;pr->sh[1]=fs;
    state_reset();memset(&g_pass,0,sizeof g_pass);g_pass.cfmt[0]=MTLPixelFormatRGBA8Unorm;g_pass.samples=1;
    pso_key keys[2];id<MTLRenderPipelineState> stand[2];
    for(int i=0;i<2;i++) {
        S.chmask.mask=i?7:15;pso_key_fill(&keys[i],vs,fs,g_pass.cfmt,0,0,1,0);
        // Build stand-ins outside the production base cache: the real pso_get
        // archive-only lookup must miss and admit its own background demand.
        stand[i]=[g_dev newRenderPipelineStateWithDescriptor:pso_desc(&keys[i],vs,fs,nil,0) error:&error];
        CHECK("real cached stand-in compiles",stand[i]!=nil);if(!stand[i])return 2;
        hm_put_id(&g_pso_hy,&keys[i],stand[i]);
    }
    if(cap) {
        for(uint32_t i=0;i<(1u<<16)-1;i++){pso_key dummy={0};dummy.samples=i+2;nvn_hm_put(&g_demand_rec,&dummy,(void *)1);}
        CHECK("admission map starts one entry below its bound",g_demand_rec.n==(1u<<16)-1);
    }
    dispatch_queue_t queues[8];int nq=0;
    for(int i=0;i<9;i++){dispatch_queue_t q=pp_bg_queue();int seen=0;for(int j=0;j<nq;j++)seen|=q==queues[j];if(seen)break;CHECK("bounded background queue count",nq<8);if(nq==8)return 2;queues[nq++]=q;dispatch_suspend(q);}
    S.chmask.mask=15;id<MTLRenderPipelineState> a=pso_get(pr,nil);
    CHECK("first real lookup returns the cached stand-in",a==stand[0]&&g_pso_pull);
    CHECK("new demand remains queued, not compiled",pending(&keys[0]));
    unsigned early=off?0:1;check_rows(rows(path),early);
    CHECK("already-pending lookup still returns stand-in",pso_get(pr,nil)==stand[0]&&g_pso_pull);
    CHECK("duplicate demand does not append a row",rows(path).count==early);
    // The second pipeline was queued independently: its *first* pso_get sees
    // st==1, covering placement of demand_record outside the st==0 branch.
    CHECK("second demand can be independently queued",pso_pend_demand(&keys[1],vs,fs));
    S.chmask.mask=7;CHECK("prequeued lookup returns its stand-in",pso_get(pr,nil)==stand[1]&&g_pso_pull);
    CHECK("both compiles remain gated",pending(&keys[0])&&pending(&keys[1]));
    early=off?0:cap?1:2;NSArray<NSString *> *before=rows(path);check_rows(before,early);
    if(cap)CHECK("map cap declines the next early record",g_demand_rec.n==(1u<<16));
    child_reload(argv[0],path,early); // fflush visibility across exec; not power-loss/fsync durability.
    CHECK("reload did not need compilation to complete",pending(&keys[0])&&pending(&keys[1]));
    for(int i=0;i<nq;i++)dispatch_resume(queues[i]);
    for(int i=0;i<nq;i++)dispatch_sync(queues[i],^{});
    for(int i=0;i<2;i++) {
        S.chmask.mask=i?7:15;id<MTLRenderPipelineState> ps=pso_get(pr,nil);
        CHECK("eventual completed pipeline replaces stand-in",ps&&ps!=stand[i]&&!g_pso_pull);
        CHECK("completed pipeline is retained in the normal cache",hm_get_id(&g_pso,&keys[i])==ps);
    }
    NSArray<NSString *> *after=rows(path);check_rows(after,2);
    for(NSUInteger i=0;i<before.count&&i<after.count;i++)CHECK("early row exactly equals eventual completion row",[before[i] isEqualToString:after[i]]);
    CHECK("completion deduplicates or records off/capped demand",after.count==2);
    child_reload(argv[0],path,2);
    if(g_pso_user){fclose(g_pso_user);g_pso_user=NULL;}unlink(path);
    printf("t_pso_demand_record %s: %d checks, %d failed\n",off?"off":cap?"cap":"on",checks,failed);return failed!=0;
} }
