// The production present thread must consume both halves of a pair, including its fallback path.
#define main existing_present_alloc_main
#include "t_present_alloc.m"
#undef main
int test_pair_screen_hz=120, test_pair_match_hz;
@interface ScriptedPresentLayer : CAMetalLayer
@property(nonatomic,strong) NSArray *sequence;
@property(atomic) NSUInteger requests;
@end
@implementation ScriptedPresentLayer
- (id<CAMetalDrawable>)nextDrawable {
    NSUInteger i=self.requests; self.requests=i+1;
    if(i>=self.sequence.count)return nil;
    id value=self.sequence[i];return value==NSNull.null?nil:value;
}
@end
static void enqueue_test(tex_t native,tex_t generated,uint64_t ticket) {
    mtl_tex_pin(native.serial);if(generated.serial)mtl_tex_pin(generated.serial);
    pthread_mutex_lock(&g_pq_mu);
    g_pq[(ticket-1)%PQ]=(pq_item){.t=native,.generated=generated,.frame=ticket,.period=0,.t_in=CACurrentMediaTime()};
    g_pq_in=ticket;pthread_cond_broadcast(&g_pq_cv);pthread_mutex_unlock(&g_pq_mu);
}
static int wait_ticket(uint64_t ticket) {
    pthread_mutex_lock(&g_pq_mu);struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_sec+=5;
    while((g_pq_done<ticket||g_pq_inflight)&&pthread_cond_timedwait(&g_pq_cv,&g_pq_mu,&deadline)!=ETIMEDOUT){}
    int done=g_pq_done>=ticket&&!g_pq_inflight;pthread_mutex_unlock(&g_pq_mu);return done;
}
int main(void){@autoreleasepool{
    alarm(30);setbuf(stdout,NULL);setenv("DM_PRESENT_LINK","0",1);setenv("DM_PRESENT_HZ","0",1);
    setenv("DM_OOT_INTERPOLATION_PREVIEW","3",1);mtl_init();
    g_link_started=1;g_present_link_env=1;g_dm_present_link=0;link_sync();
    CHECK("interpolation respects the explicitly selected classic present lane",g_dm_present_link==0);
    g_present_link_env=0;g_dm_present_link=1;link_sync();
    CHECK("automatic Mac interpolation uses the classic lane at 120 Hz",g_dm_present_link==0);
    CHECK("classic interpolation still requests its physical screen rate",test_pair_match_hz==120);
    test_pair_screen_hz=60;link_sync();
    CHECK("a 60 Hz screen keeps the native-only link",g_dm_present_link==1);
    test_pair_screen_hz=0;link_sync();
    CHECK("an unknown screen rate keeps the native-only link",g_dm_present_link==1);
    test_pair_screen_hz=120;g_present_link_env=1;g_dm_present_link=2;link_sync();
    CHECK("an explicit display-link choice is preserved",g_dm_present_link==2);

    g_link_started=0;g_link=nil;g_link_owns_layer=0;g_dm_present_link=0;
    g_layers=[NSMutableArray new];
    id<CAMetalDrawable> d0=drawable(),d1=drawable(),d2=drawable(),d3=drawable(),d4=drawable();
    ScriptedPresentLayer *layer=[ScriptedPresentLayer layer];layer.device=g_dev;
    layer.pixelFormat=MTLPixelFormatBGRA8Unorm;layer.framebufferOnly=NO;layer.drawableSize=CGSizeMake(8,8);
    layer.sequence=@[d0,d1,d2,NSNull.null,d3,d4,NSNull.null];g_test_layer=layer;mtl_present_layer(8,8);
    tex_t generated=source(92001,50,150),native=source(92002,100,100),last=source(92003,200,200);
    present_start();enqueue_test(native,generated,1);
    CHECK("paired ticket completes without hanging",wait_ticket(1));
    CHECK("classic lane acquires two physical drawables for a pair",layer.requests==2);
    if(layer.requests==2){
        CHECK("first drawable contains the generated pixels",!bad_pixels(d0.texture,50,150));
        CHECK("second drawable contains the native pixels",!bad_pixels(d1.texture,100,100));
    }
    enqueue_test(last,(tex_t){0},2);
    CHECK("native-only ticket completes",wait_ticket(2));
    CHECK("native-only ticket acquires one drawable",layer.requests==3);
    if(layer.requests==3)CHECK("native-only fallback keeps its pixels",!bad_pixels(d2.texture,200,200));
    enqueue_test(native,generated,3);
    CHECK("a refused generated drawable does not strand the native ticket",wait_ticket(3));
    CHECK("failed generated acquisition still proceeds to the native drawable",layer.requests==5);
    if(layer.requests==5)CHECK("native pixels survive failed generated acquisition",!bad_pixels(d3.texture,100,100));
    enqueue_test(native,generated,4);
    CHECK("a refused native drawable still completes its ticket",wait_ticket(4));
    CHECK("failed native acquisition does not retry or duplicate the generated half",layer.requests==7);
    if(layer.requests==7)CHECK("generated pixels survive failed native acquisition",!bad_pixels(d4.texture,50,150));
    CHECK("the queue is completely consumed",g_pq_read==g_pq_in&&g_pq_done==g_pq_in);
    {   // Snapshot capacity must also be respected when QueuePresent reaches replay late.
        uint64_t extra=0,first=snap_take(&native,&extra);
        CHECK("snapshot fixture initializes a real copy",first!=0);
        snap_commit_wait(g_snap_v);
        CHECK("initial snapshot GPU copy completes",[g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]);
        int k=(int)(atomic_load(&g_snap_next)%(uint64_t)g_snap_n);
        uint64_t blocked_ticket=g_pq_done+1;
        g_snap[k].ticket=blocked_ticket;
        dispatch_semaphore_t retired=dispatch_semaphore_create(0);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,100*NSEC_PER_MSEC),dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
            pthread_mutex_lock(&g_pq_mu);g_pq_done=blocked_ticket;pthread_cond_broadcast(&g_pq_cv);pthread_mutex_unlock(&g_pq_mu);
            dispatch_semaphore_signal(retired);
        });
        double begin=CACurrentMediaTime();uint64_t copied=snap_take(&native,&extra);
        CHECK("an occupied slot is awaited instead of losing the snapshot",copied!=0&&CACurrentMediaTime()-begin>=.05);
        dispatch_semaphore_wait(retired,dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC));
        if(copied){snap_commit_wait(g_snap_v);CHECK("retired slot copy completes",[g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]);CHECK("retired slot keeps native pixels",!bad_pixels(g_snap[k].tex,100,100));}
        id<MTLSharedEvent> block=[g_dev newSharedEvent];
        [g_q4 waitForEvent:block value:1];uint64_t pending=++g_snap_v;[g_q4 signalEvent:g_snap_ev value:pending];g_snap[k].ev=pending;
        dispatch_semaphore_t released=dispatch_semaphore_create(0);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,100*NSEC_PER_MSEC),dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{block.signaledValue=1;dispatch_semaphore_signal(released);});
        begin=CACurrentMediaTime();copied=snap_take(&native,&extra);
        CHECK("a GPU-owned slot waits before its allocator is reused",copied!=0&&CACurrentMediaTime()-begin>=.05);
        dispatch_semaphore_wait(released,dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC));
        if(copied){snap_commit_wait(g_snap_v);CHECK("GPU-owned slot copy completes",[g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]);CHECK("GPU-owned slot keeps native pixels",!bad_pixels(g_snap[k].tex,100,100));}
    }
    printf("CLASSIC_PRESENT_PAIR %d checks, %d failed\n",checks,failed);return failed!=0;
}}
