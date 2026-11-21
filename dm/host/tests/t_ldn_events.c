// Test the production LDN event/lifetime bodies. Only the AArch64 registration
// stubs are replaced with direct C calls (their ABI is covered by t_hle_wait).
// No socket is opened; the kernel-event binding fixture models close/reuse.
#include "../hle.h"
#undef HLE_FORCE
#undef HLE_FORCE_WAIT
#define HLE_FORCE(name, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#define HLE_FORCE_WAIT HLE_FORCE
#include "../hle_ldn.c"
#include <assert.h>
#include <stdatomic.h>

typedef struct { void *address; uint64_t id; unsigned signals; int active; } test_event;
static test_event events[80];
static unsigned count;
static __thread int waits;
static _Atomic unsigned wait_entries;
void dm_guest_host_wait_begin(void) { ++waits; atomic_fetch_add(&wait_entries,1); }
void dm_guest_host_wait_end(void) { --waits; assert(waits >= 0); }
void hle_bind_system_event(void *address, int autoclear) {
    assert(autoclear == 1);
    for (unsigned i = 0; i < count; ++i) if (events[i].address == address && events[i].active) return;
    assert(count < 80); events[count] = (test_event){address, count + 1, 0, 1}; ++count;
}
uint64_t hle_system_event_binding(void *address) { for (unsigned i = count; i-- > 0;) if (events[i].address == address && events[i].active) return events[i].id; return 0; }
int hle_signal_system_event_binding(uint64_t id) { for (unsigned i = 0; i < count; ++i) if (events[i].id == id && events[i].active) { ++events[i].signals; return 1; } return 0; }
int dm_net_ipconfig(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e) { (void)a;(void)b;(void)c;(void)d;(void)e; return -1; }
int dm_net_discovery_target(uint32_t broadcast, uint32_t *target) { *target=broadcast; return 0; }
int dm_net_discovery_prepare(uint32_t destination, dm_net_discovery_plan *plan) { *plan=(dm_net_discovery_plan){.target=destination,.count=1}; return 0; }
int dm_net_discovery_supplement(uint32_t destination,dm_net_discovery_plan *plan) { (void)destination;memset(plan,0,sizeof *plan);return 0; }
long dm_net_discovery_send_native(int fd,const void *bytes,size_t size,int flags,uint32_t address,uint16_t port) {
    struct sockaddr_in to={.sin_len=sizeof to,.sin_family=AF_INET,.sin_addr={address},.sin_port=port};return sendto(fd,bytes,size,flags,(struct sockaddr *)&to,sizeof to);
}
int dm_net_errno(void) { return 13; }
int dm_net_multicast_allowed(void) { return 0; }
dm_net_mode_t dm_net_mode(void) { return DM_NET_LDN; }
void dm_net_set_session(int room, const uint32_t *members, unsigned count) { (void)room; (void)members; (void)count; }
void dm_net_log_adapters(void) {}
static void *read_during_finalize(void *argument) {
    int which=(int)(uintptr_t)argument; Result r;
    if(which) { dm_ldn_security_parameter out; r=h_ldn_Security(&out); }
    else { dm_ldn_network_config out; r=h_ldn_Config(&out); }
    assert(r!=0 && waits==0);return NULL;
}
static void getter_lifetime(int which) {
    // Hold Finalize's writer lock and the native context mutex. A getter must
    // request the lifetime reader lock before it can touch the native mutex.
    // Then remove the context exactly as Finalize does; the getter resumes
    // with an unavailable result, without dereferencing freed memory.
    dm_ldn_context *c=calloc(1,sizeof *c);assert(c);pthread_mutex_init(&c->lock,NULL);c->state=LDN_CREATED;
    pthread_rwlock_wrlock(&g_ldn_lock);pthread_mutex_lock(&c->lock);g_ldn=c;atomic_store(&wait_entries,0);
    pthread_t thread;assert(!pthread_create(&thread,NULL,read_during_finalize,(void *)(uintptr_t)which));
    for(int i=0;i<400 && !atomic_load(&wait_entries);++i)usleep(5000);
    assert(atomic_load(&wait_entries)>0);g_ldn=NULL;pthread_mutex_unlock(&c->lock);pthread_mutex_destroy(&c->lock);free(c);
    pthread_rwlock_unlock(&g_ldn_lock);assert(!pthread_join(thread,NULL));
}

int main(void) {
    uint8_t storage[3][128];
    h_ldn_Attach(storage[0]); h_ldn_Attach(storage[1]); h_ldn_Attach(storage[0]);
    assert(count == 2); ldn_event(NULL); assert(events[0].signals == 1 && events[1].signals == 1);
    events[0].active = 0; h_ldn_Attach(storage[0]); assert(count == 3);
    ldn_event(NULL); assert(events[0].signals == 1 && events[1].signals == 2 && events[2].signals == 1);
    // An event attached after Finalize receives a fresh binding lifetime;
    // previously attached still-open kernel objects are no longer producers.
    h_ldn_Fini(); assert(!waits); ldn_event(NULL); assert(events[1].signals == 2 && events[2].signals == 1);
    h_ldn_Attach(storage[2]); ldn_event(NULL); assert(events[3].signals == 1 && events[1].signals == 2);
    getter_lifetime(0);getter_lifetime(1);
    setenv("DM_NETWORK", "offline", 1); assert(h_ldn_Init() == LDN_ERROR(22)); assert(h_ldn_State() == 0 && waits == 0);
    setenv("DM_NETWORK", "auto", 1); assert(ldn_enabled());
    puts("LDN event close/reuse, observers, concurrent getter/Finalize and offline state passed."); return 0;
}
