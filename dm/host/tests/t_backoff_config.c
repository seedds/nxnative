// Fresh-process configuration publication. Only libc scheduling/conversion seams
// are wrapped; both getters, their once, and the yielding sleep are production.
#include "../dm.h"
#include <assert.h>
#include <stdatomic.h>
#include <errno.h>
#include <unistd.h>

static long long test_atoll(const char *);
static long long test_strtoll(const char *,char **,int);
static int test_once(pthread_once_t *,void (*)(void));
static int test_nanosleep(const struct timespec *,struct timespec *);
#define atoll test_atoll
#define strtoll test_strtoll
#define pthread_once test_once
#define nanosleep test_nanosleep
#include "../hle_thread.c"
#undef atoll
#undef strtoll
#undef pthread_once
#undef nanosleep

typedef struct { const char *name,*env; uint64_t cap,early,late; } example;
static const example examples[]={
    {"default",NULL,250000,250000,250000}, {"empty","",250000,250000,250000},
    {"off","0",0,0,0}, {"custom","500:3000",500000,0,500000},
    {"small","5:0",5000,5000,5000}, {"zero-start","250:0",250000,250000,250000},
    {"large","1250000:3000",1250000000,0,1000000000},
    {"max","9223372036854775:0",9223372036854775000ull,312500,1000000000},
    {"max-start","250:9223372036854775",250000,0,250000},
    {"negative","-1",250000,250000,250000}, {"negative-start","500:-1",250000,250000,250000},
    {"overflow","9999999999999999999999999",250000,250000,250000},
    {"multiply-overflow","9223372036854776",250000,250000,250000},
    {"junk","500x",250000,250000,250000}, {"empty-start","500:",250000,250000,250000},
    {"extra","500:3000:1",250000,250000,250000},
    {"spaces","  +500 : 3000 ",500000,0,500000},
};
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static int controlled,entered,release_init,waiting,returned,start;
static _Thread_local int role;
static const example *wanted;
static _Atomic int failed;
static struct timespec last_sleep;
static void parse_gate(const char *text) {
    if(!controlled || role!=1 || strcmp(text,"3000")) return;
    pthread_mutex_lock(&gate);entered=1;pthread_cond_broadcast(&changed);
    while(!release_init)pthread_cond_wait(&changed,&gate);
    pthread_mutex_unlock(&gate);
}
static long long test_atoll(const char *text) { parse_gate(text);return atoll(text); }
static long long test_strtoll(const char *text,char **end,int base) { parse_gate(text);return strtoll(text,end,base); }
static int test_once(pthread_once_t *once,void (*fn)(void)) {
    if(controlled && role==2) {pthread_mutex_lock(&gate);waiting=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);}
    return pthread_once(once,fn);
}
static int test_nanosleep(const struct timespec *ts,struct timespec *rem) {
    (void)rem;last_sleep=*ts;return 0;
}
static void check(int ok,const char *what) { if(!ok){fprintf(stderr,"FAIL: %s\n",what);atomic_fetch_add(&failed,1);} }
static void *worker(void *arg) {
    role=(int)(uintptr_t)arg;
    if(!controlled){pthread_mutex_lock(&gate);while(!start)pthread_cond_wait(&changed,&gate);pthread_mutex_unlock(&gate);}
    check(yield_backoff_cap()==wanted->cap,"complete cap publication");
    check(yield_backoff_ns(2500000,0)==wanted->early,"complete start publication");
    check(yield_backoff_ns(8000000000ull,1)==wanted->late,"clock-wait cap");
    if(controlled && role==2){pthread_mutex_lock(&gate);returned=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);}
    return NULL;
}
int main(int argc,char **argv) {
    assert(argc==3);alarm(15);controlled=!strcmp(argv[1],"controlled");
    for(size_t i=0;i<sizeof examples/sizeof *examples;i++)if(!strcmp(argv[2],examples[i].name))wanted=&examples[i];
    assert(wanted);if(wanted->env)setenv("DM_YIELD_BACKOFF",wanted->env,1);else unsetenv("DM_YIELD_BACKOFF");
    if(!strcmp(argv[1],"duration")) {
        thr_t me={0};yield_for(&me,10000000000ull,1);
        check(me.nbackoff==1,"production yield performs one sleep");
        check(last_sleep.tv_sec==1 && last_sleep.tv_nsec==250000000,"1.25 s cap has normalized timespec");
        check(last_sleep.tv_nsec>=0 && last_sleep.tv_nsec<1000000000,"nanosecond field is valid");
    } else {
        pthread_t threads[8];int n=controlled?2:8;
        assert(!controlled || !strcmp(wanted->name,"custom"));
        assert(!pthread_create(&threads[0],NULL,worker,(void *)(uintptr_t)1));
        if(controlled){pthread_mutex_lock(&gate);while(!entered)pthread_cond_wait(&changed,&gate);pthread_mutex_unlock(&gate);}
        for(int i=1;i<n;i++)assert(!pthread_create(&threads[i],NULL,worker,(void *)(uintptr_t)(i+1)));
        pthread_mutex_lock(&gate);
        if(controlled){while(!waiting && !returned)pthread_cond_wait(&changed,&gate);check(!returned,"second getter waits for all config fields");release_init=1;}else start=1;
        pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);
        for(int i=0;i<n;i++)pthread_join(threads[i],NULL);
    }
    printf("t_backoff_config %s %s: %d failures\n",argv[1],argv[2],atomic_load(&failed));
    return atomic_load(&failed)!=0;
}
