#include "nvn_capture.h"
#include "nvn_hmap.h"

void *objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void *);

// Ready continuations of NVN queues share the renderer's one ordered Metal queue.
// An unresolved wait parks its queue, never the submitting CPU or another queue.
typedef struct nq_receipt { unsigned refs, published; uint64_t fence, present, copy; } nq_receipt;   // present, copy: a present's display ticket and its snapshot copy (mtl_present)
typedef struct nq_signal { volatile uint32_t *event; unsigned mode; uint32_t value; } nq_signal;
typedef struct nq_node {
    struct nq_node *next;
    uint8_t *bytes; size_t length, capacity;
    unsigned kind, mode, closed, waiting, cont; volatile uint32_t *event; uint32_t value;   // cont: NQ_COMMANDS closed at a chunk boundary (nq_chunk_cut); the job's next commands continue its submission
    uint64_t wait_fence, event_predecessor, wait_since; unsigned wait_kind, wait_said, wait_present; const sync_t *sync_source; nq_receipt *receipt, *next_receipt;
    nq_signal *signals; unsigned signal_count;   // NQ_COMMANDS: every event signal it records, in order (nq_event_issued at its commit)
    tex_t texture; double period; int crop[4];
} nq_node;
typedef struct nq_owned { struct nq_owned *next; unsigned kind; uint64_t data[]; } nq_owned;
void nvn_qt_note(uint64_t frame, int k, uint64_t ns); void nvn_qtrace(const char *what, uint64_t a, uint64_t b); extern _Atomic uint64_t g_rq_idle_ns, g_rq_idle_why[4], g_sync_pub_ns;   // the queue timeline rows (hle_nvn.c)
// An empty open job stays at the head of its queue until more nodes arrive.
typedef struct nq_job { struct nq_job *next; nq_node *first, *last; nq_receipt *receipt; nq_owned *owned; nvn_hmap objects; uint64_t frame, submitted;
    nq_node *staged_first, *staged_last; int open, waits; size_t chunk; } nq_job;   // chunk: the size the open commands node is cut at (nq_chunk_cut)
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
        if(kind==CAP_K_PROG) {
            prog_t *copy=(prog_t *)o->data;
            const prog_t *source=(const prog_t *)(uintptr_t)*field;
            if(j->open) mtl_prog_snapshot_pin(copy,source);
            else mtl_prog_snapshot(copy,source);
        } else { memcpy(o->data,(const void *)(uintptr_t)*field,bytes); mtl_tex_pin(((tex_t *)o->data)->serial); }
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
    nq_job *first, *last; nq_receipt *tail;
} nq_queue;
static pthread_mutex_t nq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t nq_cv = PTHREAD_COND_INITIALIZER;
static pthread_once_t nq_once = PTHREAD_ONCE_INIT;
static nq_queue *nq_queues;
static int g_nq_pool_on = -1;
static int nq_pool_on(void) {
    if (__builtin_expect(g_nq_pool_on < 0, 0)) {
        const char *e = getenv("DM_NVN_QPOOL");
        g_nq_pool_on = !(e && !strcmp(e, "0"));
    }
    return g_nq_pool_on;
}
enum { NQ_BPOOL_N = 8, NQ_BPOOL_MAX = 8u << 20 };
static struct { uint8_t *p; size_t cap; } g_nq_bpool[NQ_BPOOL_N];
static int g_nq_bpool_n;
static void nq_buf_give_locked(uint8_t *p, size_t cap) {
    if (!p) return;
    if (nq_pool_on() && cap >= 4096 && cap <= NQ_BPOOL_MAX) {
        if (g_nq_bpool_n < NQ_BPOOL_N) {
            g_nq_bpool[g_nq_bpool_n].p = p; g_nq_bpool[g_nq_bpool_n].cap = cap; g_nq_bpool_n++;
            return;
        }
        int sm = 0;
        for (int i = 1; i < NQ_BPOOL_N; i++) if (g_nq_bpool[i].cap < g_nq_bpool[sm].cap) sm = i;
        if (cap > g_nq_bpool[sm].cap) { free(g_nq_bpool[sm].p); g_nq_bpool[sm].p = p; g_nq_bpool[sm].cap = cap; return; }
    }
    free(p);
}
static void nq_buf_take(nq_node *n, size_t hint) {
    if (!nq_pool_on()) return;
    pthread_mutex_lock(&nq_mu);
    int pick = -1;
    for (int i = 0; i < g_nq_bpool_n; i++) {
        if (g_nq_bpool[i].cap >= hint) {
            if (pick < 0 || g_nq_bpool[i].cap < g_nq_bpool[pick].cap) pick = i;
        }
    }
    if (pick < 0 && g_nq_bpool_n > 0) {
        pick = 0;
        for (int i = 1; i < g_nq_bpool_n; i++) if (g_nq_bpool[i].cap > g_nq_bpool[pick].cap) pick = i;
    }
    if (pick >= 0) {
        n->bytes = g_nq_bpool[pick].p; n->capacity = g_nq_bpool[pick].cap;
        g_nq_bpool[pick] = g_nq_bpool[--g_nq_bpool_n];
    }
    pthread_mutex_unlock(&nq_mu);
}
static int nq_test_held;   // nvn_queue_test_hold: replay takes no queued work while set (unit tests only)
// The last GPU write of each event word not yet landed: its submission's fence, and for a WRITE signal the value it leaves (exact). A wait the queued value satisfies needs no
// completion: its continuation is encoded after the writer's on the one ordered Metal queue, as the next submission of one NVN queue is, and memory the writer's completion lands
// on the CPU (reports, render-target readbacks) is that same in-flight case. An ADD's result depends on the writes landing before it, so such a wait still waits for them
typedef struct nq_event_write { struct nq_event_write *next; volatile uint32_t *word; uint64_t fence; uint32_t value; int exact; } nq_event_write;
static nq_event_write *nq_event_writes;
static uint64_t nq_event_pending(volatile uint32_t *word) { for(nq_event_write *w=nq_event_writes;w;w=w->next) if(w->word==word) return w->fence; return 0; }
static nq_event_write *nq_event_write_of(volatile uint32_t *word) { nq_event_write *w=nq_event_writes; while(w && w->word!=word) w=w->next; return w; }
static void nq_event_issued(volatile uint32_t *word,uint64_t fence,unsigned mode,uint32_t value) {
    nq_event_write *w=nq_event_write_of(word);
    if(!w) { w=calloc(1,sizeof *w); if(!w) die("event ordering allocation failed"); w->word=word; w->next=nq_event_writes; nq_event_writes=w; }
    w->fence=fence; w->value=value; w->exact=mode==0;
}

enum { NQ_COMMANDS, NQ_EVENT, NQ_SYNC, NQ_FENCE, NQ_PRESENT };
static nq_receipt *nq_new(void) { nq_receipt *r=calloc(1,sizeof *r); if(!r) die("queue receipt allocation failed"); r->refs=1; return r; }
static void nq_put(nq_receipt *r) { if(r && !--r->refs) free(r); }
static void nq_release_sync_locked(sync_t *s) {
    if (s->fenced == 3) nq_put((nq_receipt *)(uintptr_t)s->fence);
    else if (s->fenced == 4) { uint64_t nx, tk = nvn_acq_unref_take_ticket(s->fence, &nx); if (tk) nq_put((nq_receipt *)(uintptr_t)tk); if (nx) nq_put((nq_receipt *)(uintptr_t)nx); }
    s->fenced = 0; s->fence = 0;
}
static void nq_set_sync(sync_t *s,nq_receipt *r) { nq_release_sync_locked(s); r->refs++; s->fence=(uintptr_t)r; s->fenced=3; }
void nvn_sync_release(sync_t *s) { pthread_mutex_lock(&nq_mu); nq_release_sync_locked(s); pthread_mutex_unlock(&nq_mu); }
void nvn_sync_set_acq(sync_t *s, uint64_t handle) { pthread_mutex_lock(&nq_mu); nq_release_sync_locked(s); s->fence = handle; s->fenced = 4; pthread_mutex_unlock(&nq_mu); }
void nvn_sync_set_acq_fence(sync_t *s, uint64_t fence) { pthread_mutex_lock(&nq_mu); nq_release_sync_locked(s); s->fence = fence; s->fenced = 2; pthread_mutex_unlock(&nq_mu); }
uint64_t nvn_sync_ref_acq(const sync_t *s) {
    pthread_mutex_lock(&nq_mu);
    uint64_t h = s->fenced == 4 ? s->fence : 0;
    if (h) nvn_acq_ref(h);
    pthread_mutex_unlock(&nq_mu);
    return h;
}
static nq_queue *nq_get(queue_t *q) {
    nq_queue *p=nq_queues; while(p && p->serial!=q->serial) p=p->next;
    if(!p) { p=calloc(1,sizeof *p); if(!p) die("queue state allocation failed"); p->serial=q->serial; p->origin=q->origin_ll; p->next=nq_queues; nq_queues=p; }
    return p;
}
static void nq_publish(nq_receipt *r,uint64_t fence) { r->fence=fence; r->published=1; pthread_cond_broadcast(&nq_cv); }
static int nq_predicate(uint32_t current,unsigned mode,uint32_t target) { return mode==0 ? current==target : (uint32_t)(current-target)<0x80000000u; }
static int nq_sync_encoded(const sync_t *s,uint64_t *fence) {
    if(s->fenced==3) { nq_receipt *r=(nq_receipt *)(uintptr_t)s->fence; if(!r->published) return 0; *fence=r->fence; return 1; }
    if(!s->fenced) return 0; *fence=s->fence; return 1;
}
static int nvn_sync_gpu_point(const sync_t *s, uint64_t *serial, int *from_nvn) {
    if (!s) return 0;
    pthread_mutex_lock(&nq_mu);
    unsigned kind = s->magic == MAGIC('S', 'Y', 'N', 'C') ? s->fenced : 0;
    nq_receipt *receipt = kind == 3 ? (nq_receipt *)(uintptr_t)s->fence : NULL;
    if (receipt) {
        receipt->refs++;
        while (!receipt->published) pthread_cond_wait(&nq_cv, &nq_mu);
        *serial = receipt->fence;
        nq_put(receipt);
    } else if (kind == 1 || kind == NVN_SYNC_GL) *serial = s->fence;
    *from_nvn = kind != NVN_SYNC_GL;
    pthread_mutex_unlock(&nq_mu);
    return kind == 1 || kind == 3 || kind == NVN_SYNC_GL;
}
void nvn_queue_gpu_progress(void) {
    pthread_mutex_lock(&nq_mu); uint64_t done=mtl_completed_fence(); nq_event_write **p=&nq_event_writes;
    while(*p) { nq_event_write *w=*p; if(w->fence<=done) { *p=w->next; free(w); } else p=&w->next; }
    pthread_cond_broadcast(&nq_cv); pthread_mutex_unlock(&nq_mu);
}
static void nq_event_publish(volatile uint32_t *word,unsigned mode,uint32_t value,uint64_t gpu_fence) {
    pthread_mutex_lock(&nq_mu);
    if(mode==1) atomic_fetch_add_explicit((_Atomic uint32_t *)word,value,memory_order_release);
    else atomic_store_explicit((_Atomic uint32_t *)word,value,memory_order_release);
    atomic_thread_fence(memory_order_seq_cst);
    // Latch waits already reached: a subsequent signal must not erase a release.
    for(nq_queue *q=nq_queues;q;q=q->next) if(q->first && q->first->first) {
        nq_node *n=q->first->first;
        if(n->kind==NQ_EVENT && n->waiting && n->event==word && (gpu_fence ? gpu_fence>=n->event_predecessor : nq_event_pending(word)<=mtl_completed_fence()) && nq_predicate(atomic_load_explicit((_Atomic uint32_t *)word,memory_order_acquire),n->mode,n->value)) { n->event=NULL; n->wait_fence=gpu_fence; }
    }
    pthread_cond_broadcast(&nq_cv); pthread_mutex_unlock(&nq_mu);
}
void nvn_event_publish(volatile uint32_t *word,unsigned mode,uint32_t value) { nvn_qtrace("evpublish cpu",value,mode); nq_event_publish(word,mode,value,0); }
void nvn_event_publish_gpu(volatile uint32_t *word,unsigned mode,uint32_t value,uint64_t fence) { nq_event_publish(word,mode,value,fence); }
static void nq_snapshot_sync(nq_node *n,const sync_t *s) {
    n->wait_kind=s->fenced; n->wait_fence=s->fence; n->wait_present=0;
    if(s->fenced==3) { n->wait_fence=0; n->receipt=(nq_receipt *)(uintptr_t)s->fence; n->receipt->refs++; }
    else if(s->fenced==4) {
        uint64_t gf=0, tk=0, nx=0; nvn_acq_snapshot(s->fence,&gf,&tk,&nx);
        n->wait_fence=gf;
        if(tk) { n->receipt=(nq_receipt *)(uintptr_t)tk; n->receipt->refs++; n->wait_present=1; }
        if(nx) { n->next_receipt=(nq_receipt *)(uintptr_t)nx; n->next_receipt->refs++; }
    }
}
typedef struct { uint64_t wall, cpu, instr, cycles, sys; } nq_cost;
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
    (void)unused; pthread_setname_np("NVN queue replay"); { void mtl_pc_claim(void); mtl_pc_claim(); }   // its address-to-pool cache needs no thread-local lookup (nvn_mtl.m pc_tab)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);
    void *rt=nvn_rt_join("queue replay thread");   // it carries the frame's submissions: it declares the frame's deadline too (hle_nvn.c rt_tick)
    uint64_t frame=UINT64_MAX;
    for(;;) {
        pthread_mutex_lock(&nq_mu); nq_queue *q=NULL; nq_node *n=NULL; nq_job *j=NULL;
        for(;;) {
        scan_again:;
            if(nq_test_held) { pthread_cond_wait(&nq_cv,&nq_mu); continue; }
            int mapped_event_wait=0, held_event=0, held_sync=0, any_job=0;
            for(q=nq_queues;q;q=q->next) {
                j=q->first; if(!j) continue; n=j->first; any_job=1;
                if(!n && j->open) continue;   // a streamed submission still being copied: its next node is not published yet
                if(!n) { nq_publish(j->receipt,q->physical); q->first=j->next; if(!q->first) q->last=NULL; nq_put(j->receipt);
                    pthread_mutex_unlock(&nq_mu); pthread_mutex_lock(&g_sub_mu); void *pool=objc_autoreleasePoolPush(); nq_job_free(j); objc_autoreleasePoolPop(pool); pthread_mutex_unlock(&g_sub_mu); pthread_mutex_lock(&nq_mu); goto scan_again; }
                if(n->kind==NQ_EVENT && n->event) {
                    uint64_t predecessor=nq_event_pending(n->event);
                    if(!n->waiting) n->event_predecessor=predecessor;
                    // The queued GPU write satisfies the wait: the GPU runs this continuation after it (nq_event_write). Ocarina of Time waits on about 25 events a frame; each wait
                    // for the writer's completion left the GPU idle while the host encoded the next submission, 2 ms of its 14.5 ms frame, which its dynamic resolution read as work
                    nq_event_write *queued=predecessor>mtl_completed_fence() ? nq_event_write_of(n->event) : NULL;
                    // The writer's fence goes with the wait (q->physical below): a fence right behind it waits for the writer
                    if(queued && queued->exact && nq_predicate(queued->value,n->mode,n->value)) { n->wait_fence=queued->fence; break; }
                    if(predecessor>mtl_completed_fence() || !nq_predicate(atomic_load_explicit((_Atomic uint32_t *)n->event,memory_order_acquire),n->mode,n->value)) {
                        // A queue held more than 5 s by one event wait: what it waits for, since the game's own GPU watchdog acts on the fences behind it (PRAGMATA aborts after 15 s)
                        uint64_t t=now_ns(); if(!n->wait_since) n->wait_since=t;
                        else if(t-n->wait_since>5000000000ull && !n->wait_said) { static _Atomic int said; n->wait_said=1; if(atomic_fetch_add(&said,1)<8)
                            LOG("[RED] queue %llu: an event wait has held it %.1f s: word %p is %#x, waiting for %s %#x; a GPU write of it is %s",(unsigned long long)q->serial,(t-n->wait_since)/1e9,(void *)n->event,
                                atomic_load_explicit((_Atomic uint32_t *)n->event,memory_order_acquire),n->mode==0?"equal to":"at least",n->value,predecessor>mtl_completed_fence()?"queued, not completed":"not queued"); }
                        nvn_qtrace("evwait pred-done/word",(uint64_t)(predecessor>mtl_completed_fence()?predecessor-mtl_completed_fence():0),((uint64_t)atomic_load_explicit((_Atomic uint32_t *)n->event,memory_order_acquire)<<32)|n->value);
                        n->waiting=1; mapped_event_wait=1; held_event=1; continue; }
                }
                if(n->kind==NQ_SYNC) {
                    nq_receipt *pending=(n->receipt && !n->receipt->published)?n->receipt:(n->next_receipt && !n->next_receipt->published)?n->next_receipt:NULL;
                    if(pending) {
                        uint64_t t=now_ns(); if(!n->wait_since) n->wait_since=t;
                        else if(t-n->wait_since>5000000000ull && !n->wait_said) { static _Atomic int said; n->wait_said=1; if(atomic_fetch_add(&said,1)<8)
                            LOG("[RED] queue %llu: a sync wait has held it %.1f s: the fence it waits for (receipt %p) is not published yet",(unsigned long long)q->serial,(t-n->wait_since)/1e9,(void *)pending); }
                        held_sync=1; continue; }
                    // Targets are immutable snapshots. One Metal queue orders an encoded producer before this continuation. A sync not yet fenced when the wait was submitted
                    // (wait_kind 0) is a snapshot of nothing to wait for, and the wait passes. Sparking! ZERO, on one NVN queue, waits on a sync before that queue's own first
                    // fence of it at its first load, which runs on the console; parked here, the fence behind the wait never published and its SyncWait stalled the game (#1808)
                }
                break;
            }
            if(q) break;
            // NVN events can also be changed by plain stores through mapped pool memory.
            // A bounded notification poll only while such a word is awaited complements API/callback wakes.
            uint64_t idle0=now_ns(); nvn_qtrace(held_event?"idle<event":held_sync?"idle<sync":any_job?"idle<other":"idle<none",0,0);
            if(mapped_event_wait) { struct timespec tick={0,1000000}; pthread_cond_timedwait_relative_np(&nq_cv,&nq_mu,&tick); }
            else pthread_cond_wait(&nq_cv,&nq_mu);
            { uint64_t dt=now_ns()-idle0; atomic_fetch_add_explicit(&g_rq_idle_ns,dt,memory_order_relaxed);
              atomic_fetch_add_explicit(&g_rq_idle_why[held_event?1:held_sync?2:any_job?3:0],dt,memory_order_relaxed); nvn_qtrace("idle>",dt/1000,0); }
        }
        j->first=n->next; if(!j->first) j->last=NULL;
        // Round-robin ready queues: a continually fed independent producer cannot starve another queue.
        if(q->next) { nq_queue **link=&nq_queues; while(*link!=q) link=&(*link)->next; *link=q->next; nq_queue *tail=nq_queues; while(tail->next) tail=tail->next; tail->next=q; q->next=NULL; }
        uint64_t wait_phys=0, wait_copy=0;
        int gl_fence = n->kind == NQ_SYNC && n->wait_kind == NVN_SYNC_GL;
        int wait_gl = gl_fence && !dm_gl_wait_serial(n->wait_fence, 0);
        if(n->kind==NQ_SYNC || n->kind==NQ_EVENT) {
            if(n->kind==NQ_SYNC && n->wait_present && n->receipt) { wait_phys=n->receipt->present; wait_copy=n->receipt->copy; }
            uint64_t f=n->wait_fence;
            if(n->receipt && n->receipt->fence>f) f=n->receipt->fence;
            if(n->next_receipt && n->next_receipt->fence>f) f=n->next_receipt->fence;
            if(!gl_fence && f>q->physical) q->physical=f;
        }
        if((n->kind==NQ_SYNC || n->kind==NQ_EVENT) && n->wait_since) q->wait_passed=guest_ns();   // the GPU could not start what follows before this either
        if(n->kind==NQ_FENCE) nq_publish(n->receipt,q->physical);
        pthread_mutex_unlock(&nq_mu);
        if(wait_phys || wait_copy) mtl_present_wait_texture(wait_phys,wait_copy);
        uint64_t fence=0, present=0, copy=0;
        if(n->kind==NQ_COMMANDS || n->kind==NQ_PRESENT || wait_gl) {
            nvn_rt_tick(rt); nvn_qt_note(j->frame,2,now_ns()); nvn_qtrace(n->kind==NQ_PRESENT?"rq<present":wait_gl?"rq<gl":"rq<cmds",j->frame,q->serial);
            dm_host_mutex_lock(&g_sub_mu); void *pool=objc_autoreleasePoolPush();
            dm_host_work++; mtl_prepare(); dm_host_work--;
            gfx_enter(); nq_cost cost=nq_cost_begin();
            if(frame!=j->frame) { frame=j->frame; mtl_set_frame(frame); }
            mtl_queue_activate(q->serial); mtl_set_queue_origin_lower_left(q->origin);
            if (wait_gl) {
                mtl_wait_gl_event(dm_gl_event(), n->wait_fence);
                mtl_sub_begin();
                mtl_exec_rest(NULL, 0);
                fence = mtl_sub_commit();
            } else if(n->kind==NQ_COMMANDS) {
                uint64_t handle=(uintptr_t)n->bytes;
                nvn_cap_submit(1,&handle);
                mtl_sub_begin(); mtl_sub_ready(j->submitted>q->wait_passed?j->submitted:q->wait_passed); nvn_ft_poll();
                mtl_exec_rest(NULL,0); mtl_exec(n->bytes);
                // A chunked node (nq_chunk_cut): the job's next commands node continues this submission. Wait for it while its producer copies (it holds no lock the replay holds)
                for(nq_node *c=n;c->cont;) {
                    uint64_t w0=now_ns(); pthread_mutex_lock(&nq_mu);
                    while(!j->first && j->open) pthread_cond_wait(&nq_cv,&nq_mu);
                    nq_node *x=j->first;
                    if(!x || x->kind!=NQ_COMMANDS) { pthread_mutex_unlock(&nq_mu); c->next=NULL; break; }   // the job's list keeps x; the chain ends at c
                    j->first=x->next; if(!j->first) j->last=NULL;
                    pthread_mutex_unlock(&nq_mu);
                    { uint64_t dt=now_ns()-w0; atomic_fetch_add_explicit(&g_rq_idle_ns,dt,memory_order_relaxed); atomic_fetch_add_explicit(&g_rq_idle_why[3],dt,memory_order_relaxed); }
                    x->next=NULL; c->next=x; c=x;   // kept on n's chain until the commit: their signals are issued with its fence
                    mtl_exec(x->bytes);
                }
                fence=mtl_sub_commit();
            } else {
                nvn_cap_present(&n->texture,n->crop);
                present=mtl_present(&n->texture,n->period,n->crop,&copy); mtl_tex_unpin(n->texture.serial);
                { void dm_vi_frame_ticket(uint64_t,uint64_t); dm_vi_frame_ticket(j->frame+1,present); }
            }
            nq_cost_end(cost); gfx_leave(); objc_autoreleasePoolPop(pool); pthread_mutex_unlock(&g_sub_mu);
            if(n->kind==NQ_PRESENT) nvn_qt_note(j->frame,3,now_ns());
            nvn_qtrace("rq>",j->frame,n->length);
        }
        pthread_mutex_lock(&nq_mu); if(n->kind==NQ_PRESENT) { q->present=present; q->copy=copy; j->receipt->present=present; j->receipt->copy=copy; }
        if(fence) q->physical=fence;
        int chain=n->kind==NQ_COMMANDS && n->cont;   // n->next is then the chain of chunks run with it (above), not the job's list
        for(nq_node *c=n,*nx;c;c=nx) { nx=chain?c->next:NULL;
            if(fence) for(unsigned i=0;i<c->signal_count;i++) nq_event_issued(c->signals[i].event,fence,c->signals[i].mode,c->signals[i].value);
            if(c->receipt) nq_put(c->receipt); if(c->next_receipt) nq_put(c->next_receipt); free(c->signals); nq_buf_give_locked(c->bytes, c->capacity); free(c); }
        pthread_cond_broadcast(&nq_cv); pthread_mutex_unlock(&nq_mu);
    }
    return NULL;
}
static void nq_start(void) { pthread_t t; if(pthread_create(&t,NULL,nq_worker,NULL)) die("NVN queue worker failed"); pthread_detach(t); }
// Only the producer touches the staged list. The worker takes nodes from first / last.
static nq_node *nq_append(nq_job *j,unsigned kind) {
    nq_node *n=calloc(1,sizeof *n); if(!n) die("queue continuation allocation failed");
    n->kind=kind;
    if(kind==NQ_COMMANDS) {
        size_t hint = (j->open && j->chunk) ? (j->chunk <= (4u << 20) ? j->chunk + 8192 : (4u << 20)) : 4096;
        nq_buf_take(n, hint);
    }
    if(j->staged_last) j->staged_last->next=n; else j->staged_first=n;
    j->staged_last=n;
    j->waits|=kind==NQ_EVENT || kind==NQ_SYNC;
    return n;
}
// Under nq_mu at a wait/fence boundary or at end of input: snapshot the syncs in
// command order before the worker can take any of these completed nodes.
static void nq_publish_staged(nq_job *j) {
    if(!j->staged_first) return;
    for(nq_node *n=j->staged_first;n;n=n->next) {
        if(!n->sync_source) continue;
        if(n->kind==NQ_FENCE) nq_set_sync((sync_t *)n->sync_source,n->receipt);
        else nq_snapshot_sync(n,n->sync_source);
        n->sync_source=NULL;
    }
    if(j->last) j->last->next=j->staged_first; else j->first=j->staged_first;
    j->last=j->staged_last;
    j->staged_first=j->staged_last=NULL;
}
// Grow geometrically so flattening a large submission does not reallocate its command records at every page.
static __attribute__((noinline, cold)) void nq_grow(nq_node *n, size_t want) {
    size_t cap = n->capacity <= SIZE_MAX / 2 ? 2 * n->capacity : want;
    cap = cap < want ? want : cap;
    if (cap < 4096) cap = 4096;
    void *grown = realloc(n->bytes, cap);
    if (!grown) die("queue commands allocation failed");
    n->bytes = grown;
    n->capacity = cap;
}
static inline void nq_reserve(nq_node *n, size_t extra) {
    if (__builtin_expect(extra > SIZE_MAX - n->length, 0)) die("queue commands allocation overflow");
    size_t want = n->length + extra;
    if (__builtin_expect(want > n->capacity, 0)) nq_grow(n, want);
}
static inline void nq_bytes(nq_node *n, const void *bytes, size_t length) {
    nq_reserve(n, length);
    memcpy(n->bytes + n->length, bytes, length);
    n->length += length;
}
_Static_assert(OP_COUNT_ == 65, "nvn_queue.inc: update NQ_SPECIAL_MASK for new ops");
static const uint64_t NQ_SPECIAL_MASK =
    (1ull << OP_END) | (1ull << OP_JUMP) | (1ull << OP_CALL) |
    (1ull << OP_WAIT_EVENT) | (1ull << OP_WAIT_SYNC) | (1ull << OP_FENCE_SYNC) |
    (1ull << OP_SIGNAL_EVENT) | (1ull << OP_AS_BUILD) |
    (1ull << OP_SET_RT) | (1ull << OP_BIND_PROGRAM) |
    (1ull << OP_COPY_B2T) | (1ull << OP_COPY_T2B) | (1ull << OP_COPY_T2T) |
    (1ull << OP_CLEAR_TEX) | (1ull << OP_DLSS);
static inline int nq_is_plain_op(uint32_t op) {
    return op < 64 ? !((NQ_SPECIAL_MASK >> op) & 1u) : (op == OP_DEPTH_BOUNDS);
}
static int nq_fast_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_QFAST"); v = !(e && !strcmp(e, "0")); } return v; }
static void nq_signal_add(nq_node *n,nq_signal s) {
    if(!(n->signal_count&(n->signal_count-1))) { n->signals=realloc(n->signals,(n->signal_count?2*n->signal_count:1)*sizeof *n->signals); if(!n->signals) die("queue signals allocation failed"); } // NOLINT(bugprone-suspicious-realloc-usage): allocation failure aborts
    n->signals[n->signal_count++]=s;
}
static void nq_end(nq_job *j) { if(j->staged_last && j->staged_last->kind==NQ_COMMANDS && !j->staged_last->closed) { op_hdr end={OP_END,8}; nq_bytes(j->staged_last,&end,8); j->staged_last->closed=1; } }
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
        if(j->open) {
            pthread_mutex_lock(&nq_mu);
            nq_publish_staged(j);
            pthread_cond_broadcast(&nq_cv);
            pthread_mutex_unlock(&nq_mu);
        }
    } else {
        nq_node *n=j->staged_last; if(!n || n->kind!=NQ_COMMANDS || n->closed) n=nq_append(j,NQ_COMMANDS);
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
// DM_NVN_SUB_CHUNK_KB (default 128, 0 = off): a streamed submission's commands are handed to the replay in chunks, the first of this size and each next one 4 times the last,
// rather than as one node at its next wait or its end. The replay runs a job's chunks as one submission (nq_worker: no commit between them, so no render pass ends there). Donkey
// Kong Bananza's presentation thread submits about 5 MB of commands a frame after the frame's SyncWait, its first node 3 MB: the replay started it 1.5 ms after the submit,
// in the frame's critical path (the replay translates about 0.34 MB a ms, the copy runs at about 2 MB a ms, so only the first chunk is waited for)
static size_t nq_chunk0(void) { static long v=-1; if(v<0) { const char *e=getenv("DM_NVN_SUB_CHUNK_KB"); v=e?atol(e):128; if(v<0) v=0; } return (size_t)v*1024; }
static void nq_chunk_cut(nq_job *j) {
    nq_node *n=j->staged_last; if(!j->open || !n || n->kind!=NQ_COMMANDS || n->closed) return;
    if(!j->chunk) { j->chunk=nq_chunk0(); if(!j->chunk) return; }
    if(n->length<j->chunk) return;
    nq_end(j); n->cont=1; if(j->chunk<(SIZE_MAX>>3)) j->chunk*=4;
    pthread_mutex_lock(&nq_mu); nq_publish_staged(j); pthread_cond_broadcast(&nq_cv); pthread_mutex_unlock(&nq_mu);
}
// A compact record (nvn_obj.h OPC_*) goes into the job as the canonical records it stands for
static void nq_record(nq_job *j,const uint8_t *p,op_hdr h) {
    if(!rec_compact(h.op)) { nq_record_one(j,p,h); nq_chunk_cut(j); return; }
    uint8_t x[REC_EXPAND_MAX] __attribute__((aligned(8))); size_t n=rec_expand(p,x);
    for(size_t o=0;o<n;) { op_hdr c; memcpy(&c,x+o,8); nq_record_one(j,x+o,c); o+=c.size; }
    nq_chunk_cut(j);
}
// A handle is the recording's control list (nvn_obj.h ctl_e): its pieces of records, its links to more control memory. Calls were copied in when they were recorded.
// A piece runs what its command memory holds at submit, as the GPU runs a command set's bytes: the records that fit in it (nvn_piece_record). A game may record over a called
// recording before the list that calls it is submitted (docs/reference/nvn-objects.md, "A called recording recorded over before its caller is submitted")
static void nq_flatten(nq_job *j,const uint8_t *list) {
    int fast = nq_fast_on();
    for(const uint8_t *e=list;;e+=8) {
        uint64_t v; memcpy(&v,e,8); unsigned k=ctl_kind(v);
        if(k==CTL_LINK) { e=ctl_addr(v)-8; continue; }
        if(k!=CTL_CHUNK) die("NVN command list %p: corrupt control entry %#llx at %p",(const void *)list,(unsigned long long)v,(const void *)e);
        const uint8_t *p=ctl_addr(v),*pe=p+ctl_chunk_bytes(v); op_hdr h;
        if(!fast) {
            while(nvn_piece_record(list,v,p,pe,&h)) { nq_record(j,p,h); p+=h.size; }
        } else {
            size_t clim = j->open ? (j->chunk ? j->chunk : (j->chunk = nq_chunk0())) : 0;
            nq_node *cur = (j->staged_last && j->staged_last->kind == NQ_COMMANDS && !j->staged_last->closed) ? j->staged_last : NULL;
            for(int have = nvn_piece_record(list,v,p,pe,&h); have; ) {
                if(nq_is_plain_op(h.op)) {
                    if(!cur) cur = nq_append(j, NQ_COMMANDS);
                    const uint8_t *run = p;
                    p += h.size;
                    have = nvn_piece_record(list,v,p,pe,&h);
                    while(have && nq_is_plain_op(h.op) && (!clim || cur->length + (size_t)(p - run) < clim)) {
                        p += h.size;
                        have = nvn_piece_record(list,v,p,pe,&h);
                    }
                    nq_bytes(cur, run, (size_t)(p - run));
                    if(__builtin_expect(clim && cur->length >= clim, 0)) { nq_chunk_cut(j); clim = j->chunk; cur = NULL; }
                } else if(rec_compact(h.op)) {
                    if(!cur) cur = nq_append(j, NQ_COMMANDS);
                    uint8_t *base = cur->bytes, *dst = base ? base + cur->length : NULL, *dend = base ? base + cur->capacity : NULL;
                    do {
                        if(__builtin_expect(!base || (size_t)(dend - dst) < REC_EXPAND_MAX, 0)) {
                            cur->length = base ? (size_t)(dst - base) : 0;
                            nq_grow(cur, cur->length + REC_EXPAND_MAX);
                            base = cur->bytes; dst = base + cur->length; dend = base + cur->capacity;
                        }
                        dst += rec_expand_h(h, p, dst);
                        p += h.size;
                        have = nvn_piece_record(list,v,p,pe,&h);
                    } while(have && rec_compact(h.op) && (!clim || (size_t)(dst - base) < clim));
                    cur->length = (size_t)(dst - base);
                    if(__builtin_expect(clim && cur->length >= clim, 0)) { nq_chunk_cut(j); clim = j->chunk; cur = NULL; }
                } else {
                    nq_record_one(j,p,h);
                    p += h.size;
                    cur = (j->staged_last && j->staged_last->kind == NQ_COMMANDS && !j->staged_last->closed) ? j->staged_last : NULL;
                    if(__builtin_expect(clim && cur && cur->length >= clim, 0)) { nq_chunk_cut(j); clim = j->chunk; cur = NULL; }
                    have = nvn_piece_record(list,v,p,pe,&h);
                }
            }
        }
        if(v&CTL_LAST) return;
    }
}
static void nq_enqueue(nq_queue *q,nq_job *j) { if(q->last) q->last->next=j; else q->first=j; q->last=j; nq_put(q->tail); q->tail=j->receipt; q->tail->refs++; pthread_cond_broadcast(&nq_cv); }
// Publish the job before copying so the worker can replay closed nodes while
// the game thread still owns the recording and copies its remaining commands.
void nvn_queue_submit(queue_t *q,int count,const uint64_t *handles) {
    pthread_once(&nq_once,nq_start); nq_job *j=calloc(1,sizeof *j); if(!j) die("queue job allocation failed"); j->receipt=nq_new();
    j->objects.klen=16; j->frame=dm_nvn_frame_count(); j->submitted=guest_ns();
    // No scheduler lock while copying descriptors or resolving shader libraries.
    { uint64_t f0=now_ns(); nvn_qt_note(j->frame,0,f0); nvn_qt_note(j->frame,1,f0); nvn_qtrace("submit",q->serial,count); }
    dm_host_mutex_lock(&g_sub_mu); void *pool=objc_autoreleasePoolPush(); dm_host_work++; mtl_prepare(); dm_host_work--; nq_cost cost=nq_cost_begin();
    int stream=mtl_sub_stream_on();
    if(stream) {
        j->open=1;
        pthread_mutex_lock(&nq_mu); nq_enqueue(nq_get(q),j); pthread_mutex_unlock(&nq_mu);
        pthread_mutex_unlock(&g_sub_mu);
    }
    for(int i=0;i<count;i++) nq_flatten(j,(const uint8_t *)(uintptr_t)handles[i]); nq_end(j);
    nvn_qtrace("flat",q->serial,(now_ns()-cost.wall)/1000);
    nq_cost_end(cost); objc_autoreleasePoolPop(pool);
    nvn_cap_queue(q->serial,j->waits);
    pthread_mutex_lock(&nq_mu); nq_publish_staged(j);
    if(stream) { j->open=0; pthread_cond_broadcast(&nq_cv); }
    else nq_enqueue(nq_get(q),j);
    pthread_mutex_unlock(&nq_mu);
    if(!stream) pthread_mutex_unlock(&g_sub_mu);
}
// The window retains this logical receipt until it reacquires or finalizes that image.
// Only replay turns it into a physical display ticket; present never waits on the guest thread.
uint64_t nvn_queue_present(queue_t *q,const tex_t *t,double period,const int crop[4]) {
    nvn_cap_queue(q->serial,0);
    pthread_once(&nq_once,nq_start); nq_job *j=calloc(1,sizeof *j); if(!j) die("queue present allocation failed");
    j->receipt=nq_new(); j->frame=dm_nvn_frame_count(); nvn_qtrace("present",q->serial,0); { void mtl_present_recorded(uint64_t); mtl_present_recorded(j->frame); }   // the game's own present count, for the screen wait (nvn_mtl.m)
    nq_node *n=nq_append(j,NQ_PRESENT);
    n->texture=*t; mtl_tex_pin(t->serial); n->period=period; if(crop) memcpy(n->crop,crop,sizeof n->crop);
    pthread_mutex_lock(&nq_mu); nq_publish_staged(j); j->receipt->refs++; uint64_t ticket=(uintptr_t)j->receipt;
    nq_enqueue(nq_get(q),j); pthread_mutex_unlock(&nq_mu); return ticket;
}
void nvn_queue_test_hold(int on) { pthread_mutex_lock(&nq_mu); nq_test_held=on; pthread_cond_broadcast(&nq_cv); pthread_mutex_unlock(&nq_mu); }
void nvn_queue_present_ref(uint64_t ticket) {
    if(!ticket) return; pthread_mutex_lock(&nq_mu); ((nq_receipt *)(uintptr_t)ticket)->refs++; pthread_mutex_unlock(&nq_mu);
}
void nvn_queue_present_release(uint64_t ticket) {
    if(!ticket) return; pthread_mutex_lock(&nq_mu); nq_put((nq_receipt *)(uintptr_t)ticket); pthread_mutex_unlock(&nq_mu);
}
static int nq_receipt_wait_published(nq_receipt *r, uint64_t timeout_ns) {
    pthread_mutex_lock(&nq_mu);
    if(r->published) { pthread_mutex_unlock(&nq_mu); return 1; }
    if(!timeout_ns) { pthread_mutex_unlock(&nq_mu); return 0; }
    uint64_t start=timeout_ns==UINT64_MAX?0:now_ns();
    while(!r->published) {
        if(timeout_ns==UINT64_MAX) pthread_cond_wait(&nq_cv,&nq_mu);
        else {
            uint64_t el=now_ns()-start; if(el>=timeout_ns) { pthread_mutex_unlock(&nq_mu); return 0; }
            uint64_t left=timeout_ns-el; struct timespec ts={(time_t)(left/1000000000ull),(long)(left%1000000000ull)};
            pthread_cond_timedwait_relative_np(&nq_cv,&nq_mu,&ts);
        }
    }
    pthread_mutex_unlock(&nq_mu); return 1;
}
void nvn_queue_present_wait(uint64_t ticket) {
    if(!ticket) return; nq_receipt *r=(nq_receipt *)(uintptr_t)ticket;
    pthread_mutex_lock(&nq_mu); while(!r->published) pthread_cond_wait(&nq_cv,&nq_mu); uint64_t physical=r->present, copy=r->copy; pthread_mutex_unlock(&nq_mu);
    if(physical || copy) mtl_present_wait_texture(physical,copy);   // a snapshot can outlive its dropped display ticket
}
int nvn_queue_present_ready(uint64_t ticket) {
    if(!ticket) return 1; nq_receipt *r=(nq_receipt *)(uintptr_t)ticket;
    pthread_mutex_lock(&nq_mu); int pub=r->published; uint64_t physical=r->present, copy=r->copy; pthread_mutex_unlock(&nq_mu);
    return pub && mtl_present_texture_ready(physical,copy);
}
int nvn_queue_present_wait_timeout(uint64_t ticket, uint64_t timeout_ns) {
    if(!ticket) return 1;
    if(timeout_ns==UINT64_MAX) { nvn_queue_present_wait(ticket); return 1; }
    if(!timeout_ns) return nvn_queue_present_ready(ticket);
    uint64_t start=now_ns(); nq_receipt *r=(nq_receipt *)(uintptr_t)ticket;
    pthread_mutex_lock(&nq_mu);
    while(!r->published) {
        uint64_t el=now_ns()-start; if(el>=timeout_ns) { pthread_mutex_unlock(&nq_mu); return 0; }
        uint64_t left=timeout_ns-el; struct timespec ts={(time_t)(left/1000000000ull),(long)(left%1000000000ull)};
        pthread_cond_timedwait_relative_np(&nq_cv,&nq_mu,&ts);
    }
    uint64_t physical=r->present, copy=r->copy; pthread_mutex_unlock(&nq_mu);
    if(!physical && !copy) return 1;
    for(;;) {
        if(mtl_present_texture_ready(physical,copy)) return 1;
        uint64_t el=now_ns()-start; if(el>=timeout_ns) return 0;
        uint64_t left=timeout_ns-el, sl=left<1000000ull?left:1000000ull;
        sleep_until_ns(now_ns()+sl);
    }
}
int nvn_queue_present_gpu_ready(uint64_t ticket, int wait_tex) {
    if(!ticket) return 1; nq_receipt *r=(nq_receipt *)(uintptr_t)ticket;
    pthread_mutex_lock(&nq_mu); int pub=r->published; uint64_t physical=r->present, copy=r->copy, f=r->fence; pthread_mutex_unlock(&nq_mu);
    if(!pub) return 0;
    if(wait_tex && !mtl_present_texture_ready(physical,copy)) return 0;
    return !f || mtl_wait_acq(f,0);
}
// A present's GPU work done: its job replayed (the receipt published), optionally its texture release (wait_tex: the image's own present), and the GPU past the fence it ended at
int nvn_queue_present_gpu_wait_timeout(uint64_t ticket, uint64_t timeout_ns, int wait_tex) {
    if(!ticket) return 1;
    if(!timeout_ns) return nvn_queue_present_gpu_ready(ticket,wait_tex);
    uint64_t start=timeout_ns==UINT64_MAX?0:now_ns();
    if(wait_tex) { if(!nvn_queue_present_wait_timeout(ticket,timeout_ns)) return 0; }
    else { if(!nq_receipt_wait_published((nq_receipt *)(uintptr_t)ticket,timeout_ns)) return 0; }
    nq_receipt *r=(nq_receipt *)(uintptr_t)ticket;
    pthread_mutex_lock(&nq_mu); uint64_t f=r->fence; pthread_mutex_unlock(&nq_mu);
    if(!f) return 1;
    uint64_t el=timeout_ns==UINT64_MAX?0:now_ns()-start;
    uint64_t rem=timeout_ns==UINT64_MAX?UINT64_MAX:el>=timeout_ns?0:timeout_ns-el;
    return mtl_wait_acq(f,rem);
}
void nvn_queue_fence(queue_t *q,sync_t *s) {
    pthread_mutex_lock(&nq_mu); nq_queue *p=nq_get(q);
    if(p->tail) nq_set_sync(s,p->tail); else { nq_release_sync_locked(s); s->fenced=1; s->fence=p->physical; }
    pthread_mutex_unlock(&nq_mu);
}
int nvn_sync_wait(const sync_t *s,uint64_t timeout) {
    uint64_t start=now_ns(),fence=0; pthread_mutex_lock(&nq_mu);
    while(!nq_sync_encoded(s,&fence)) {
        uint64_t elapsed=now_ns()-start; if(timeout!=UINT64_MAX && elapsed>=timeout) { pthread_mutex_unlock(&nq_mu); return 0; }
        if(timeout==UINT64_MAX) pthread_cond_wait(&nq_cv,&nq_mu);
        else { uint64_t left=timeout-elapsed; struct timespec ts={(time_t)(left/1000000000ull),(long)(left%1000000000ull)}; pthread_cond_timedwait_relative_np(&nq_cv,&nq_mu,&ts); }
    }
    pthread_mutex_unlock(&nq_mu); uint64_t elapsed=now_ns()-start; if(timeout) atomic_fetch_add(&g_sync_pub_ns,elapsed);
    return mtl_wait(fence,timeout==UINT64_MAX?timeout:elapsed>=timeout?0:timeout-elapsed);
}
void nvn_queue_wait_sync(queue_t *q,const sync_t *s) {
    nvn_cap_queue(q->serial,1);
    pthread_once(&nq_once,nq_start); nq_job *j=calloc(1,sizeof *j); if(!j) die("queue wait allocation failed"); j->receipt=nq_new();
    pthread_mutex_lock(&nq_mu); nq_snapshot_sync(nq_append(j,NQ_SYNC),s); nq_publish_staged(j); nq_enqueue(nq_get(q),j); pthread_mutex_unlock(&nq_mu);
}
void nvn_queue_finish(queue_t *q) { sync_t s={0}; nvn_queue_fence(q,&s); nvn_sync_wait(&s,UINT64_MAX); nvn_sync_release(&s);
    pthread_mutex_lock(&nq_mu); nq_queue *p=nq_get(q); uint64_t present=p->present, copy=p->copy; pthread_mutex_unlock(&nq_mu);
    if(present || copy) mtl_present_wait_texture(present,copy);   // a snapshot present: its copy has run, not the screen (the display link would pace a game that finishes every frame)
}
void nvn_queue_forget(queue_t *q) { nvn_queue_finish(q); pthread_mutex_lock(&nq_mu); nq_queue **p=&nq_queues; while(*p && (*p)->serial!=q->serial) p=&(*p)->next; if(*p) { nq_queue *old=*p; *p=old->next; nq_put(old->tail); free(old); } pthread_mutex_unlock(&nq_mu); }
