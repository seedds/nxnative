#include "nvn_capture.h"
#include "nvn_hmap.h"

void *objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void *);

// Ready continuations of NVN queues share the renderer's one ordered Metal queue.
// An unresolved wait parks its queue, never the submitting CPU or another queue.
typedef struct nq_receipt { unsigned refs, published; uint64_t fence, present, copy, fence2; } nq_receipt;   // present, copy: a present's display ticket and its snapshot copy (mtl_present); fence2: DM_NVN_MTLQ2, the other Metal queue's newest fence of the NVN queue not yet ordered before fence (0: none)
typedef struct nq_signal { volatile uint32_t *event; unsigned mode; uint32_t value; } nq_signal;
typedef struct nq_node {
    struct nq_node *next;
    uint8_t *bytes; size_t length, capacity;
    unsigned kind, mode, closed, waiting; volatile uint32_t *event; uint32_t value;
    uint64_t wait_fence, event_predecessor, wait_since; unsigned wait_kind, wait_said, cls_first, cls_last, cls_any, defers; uint64_t defer_since; const sync_t *sync_source; nq_receipt *receipt;
    nq_signal *signals; unsigned signal_count;   // NQ_COMMANDS: every event signal it records, in order (nq_event_issued at its commit)
    volatile uint32_t *ev0; unsigned rel_how; uint64_t rel_fence;   // DM_NVN_MTLQ2_TRACE: an event wait's word and how it was released (1 its queued exact write, 2 a GPU write's publish, 3 its word already met with no write queued, 4 a CPU publish) and the writer's fence
    unsigned bar_any, bar_tail, worked, bar_bits, bar_head;   // bar_bits / bar_head: the bits of its game barriers, all and before its first work (DM_NVN_MTLQ2_TRACE); NQ_COMMANDS: the classes of work it holds (1 << NQC_*); a game barrier with bits anywhere in it; one after its last work (DM_NVN_MTLQ2, V1)
    tex_t texture; double period; int crop[4];
} nq_node;
typedef struct nq_owned { struct nq_owned *next; unsigned kind; uint64_t data[]; } nq_owned;
typedef struct nq_job { struct nq_job *next; nq_node *first, *last; nq_receipt *receipt; nq_owned *owned; nvn_hmap objects; uint64_t frame, submitted; } nq_job;   // submitted: guest ns of its nvnQueueSubmitCommands (mtl_sub_ready)
static void nq_job_free(nq_job *j) {
    for(nq_owned *o=j->owned,*next;o;o=next) { next=o->next; if(o->kind==CAP_K_PROG) mtl_prog_unpin((prog_t *)o->data); else if(o->kind==CAP_K_TEX) mtl_tex_unpin(((tex_t *)o->data)->serial); free(o); }
    free(j->objects.keys); free(j->objects.vals); free(j);
}
static int nq_snapshot_address(void *ctx,uint64_t *field,uint64_t len,int writable) { (void)ctx;(void)field;(void)len;(void)writable;return 1; }
static int nq_snapshot_object(void *ctx,int kind,uint64_t *field,uint8_t *payload,uint32_t size) {
    (void)payload;(void)size; nq_job *j=ctx; if(!*field || (kind!=CAP_K_PROG && kind!=CAP_K_TEX)) return 1;
    uint64_t key[2]={*field,(unsigned)kind}; nq_owned *o=nvn_hm_get(&j->objects,key);
    if(!o) {
        size_t bytes=kind==CAP_K_PROG?sizeof(prog_t):sizeof(tex_t); o=calloc(1,sizeof *o+bytes); if(!o) die("queue descriptor allocation failed");
        o->kind=kind; o->next=j->owned; j->owned=o;
        if(kind==CAP_K_PROG) mtl_prog_snapshot((prog_t *)o->data,(const prog_t *)(uintptr_t)*field);
        else { memcpy(o->data,(const void *)(uintptr_t)*field,bytes); mtl_tex_pin(((tex_t *)o->data)->serial); }
        nvn_hm_put(&j->objects,key,o);
    }
    *field=(uintptr_t)o->data;return 1;
}
static const cap_ops nq_snapshot_ops={nq_snapshot_address,nq_snapshot_object};
static void *nq_copy_cpu(nq_job *j,const void *src,uint64_t count,size_t stride) {
    if(!count)return NULL;
    if(!src || count>(SIZE_MAX-sizeof(nq_owned))/stride)die("invalid acceleration-structure descriptor array");
    size_t n=(size_t)count*stride;nq_owned *o=calloc(1,sizeof *o+n);if(!o)die("AS descriptor snapshot allocation failed");
    o->next=j->owned;j->owned=o;memcpy(o->data,src,n);return o->data;
}
static void nq_as_snapshot(nq_job *j,p_as_build *p) {
    nvn_as_build *builds=nq_copy_cpu(j,p->builds,p->count,sizeof *builds);p->builds=builds;
    for(uint64_t i=0;i<p->count;i++)builds[i].geometries=nq_copy_cpu(j,builds[i].geometries,builds[i].count,sizeof(nvn_as_geometry));
    p->post=nq_copy_cpu(j,p->post,p->post_count,sizeof(nvn_as_post));
}
typedef struct nq_queue {
    struct nq_queue *next; uint64_t serial, physical, present, copy, wait_passed; int origin;   // wait_passed: guest ns a wait it held for passed (mtl_sub_ready)
    // DM_NVN_MTLQ2: command pieces seen and compute-only ones; per Metal queue, the newest fence of this NVN queue not yet ordered before a piece on the other queue (lf) and whether a game
    // barrier was recorded after that piece's last work (pend: the next piece on the other queue is ordered after it); the Metal queue of the last piece; fences its next piece waits for
    uint32_t q2_cmd, q2_comp; uint64_t lf[2]; int pend[2], lmq; uint64_t q2_need[2]; unsigned q2_why;   // q2_why: what asked for q2_need (1 an exact event write, 2 a sync receipt, 4 a fenced sync; DM_NVN_MTLQ2_TRACE)
    nq_job *first, *last; nq_receipt *tail;
} nq_queue;
static pthread_mutex_t nq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t nq_cv = PTHREAD_COND_INITIALIZER;
static pthread_once_t nq_once = PTHREAD_ONCE_INIT;
static nq_queue *nq_queues;
static int nq_test_held;   // nvn_queue_test_hold: replay takes no queued work while set (unit tests only)
// The last GPU write of each event word not yet landed: its submission's fence, and for a WRITE signal the value it leaves (exact). A wait the queued value satisfies needs no
// completion: its continuation is encoded after the writer's on the one ordered Metal queue, as the next submission of one NVN queue is, and memory the writer's completion lands
// on the CPU (reports, render-target readbacks) is that same in-flight case. An ADD's result depends on the writes landing before it, so such a wait still waits for them
typedef struct nq_event_write { struct nq_event_write *next; volatile uint32_t *word; uint64_t fence, fence2; uint32_t value; int exact; } nq_event_write;   // fence2: as nq_receipt's
static nq_event_write *nq_event_writes;
int mtl_fence_done(uint64_t fence);   // one command buffer complete with its records run (DM_NVN_MTLQ2: per Metal queue; else the same as fence <= mtl_completed_fence())
void mtl_sub_route(int want_b,const uint64_t need[2]); int mtl_q2_trace(void); void mtl_q2_chain(unsigned mask); void mtl_barrier_stats(uint64_t *skip,uint64_t *full); int mtl_sub_mq(void); int mtl_fence_mq(uint64_t fence); int mtl_q2_strict(void); void mtl_q2_note(int kind); void mtl_q2_wait_q0(uint64_t fence);
enum { Q2N_CHAIN, Q2N_HANDOFF, Q2N_PRESENT };   // mtl_q2_note: a piece asked to wait for its own NVN queue's piece on the other Metal queue (a game barrier), for another queue's write (an event or sync), a present for a second-queue piece
static uint64_t nq_event_pending(volatile uint32_t *word) { for(nq_event_write *w=nq_event_writes;w;w=w->next) if(w->word==word) return w->fence; return 0; }
static nq_event_write *nq_event_write_of(volatile uint32_t *word) { nq_event_write *w=nq_event_writes; while(w && w->word!=word) w=w->next; return w; }
// The write has landed: its command buffer and the other queue's earlier pieces of its NVN queue (fence2) are complete
static int nq_event_landed(const nq_event_write *w) { return !w || (mtl_fence_done(w->fence) && mtl_fence_done(w->fence2)); }
int mtl_q2_trace(void); int mtl_fence_mq(uint64_t fence); uint64_t mtl_q2_last(int k); int mtl_q2_waitall(void);
static void nq_event_issued(volatile uint32_t *word,uint64_t fence,uint64_t fence2,unsigned mode,uint32_t value) {
    nq_event_write *w=nq_event_write_of(word);
    if(!w) { w=calloc(1,sizeof *w); if(!w) die("event ordering allocation failed"); w->word=word; w->next=nq_event_writes; nq_event_writes=w; }
    w->fence=fence; w->fence2=fence2; w->value=value; w->exact=mode==0;
    if(mtl_q2_trace()) LOG("[q2trace] signal word %p mode %u value %#x fence %llu (mq %d) fence2 %llu",(void *)word,mode,value,(unsigned long long)fence,fence?mtl_fence_mq(fence):-1,(unsigned long long)fence2);
}

enum { NQ_COMMANDS, NQ_EVENT, NQ_SYNC, NQ_FENCE, NQ_PRESENT };
// The class of work of a record (E0): a piece whose records are all compute (dispatches, copies, buffer clears) may run on the second Metal queue (DM_NVN_MTLQ2)
enum { NQC_NONE, NQC_RENDER, NQC_COMPUTE };
static unsigned nq_op_class(uint32_t op) {
    switch(op) {
    case OP_DRAW: case OP_DRAW_IDX: case OP_MULTI_DRAW: case OP_DRAW_TEX: case OP_CLEAR_COLOR: case OP_CLEAR_DS: case OP_CLEAR_TEX: return NQC_RENDER;   // a texture clear lands as a pass
    case OP_DISPATCH: case OP_COPY_B2B: case OP_COPY_B2T: case OP_COPY_STRIDE: case OP_COPY_T2B: case OP_COPY_T2T: case OP_CLEAR_BUF: case OP_DLSS: return NQC_COMPUTE;
    default: return NQC_NONE;
    }
}
// DM_NVN_MTLQ2: an NVN queue is async (its compute-only pieces may go to the second Metal queue) once 32 of its command pieces were all compute only; one other piece ends that
// for good. DM_NVN_MTLQ2_ASYNC=all treats every queue so (the tests)
static int nq_q2_async(const nq_queue *q) { static int all=-1; if(all<0) { const char *e=getenv("DM_NVN_MTLQ2_ASYNC"); all=e && !strcmp(e,"all"); } return all || (q->q2_comp>=32 && q->q2_comp==q->q2_cmd); }
static nq_receipt *nq_new(void) { nq_receipt *r=calloc(1,sizeof *r); if(!r) die("queue receipt allocation failed"); r->refs=1; return r; }
static void nq_put(nq_receipt *r) { if(r && !--r->refs) free(r); }
static void nq_set_sync(sync_t *s,nq_receipt *r) { if(s->fenced==3) nq_put((nq_receipt *)(uintptr_t)s->fence); r->refs++; s->fence=(uintptr_t)r; s->fenced=3; }
void nvn_sync_release(sync_t *s) { pthread_mutex_lock(&nq_mu); if(s->fenced==3) nq_put((nq_receipt *)(uintptr_t)s->fence); s->fenced=0; s->fence=0; pthread_mutex_unlock(&nq_mu); }
static nq_queue *nq_get(queue_t *q) {
    nq_queue *p=nq_queues; while(p && p->serial!=q->serial) p=p->next;
    if(!p) { p=calloc(1,sizeof *p); if(!p) die("queue state allocation failed"); p->serial=q->serial; p->origin=q->origin_ll; p->next=nq_queues; nq_queues=p; }
    return p;
}
static void nq_publish(nq_receipt *r,uint64_t fence) { r->fence=fence; r->published=1; pthread_cond_broadcast(&nq_cv); }
// DM_NVN_MTLQ2: what a FenceSync or an event signal of this NVN queue orders besides its newest piece: its newest piece on the other Metal queue not ordered before that one
static uint64_t nq_fence2(const nq_queue *q) { return q->lf[!q->lmq]; }
int mtl_q2_syncdone(void);
static void nq_q2_need(nq_queue *q,uint64_t fence) { if(!fence || (mtl_fence_done(fence) && !mtl_q2_syncdone())) return; int m=mtl_fence_mq(fence); if(fence>q->q2_need[m]) q->q2_need[m]=fence; }   // a GPU wait for that fence if the piece runs on the other queue
static int nq_predicate(uint32_t current,unsigned mode,uint32_t target) { return mode==0 ? current==target : (uint32_t)(current-target)<0x80000000u; }
static int nq_sync_encoded(const sync_t *s,uint64_t *fence,uint64_t *fence2) {
    *fence2=0;
    if(s->fenced==3) { nq_receipt *r=(nq_receipt *)(uintptr_t)s->fence; if(!r->published) return 0; *fence=r->fence; *fence2=r->fence2; return 1; }
    if(!s->fenced) return 0; *fence=s->fence; return 1;
}
void nvn_queue_gpu_progress(void) {
    pthread_mutex_lock(&nq_mu); nq_event_write **p=&nq_event_writes;
    while(*p) { nq_event_write *w=*p; if(nq_event_landed(w) && !mtl_q2_syncdone()) { *p=w->next; free(w); } else p=&w->next; }   // DM_NVN_MTLQ2_SYNCDONE: kept, so a wait its word already meets still knows its writer
    pthread_cond_broadcast(&nq_cv); pthread_mutex_unlock(&nq_mu);
}
// A GPU write of the word published at its command buffer's completion (gpu_fence) releases a wait that parked behind the write at predecessor: that write itself, with the other
// Metal queue's earlier pieces of its NVN queue (DM_NVN_MTLQ2), or a later one once the predecessor's command buffer is complete too (one Metal queue: the same as gpu_fence>=predecessor)
static int nq_gpu_landed(volatile uint32_t *word,uint64_t gpu_fence,uint64_t predecessor) {
    if(gpu_fence<predecessor) return 0;
    const nq_event_write *w=nq_event_write_of(word); uint64_t f2=w && w->fence==gpu_fence ? w->fence2 : 0;
    return (gpu_fence==predecessor || mtl_fence_done(predecessor)) && mtl_fence_done(f2);
}
static void nq_event_publish(volatile uint32_t *word,unsigned mode,uint32_t value,uint64_t gpu_fence) {
    pthread_mutex_lock(&nq_mu);
    if(mode==1) atomic_fetch_add_explicit((_Atomic uint32_t *)word,value,memory_order_release);
    else atomic_store_explicit((_Atomic uint32_t *)word,value,memory_order_release);
    atomic_thread_fence(memory_order_seq_cst);
    // Latch waits already reached: a subsequent signal must not erase a release.
    for(nq_queue *q=nq_queues;q;q=q->next) if(q->first && q->first->first) {
        nq_node *n=q->first->first;
        if(n->kind==NQ_EVENT && n->waiting && n->event==word && (gpu_fence ? nq_gpu_landed(word,gpu_fence,n->event_predecessor) : nq_event_landed(nq_event_write_of(word))) && nq_predicate(atomic_load_explicit((_Atomic uint32_t *)word,memory_order_acquire),n->mode,n->value)) { n->ev0=word; n->rel_how=gpu_fence?2:4; n->rel_fence=gpu_fence; n->event=NULL; n->wait_fence=gpu_fence; }
    }
    pthread_cond_broadcast(&nq_cv); pthread_mutex_unlock(&nq_mu);
}
void nvn_event_publish(volatile uint32_t *word,unsigned mode,uint32_t value) { nq_event_publish(word,mode,value,0); }
void nvn_event_publish_gpu(volatile uint32_t *word,unsigned mode,uint32_t value,uint64_t fence) { nq_event_publish(word,mode,value,fence); }
static void nq_snapshot_sync(nq_node *n,const sync_t *s) {
    n->wait_kind=s->fenced; n->wait_fence=s->fence;
    if(s->fenced==3) { n->receipt=(nq_receipt *)(uintptr_t)s->fence; n->receipt->refs++; }
}
typedef struct { uint64_t wall, cpu, instr, cycles, sys; } nq_cost;
// DM_NVN_QDEFER (0 / 1 / ab, off by default): between ready pieces of different NVN queues, the replay keeps to the class of work it is in. Each piece the scheduler
// replays is a Metal command buffer whose render passes and compute encoders end with it, and the order between pieces of different queues is the host's choice wherever no
// event or sync wait orders them; round robin alternates them. Ocarina of Time's queue 5 holds all of its frame's compute (culling, wind, particles, AO, the ambient cache: 139
// dispatches in 11 pieces, no render pass) and queue 1 every render pass, and round robin replays them RCRCRC...: 22 switches between compute and render a frame from the
// pieces alone, at 35-46 us median each on an M2 with a long compute → render tail. Ampere runs the two queues side by side; one Metal queue cannot, but it can keep runs
// together. With the switch on, a ready command piece whose first class (render or compute) is not the class the last replayed piece ended in is held while the scan looks for
// a ready wait (bookkeeping, which may release more of the same class) or a ready piece of the last class, and goes first otherwise. On a frame of Ocarina of Time's (its queue order replayed
// offline with its waits) that is 5 switches between pieces instead of 22, the same as the best order its waits allow. A held piece runs at once when a CPU thread waits
// on a fence (nvn_sync_wait), when it has been held DM_NVN_QDEFER_US (default 3000 us), when the alternative is a present or of a later frame, or when nothing else qualifies.
// NVN allows this: work of two queues is ordered only by event and sync waits, the scan takes only pieces whose waits are met, and one queue's order never changes
// (the classes: nq_op_class above, E0)
// Whether the first ready node is held: a command piece of a class other than the one the last replayed piece ended in, unless a CPU thread waits on a fence
static int nq_holdable(const nq_node *n,unsigned last_cls,unsigned cpu_waiters) {
    return !cpu_waiters && n->kind==NQ_COMMANDS && n->cls_first!=NQC_NONE && last_cls!=NQC_NONE && n->cls_first!=last_cls;
}
enum { NQD_RUN_NONE_ELSE, NQD_RUN_CAP, NQD_RUN_BEFORE_PRESENT, NQD_RUN_LATER_FRAME, NQD_RUN_OTHER_CLASS, NQD_DEFER, NQD_N };
// For a held piece and another queue's ready node: NQD_DEFER takes the other node first; NQD_RUN_* says why the other does not qualify (the scan keeps looking; NQD_RUN_CAP
// runs the held piece at once)
static unsigned nq_defer_decide(unsigned last_cls,uint64_t held_frame,uint64_t held_since,const nq_node *other,uint64_t other_frame,uint64_t now,uint64_t cap_ns) {
    if(held_since && now-held_since>=cap_ns) return NQD_RUN_CAP;
    if(!other) return NQD_RUN_NONE_ELSE;
    if(other->kind==NQ_PRESENT) return NQD_RUN_BEFORE_PRESENT;
    if(other_frame>held_frame) return NQD_RUN_LATER_FRAME;
    if(other->kind!=NQ_COMMANDS) return NQD_DEFER;   // a wait, sync or fence: bookkeeping, no GPU work
    if(other->cls_first==NQC_NONE || other->cls_first==last_cls) return NQD_DEFER;
    return NQD_RUN_OTHER_CLASS;
}
static uint64_t nq_defer_n[NQD_N+1];   // decisions by reason; [NQD_N]: pieces that waited at least once
static unsigned nq_last_cls, nq_cpu_waiters;
int mtl_sw_mode(const char *name, int dflt); int mtl_sw_on(int m); int mtl_q2_on(void);
// DM_NVN_MTLQ2: the switches DM_NVN_QDEFER avoids are those on the first Metal queue. A piece bound for the second (compute only, of an async NVN queue) is not held, since it
// interrupts nothing there, and does not set the class in progress (nq_last_cls)
static int nq_to_q1(const nq_queue *q,const nq_node *n) { return n->kind==NQ_COMMANDS && n->cls_any==1u<<NQC_COMPUTE && nq_q2_async(q) && mtl_q2_on(); }
static int nq_defer_mode(void) { static int m=-1; if(m<0) m=mtl_sw_mode("DM_NVN_QDEFER",0); return m; }
static uint64_t nq_defer_cap(void) { static uint64_t c; if(!c) { const char *e=getenv("DM_NVN_QDEFER_US"); long v=e?atol(e):0; c=(uint64_t)(v>0?v:3000)*1000; } return c; }
static void nq_defer_stats(uint64_t s[NQD_N+1]) { pthread_mutex_lock(&nq_mu); memcpy(s,nq_defer_n,sizeof nq_defer_n); pthread_mutex_unlock(&nq_mu); }
static nq_cost nq_cost_begin(void) {
    nq_cost c={.wall=now_ns(),.cpu=clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID),.sys=thr_sys_ns()};
    thr_counts(&c.instr,&c.cycles); return c;
}
static void nq_cost_end(nq_cost c) {
    uint64_t dw=now_ns()-c.wall, dc=clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)-c.cpu, ins, cyc; thr_counts(&ins,&cyc);
    atomic_fetch_add(&g_nvn_ns,dw); atomic_fetch_add(&g_sub_wall_ns,dw); atomic_fetch_add(&g_cum_sub_wall_ns,dw);
    atomic_fetch_add(&g_sub_cpu_ns,dc); atomic_fetch_add(&g_cum_sub_cpu_ns,dc);
    atomic_fetch_add(&g_sub_instr,ins-c.instr); atomic_fetch_add(&g_cum_sub_instr,ins-c.instr);
    atomic_fetch_add(&g_cum_sub_cyc,cyc-c.cycles); atomic_fetch_add(&g_cum_sub_sys_ns,thr_sys_ns()-c.sys);
}
static void *nq_worker(void *unused) {
    (void)unused; pthread_setname_np("NVN queue replay");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);
    void *rt=nvn_rt_join("queue replay thread");   // it carries the frame's submissions: it declares the frame's deadline too (hle_nvn.c rt_tick)
    uint64_t frame=UINT64_MAX;
    for(;;) {
        pthread_mutex_lock(&nq_mu); nq_queue *q=NULL; nq_node *n=NULL; nq_job *j=NULL;
        for(;;) {
        scan_again:;
            if(nq_test_held) { pthread_cond_wait(&nq_cv,&nq_mu); continue; }
            int mapped_event_wait=0, qdefer=mtl_sw_on(nq_defer_mode()); nq_queue *hq=NULL; nq_node *hn=NULL; nq_job *hj=NULL; unsigned hwhy=NQD_RUN_NONE_ELSE;   // hq: a piece held (DM_NVN_QDEFER)
            for(q=nq_queues;q;q=q->next) {
                j=q->first; if(!j) continue; n=j->first;
                if(!n) { j->receipt->fence2=nq_fence2(q); nq_publish(j->receipt,q->physical); q->first=j->next; if(!q->first) q->last=NULL; nq_put(j->receipt);
                    pthread_mutex_unlock(&nq_mu); pthread_mutex_lock(&g_sub_mu); void *pool=objc_autoreleasePoolPush(); nq_job_free(j); objc_autoreleasePoolPop(pool); pthread_mutex_unlock(&g_sub_mu); pthread_mutex_lock(&nq_mu); goto scan_again; }
                if(n->kind==NQ_EVENT && n->event) {
                    uint64_t predecessor=nq_event_pending(n->event);
                    if(!n->waiting) n->event_predecessor=predecessor;
                    // The queued GPU write satisfies the wait: the GPU runs this continuation after it (nq_event_write). Ocarina of Time waits on about 25 events a frame; each wait
                    // for the writer's completion left the GPU idle while the host encoded the next submission, 2 ms of its 14.5 ms frame, which its dynamic resolution read as work
                    // DM_NVN_MTLQ2: the writer may run on the other Metal queue: the continuation waits for it there on the GPU (nq_q2_need); landed means complete on its own queue (mtl_fence_done)
                    nq_event_write *pw=nq_event_write_of(n->event), *queued=!nq_event_landed(pw) ? pw : NULL;
                    if(queued && queued->exact && nq_predicate(queued->value,n->mode,n->value)) { n->ev0=n->event; n->rel_how=1; n->rel_fence=queued->fence; nq_q2_need(q,queued->fence); nq_q2_need(q,queued->fence2); q->q2_why|=1; mtl_q2_note(Q2N_HANDOFF); break; }
                    if(!nq_event_landed(pw) || !nq_predicate(atomic_load_explicit((_Atomic uint32_t *)n->event,memory_order_acquire),n->mode,n->value)) {
                        // A queue held more than 5 s by one event wait: what it waits for, since the game's own GPU watchdog acts on the fences behind it (PRAGMATA aborts after 15 s)
                        uint64_t t=now_ns(); if(!n->wait_since) n->wait_since=t;
                        else if(t-n->wait_since>5000000000ull && !n->wait_said) { static _Atomic int said; n->wait_said=1; if(atomic_fetch_add(&said,1)<8)
                            LOG("[RED] queue %llu: an event wait has held it %.1f s: word %p is %#x, waiting for %s %#x; a GPU write of it is %s",(unsigned long long)q->serial,(t-n->wait_since)/1e9,(void *)n->event,
                                atomic_load_explicit((_Atomic uint32_t *)n->event,memory_order_acquire),n->mode==0?"equal to":"at least",n->value,!nq_event_landed(pw)?"queued, not completed":"not queued"); }
                        n->waiting=1; mapped_event_wait=1; continue; }
                    n->ev0=n->event; n->rel_how=3; n->rel_fence=pw?pw->fence:0;   // its word already meets the wait; the last write it knows of (pw) is complete, or none
                }
                if(n->kind==NQ_SYNC) {
                    if(n->receipt && !n->receipt->published) {
                        uint64_t t=now_ns(); if(!n->wait_since) n->wait_since=t;
                        else if(t-n->wait_since>5000000000ull && !n->wait_said) { static _Atomic int said; n->wait_said=1; if(atomic_fetch_add(&said,1)<8)
                            LOG("[RED] queue %llu: a sync wait has held it %.1f s: the fence it waits for (receipt %p) is not published yet",(unsigned long long)q->serial,(t-n->wait_since)/1e9,(void *)n->receipt); }
                        continue; }
                    if(n->receipt) { nq_q2_need(q,n->receipt->fence); nq_q2_need(q,n->receipt->fence2); q->q2_why|=2; } else if(n->wait_kind==1) { nq_q2_need(q,n->wait_fence); q->q2_why|=4; }   // DM_NVN_MTLQ2: its fences may still run on the other Metal queue
                    if(n->receipt || n->wait_kind==1) mtl_q2_note(Q2N_HANDOFF);
                    // Targets are immutable snapshots. One Metal queue orders an encoded producer before this continuation. A sync not yet fenced when the wait was submitted
                    // (wait_kind 0) is a snapshot of nothing to wait for, and the wait passes. Sparking! ZERO, on one NVN queue, waits on a sync before that queue's own first
                    // fence of it at its first load, which runs on the console; parked here, the fence behind the wait never published and its SyncWait stalled the game (#1808)
                }
                if(qdefer) {   // DM_NVN_QDEFER: hold a piece of the other class while another queue offers a wait or a piece of the class in progress
                    if(!hq) { if(nq_holdable(n,nq_last_cls,nq_cpu_waiters) && !nq_to_q1(q,n)) { hq=q; hn=n; hj=j; continue; } break; }
                    unsigned why=nq_defer_decide(nq_last_cls,hj->frame,hn->defer_since,n,j->frame,now_ns(),nq_defer_cap());
                    if(why==NQD_DEFER) break;
                    hwhy=why; if(why==NQD_RUN_CAP) { q=NULL; break; }
                    continue;
                }
                break;
            }
            if(hq) {
                if(q) { nq_defer_n[NQD_DEFER]++; if(!hn->defers) { hn->defer_since=now_ns(); nq_defer_n[NQD_N]++; } hn->defers++; }
                else { nq_defer_n[hwhy]++; q=hq; n=hn; j=hj; }
            }
            if(q) break;
            // NVN events can also be changed by plain stores through mapped pool memory.
            // A bounded notification poll only while such a word is awaited complements API/callback wakes.
            if(mapped_event_wait) { struct timespec tick={0,1000000}; pthread_cond_timedwait_relative_np(&nq_cv,&nq_mu,&tick); }
            else pthread_cond_wait(&nq_cv,&nq_mu);
        }
        j->first=n->next; if(!j->first) j->last=NULL;
        if(n->kind==NQ_EVENT && mtl_q2_trace()) { volatile uint32_t *w=n->event?n->event:n->ev0; nq_event_write *r=w?nq_event_write_of(w):NULL;
            LOG("[q2trace] F%llu t %.6f wait q%llu word %p mode %u value %#x now %#x how %u fence %llu (mq %d, %s) record %llu",(unsigned long long)j->frame,clock_gettime_nsec_np(CLOCK_UPTIME_RAW)/1e9,(unsigned long long)q->serial,(void *)w,n->mode,n->value,
                w?atomic_load_explicit((_Atomic uint32_t *)w,memory_order_acquire):0,n->rel_how,(unsigned long long)n->rel_fence,n->rel_fence?mtl_fence_mq(n->rel_fence):-1,n->rel_fence&&mtl_fence_done(n->rel_fence)?"done":"running",(unsigned long long)(r?r->fence:0)); }
        // DM_NVN_MTLQ2_SYNCDONE=1 (diagnostic): a wait released by its writer's completion or by its word (the writer complete) still waits for that writer on the GPU: a cross-queue
        // event wait is what makes the other queue's writes visible to this one, completion seen on the CPU is not
        if(n->kind==NQ_EVENT && n->rel_fence && mtl_q2_syncdone()) nq_q2_need(q,n->rel_fence);
        // DM_NVN_MTLQ2_WAITALL=1 (diagnostic): a game wait also waits for everything committed before it on the other Metal queue, as one Metal queue ordered it
        if(n->kind==NQ_EVENT || n->kind==NQ_SYNC) { int wa=mtl_q2_waitall(); for(int k=0;k<2;k++) if(wa>>k&1) nq_q2_need(q,mtl_q2_last(k)); }   // bit k: wait for Metal queue k's last commit
        // Round-robin ready queues: a continually fed independent producer cannot starve another queue.
        if(q->next) { nq_queue **link=&nq_queues; while(*link!=q) link=&(*link)->next; *link=q->next; nq_queue *tail=nq_queues; while(tail->next) tail=tail->next; tail->next=q; q->next=NULL; }
        if(n->kind==NQ_SYNC || n->kind==NQ_EVENT) { uint64_t f=n->receipt?n->receipt->fence:n->wait_fence; if(f>q->physical) q->physical=f; }
        if((n->kind==NQ_SYNC || n->kind==NQ_EVENT) && n->wait_since) q->wait_passed=guest_ns();   // the GPU could not start what follows before this either
        if(n->kind==NQ_FENCE) { n->receipt->fence2=nq_fence2(q); nq_publish(n->receipt,q->physical); }
        pthread_mutex_unlock(&nq_mu);
        uint64_t fence=0, present=0, copy=0;
        if(n->kind==NQ_COMMANDS || n->kind==NQ_PRESENT) {
            nvn_rt_tick(rt);
            dm_host_mutex_lock(&g_sub_mu); void *pool=objc_autoreleasePoolPush();
            dm_host_work++; mtl_prepare(); dm_host_work--;
            gfx_enter(); nq_cost cost=nq_cost_begin();
            if(frame!=j->frame) { frame=j->frame; mtl_set_frame(frame); }
            mtl_queue_activate(q->serial); mtl_set_queue_origin_lower_left(q->origin);
            if(n->kind==NQ_COMMANDS) {
                uint64_t handle=(uintptr_t)n->bytes;
                nvn_cap_submit(1,&handle);
                mtl_sub_begin(); mtl_sub_ready(j->submitted>q->wait_passed?j->submitted:q->wait_passed); nvn_ft_poll();
                int q2tr=mtl_q2_trace(); uint64_t bs0=0,bf0=0; if(q2tr) mtl_barrier_stats(&bs0,&bf0);
                mtl_exec_rest(NULL,0); mtl_exec(n->bytes);
                // DM_NVN_MTLQ2 (E2 v2, notes/.specs/e2v2.md V1): a piece waits for its own NVN queue's newest piece on the other Metal queue only where the game orders them, by a barrier
                // with bits recorded after that piece's last work (pend) or anywhere in this one; NVN orders nothing else between a queue's submissions. DM_NVN_MTLQ2_STRICT=1: always (E2 as built)
                if((mtl_q2_waitall()&4) && nq_to_q1(q,n)) nq_q2_need(q,mtl_q2_last(0));   // DM_NVN_MTLQ2_WAITALL=q0all: every second-queue piece waits for the first queue's last commit
                uint64_t nd[2]={q->q2_need[0],q->q2_need[1]}, hn[2]={nd[0],nd[1]}, cf[2]={0,0}; int strict=mtl_q2_strict(); unsigned chw=0;
                unsigned chk=0;   // the Metal queues a chain asks for: counted in q2_route against the queue the piece goes to (a chain on its own queue orders itself)
                for(int k=0;k<2;k++) if(q->lf[k] && (strict || q->pend[k] || n->bar_any) && !mtl_fence_done(q->lf[k])) { if(q->lf[k]>nd[k]) nd[k]=q->lf[k]; cf[k]=q->lf[k]; chw|=((strict?1u:0u)|(q->pend[k]?2u:0u)|(n->bar_any?4u:0u))<<(4*k); chk|=1u<<k; }
                mtl_q2_chain(chk);
                mtl_sub_route(n->cls_any==1u<<NQC_COMPUTE && nq_q2_async(q),nd);
                fence=mtl_sub_commit();
                int mq=mtl_sub_mq(), ot=!mq;
                if(!q->lf[ot] || mtl_fence_done(q->lf[ot]) || (nd[ot] && nd[ot]>=q->lf[ot])) { q->lf[ot]=0; q->pend[ot]=0; }   // ordered before this piece (its wait), or complete
                // DM_NVN_MTLQ2_TRACE=A-B (frames, diagnostic): one line a piece: its NVN queue, classes, game barrier bits (all, before its work, after it), the Metal queue, the hand-off fences
                // it waits for (and why) and the chain fences (why: 1 strict, 2 the earlier piece ended with a barrier, 4 a barrier in this one; per Metal queue in nibbles), its draw passes' barriers
                if(q2tr) { uint64_t bs1,bf1; mtl_barrier_stats(&bs1,&bf1);
                    LOG("[q2trace] F%llu t %.6f piece q%llu fence %llu mq %d cls %x bar %x head %x tail %u handoff %llu/%llu why %x chain %llu/%llu chw %x passes full %llu skip %llu",(unsigned long long)frame,clock_gettime_nsec_np(CLOCK_UPTIME_RAW)/1e9,
                        (unsigned long long)q->serial,(unsigned long long)fence,mq,n->cls_any,n->bar_bits,n->bar_head,n->bar_tail,(unsigned long long)hn[0],(unsigned long long)hn[1],q->q2_why,(unsigned long long)cf[0],(unsigned long long)cf[1],chw,(unsigned long long)(bf1-bf0),(unsigned long long)(bs1-bs0)); }
                q->lf[mq]=fence; q->pend[mq]=n->bar_tail; q->lmq=mq; q->q2_need[0]=q->q2_need[1]=0; q->q2_why=0;
                if(n->cls_any) { q->q2_cmd++; if(n->cls_any==1u<<NQC_COMPUTE) q->q2_comp++; }
            } else {
                nvn_cap_present(&n->texture,n->crop);
                if(mtl_q2_trace()) LOG("[q2trace] F%llu t %.6f present q%llu waits %llu",(unsigned long long)frame,clock_gettime_nsec_np(CLOCK_UPTIME_RAW)/1e9,(unsigned long long)q->serial,(unsigned long long)q->lf[1]);
                if(q->lf[1]) { mtl_q2_note(Q2N_PRESENT); mtl_q2_wait_q0(q->lf[1]); q->lf[1]=0; q->pend[1]=0; }   // DM_NVN_MTLQ2: the present (queue 0) comes after its NVN queue's work, a second-queue piece too
                present=mtl_present(&n->texture,n->period,n->crop,&copy); mtl_tex_unpin(n->texture.serial);
            }
            nq_cost_end(cost); gfx_leave(); objc_autoreleasePoolPop(pool); pthread_mutex_unlock(&g_sub_mu);
        }
        if(n->kind==NQ_COMMANDS && n->cls_last && !(fence && mtl_fence_mq(fence)==1)) nq_last_cls=n->cls_last;   // DM_NVN_QDEFER: what the last piece replayed on the first Metal queue ended in
        pthread_mutex_lock(&nq_mu); if(n->kind==NQ_PRESENT) { q->present=present; q->copy=copy; j->receipt->present=present; j->receipt->copy=copy; } if(fence) { q->physical=fence; for(unsigned i=0;i<n->signal_count;i++) nq_event_issued(n->signals[i].event,fence,nq_fence2(q),n->signals[i].mode,n->signals[i].value); } if(n->receipt) nq_put(n->receipt); free(n->signals); free(n->bytes); free(n); pthread_cond_broadcast(&nq_cv); pthread_mutex_unlock(&nq_mu);
    }
    return NULL;
}
static void nq_start(void) { pthread_t t; if(pthread_create(&t,NULL,nq_worker,NULL)) die("NVN queue worker failed"); pthread_detach(t); }
static nq_node *nq_append(nq_job *j,unsigned kind) { nq_node *n=calloc(1,sizeof *n); if(!n) die("queue continuation allocation failed"); n->kind=kind; if(j->last) j->last->next=n; else j->first=n; j->last=n; return n; }
// Grow geometrically so flattening a large submission does not reallocate its command records at every page.
static void nq_bytes(nq_node *n, const void *bytes, size_t length) {
    if (length > SIZE_MAX - n->length) die("queue commands allocation overflow");
    size_t want = n->length + length;
    if (want > n->capacity) {
        size_t cap = n->capacity <= SIZE_MAX / 2 ? 2 * n->capacity : want;
        cap = cap < want ? want : cap;
        if (cap < 4096) cap = 4096;
        void *grown = realloc(n->bytes, cap);
        if (!grown) die("queue commands allocation failed");
        n->bytes = grown;
        n->capacity = cap;
    }
    memcpy(n->bytes + n->length, bytes, length);
    n->length = want;
}
static void nq_signal_add(nq_node *n,nq_signal s) {
    if(!(n->signal_count&(n->signal_count-1))) { n->signals=realloc(n->signals,(n->signal_count?2*n->signal_count:1)*sizeof *n->signals); if(!n->signals) die("queue signals allocation failed"); } // NOLINT(bugprone-suspicious-realloc-usage): allocation failure aborts
    n->signals[n->signal_count++]=s;
}
static void nq_end(nq_job *j) { if(j->last && j->last->kind==NQ_COMMANDS && !j->last->closed) { op_hdr end={OP_END,8}; nq_bytes(j->last,&end,8); j->last->closed=1; } }
// One record of a recording, copied into the job (its object pointers snapshotted); waits and fences cut the job. A signal does not: its value is published when the commands
// it is among complete (mtl_sub_event), and a wait on it is ordered by that value or, while it is in flight, by the fence of those commands (nq_event_write), so the node keeps
// every signal it records. Each node is a Metal command buffer of its own, so a cut ends the open render pass: BioShock Infinite signals an event per occlusion query, and a cut
// at each took its title screen from 88 to 189 passes a frame and from 734 MB to 2.58 GB of attachment loads (issue #2112)
static void nq_record_one(nq_job *j,const uint8_t *p,op_hdr h) {
    const uint8_t *a=p+8;
    if(h.op==OP_END || h.op==OP_JUMP || h.op==OP_CALL) die("NVN command list: a %s record inside a recording's piece at %p",h.op==OP_END?"END":h.op==OP_JUMP?"JUMP":"CALL",(const void *)p);
    if(h.op==OP_WAIT_EVENT || h.op==OP_WAIT_SYNC || h.op==OP_FENCE_SYNC) {
        nq_end(j); nq_node *n=nq_append(j,h.op==OP_WAIT_EVENT?NQ_EVENT:h.op==OP_WAIT_SYNC?NQ_SYNC:NQ_FENCE);
        if(h.op==OP_WAIT_EVENT) { memcpy(&n->event,a,8); memcpy(&n->mode,a+8,4); memcpy(&n->value,a+12,4); }
        else { memcpy(&n->sync_source,a,8); if(h.op==OP_FENCE_SYNC) n->receipt=nq_new(); }
    } else {
        nq_node *n=j->last; if(!n || n->kind!=NQ_COMMANDS || n->closed) n=nq_append(j,NQ_COMMANDS);
        unsigned cls=nq_op_class(h.op); if(cls) { if(!n->cls_first) n->cls_first=cls; n->cls_last=cls; n->cls_any|=1u<<cls; n->worked=1; n->bar_tail=0; }   // DM_NVN_QDEFER; DM_NVN_MTLQ2: what the piece holds, and its game barriers (V1)
        if(h.op==OP_BARRIER) { uint32_t bits; memcpy(&bits,a,4); if(bits) { n->bar_any=1; if(n->worked) n->bar_tail=1; n->bar_bits|=bits; if(!n->worked) n->bar_head|=bits; } }
        size_t offset=n->length; nq_bytes(n,p,h.size); if(!n->bytes) die("queue commands: a record of %u bytes",h.size);
        if(h.op==OP_AS_BUILD) {
            if(h.size<8+sizeof(p_as_build))die("invalid acceleration-structure command size");
            nq_as_snapshot(j,(p_as_build *)(n->bytes+offset+8));
        }
        if(!cap_rec_fields(h.op,n->bytes+offset+8,h.size,&nq_snapshot_ops,j)) die("queue snapshot: unknown command %u",h.op);
        if(h.op==OP_SIGNAL_EVENT) {
            nq_signal sig; memcpy(&sig.event,a,8); memcpy(&sig.mode,a+8,4); memcpy(&sig.value,a+12,4); nq_signal_add(n,sig);
        }
    }
}
// A compact record (nvn_obj.h OPC_*) goes into the job as the canonical records it stands for
static void nq_record(nq_job *j,const uint8_t *p,op_hdr h) {
    if(!rec_compact(h.op)) { nq_record_one(j,p,h); return; }
    uint8_t x[REC_EXPAND_MAX] __attribute__((aligned(8))); size_t n=rec_expand(p,x);
    for(size_t o=0;o<n;) { op_hdr c; memcpy(&c,x+o,8); nq_record_one(j,x+o,c); o+=c.size; }
}
// A handle is the recording's control list (nvn_obj.h ctl_e): its pieces of records, its links to more control memory. Calls were copied in when they were recorded.
// A piece runs what its command memory holds at submit, as the GPU runs a command set's bytes: the records that fit in it (nvn_piece_record). A game may record over a called
// recording before the list that calls it is submitted (docs/reference/nvn-objects.md, "A called recording recorded over before its caller is submitted")
static void nq_flatten(nq_job *j,const uint8_t *list) {
    for(const uint8_t *e=list;;e+=8) {
        uint64_t v; memcpy(&v,e,8); unsigned k=ctl_kind(v);
        if(k==CTL_LINK) { e=ctl_addr(v)-8; continue; }
        if(k!=CTL_CHUNK) die("NVN command list %p: corrupt control entry %#llx at %p",(const void *)list,(unsigned long long)v,(const void *)e);
        const uint8_t *p=ctl_addr(v),*pe=p+ctl_chunk_bytes(v); op_hdr h;
        while(nvn_piece_record(list,v,p,pe,&h)) { nq_record(j,p,h); p+=h.size; }
        if(v&CTL_LAST) return;
    }
}
static void nq_enqueue(nq_queue *q,nq_job *j) { if(q->last) q->last->next=j; else q->first=j; q->last=j; nq_put(q->tail); q->tail=j->receipt; q->tail->refs++; pthread_cond_broadcast(&nq_cv); }
void nvn_queue_submit(queue_t *q,int count,const uint64_t *handles) {
    pthread_once(&nq_once,nq_start); nq_job *j=calloc(1,sizeof *j); if(!j) die("queue job allocation failed"); j->receipt=nq_new();
    j->objects.klen=16; j->frame=dm_nvn_frame_count(); j->submitted=guest_ns();
    // No scheduler lock while copying descriptors or resolving shader libraries.
    dm_host_mutex_lock(&g_sub_mu); void *pool=objc_autoreleasePoolPush(); dm_host_work++; mtl_prepare(); dm_host_work--; nq_cost cost=nq_cost_begin();
    for(int i=0;i<count;i++) nq_flatten(j,(const uint8_t *)(uintptr_t)handles[i]); nq_end(j);
    nq_cost_end(cost); objc_autoreleasePoolPop(pool); pthread_mutex_unlock(&g_sub_mu);
    int waits=0; for(nq_node *node=j->first;node;node=node->next) waits|=node->kind==NQ_EVENT || node->kind==NQ_SYNC;
    nvn_cap_queue(q->serial,waits);
    pthread_mutex_lock(&nq_mu);
    for(nq_node *node=j->first;node;node=node->next) if(node->sync_source) {
        if(node->kind==NQ_FENCE) nq_set_sync((sync_t *)node->sync_source,node->receipt); else nq_snapshot_sync(node,node->sync_source);
        node->sync_source=NULL;
    }
    nq_enqueue(nq_get(q),j); pthread_mutex_unlock(&nq_mu);
}
// The window retains this logical receipt until it reacquires or finalizes that image.
// Only replay turns it into a physical display ticket; present never waits on the guest thread.
uint64_t nvn_queue_present(queue_t *q,const tex_t *t,double period,const int crop[4]) {
    nvn_cap_queue(q->serial,0);
    pthread_once(&nq_once,nq_start); nq_job *j=calloc(1,sizeof *j); if(!j) die("queue present allocation failed");
    j->receipt=nq_new(); j->frame=dm_nvn_frame_count(); { void mtl_present_recorded(uint64_t); mtl_present_recorded(j->frame); }   // the game's own present count, for the screen wait (nvn_mtl.m)
    nq_node *n=nq_append(j,NQ_PRESENT);
    n->texture=*t; mtl_tex_pin(t->serial); n->period=period; if(crop) memcpy(n->crop,crop,sizeof n->crop);
    pthread_mutex_lock(&nq_mu); j->receipt->refs++; uint64_t ticket=(uintptr_t)j->receipt;
    nq_enqueue(nq_get(q),j); pthread_mutex_unlock(&nq_mu); return ticket;
}
void nvn_queue_test_hold(int on) { pthread_mutex_lock(&nq_mu); nq_test_held=on; pthread_cond_broadcast(&nq_cv); pthread_mutex_unlock(&nq_mu); }
void nvn_queue_present_release(uint64_t ticket) {
    if(!ticket) return; pthread_mutex_lock(&nq_mu); nq_put((nq_receipt *)(uintptr_t)ticket); pthread_mutex_unlock(&nq_mu);
}
void nvn_queue_present_wait(uint64_t ticket) {
    if(!ticket) return; nq_receipt *r=(nq_receipt *)(uintptr_t)ticket;
    pthread_mutex_lock(&nq_mu); while(!r->published) pthread_cond_wait(&nq_cv,&nq_mu); uint64_t physical=r->present, copy=r->copy; pthread_mutex_unlock(&nq_mu);
    if(physical) mtl_present_wait_texture(physical,copy);   // the texture's next acquire: a snapshot's copy, else the present itself
}
void nvn_queue_fence(queue_t *q,sync_t *s) {
    pthread_mutex_lock(&nq_mu); nq_queue *p=nq_get(q);
    if(p->tail) nq_set_sync(s,p->tail); else { if(s->fenced==3) nq_put((nq_receipt *)(uintptr_t)s->fence); s->fenced=1; s->fence=p->physical; }
    pthread_mutex_unlock(&nq_mu);
}
int nvn_sync_wait(const sync_t *s,uint64_t timeout) {
    uint64_t start=now_ns(),fence=0,fence2=0; pthread_mutex_lock(&nq_mu); nq_cpu_waiters++; pthread_cond_broadcast(&nq_cv);   // DM_NVN_QDEFER: a held piece runs while the CPU waits
    while(!nq_sync_encoded(s,&fence,&fence2)) {
        uint64_t elapsed=now_ns()-start; if(timeout!=UINT64_MAX && elapsed>=timeout) { nq_cpu_waiters--; pthread_mutex_unlock(&nq_mu); return 0; }
        if(timeout==UINT64_MAX) pthread_cond_wait(&nq_cv,&nq_mu);
        else { uint64_t left=timeout-elapsed; struct timespec ts={(time_t)(left/1000000000ull),(long)(left%1000000000ull)}; pthread_cond_timedwait_relative_np(&nq_cv,&nq_mu,&ts); }
    }
    nq_cpu_waiters--; pthread_mutex_unlock(&nq_mu); uint64_t elapsed=now_ns()-start;
    if(fence2 && !mtl_wait(fence2,timeout==UINT64_MAX?timeout:elapsed>=timeout?0:timeout-elapsed)) return 0;   // DM_NVN_MTLQ2: the NVN queue's piece on the other Metal queue first
    elapsed=now_ns()-start;
    return mtl_wait(fence,timeout==UINT64_MAX?timeout:elapsed>=timeout?0:timeout-elapsed);
}
void nvn_queue_wait_sync(queue_t *q,const sync_t *s) {
    nvn_cap_queue(q->serial,1);
    pthread_once(&nq_once,nq_start); nq_job *j=calloc(1,sizeof *j); if(!j) die("queue wait allocation failed"); j->receipt=nq_new();
    pthread_mutex_lock(&nq_mu); nq_snapshot_sync(nq_append(j,NQ_SYNC),s); nq_enqueue(nq_get(q),j); pthread_mutex_unlock(&nq_mu);
}
void nvn_queue_finish(queue_t *q) { sync_t s={0}; nvn_queue_fence(q,&s); nvn_sync_wait(&s,UINT64_MAX); nvn_sync_release(&s);
    pthread_mutex_lock(&nq_mu); nq_queue *p=nq_get(q); uint64_t present=p->present, copy=p->copy; pthread_mutex_unlock(&nq_mu);
    if(present) mtl_present_wait_texture(present,copy);   // a snapshot present: its copy has run, not the screen (the display link would pace a game that finishes every frame)
}
void nvn_queue_forget(queue_t *q) { nvn_queue_finish(q); pthread_mutex_lock(&nq_mu); nq_queue **p=&nq_queues; while(*p && (*p)->serial!=q->serial) p=&(*p)->next; if(*p) { nq_queue *old=*p; *p=old->next; nq_put(old->tail); free(old); } pthread_mutex_unlock(&nq_mu); }
