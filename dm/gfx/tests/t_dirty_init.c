// Fresh-process first-use regression for the real dirty tracker and crash hooks.
// Stop the first initializer at getpagesize, before geometry or hooks exist.
// A second registration must wait for initialization, not publish an empty range.
// The pthread_once wrapper observes entry only; it still executes the real once.
// No tracker state is reset or written by the fixture, and no shipping delay is added.
#include "../../host/dm.h"
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int t_getpagesize(void);
static int t_once(pthread_once_t *, void (*)(void));
static void t_fault_register(int (*)(void *, int, void *));
static void t_prep_register(void (*)(void *, size_t));
#define getpagesize t_getpagesize
#define pthread_once t_once
#define dm_fault_hook_register t_fault_register
#define dm_guest_write_prep_register t_prep_register
#include "../nvn/nvn_dirty.c"
#undef getpagesize
#undef pthread_once
#undef dm_fault_hook_register
#undef dm_guest_write_prep_register

module_t *g_mod;
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int controlled, init_entered, release_init, second_waiting, second_returned, start;
static _Thread_local int role;
static _Atomic int fault_hooks, prep_hooks;
static int failures, checks;
#define EXPECT(label, want, value) do { \
    long long got_=(long long)(value), want_=(long long)(want); checks++; \
    printf("%s: expected %lld, got %lld%s\n",label,want_,got_,got_==want_?"":" FAILED"); \
    failures+=got_!=want_; \
} while(0)

static int t_getpagesize(void) {
    if(controlled && role==1) {
        pthread_mutex_lock(&gate); init_entered=1; pthread_cond_broadcast(&changed);
        while(!release_init) pthread_cond_wait(&changed,&gate);
        pthread_mutex_unlock(&gate);
    }
    return getpagesize();
}
static int t_once(pthread_once_t *once, void (*fn)(void)) {
    if(controlled && role==2) {
        pthread_mutex_lock(&gate); second_waiting=1; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&gate);
    }
    return pthread_once(once,fn);
}
static void t_fault_register(int (*fn)(void *,int,void *)) {
    dm_fault_hook_register(fn); atomic_fetch_add(&fault_hooks,1);
}
static void t_prep_register(void (*fn)(void *,size_t)) {
    dm_guest_write_prep_register(fn); atomic_fetch_add(&prep_hooks,1);
}
typedef struct { uint8_t *page; size_t size; int id, ready; } registration;
static void *register_pool(void *arg) {
    registration *r=arg; role=r->id;
    if(!controlled) {
        pthread_mutex_lock(&gate);
        while(!start) pthread_cond_wait(&changed,&gate);
        pthread_mutex_unlock(&gate);
    }
    nvn_dirty_add_range(r->page,r->size);
    r->ready=atomic_load(&fault_hooks)==1 && atomic_load(&prep_hooks)==1;
    if(controlled && role==2) {
        pthread_mutex_lock(&gate); second_returned=1; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&gate);
    }
    return NULL;
}
static void check_pool(registration *r) {
    EXPECT("registration returns with both hooks ready",1,r->ready);
    EXPECT("whole registered page is tracked",1,nvn_dirty_tracked(r->page,r->size));
    uint64_t stamp=nvn_dirty_now(), f0,p0,f1,p1;
    nvn_dirty_protect(r->page,r->size);
    EXPECT("protected unwritten page is clean",0,nvn_dirty_since(r->page,r->size,stamp));
    nvn_dirty_stats(&f0,&p0);
    ((volatile uint8_t *)r->page)[r->size-1]=0x5a; // actual SIGBUS/SIGSEGV -> production crash hook
    nvn_dirty_stats(&f1,&p1);
    EXPECT("direct write goes through one fault hook",1,f1-f0);
    EXPECT("direct write is dirty",1,nvn_dirty_since(r->page,r->size,stamp));
    EXPECT("direct write keeps its value",0x5a,r->page[r->size-1]);
    stamp=nvn_dirty_now(); nvn_dirty_protect(r->page,r->size);
    nvn_dirty_stats(&f0,&p0); dm_guest_write_prepare(r->page,1); r->page[0]=0x63;
    nvn_dirty_stats(&f1,&p1);
    EXPECT("host write invokes one pre-write hook",1,p1-p0);
    EXPECT("prepared host write needs no fault",0,f1-f0);
    EXPECT("host write is dirty",1,nvn_dirty_since(r->page,r->size,stamp));
    nvn_dirty_remove_range(r->page); munmap(r->page,r->size);
}
int main(int argc,char **argv) {
    controlled=argc==1;
    if(argc>2 || (argc==2 && strcmp(argv[1],"concurrent"))) return 64;
    alarm(15);
    enum { N=8 }; registration pools[N]={0}; pthread_t threads[N];
    int n=controlled?2:N; size_t page=(size_t)getpagesize();
    for(int i=0;i<n;i++) {
        pools[i]=(registration){mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0),page,i+1,0};
        if(pools[i].page==MAP_FAILED) return 2;
    }
    if(pthread_create(&threads[0],NULL,register_pool,&pools[0])) return 2;
    if(controlled) {
        pthread_mutex_lock(&gate);
        while(!init_entered) pthread_cond_wait(&changed,&gate);
        pthread_mutex_unlock(&gate);
    }
    for(int i=1;i<n;i++) if(pthread_create(&threads[i],NULL,register_pool,&pools[i])) return 2;
    pthread_mutex_lock(&gate);
    if(controlled) {
        // Both versions reach a deterministic checkpoint: the fixed second
        // caller enters once; the old caller returns without waiting at all.
        while(!second_waiting && !second_returned) pthread_cond_wait(&changed,&gate);
        EXPECT("second registration cannot return during initialization",0,second_returned);
        release_init=1;
    } else start=1;
    pthread_cond_broadcast(&changed); pthread_mutex_unlock(&gate);
    for(int i=0;i<n;i++) pthread_join(threads[i],NULL);
    crash_install(); // leave sanitizer startup failures to its own signal handler
    EXPECT("exactly one fault-hook registration",1,atomic_load(&fault_hooks));
    EXPECT("exactly one pre-write-hook registration",1,atomic_load(&prep_hooks));
    for(int i=0;i<n;i++) check_pool(&pools[i]);
    int live,peak,cap; nvn_dirty_stat(&live,&peak,&cap);
    EXPECT("all ranges retire",0,live);
    printf("t_dirty_init: %d checks, %d failed (%s)\n",checks,failures,controlled?"controlled":"concurrent");
    return failures!=0;
}
