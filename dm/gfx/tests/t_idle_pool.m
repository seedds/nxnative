// Idle streaming must release retired pools without another frame, while open submissions retain them.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(s,c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", s); } } while (0)
static NSUInteger queued(void) { pthread_mutex_lock(&g_deferred_mu); NSUInteger n = g_deferred.count; pthread_mutex_unlock(&g_deferred_mu); return n; }
static int empty_soon(void) { for (int i=0;i<200 && queued();i++) usleep(10000); return !queued(); }
int main(void) { @autoreleasepool {
 mtl_init(); void *mem=NULL; if(posix_memalign(&mem,(size_t)getpagesize(),1<<20)) abort(); memset(mem,0,1<<20);
 for(int i=0;i<25;i++){ mtl_pool_add(mem,1<<20); mtl_pool_del(mem); }
 CHECK("idle pool replacements drain without a new submission",empty_soon());
 // A real submission must keep its pool alive until that submission completes.
 mtl_sub_begin(); mtl_pool_add(mem,1<<20); void *buf;uint64_t off,gpu;
 CHECK("open submission resolves pool",mtl_gpu_addr((uint64_t)(uintptr_t)mem,4,&buf,&off,&gpu));
 mtl_pool_del(mem); NSUInteger pending=queued(); usleep(250000);
 CHECK("open submission retains its pool",pending && queued()>=pending);
 uint64_t f=mtl_sub_commit();CHECK("submission finishes",mtl_wait(f,5000000000ull));
 CHECK("no GPU error",!mtl_last_commit_error());CHECK("completed pools drain",empty_soon());
 for(int i=0;i<25;i++){ mtl_pool_add(mem,1<<20); mtl_pool_del(mem); }
 CHECK("idle retirement after a committed fence also drains",empty_soon());
 // Model the commit interval: the committed fence advanced, while the open
 // submission marker still names that same fence. Retirement must not wait
 // for another frame that may never arrive after a focus change.
 uint64_t saved_fence=atomic_load(&g_fence), saved_done=atomic_load(&g_done);
 atomic_store(&g_pool_open_fence,saved_fence+1);atomic_store(&g_fence,saved_fence+1);
 NSObject *marker=[NSObject new];defer_pool_release(marker);
 pthread_mutex_lock(&g_deferred_mu);
 CHECK("commit interval retires behind current fence",g_deferred.count && g_deferred.lastObject->fence==saved_fence+1);
 pthread_mutex_unlock(&g_deferred_mu);
 atomic_store(&g_done,saved_fence+1);defer_drain();
 CHECK("commit interval does not need a later submission",!queued());
 atomic_store(&g_pool_open_fence,0);atomic_store(&g_fence,saved_fence);atomic_store(&g_done,saved_done);
 mtl_drain();free(mem);printf("idle pool lifetime: %d checks, %d failed\n",checks,failed);return failed?1:0;
}}
