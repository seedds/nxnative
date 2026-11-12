// Real UDP discovery/reply traffic through the production socket translator.
// The adapter fixture is confined to 127/8. No LAN is scanned or broadcast sent.
#include "../net_socket.h"
#include "../net_vpn.h"
#include <assert.h>
#include <errno.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr,"FAIL %d: %s (guest errno %d)\n",__LINE__,#c,dm_net_errno()); abort(); } } while (0)
void dm_guest_host_wait_begin(void) {}
void dm_guest_host_wait_end(void) {}
void dm_guest_sleep_ns(int64_t ns) { struct timespec t={ns/1000000000,ns%1000000000}; nanosleep(&t,NULL); }
static uint64_t ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000; }
static _Atomic unsigned injected_calls;
static _Atomic int injected_bad_target;
static int injected_error;
static unsigned injected_at;
static long first_unreachable(int fd,const void *bytes,size_t size,int flags,uint32_t ip,uint16_t port) {
    unsigned call=atomic_fetch_add(&injected_calls,1);
    if(call==injected_at) {
        atomic_store(&injected_bad_target,ip!=htonl(injected_at?0x7f000003:INADDR_LOOPBACK));
        errno=injected_error;return -1;
    }
    // Darwin has only 127.0.0.1 assigned: map the next logical neighbor to it.
    // Actual UDP delivery, original fd/source port and bytes remain real.
    struct sockaddr_in to={.sin_len=sizeof to,.sin_family=AF_INET,.sin_port=port,.sin_addr={htonl(INADDR_LOOPBACK)}};
    return sendto(fd,bytes,size,flags,(struct sockaddr *)&to,sizeof to);
}
typedef struct { uint8_t len,family; uint16_t port; uint32_t ip; uint8_t zero[8]; } address;
static address endpoint(uint32_t ip,unsigned port) { return (address){16,2,htons(port),htonl(ip),{0}}; }
static dm_net_test_interface adapter(uint32_t mask) { return (dm_net_test_interface){htonl(0x7f000002),htonl(mask),if_nametoindex("lo0"),IFF_UP|IFF_BROADCAST,1}; }
static int receiver(unsigned *port) {
    int fd=socket(AF_INET,SOCK_DGRAM,0); CHECK(fd>=0);
    struct sockaddr_in a={.sin_len=sizeof a,.sin_family=AF_INET,.sin_addr={htonl(INADDR_LOOPBACK)}};
    CHECK(!bind(fd,(struct sockaddr *)&a,sizeof a)); socklen_t n=sizeof a; CHECK(!getsockname(fd,(struct sockaddr *)&a,&n)); *port=ntohs(a.sin_port); return fd;
}
static ssize_t read_packet(int fd,void *bytes,size_t size,struct sockaddr_in *from) {
    struct pollfd p={fd,POLLIN,0}; if(poll(&p,1,1000)!=1)return -1;
    socklen_t n=sizeof *from; return recvfrom(fd,bytes,size,0,(struct sockaddr *)from,&n);
}
static void ranges(void) {
    dm_net_discovery_plan p; uint32_t ip;
    CHECK(!dm_net_discovery_subnet(&p,htonl(0xc0a80819),htonl(0xffffff00)) && p.count==253);
    unsigned n=0; while(dm_net_discovery_next(&p,&ip)) { CHECK(ntohl(ip)>0xc0a80800 && ntohl(ip)<0xc0a808ff && ntohl(ip)!=0xc0a80819); ++n; } CHECK(n==253);
    CHECK(!dm_net_discovery_subnet(&p,htonl(0xac101911),htonl(0xfffff000)) && p.count==4093 && dm_net_discovery_duration(&p)==1024);
    n=0; while(dm_net_discovery_next(&p,&ip)) { CHECK(ntohl(ip)>0xac101000 && ntohl(ip)<0xac101fff && ntohl(ip)!=0xac101911); ++n; } CHECK(n==4093);
    const uint32_t masks[]={0,0xffff0000,0xffff00ff,0xfffffffe,0xffffffff};
    for(unsigned i=0;i<sizeof masks/sizeof masks[0];++i) CHECK(dm_net_discovery_subnet(&p,htonl(0xc0a80819),htonl(masks[i]))<0);
    CHECK(dm_net_discovery_subnet(&p,htonl(0xc0a80800),htonl(0xffffff00))<0);
    CHECK(dm_net_discovery_subnet(&p,htonl(0xc0a808ff),htonl(0xffffff00))<0);
}
static void traffic(void) {
    dm_net_test_interface a=adapter(0xfffffffc); CHECK(a.index); dm_net_test_set_interface(&a);
    unsigned port; int native=receiver(&port),guest=dm_net_socket(2,2,0); CHECK(guest>=0);
    address own=endpoint(INADDR_LOOPBACK,0); CHECK(!dm_net_bind(guest,&own,sizeof own)); unsigned n=sizeof own; CHECK(!dm_net_name(guest,&own,&n,0));
    CHECK(!dm_net_fcntl(guest,4,0x800)); // nonblocking guest SendTo stays nonblocking
    address destination=endpoint(UINT32_MAX,port);
    const unsigned char query[]={0x80,0x34,0,0xff,'L','A','N',0,1,2,3};
    uint64_t start=ms(); CHECK(dm_net_send(guest,query,sizeof query,0,&destination,sizeof destination)==sizeof query); CHECK(ms()-start<100);
    unsigned char bytes[64]; struct sockaddr_in from;
    CHECK(read_packet(native,bytes,sizeof bytes,&from)==sizeof query && !memcmp(bytes,query,sizeof query));
    CHECK(from.sin_port==own.port && ntohl(from.sin_addr.s_addr)==INADDR_LOOPBACK);
    CHECK(sendto(native,"answer",6,0,(struct sockaddr *)&from,sizeof from)==6);
    dm_net_pollfd ready={guest,1,0}; CHECK(dm_net_poll(&ready,1,1000)==1);
    address peer; n=sizeof peer; CHECK(dm_net_recv(guest,bytes,sizeof bytes,0,&peer,&n)==6 && !memcmp(bytes,"answer",6));
    CHECK(peer.ip==htonl(INADDR_LOOPBACK) && peer.port==htons(port));
    // Directed subnet broadcast enters exactly the same unicast route.
    destination.ip=htonl(0x7f000003);
    // A distinct nonce tests this route independently: an identical outstanding
    // query may legitimately be coalesced with the first subnet sweep.
    unsigned char directed[sizeof query]; memcpy(directed,query,sizeof directed); directed[sizeof directed-1]^=1;
    CHECK(dm_net_send(guest,directed,sizeof directed,0,&destination,sizeof destination)==sizeof directed);
    CHECK(read_packet(native,bytes,sizeof bytes,&from)==sizeof directed && !memcmp(bytes,directed,sizeof directed));
    // A broken optional override cannot interfere with ordinary peer traffic.
    setenv("DM_NET_DISCOVERY_PEER","invalid",1); destination.ip=htonl(INADDR_LOOPBACK);
    CHECK(dm_net_send(guest,"ordinary",8,0,&destination,sizeof destination)==8);
    CHECK(read_packet(native,bytes,sizeof bytes,&from)==8 && !memcmp(bytes,"ordinary",8));
    destination.ip=htonl(UINT32_MAX); CHECK(dm_net_send(guest,query,sizeof query,0,&destination,sizeof destination)<0 && dm_net_errno()==22);
    unsetenv("DM_NET_DISCOVERY_PEER"); CHECK(!dm_net_close(guest)); close(native);
}
static void bounds_and_lifetime(void) {
    dm_net_test_interface a=adapter(0xfffff000); dm_net_test_set_interface(&a);
    unsigned port; int native=receiver(&port),guest=dm_net_socket(2,2,0); CHECK(guest>=0);
    address destination=endpoint(UINT32_MAX,port);
    uint64_t start=ms();
    CHECK(dm_net_send(guest,"nonce1",6,0,&destination,sizeof destination)==6);
    for(unsigned i=0;i<100;++i) CHECK(dm_net_send(guest,"nonce1",6,0,&destination,sizeof destination)==6);
    CHECK(ms()-start<100 && dm_net_test_discovery_pending()==1);
    usleep(20000); CHECK(dm_net_test_discovery_pending()==1); // the /20 sweep is paced, not one burst
    CHECK(dm_net_send(guest,"nonce2",6,0,&destination,sizeof destination)==6);
    CHECK(dm_net_send(guest,"nonce3",6,0,&destination,sizeof destination)<0 && dm_net_errno()==105);
    CHECK(dm_net_test_discovery_pending()==2);
    unsigned char bytes[16]; struct sockaddr_in from;
    CHECK(read_packet(native,bytes,sizeof bytes,&from)==6 && !memcmp(bytes,"nonce1",6));
    CHECK(read_packet(native,bytes,sizeof bytes,&from)==6 && !memcmp(bytes,"nonce2",6));
    CHECK(!dm_net_close(guest) && !dm_net_test_discovery_pending());
    int reused=dm_net_socket(2,2,0); CHECK(reused==guest); destination.ip=htonl(INADDR_LOOPBACK);
    CHECK(dm_net_send(reused,"reused",6,0,&destination,sizeof destination)==6);
    CHECK(read_packet(native,bytes,sizeof bytes,&from)==6 && !memcmp(bytes,"reused",6));
    CHECK(!dm_net_close(reused)); close(native);
    // The global queue has a real bound even across distinct guest sockets.
    native=receiver(&port); destination=endpoint(UINT32_MAX,port); int sockets[17];
    for(unsigned i=0;i<17;++i) {
        sockets[i]=dm_net_socket(2,2,0); CHECK(sockets[i]>=0);
        long result=dm_net_send(sockets[i],"bounded",7,0,&destination,sizeof destination);
        CHECK(i<16 ? result==7 : result<0 && dm_net_errno()==105);
    }
    CHECK(dm_net_test_discovery_pending()==16);
    for(unsigned i=0;i<17;++i) CHECK(!dm_net_close(sockets[i]));
    CHECK(!dm_net_test_discovery_pending()); close(native);
    dm_net_discovery_plan plan;
    a=adapter(0xffff0000); dm_net_test_set_interface(&a); CHECK(dm_net_discovery_prepare(htonl(UINT32_MAX),&plan)<0 && dm_net_errno()==95);
    const unsigned flags[]={IFF_UP,IFF_BROADCAST,IFF_UP|IFF_BROADCAST|IFF_POINTOPOINT,IFF_UP|IFF_BROADCAST|IFF_LOOPBACK};
    for(unsigned i=0;i<sizeof flags/sizeof flags[0];++i) { a=adapter(0xffffff00); a.flags=flags[i]; dm_net_test_set_interface(&a); CHECK(dm_net_discovery_prepare(htonl(UINT32_MAX),&plan)<0 && dm_net_errno()==99); }
    a=adapter(0xffffff00); a.index=0; dm_net_test_set_interface(&a); CHECK(dm_net_discovery_prepare(htonl(UINT32_MAX),&plan)<0 && dm_net_errno()==99);
    setenv("DM_NET_DISCOVERY_PEER","127.0.0.1",1); CHECK(!dm_net_discovery_prepare(htonl(UINT32_MAX),&plan) && !plan.automatic && plan.target==htonl(INADDR_LOOPBACK)); unsetenv("DM_NET_DISCOVERY_PEER");
    a=adapter(0xfffff000); dm_net_test_set_interface(&a); guest=dm_net_socket(2,2,0); destination=endpoint(UINT32_MAX,9);
    CHECK(dm_net_send(guest,"close",5,0,&destination,sizeof destination)==5); setenv("DM_NETWORK","offline",1); dm_net_finalize(); CHECK(!dm_net_test_discovery_pending());
}
static void unreachable_neighbor(void) {
    dm_net_test_interface a=adapter(0xfffffff8);dm_net_test_set_interface(&a);
    unsigned port;int native=receiver(&port);address destination=endpoint(UINT32_MAX,port);
    const int errors[]={EHOSTUNREACH,ENETUNREACH,EHOSTDOWN,EACCES,ENETDOWN};
    for(unsigned i=0;i<5;++i) {
        int guest=dm_net_socket(2,2,0);CHECK(guest>=0);CHECK(!dm_net_fcntl(guest,4,0x800));
        injected_error=errors[i];injected_at=0;atomic_store(&injected_bad_target,0);atomic_store(&injected_calls,0);dm_net_test_set_discovery_sender(first_unreachable);
        uint64_t start=ms();long result=dm_net_send(guest,"later neighbor",14,0,&destination,sizeof destination);
        if(i<3) {
            CHECK(result==14 && ms()-start<100 && atomic_load(&injected_calls)>=2);
            char bytes[32];struct sockaddr_in from;CHECK(read_packet(native,bytes,sizeof bytes,&from)==14 && !memcmp(bytes,"later neighbor",14));
        } else CHECK(result<0 && dm_net_errno()==(i==3?13:100) && atomic_load(&injected_calls)==1);
        CHECK(!atomic_load(&injected_bad_target));
        CHECK(!dm_net_close(guest));dm_net_test_set_discovery_sender(NULL);
        struct pollfd ready={native,POLLIN,0};char drain[32];while(poll(&ready,1,0)>0)recv(native,drain,sizeof drain,0);
    }
    // The first send succeeds; a later worker neighbor reports EHOSTDOWN.
    // The worker must skip it and still deliver to the following neighbor.
    int guest=dm_net_socket(2,2,0);CHECK(guest>=0);injected_error=EHOSTDOWN;injected_at=1;
    atomic_store(&injected_calls,0);atomic_store(&injected_bad_target,0);dm_net_test_set_discovery_sender(first_unreachable);
    CHECK(dm_net_send(guest,"worker neighbor",15,0,&destination,sizeof destination)==15);
    char bytes[32];struct sockaddr_in from;
    CHECK(read_packet(native,bytes,sizeof bytes,&from)==15&&!memcmp(bytes,"worker neighbor",15));
    CHECK(read_packet(native,bytes,sizeof bytes,&from)==15&&!memcmp(bytes,"worker neighbor",15));
    CHECK(atomic_load(&injected_calls)>=3&&!atomic_load(&injected_bad_target));CHECK(!dm_net_close(guest));dm_net_test_set_discovery_sender(NULL);injected_at=0;
    close(native);
}
static _Atomic unsigned hybrid_native,hybrid_unicast;
static unsigned hybrid_native_port,hybrid_phone_port;
static int hybrid_failure;
static long hybrid_sender(int fd,const void *bytes,size_t size,int flags,uint32_t ip,uint16_t port) {
    (void)port;unsigned destination;
    if(ntohl(ip)==UINT32_MAX) {
        atomic_fetch_add(&hybrid_native,1);
        if(hybrid_failure) { errno=hybrid_failure;return -1; }
        destination=hybrid_native_port;
    } else { atomic_fetch_add(&hybrid_unicast,1);destination=hybrid_phone_port; }
    struct sockaddr_in to={.sin_len=sizeof to,.sin_family=AF_INET,.sin_port=htons((uint16_t)destination),.sin_addr={htonl(INADDR_LOOPBACK)}};
    return sendto(fd,bytes,size,flags,(struct sockaddr *)&to,sizeof to);
}
static void hybrid_reset(void) { atomic_store(&hybrid_native,0);atomic_store(&hybrid_unicast,0);hybrid_failure=0; }
static void drain(int fd) { struct pollfd p={fd,POLLIN,0};char bytes[2048];while(poll(&p,1,0)>0)recv(fd,bytes,sizeof bytes,0); }
static void supplemental_queries(void) {
    dm_net_test_interface a=adapter(0xfffffffc);dm_net_test_set_interface(&a);dm_net_set_multicast_allowed(1);
    int native=receiver(&hybrid_native_port),phone=receiver(&hybrid_phone_port);dm_net_test_set_discovery_sender(hybrid_sender);
    unsigned char pia[0x12f]={0};pia[5]=1;pia[14]=1;
    const unsigned char ldn[12]={0,0x14,0x45,0x11};
    for(unsigned protocol=0;protocol<2;++protocol) {
        const void *query=protocol?(const void *)ldn:(const void *)pia;size_t size=protocol?sizeof ldn:sizeof pia;
        int guest=dm_net_socket(2,2,0);CHECK(guest>=0);address own=endpoint(INADDR_LOOPBACK,0);CHECK(!dm_net_bind(guest,&own,sizeof own));unsigned length=sizeof own;CHECK(!dm_net_name(guest,&own,&length,0));
        address destination=endpoint(UINT32_MAX,protocol?11452:30000);hybrid_reset();dm_net_set_errno(777);
        CHECK(dm_net_send(guest,query,size,0,&destination,sizeof destination)==(long)size&&dm_net_errno()==777);
        unsigned char bytes[1024];struct sockaddr_in from;
        CHECK(read_packet(native,bytes,sizeof bytes,&from)==(ssize_t)size&&!memcmp(bytes,query,size)&&from.sin_port==own.port);
        CHECK(read_packet(phone,bytes,sizeof bytes,&from)==(ssize_t)size&&!memcmp(bytes,query,size)&&from.sin_port==own.port);
        CHECK(atomic_load(&hybrid_native)==1&&atomic_load(&hybrid_unicast)==1&&dm_net_multicast_allowed()==1);
        CHECK(sendto(phone,"phone reply",11,0,(struct sockaddr *)&from,sizeof from)==11);
        dm_net_pollfd ready={guest,1,0};CHECK(dm_net_poll(&ready,1,1000)==1);CHECK(dm_net_recv(guest,bytes,sizeof bytes,0,NULL,NULL)==11&&!memcmp(bytes,"phone reply",11));
        CHECK(!dm_net_close(guest));
    }
    // Native errors remain errors and never trigger supplemental traffic.
    const int failures[]={EACCES,EHOSTUNREACH,ENETDOWN,EHOSTDOWN},expected[]={13,113,100,112};
    address destination=endpoint(UINT32_MAX,30000);
    for(unsigned i=0;i<4;++i) {
        int guest=dm_net_socket(2,2,0);CHECK(guest>=0);hybrid_reset();hybrid_failure=failures[i];
        CHECK(dm_net_send(guest,pia,sizeof pia,0,&destination,sizeof destination)<0&&dm_net_errno()==expected[i]);
        CHECK(atomic_load(&hybrid_native)==1&&!atomic_load(&hybrid_unicast)&&!dm_net_test_discovery_pending());CHECK(!dm_net_close(guest));
    }
    // Unknown game data, replies and ports retain the sole native send.
    for(unsigned i=0;i<4;++i) {
        int guest=dm_net_socket(2,2,0);CHECK(guest>=0);hybrid_reset();unsigned char packet[0x12f];memcpy(packet,pia,sizeof packet);
        size_t packet_size=sizeof packet;
        if(i==0) { packet[0]=1;packet[14]=17;packet_size=0x3f; } // valid host response, not a query
        if(i==1)packet[5]=2;if(i==3) { memcpy(packet,"gameplay",8);packet_size=8; }
        destination=endpoint(UINT32_MAX,i==2?30001:30000);
        CHECK(dm_net_send(guest,packet,packet_size,0,&destination,sizeof destination)==(long)packet_size);
        CHECK(atomic_load(&hybrid_native)==1&&!atomic_load(&hybrid_unicast)&&!dm_net_test_discovery_pending());CHECK(!dm_net_close(guest));
    }
    drain(native);drain(phone);
    int guest=dm_net_socket(2,2,0);CHECK(guest>=0);destination=endpoint(INADDR_LOOPBACK,30000);hybrid_reset();
    CHECK(dm_net_send(guest,pia,sizeof pia,0,&destination,sizeof destination)==sizeof pia);CHECK(!atomic_load(&hybrid_native)&&atomic_load(&hybrid_unicast)==1&&!dm_net_test_discovery_pending());CHECK(!dm_net_close(guest));
    // A user-selected peer stays a single unicast, with no additional sweep.
    guest=dm_net_socket(2,2,0);CHECK(guest>=0);setenv("DM_NET_DISCOVERY_PEER","127.0.0.1",1);destination=endpoint(UINT32_MAX,30000);hybrid_reset();
    CHECK(dm_net_send(guest,pia,sizeof pia,0,&destination,sizeof destination)==sizeof pia);CHECK(!atomic_load(&hybrid_native)&&atomic_load(&hybrid_unicast)==1&&!dm_net_test_discovery_pending());CHECK(!dm_net_close(guest));unsetenv("DM_NET_DISCOVERY_PEER");
    // A /16 cannot be swept, but its accepted native broadcast still succeeds.
    a=adapter(0xffff0000);dm_net_test_set_interface(&a);guest=dm_net_socket(2,2,0);CHECK(guest>=0);hybrid_reset();dm_net_set_errno(777);
    CHECK(dm_net_send(guest,pia,sizeof pia,0,&destination,sizeof destination)==sizeof pia&&dm_net_errno()==777);CHECK(atomic_load(&hybrid_native)==1&&!atomic_load(&hybrid_unicast));CHECK(!dm_net_close(guest));
    // Cap0 remains unicast-only, preserving the current phone path.
    a=adapter(0xfffffffc);dm_net_test_set_interface(&a);dm_net_set_multicast_allowed(0);guest=dm_net_socket(2,2,0);CHECK(guest>=0);hybrid_reset();
    CHECK(dm_net_send(guest,pia,sizeof pia,0,&destination,sizeof destination)==sizeof pia);CHECK(!atomic_load(&hybrid_native)&&atomic_load(&hybrid_unicast)==1);CHECK(!dm_net_close(guest));
    // Extra queue pressure must not change the accepted native result.
    a=adapter(0xfffff000);dm_net_test_set_interface(&a);dm_net_set_multicast_allowed(1);guest=dm_net_socket(2,2,0);CHECK(guest>=0);hybrid_reset();
    pia[14]=1;for(unsigned repeat=0;repeat<3;++repeat) { dm_net_set_errno(777);CHECK(dm_net_send(guest,pia,sizeof pia,0,&destination,sizeof destination)==sizeof pia&&dm_net_errno()==777); }
    CHECK(atomic_load(&hybrid_native)==3&&dm_net_test_discovery_pending()==1); // native retries stay native; extra sweep coalesces
    for(unsigned i=2;i<=3;++i) { pia[14]=(unsigned char)i;dm_net_set_errno(777);CHECK(dm_net_send(guest,pia,sizeof pia,0,&destination,sizeof destination)==sizeof pia&&dm_net_errno()==777); }
    CHECK(atomic_load(&hybrid_native)==5&&dm_net_test_discovery_pending()==2);CHECK(!dm_net_close(guest)&&!dm_net_test_discovery_pending());
    unsigned sent=atomic_load(&hybrid_unicast);usleep(20000);CHECK(atomic_load(&hybrid_unicast)==sent);drain(phone);
    int reused=dm_net_socket(2,2,0);CHECK(reused==guest);destination=endpoint(INADDR_LOOPBACK,30000);CHECK(dm_net_send(reused,"ordinary",8,0,&destination,sizeof destination)==8);CHECK(!dm_net_test_discovery_pending());CHECK(!dm_net_close(reused));
    dm_net_test_set_discovery_sender(NULL);dm_net_set_multicast_allowed(0);close(native);close(phone);
}
// VPN play's adapter choice and mask (net_vpn.h), then the backend over a tunnel through the adapter fixture
static void vpn(void) {
    const dm_net_vpn_candidate wifi={htonl(0xc0a80105),htonl(0xffffff00),1,IFF_UP|IFF_BROADCAST,IFRTYPE_FUNCTIONAL_WIFI_INFRA},
        cell={htonl(0x0a400001),htonl(0xffffffff),2,IFF_UP|IFF_POINTOPOINT,IFRTYPE_FUNCTIONAL_CELLULAR},
        lo={htonl(INADDR_LOOPBACK),htonl(0xff000000),3,IFF_UP|IFF_LOOPBACK,IFRTYPE_FUNCTIONAL_LOOPBACK},
        tail={htonl(0x64650203),htonl(0xffffffff),4,IFF_UP|IFF_POINTOPOINT,IFRTYPE_FUNCTIONAL_UNKNOWN},   // Tailscale: 100.101.2.3/32
        zt={htonl(0x0a931105),htonl(0xffffff00),5,IFF_UP|IFF_POINTOPOINT,IFRTYPE_FUNCTIONAL_UNKNOWN},     // ZeroTier: 10.147.17.5/24
        down={htonl(0x64650204),htonl(0xffffffff),6,IFF_POINTOPOINT,IFRTYPE_FUNCTIONAL_UNKNOWN},
        linklocal={htonl(0xa9fe0102),htonl(0xffff0000),7,IFF_UP,IFRTYPE_FUNCTIONAL_UNKNOWN};
    const dm_net_vpn_candidate one[]={wifi,cell,lo,down,tail},two[]={wifi,tail,zt},none[]={wifi,cell,lo,down,linklocal};
    CHECK(dm_net_vpn_pick(one,5,0)==4);
    CHECK(dm_net_vpn_pick(two,3,0)==DM_NET_VPN_SEVERAL);                    // two tunnels and nothing to tell them apart: never a guess
    CHECK(dm_net_vpn_pick(two,3,zt.address)==2 && dm_net_vpn_pick(two,3,tail.address)==1);   // the route to the host's address settles it
    CHECK(dm_net_vpn_pick(two,3,wifi.address)==DM_NET_VPN_NONE);            // the host is reached over Wi-Fi: not through a VPN
    CHECK(dm_net_vpn_pick(none,5,0)==DM_NET_VPN_NONE);
    CHECK(dm_net_vpn_mask(tail.address,tail.mask)==htonl(0xffc00000) && dm_net_vpn_mask(tail.address,0)==htonl(0xffc00000));
    CHECK(dm_net_vpn_mask(htonl(0x0a000005),htonl(0xfffffffe))==htonl(0xff000000));
    CHECK(dm_net_vpn_mask(htonl(0xac140001),htonl(0xffffffff))==htonl(0xfff00000));
    CHECK(dm_net_vpn_mask(htonl(0xc0a80a02),htonl(0xffffffff))==htonl(0xffff0000));
    CHECK(dm_net_vpn_mask(zt.address,zt.mask)==zt.mask);                    // a mask that names a network is kept
    CHECK(dm_net_vpn_mask(htonl(0x08080808),htonl(0xffffffff))==htonl(0xffffffff));   // no private block to name
    // A Tailscale tunnel: the game is told the /10, its broadcast address is still a broadcast, and a search needs the host's address. Even where native
    // broadcast works (the Mac), a tunnel does not carry it.
    setenv("DM_NETWORK","ldn",1); setenv("DM_NET_VPN","1",1); dm_net_set_multicast_allowed(1);
    dm_net_test_interface t={tail.address,tail.mask,if_nametoindex("lo0"),IFF_UP|IFF_POINTOPOINT,0}; dm_net_test_set_interface(&t);
    uint32_t own,mask; CHECK(!dm_net_ipconfig(&own,&mask,NULL,NULL,NULL) && own==tail.address && mask==htonl(0xffc00000));
    dm_net_discovery_plan plan,extra; CHECK(dm_net_discovery_prepare(htonl(UINT32_MAX),&plan)<0 && dm_net_errno()==95);
    CHECK(!dm_net_discovery_supplement(htonl(UINT32_MAX),&extra));
    int guest=dm_net_socket(2,2,0); CHECK(guest>=0); address directed=endpoint(0x647fffff,30000);   // 100.127.255.255, from the /10
    CHECK(dm_net_send(guest,"query",5,0,&directed,sizeof directed)<0 && dm_net_errno()==95); CHECK(!dm_net_close(guest));
    setenv("DM_NET_DISCOVERY_PEER","100.101.9.9",1);
    CHECK(!dm_net_discovery_prepare(htonl(UINT32_MAX),&plan) && !plan.automatic && plan.target==htonl(0x64650909)); unsetenv("DM_NET_DISCOVERY_PEER");
    // A ZeroTier tunnel: its /24 is searched, over the tunnel
    dm_net_test_interface z={zt.address,zt.mask,if_nametoindex("lo0"),IFF_UP|IFF_POINTOPOINT,0}; dm_net_test_set_interface(&z);
    CHECK(!dm_net_discovery_prepare(htonl(UINT32_MAX),&plan) && plan.automatic && plan.count==253);
    // Off a VPN the same tunnel is no local network
    unsetenv("DM_NET_VPN"); dm_net_set_multicast_allowed(0); CHECK(dm_net_discovery_prepare(htonl(UINT32_MAX),&plan)<0 && dm_net_errno()==99);
    setenv("DM_NETWORK","auto",1); dm_net_test_set_interface(NULL);
}
// In a room a broadcast goes to each other member (dm_net_set_session), which a recording sender sees without sending anything
static _Atomic unsigned recorded;
static uint32_t recorded_to[8], refused;
static uint16_t recorded_port;
static long record(int fd,const void *bytes,size_t size,int flags,uint32_t ip,uint16_t port) {
    (void)fd;(void)bytes;(void)flags; unsigned i=atomic_fetch_add(&recorded,1); if(i<8)recorded_to[i]=ip; recorded_port=port;
    if(ip==refused){errno=EHOSTUNREACH;return -1;}
    return (long)size;
}
static void room_broadcasts(void) {
    dm_net_test_interface a=adapter(0xffffff00); dm_net_test_set_interface(&a); dm_net_test_set_discovery_sender(record);
    int guest=dm_net_socket(2,2,0); CHECK(guest>=0);
    const uint32_t room[]={htonl(0x0a000002),htonl(0x0a000003),htonl(0x0a000004)}; dm_net_set_session(1,room,3);
    address everyone=endpoint(UINT32_MAX,49152),subnet=endpoint(0x7f0000ff,49152),one=endpoint(0x0a000002,49152);
    atomic_store(&recorded,0); CHECK(dm_net_send(guest,"hello",5,0,&everyone,sizeof everyone)==5);
    CHECK(atomic_load(&recorded)==3 && !memcmp(recorded_to,room,sizeof room) && recorded_port==htons(49152));
    atomic_store(&recorded,0); CHECK(dm_net_send(guest,"hello",5,0,&subnet,sizeof subnet)==5 && atomic_load(&recorded)==3);
    // One unreachable member does not fail the broadcast; when none is reachable it fails with that error
    refused=room[1]; atomic_store(&recorded,0); CHECK(dm_net_send(guest,"hello",5,0,&everyone,sizeof everyone)==5 && atomic_load(&recorded)==3);
    dm_net_set_session(1,room+1,1); atomic_store(&recorded,0);
    CHECK(dm_net_send(guest,"hello",5,0,&everyone,sizeof everyone)<0 && dm_net_errno()==113 && atomic_load(&recorded)==1); refused=0;
    // Ordinary unicast is never fanned out
    dm_net_set_session(1,room,3); atomic_store(&recorded,0);
    CHECK(dm_net_send(guest,"hello",5,0,&one,sizeof one)==5 && atomic_load(&recorded)==1 && recorded_to[0]==room[0]);
    // Alone in the room (a host before anyone joins), a broadcast reaches nobody and is no error, even with nothing to search over a VPN
    dm_net_set_session(1,NULL,0); setenv("DM_NETWORK","ldn",1); setenv("DM_NET_VPN","1",1); atomic_store(&recorded,0);
    CHECK(dm_net_send(guest,"hello",5,0,&everyone,sizeof everyone)==5 && !atomic_load(&recorded));
    unsetenv("DM_NET_VPN"); setenv("DM_NETWORK","auto",1); dm_net_set_session(1,room,3);
    // Where native broadcast works and no VPN is in the way it is used; over a VPN the room is used even there
    dm_net_set_multicast_allowed(1); atomic_store(&recorded,0);
    CHECK(dm_net_send(guest,"hello",5,0,&everyone,sizeof everyone)==5 && atomic_load(&recorded)==1 && recorded_to[0]==htonl(UINT32_MAX));
    setenv("DM_NETWORK","ldn",1); setenv("DM_NET_VPN","1",1); atomic_store(&recorded,0);
    CHECK(dm_net_send(guest,"hello",5,0,&everyone,sizeof everyone)==5 && atomic_load(&recorded)==3);
    unsetenv("DM_NET_VPN"); setenv("DM_NETWORK","auto",1); dm_net_set_multicast_allowed(0);
    // Out of the room, discovery is as before: here the host's address
    dm_net_set_session(0,NULL,0); setenv("DM_NET_DISCOVERY_PEER","127.0.0.1",1); atomic_store(&recorded,0);
    CHECK(dm_net_send(guest,"hello",5,0,&everyone,sizeof everyone)==5 && atomic_load(&recorded)==1 && recorded_to[0]==htonl(INADDR_LOOPBACK));
    unsetenv("DM_NET_DISCOVERY_PEER"); CHECK(!dm_net_close(guest)); dm_net_test_set_discovery_sender(NULL); dm_net_test_set_interface(NULL);
}
int main(void) {
    setenv("DM_NETWORK","auto",1); unsetenv("DM_NET_DISCOVERY_PEER"); unsetenv("DM_NET_VPN"); dm_net_set_multicast_allowed(0);
    ranges(); traffic(); unreachable_neighbor(); supplemental_queries(); bounds_and_lifetime(); dm_net_test_set_interface(NULL);
    vpn(); room_broadcasts();
    printf("automatic discovery: %u checks passed (real UDP reply, ranges, bounded queue, close/reuse, VPN adapters, room broadcasts).\n",checks); return 0;
}
