// Production resolver, synthetic Apple API. No DNS traffic or account data.
#include <dns_sd.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct _DNSServiceRef_t {
    DNSServiceGetAddrInfoReply callback;
    void *context;
    dispatch_queue_t queue;
    _Atomic int closed;
};
static pthread_mutex_t fixture_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fixture_changed = PTHREAD_COND_INITIALIZER;
static DNSServiceRef services[512];
static unsigned service_count, deallocations, checks;
static int start_error, queue_error;
static char queue_key;
static DNSServiceErrorType fixture_start(DNSServiceRef *, DNSServiceFlags, uint32_t,
                                        DNSServiceProtocol, const char *, DNSServiceGetAddrInfoReply, void *);
static DNSServiceErrorType fixture_queue(DNSServiceRef, dispatch_queue_t);
static void fixture_deallocate(DNSServiceRef);
#define DNSServiceGetAddrInfo fixture_start
#define DNSServiceSetDispatchQueue fixture_queue
#define DNSServiceRefDeallocate fixture_deallocate
#include "../hle_net_backend.c"
#undef DNSServiceGetAddrInfo
#undef DNSServiceSetDispatchQueue
#undef DNSServiceRefDeallocate

static _Atomic unsigned waits, balances;
void dm_guest_host_wait_begin(void) {
    atomic_fetch_add(&waits, 1); atomic_fetch_add(&balances, 1);
    pthread_mutex_lock(&fixture_lock); pthread_cond_broadcast(&fixture_changed); pthread_mutex_unlock(&fixture_lock);
}
void dm_guest_host_wait_end(void) { atomic_fetch_sub(&balances, 1); }
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); abort(); } } while (0)
static DNSServiceErrorType fixture_start(DNSServiceRef *out, DNSServiceFlags flags, uint32_t index,
                                        DNSServiceProtocol protocol, const char *name,
                                        DNSServiceGetAddrInfoReply callback, void *context) {
    assert(dispatch_get_specific(&queue_key) == &queue_key);
    assert(flags == kDNSServiceFlagsTimeout && !index && protocol == kDNSServiceProtocol_IPv4 && name && callback);
    if (start_error) { *out=(DNSServiceRef)(uintptr_t)1; return start_error; }
    DNSServiceRef service = calloc(1, sizeof *service); assert(service);
    service->callback = callback; service->context = context; *out = service;
    pthread_mutex_lock(&fixture_lock); assert(service_count < 512);
    services[service_count++] = service; pthread_cond_broadcast(&fixture_changed); pthread_mutex_unlock(&fixture_lock);
    return 0;
}
static DNSServiceErrorType fixture_queue(DNSServiceRef service, dispatch_queue_t queue) {
    assert(service && queue && dispatch_get_specific(&queue_key) == &queue_key);
    service->queue = queue; return queue_error;
}
static void fixture_deallocate(DNSServiceRef service) {
    assert(dispatch_get_specific(&queue_key) == &queue_key && !atomic_exchange(&service->closed, 1));
    ++deallocations;
}
static void wait_services(unsigned target) {
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += 5;
    pthread_mutex_lock(&fixture_lock);
    while (service_count < target) assert(!pthread_cond_timedwait(&fixture_changed, &fixture_lock, &until));
    pthread_mutex_unlock(&fixture_lock);
}
static int blocked, unblocked;
static void block_queue(void *unused) {
    (void)unused;
    pthread_mutex_lock(&fixture_lock); blocked=1; pthread_cond_broadcast(&fixture_changed);
    while (!unblocked) pthread_cond_wait(&fixture_changed,&fixture_lock);
    pthread_mutex_unlock(&fixture_lock);
}
static void wait_registry(unsigned expected, int finalizing) {
    struct timespec until; clock_gettime(CLOCK_REALTIME,&until); until.tv_sec+=5;
    pthread_mutex_lock(&fixture_lock);
    for (;;) {
        pthread_mutex_lock(&g_resolver_lock); unsigned active=0;
        for (unsigned i=0;i<64;++i) active+=g_resolver_jobs[i]!=NULL;
        int ready=blocked && active>=expected && (!finalizing || g_resolver_finalizing);
        pthread_mutex_unlock(&g_resolver_lock);
        if (ready) break;
        assert(!pthread_cond_timedwait(&fixture_changed,&fixture_lock,&until));
    }
    pthread_mutex_unlock(&fixture_lock);
}
static void unblock_queue(void) {
    pthread_mutex_lock(&fixture_lock); unblocked=1; pthread_cond_broadcast(&fixture_changed); pthread_mutex_unlock(&fixture_lock);
}
static void *finalizer(void *unused) { (void)unused; dm_net_finalize(); return NULL; }
typedef struct { DNSServiceRef service; DNSServiceFlags flags; int error; uint32_t address; } response;
static void respond(void *context) {
    response *r = context;
    struct sockaddr_in address = {.sin_len=sizeof address,.sin_family=AF_INET,.sin_addr={r->address}};
    // An explicit late callback test holds its own job reference. Real Apple
    // deallocation cancels callbacks; this also checks the publication guard.
    r->service->callback(r->service, r->flags, 0, r->error, "fixture.invalid",
                          r->error ? NULL : (struct sockaddr *)&address, 60, r->service->context);
}
static void reply(DNSServiceRef service, DNSServiceFlags flags, int error, uint32_t address) {
    response r = {service, flags, error, htonl(address)};
    dispatch_sync_f(g_resolver_queue, &r, respond);
}
typedef struct { uint32_t id; const char *name; pthread_t thread; int success, error; uint32_t addresses[65]; size_t count; } query;
static void *lookup(void *context) {
    query *q = context;
    typedef struct { char *name, **aliases; int family, length; char **addresses; } hostent;
    dm_net_set_errno(37);
    hostent *out = dm_net_gethostbyname_cancel(q->name, q->id);
    q->success = out != NULL; q->error = dm_net_errno();
    if (out) {
        assert(out->family == 2 && out->length == 4 && !out->aliases[0] && !strcmp(out->name,q->name));
        while (out->addresses[q->count]) { assert(q->count < 64); memcpy(&q->addresses[q->count],out->addresses[q->count],4); ++q->count; }
    }
    return NULL;
}
static void launch(query *q, uint32_t id) { *q=(query){.id=id,.name="fixture.invalid"}; CHECK(!pthread_create(&q->thread,NULL,lookup,q)); }
static void join(query *q) { CHECK(!pthread_join(q->thread,NULL)); }
int main(void) {
    setenv("DM_NETWORK","lan",1);
    CHECK(net_resolver_queue() != NULL);
    dispatch_queue_set_specific(g_resolver_queue,&queue_key,&queue_key,NULL);
    uint32_t id=dm_net_request_cancel_handle(); CHECK(id && dm_net_request_cancel_handle()!=id);
    CHECK(dm_net_cancel(0)==0 && dm_net_errno()==22);
    CHECK(dm_net_cancel(id)==0); // No tombstone for a future query.
    query q; unsigned n=service_count; launch(&q,id); wait_services(n+1);
    reply(services[n],kDNSServiceFlagsAdd|kDNSServiceFlagsMoreComing,0,0x7f000002);
    reply(services[n],kDNSServiceFlagsAdd|kDNSServiceFlagsMoreComing,0,0x7f000002);
    reply(services[n],kDNSServiceFlagsAdd,0,0x7f000003); join(&q);
    CHECK(q.success && q.count==2 && q.addresses[0]==htonl(0x7f000002) && atomic_load(&services[n]->closed));
    // Reuse the same ID after successful completion; cancel only this generation.
    n=service_count; launch(&q,id); wait_services(n+1);
    net_resolver_job *late=services[n]->context; net_resolver_retain(late);
    CHECK(dm_net_cancel(id)==0); join(&q);
    CHECK(!q.success && q.error==125 && atomic_load(&services[n]->closed));
    reply(services[n],kDNSServiceFlagsAdd,0,0x7f000009); CHECK(late->count==0);
    net_resolver_release(late);
    n=service_count; launch(&q,id); wait_services(n+1);
    reply(services[n],kDNSServiceFlagsAdd,0,0x7f000004); join(&q); CHECK(q.success && q.count==1);
    // Queue an old generation's cancellation, then register a new query using
    // the same ID before that cancellation runs. It must not target the new one.
    query next; n=service_count; launch(&q,id); wait_services(n+1);
    dispatch_async_f(g_resolver_queue,NULL,block_queue); wait_registry(1,0);
    CHECK(!dm_net_cancel(id)); launch(&next,id); wait_registry(2,0); unblock_queue();
    wait_services(n+2); join(&q); CHECK(!q.success && q.error==125);
    reply(services[n+1],kDNSServiceFlagsAdd,0,0x7f000005); join(&next); CHECK(next.success);
    // A zero ID is a valid query, although Cancel(0) cannot cancel it.
    n=service_count; launch(&q,0); wait_services(n+1); CHECK(dm_net_cancel(0)==0);
    reply(services[n],0,kDNSServiceErr_NoSuchRecord,0); join(&q); CHECK(!q.success && q.error==37);
    const int negative[]={kDNSServiceErr_NoSuchRecord,kDNSServiceErr_NoSuchName,0};
    for (unsigned i=0;i<3;++i) {
        n=service_count; launch(&q,id); wait_services(n+1);
        net_resolver_job *empty=services[n]->context; net_resolver_retain(empty);
        reply(services[n],0,negative[i],0x7f000001); join(&q);
        CHECK(!q.success && q.error==37 && empty->count==0);
        reply(services[n],kDNSServiceFlagsAdd,0,0x7f000009); CHECK(empty->count==0);
        net_resolver_release(empty);
    }
    const int errors[]={kDNSServiceErr_NoMemory,kDNSServiceErr_Timeout,kDNSServiceErr_ServiceNotRunning,kDNSServiceErr_PolicyDenied};
    const int expected[]={12,110,100,13};
    for (unsigned i=0;i<4;++i) {
        n=service_count; launch(&q,id); wait_services(n+1);
        net_resolver_job *failed=services[n]->context; net_resolver_retain(failed);
        reply(services[n],0,errors[i],0); join(&q); CHECK(!q.success && q.error==expected[i]);
        reply(services[n],kDNSServiceFlagsAdd,0,0x7f000001); CHECK(failed->count==0);
        net_resolver_release(failed);
    }
    start_error=kDNSServiceErr_NoMemory; launch(&q,id); join(&q); CHECK(!q.success && q.error==12); start_error=0;
    queue_error=kDNSServiceErr_BadParam; launch(&q,id); join(&q); CHECK(!q.success && q.error==22); queue_error=0;
    // Finalize fences new native queries until all existing services are closed.
    n=service_count; launch(&q,id); wait_services(n+1); blocked=unblocked=0;
    dispatch_async_f(g_resolver_queue,NULL,block_queue); wait_registry(1,0);
    pthread_t finisher; CHECK(!pthread_create(&finisher,NULL,finalizer,NULL)); wait_registry(1,1);
    launch(&next,id); join(&next); CHECK(!next.success && next.error==11 && service_count==n+1);
    unblock_queue(); CHECK(!pthread_join(finisher,NULL)); join(&q); CHECK(!q.success && q.error==125);
    // All 64 slots can be live; a 65th operation fails without starting DNS.
    query many[64]; n=service_count;
    for (unsigned i=0;i<64;++i) launch(many+i,dm_net_request_cancel_handle());
    wait_services(n+64); launch(&q,id); join(&q); CHECK(!q.success && q.error==11 && service_count==n+64);
    dm_net_finalize();
    for (unsigned i=0;i<64;++i) { join(many+i); CHECK(!many[i].success && many[i].error==125); }
    CHECK(deallocations==service_count);
    for (unsigned i=0;i<64;++i) CHECK(!g_resolver_jobs[i]);
    // Numeric answers do not start DNS; offline mode remains unavailable.
    setenv("DM_NETWORK","auto",1); n=service_count;
    CHECK(dm_net_gethostbyname_cancel("127.0.0.1",0)!=NULL);
    CHECK(dm_net_gethostbyname_cancel(NULL,id)==NULL && dm_net_errno()==22);
    setenv("DM_NETWORK","offline",1);
    CHECK(dm_net_gethostbyname_cancel("fixture.invalid",id)==NULL && dm_net_errno()==100);
    CHECK(service_count==n && atomic_load(&waits)>0 && !atomic_load(&balances));
    atomic_store(&g_resolver_next_id,UINT64_C(0x100000000));
    CHECK(!dm_net_request_cancel_handle() && dm_net_errno()==11);
    for (unsigned i=0;i<service_count;++i) free(services[i]);
    printf("cancellable resolver: %u checks PASS\n",checks); return 0;
}
