// Real SleepThread SVC decisions with a controllable clock and recorded host
// sleeps: boundary and deterministic-clock checks must not depend on CI timing.
#include "../dm.h"
#include <assert.h>
#include <unistd.h>
static uint64_t tick=1000000000ull;
static int deterministic;
static struct timespec last_sleep;
static uint64_t test_now(void){return tick;}
static int test_detclock(void){return deterministic;}
static int test_nanosleep(const struct timespec *ts,struct timespec *rem){(void)rem;last_sleep=*ts;tick+=(uint64_t)ts->tv_sec*1000000000ull+(uint64_t)ts->tv_nsec;return 0;}
static int test_yield(void){tick+=500;return 0;}
#define now_ns test_now
#define guest_ns test_now
#define dm_detclock_enabled test_detclock
#define nanosleep test_nanosleep
#define sched_yield test_yield
#include "../hle_thread.c"
#undef now_ns
#undef guest_ns
#undef dm_detclock_enabled
#undef nanosleep
#undef sched_yield
static int checks;
static void sleep_case(thr_t *me,int64_t ns,uint64_t run,uint64_t work,int want){
    me->yrun_t0=tick-run;me->yrun_svc=me->nsvc;me->poll_ret=tick-work;
    uint64_t before=me->nbackoff;last_sleep=(struct timespec){0};uint64_t r[32]={(uint64_t)ns};svc_thread(0x0B,r);
    assert(me->nbackoff-before==(uint64_t)want);checks++;
    if(want){assert(last_sleep.tv_sec==0 && last_sleep.tv_nsec==250000);checks++;}
}
int main(int argc,char **argv){
    alarm(10);assert(argc<=2);int off=argc==2 && !strcmp(argv[1],"off");assert(argc==1 || off);
    setenv("DM_YIELD_BACKOFF",off?"0":"250:2000",1);
    uint32_t handle=svc_thread_main_init();thr_t *me=pthread_getspecific(g_keythr);assert(me);
    sleep_case(me,19999,0,10000,0);
    sleep_case(me,25000,1999999,10000,0);
    sleep_case(me,25000,2000000,10000,!off);
    sleep_case(me,20000,2000000,10000,!off);
    sleep_case(me,249999,2000000,10000,!off);
    sleep_case(me,250000,4000000,10000,0);
    sleep_case(me,300000,4000000,10000,0);
    sleep_case(me,25000,4000000,20001,0);
    sleep_case(me,25000,4000000,20000,!off);
    deterministic=1;sleep_case(me,25000,4000000,10000,0);deterministic=0;
    me->yrun_t0=tick-4000000;me->yrun_svc=me->nsvc;me->poll_ret=tick;
    uint64_t before=me->nbackoff,r[32]={0,handle};svc_thread(0x0C,r);assert(r[0]==0);
    r[0]=25000;svc_thread(0x0B,r);assert(me->nbackoff==before);checks++;
    printf("t_poll_sleep: %d checks passed (%s)\n",checks,off?"off":"default");return 0;
}
