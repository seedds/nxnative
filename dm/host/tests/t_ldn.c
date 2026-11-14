// Original protocol and service tests. The peer fixture uses loopback only;
// no broadcast, installed game, Switch, or external network is contacted.
#include "../hle_ldn.h"
#include <arpa/inet.h>
#ifdef DM_LDN_NET_TEST
#include "../net_socket.h"
void dm_guest_host_wait_begin(void) {}
void dm_guest_host_wait_end(void) {}
typedef struct { uint8_t len, family; uint16_t port; uint32_t ip; uint8_t reserved[8]; } guest_address;
static guest_address guest_addr(uint32_t ip, unsigned port) { return (guest_address){16,2,htons(port),ip,{0}}; }
#endif
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
static void pause_ms(unsigned ms) { usleep(ms * 1000); }
static dm_ldn_network_config config(void) { dm_ldn_network_config n = {0}; n.intent.local_id = 0x12345678; n.intent.scene = 17; n.max_nodes = 8; n.version = 5; return n; }
static dm_ldn_user user(const char *name) { dm_ldn_user u = {0}; snprintf(u.name, sizeof u.name, "%s", name); return u; }
static dm_ldn_security_config security(void) { dm_ldn_security_config s = {.mode = 1, .size = 16}; memset(s.passphrase, 42, 16); return s; }
static dm_ldn_network example(void) {
    dm_ldn_network n = {0}; n.id.intent = config().intent; memset(n.id.session, 7, 16); n.common.ssid.len = 3; memcpy(n.common.ssid.text, "abc", 4); n.common.type = 2; n.common.channel = 6; n.max_nodes = 8; n.node_count = 1;
    n.nodes[0].ip = 0x7f000001; n.nodes[0].mac[0] = 2; n.nodes[0].mac[5] = 1; n.nodes[0].connected = 1; n.nodes[0].version = 5; strcpy(n.nodes[0].name, "host"); return n;
}
static unsigned free_port(int type) { int fd = socket(AF_INET, type, 0); CHECK(fd >= 0); struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); CHECK(!bind(fd, (struct sockaddr *)&a, sizeof a)); socklen_t len = sizeof a; CHECK(!getsockname(fd, (struct sockaddr *)&a, &len)); unsigned p = ntohs(a.sin_port); close(fd); return p; }
static dm_ldn_transport transport(unsigned port, unsigned host, int identity) { dm_ldn_transport t = {.address=htonl(INADDR_LOOPBACK), .mask=htonl(0xff000000), .discovery=htonl(INADDR_LOOPBACK), .bind_address=htonl(INADDR_LOOPBACK), .port=port, .discovery_port=host, .scan_ms=150, .connect_ms=1000}; t.mac[0] = 2; t.mac[5] = identity; return t; }
static int wait_nodes(dm_ldn_context *c, int count, dm_ldn_network *out) { for (int i = 0; i < 200; ++i) { if (!dm_ldn_get_network(c, out, NULL, 0) && out->node_count == count) return 1; pause_ms(5); } return 0; }
static void protocol(void) {
    uint8_t packet[2048], changed[2048]; dm_ldn_network n = example(), out; unsigned type;
    CHECK(dm_ldn_network_valid(&n)); CHECK(dm_ldn_packet_encode(packet, sizeof packet, 0, NULL, 0, 0) == 12);
    CHECK(!memcmp(packet, "\0\x14\x45\x11\0\0\0\0\0\0\0\0", 12)); CHECK(dm_ldn_packet_decode(packet, 12, &type, NULL, 0) == 0 && type == 0);
    for (unsigned p = 1; p <= 3; ++p) for (int compressed = 0; compressed <= 1; ++compressed) {
        const void *data = p == 2 ? (void *)&n.nodes[0] : (void *)&n; unsigned size = p == 2 ? 64 : 1152;
        int bytes = dm_ldn_packet_encode(packet, sizeof packet, p, data, size, compressed); CHECK(bytes > 12); CHECK(dm_ldn_packet_size(packet) == bytes);
        CHECK(dm_ldn_packet_decode(packet, bytes, &type, &out, sizeof out) == (int)size && type == p); CHECK(!memcmp(&out, data, size));
        for (int i = 0; i < bytes; ++i) CHECK(dm_ldn_packet_decode(packet, i, &type, &out, sizeof out) < 0);
        memcpy(changed, packet, bytes); changed[0] ^= 1; CHECK(dm_ldn_packet_decode(changed, bytes, &type, &out, sizeof out) < 0);
        memcpy(changed, packet, bytes); changed[4] = 4; CHECK(dm_ldn_packet_decode(changed, bytes, &type, &out, sizeof out) < 0);
        memcpy(changed, packet, bytes); changed[5] = 2; CHECK(dm_ldn_packet_decode(changed, bytes, &type, &out, sizeof out) < 0);
        memcpy(changed, packet, bytes); changed[10] = 1; CHECK(dm_ldn_packet_decode(changed, bytes, &type, &out, sizeof out) < 0);
        CHECK(dm_ldn_packet_decode(packet, bytes, &type, &out, size - 1) < 0);
    }
    // Exactly one zero run of 64 bytes is a valid compressed NodeInfo payload.
    memset(packet, 0, sizeof packet); packet[1]=0x14; packet[2]=0x45; packet[3]=0x11; packet[4]=2; packet[5]=1; packet[6]=2; packet[8]=64; packet[13]=63;
    CHECK(dm_ldn_packet_decode(packet, 14, &type, &out, sizeof out) == 64);
    dm_ldn_node zeros = {0}; CHECK(dm_ldn_packet_encode(changed, sizeof changed, 2, &zeros, sizeof zeros, 1) == 14); CHECK(!memcmp(changed,packet,14));
    packet[13] = 64; CHECK(dm_ldn_packet_decode(packet, 14, &type, &out, sizeof out) < 0); packet[13] = 62; CHECK(dm_ldn_packet_decode(packet, 14, &type, &out, sizeof out) < 0);
    packet[6] = 1; CHECK(dm_ldn_packet_decode(packet, 13, &type, &out, sizeof out) < 0);
    CHECK(dm_ldn_packet_encode(packet, sizeof packet, 2, &n, 1152, 0) < 0); CHECK(dm_ldn_packet_encode(packet, 12, 1, &n, sizeof n, 0) < 0);
    dm_ldn_filter f = {0}; CHECK(dm_ldn_filter_match(&n, &f, 0)); CHECK(!dm_ldn_filter_match(&n, &f, 1)); f.flags = 1; CHECK(!dm_ldn_filter_match(&n, &f, 0)); f.id.intent.local_id = n.id.intent.local_id; CHECK(dm_ldn_filter_match(&n, &f, 0));
    f.flags = 2; CHECK(!dm_ldn_filter_match(&n, &f, 0)); memcpy(f.id.session, n.id.session, 16); CHECK(dm_ldn_filter_match(&n, &f, 0));
    f.flags = 16; f.ssid = n.common.ssid; CHECK(dm_ldn_filter_match(&n, &f, 0)); f.ssid.text[0] = 'z'; CHECK(!dm_ldn_filter_match(&n, &f, 0));
    f.flags = 4; f.type = 3; f.ssid.len = 255; CHECK(dm_ldn_filter_match(&n,&f,0)); f.type = 1; CHECK(!dm_ldn_filter_match(&n,&f,0));
    f.flags = 64; CHECK(!dm_ldn_filter_match(&n, &f, 0)); n.node_count = 2; CHECK(!dm_ldn_network_valid(&n)); n = example(); n.advertise_size = 385; CHECK(!dm_ldn_network_valid(&n));
    n = example(); n.id.intent.local_id = -1; f = (dm_ldn_filter){.flags=1}; f.id.intent.local_id = -1; CHECK(dm_ldn_filter_match(&n,&f,0));
    // ldn_mitm deliberately preserves the caller's -1 communication ID. The
    // Switch module compares this exact wire value, rather than NACP identity.
    int encoded=dm_ldn_packet_encode(packet,sizeof packet,1,&n,sizeof n,1); CHECK(encoded>0); CHECK(dm_ldn_packet_decode(packet,encoded,&type,&out,sizeof out)==sizeof out && out.id.intent.local_id==-1);
    // Malformed headers/runs and arbitrary bytes remain bounded under ASan.
    uint32_t rng=0x12345678; for (int i=0;i<12000;++i) { memcpy(changed,packet,encoded); rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; changed[rng%(unsigned)encoded]^=(uint8_t)(rng>>16); int result=dm_ldn_packet_decode(changed,encoded,&type,&out,sizeof out); CHECK(result<0 || result==sizeof out); }
}
static int raw_connect(unsigned port) { int fd = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port); CHECK(fd >= 0 && !connect(fd, (struct sockaddr *)&a, sizeof a)); return fd; }
static int raw_read_frame(int fd, dm_ldn_network *out) {
    uint8_t packet[2048]; size_t have = 0; int total = 12;
    while (have < (size_t)total) { struct pollfd p = {fd, POLLIN, 0}; if (poll(&p, 1, 2000) <= 0) return -1; ssize_t n = recv(fd, packet + have, total - have, 0); if (n <= 0) return -1; have += n; if (have >= 12) { total = dm_ldn_packet_size(packet); if (total < 0) return -1; } }
    unsigned type; return dm_ldn_packet_decode(packet, have, &type, out, sizeof *out) == sizeof *out && type == 3 ? 0 : -1;
}
static void local(void) {
    unsigned port = free_port(SOCK_STREAM); dm_ldn_transport t = transport(port, port, 1); dm_ldn_context *host = dm_ldn_start(&t, NULL, NULL); CHECK(host);
    CHECK(dm_ldn_state(host) == 1); CHECK(dm_ldn_open(host, 1) == 0); CHECK(dm_ldn_open(host, 0) != 0);
    dm_ldn_network_config cfg = config(); dm_ldn_security_config sec = security(); dm_ldn_user u = user("host");
    CHECK(!dm_ldn_advertise(host, "initial", 7)); CHECK(!dm_ldn_create(host, &cfg, &sec, &u, NULL, 0, NULL)); CHECK(dm_ldn_state(host) == 3);
    dm_ldn_network n; dm_ldn_update updates[8]; CHECK(!dm_ldn_get_network(host, &n, updates, 8)); CHECK(updates[0].change == 1); CHECK(n.advertise_size == 7 && !memcmp(n.advertise, "initial", 7));
    CHECK(!dm_ldn_get_network(host, &n, updates, 8)); CHECK(updates[0].change == 0);
    // Split every byte of a compressed Connect frame across writes. A complete
    // Sync follows only once the complete, valid peer announcement is received.
    int fd = raw_connect(port); dm_ldn_node node = n.nodes[0]; strcpy(node.name, "raw peer"); node.mac[5] = 9;
    uint8_t packet[2048]; int bytes = dm_ldn_packet_encode(packet, sizeof packet, 2, &node, sizeof node, 1);
    for (int i = 0; i < bytes; ++i) CHECK(send(fd, packet+i, 1, 0) == 1);
    CHECK(!raw_read_frame(fd, &n)); CHECK(n.node_count == 2 && n.nodes[1].id == 1); CHECK(!dm_ldn_get_network(host, &n, updates, 8)); CHECK(updates[1].change == 1);
    CHECK(!dm_ldn_advertise(host, "actual bytes", 12)); CHECK(!raw_read_frame(fd, &n)); CHECK(n.advertise_size == 12 && !memcmp(n.advertise, "actual bytes", 12));
    close(fd); CHECK(wait_nodes(host, 1, &n)); CHECK(!dm_ldn_get_network(host, &n, updates, 8)); CHECK(updates[1].change == 2);
    // Invalid TCP header / truncated connection must never publish a node.
    fd = raw_connect(port); memset(packet, 0xff, 12); CHECK(send(fd, packet, 12, 0) == 12); struct pollfd p = {fd, POLLIN, 0}; CHECK(poll(&p, 1, 2000) > 0); CHECK(recv(fd, packet, 1, 0) <= 0); close(fd); CHECK(wait_nodes(host, 1, &n));
    fd = raw_connect(port); bytes = dm_ldn_packet_encode(packet, sizeof packet, 2, &node, sizeof node, 1); CHECK(send(fd, packet, bytes-1, 0) == bytes-1); close(fd); pause_ms(20); CHECK(wait_nodes(host, 1, &n));
    CHECK(dm_ldn_advertise(host, NULL, 1) != 0); CHECK(dm_ldn_advertise(host, packet, 385) != 0); CHECK(dm_ldn_policy(host, 4) != 0);
    CHECK(!dm_ldn_policy(host, 1)); fd = raw_connect(port); p = (struct pollfd){fd,POLLIN,0}; CHECK(poll(&p,1,2000)>0); CHECK(recv(fd,packet,1,0)<=0); close(fd);
    unsigned station_port=free_port(SOCK_DGRAM); dm_ldn_transport st=transport(station_port,port,2); dm_ldn_context *station=dm_ldn_start(&st,NULL,NULL); CHECK(station); CHECK(!dm_ldn_open(station,0));
    dm_ldn_filter f={.flags=1}; f.id.intent.local_id=123; dm_ldn_network rooms[24]; int count; CHECK(!dm_ldn_scan(station,rooms,&count,24,&f,0) && count==0); f.flags=0; CHECK(!dm_ldn_scan(station,rooms,&count,24,&f,0) && count==1);
    dm_ldn_user su=user("policy station"); CHECK(dm_ldn_connect(station,rooms,&sec,&su,5,0)!=0); CHECK(dm_ldn_state(station)==4);
    CHECK(!dm_ldn_policy(host,2)); CHECK(!dm_ldn_filter_add(host,st.mac)); CHECK(dm_ldn_connect(station,rooms,&sec,&su,5,0)!=0); CHECK(!dm_ldn_filter_clear(host));
    CHECK(!dm_ldn_connect(station,rooms,&sec,&su,5,0)); CHECK(wait_nodes(host,2,&n)); CHECK(!dm_ldn_reject(host,n.nodes[1].ip));
    int lost=0; for(int i=0;i<200;++i) { if(dm_ldn_state(station)==4) {lost=1;break;} pause_ms(5); } CHECK(lost); CHECK(wait_nodes(host,1,&n));
    CHECK(!dm_ldn_policy(host,3)); CHECK(dm_ldn_connect(station,rooms,&sec,&su,5,0)!=0); CHECK(!dm_ldn_filter_add(host,st.mac)); CHECK(!dm_ldn_connect(station,rooms,&sec,&su,5,0));
    CHECK(!dm_ldn_disconnect(host, 1)); CHECK(dm_ldn_state(host) == 2 && dm_ldn_reason(host) == 3); CHECK(!dm_ldn_close(host, 1)); CHECK(dm_ldn_state(host) == 1);
    dm_ldn_stop(station); dm_ldn_stop(host);
}
static void peer_fixture(void) {
    // Fork before creating either worker. Child and parent then own completely
    // separate contexts, descriptors, locks and address-space snapshots.
    int command[2], result[2]; CHECK(!pipe(command) && !pipe(result)); unsigned hp = free_port(SOCK_STREAM), sp = free_port(SOCK_DGRAM);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        close(command[1]); close(result[0]); dm_ldn_transport t = transport(hp,hp,1); dm_ldn_context *c = dm_ldn_start(&t,NULL,NULL); CHECK(c); CHECK(!dm_ldn_open(c,1)); dm_ldn_network_config cfg=config(); dm_ldn_security_config sec=security(); dm_ldn_user u=user("process host"); CHECK(!dm_ldn_create(c,&cfg,&sec,&u,NULL,0,NULL));
        CHECK(write(result[1],"R",1)==1); char b;
        while (read(command[0],&b,1)==1) {
            dm_ldn_network n;
            if (b=='J') { CHECK(wait_nodes(c,2,&n)); CHECK(!strcmp(n.nodes[1].name,"process station")); CHECK(write(result[1],"J",1)==1); }
            if (b=='A') { CHECK(!dm_ldn_advertise(c,"cross process",13)); CHECK(write(result[1],"A",1)==1); }
#ifdef DM_LDN_NET_TEST
            if (b=='B') {
                int data=dm_net_socket(2,2,0); CHECK(data>=0); guest_address a=guest_addr(htonl(INADDR_LOOPBACK),0); CHECK(!dm_net_bind(data,&a,sizeof a)); unsigned size=sizeof a; CHECK(!dm_net_name(data,&a,&size,0)); CHECK(write(result[1],&a.port,2)==2);
                dm_net_pollfd ready={data,1,0}; CHECK(dm_net_poll(&ready,1,2000)==1); char payload[32]; guest_address from; size=sizeof from;
                CHECK(dm_net_recv(data,payload,sizeof payload,0,&from,&size)==18 && !memcmp(payload,"guest game request",18)); CHECK(dm_net_send(data,"guest game reply",16,0,&from,size)==16); CHECK(!dm_net_close(data)); CHECK(write(result[1],"B",1)==1);
            }
#endif
            if (b=='D') { CHECK(wait_nodes(c,1,&n)); CHECK(write(result[1],"D",1)==1); }
            if (b=='X') break;
        }
        dm_ldn_stop(c); close(command[0]); close(result[1]); _exit(0);
    }
    close(command[0]);close(result[1]); char b; CHECK(read(result[0],&b,1)==1 && b=='R'); dm_ldn_transport t=transport(sp,hp,2); dm_ldn_context *c=dm_ldn_start(&t,NULL,NULL); CHECK(c); CHECK(!dm_ldn_open(c,0));
    dm_ldn_filter f={.flags=33}; f.id.intent=config().intent; dm_ldn_network rooms[24], n; int count; CHECK(!dm_ldn_scan(c,rooms,&count,24,&f,0)); CHECK(count==1 && !strcmp(rooms[0].nodes[0].name,"process host"));
    dm_ldn_security_config sec=security(); dm_ldn_user u=user("process station"); CHECK(dm_ldn_connect(c,&rooms[0],&sec,&u,4,0)!=0); CHECK(dm_ldn_state(c)==4);
    CHECK(!dm_ldn_connect(c,&rooms[0],&sec,&u,5,0)); CHECK(dm_ldn_state(c)==5); CHECK(!dm_ldn_get_network(c,&n,NULL,0) && n.node_count==2);
    CHECK(write(command[1],"J",1)==1); CHECK(read(result[0],&b,1)==1 && b=='J'); CHECK(write(command[1],"A",1)==1); CHECK(read(result[0],&b,1)==1 && b=='A');
    int synced=0; for(int i=0;i<200;++i) { if (!dm_ldn_get_network(c,&n,NULL,0) && n.advertise_size==13 && !memcmp(n.advertise,"cross process",13)) {synced=1;break;} pause_ms(5); } CHECK(synced);
#ifdef DM_LDN_NET_TEST
    // After discovery/control, game datagrams traverse the actual Horizon
    // socket translator, using the address learned from the remote NodeInfo.
    CHECK(write(command[1],"B",1)==1); uint16_t port; CHECK(read(result[0],&port,2)==2); int data=dm_net_socket(2,2,0); CHECK(data>=0);
    guest_address a=guest_addr(htonl(n.nodes[0].ip),ntohs(port)); CHECK(dm_net_send(data,"guest game request",18,0,&a,sizeof a)==18);
    dm_net_pollfd ready={data,1,0}; CHECK(dm_net_poll(&ready,1,2000)==1); char payload[32]; CHECK(dm_net_recv(data,payload,sizeof payload,0,NULL,NULL)==16 && !memcmp(payload,"guest game reply",16)); CHECK(!dm_net_close(data)); CHECK(read(result[0],&b,1)==1 && b=='B');
#endif
    CHECK(!dm_ldn_disconnect(c,0)); CHECK(dm_ldn_state(c)==4 && dm_ldn_reason(c)==1); CHECK(write(command[1],"D",1)==1); CHECK(read(result[0],&b,1)==1 && b=='D');
    // Reconnect, then observe an actual EOF from the host worker shutting down.
    CHECK(!dm_ldn_scan(c,rooms,&count,24,&f,0) && count==1); CHECK(!dm_ldn_connect(c,&rooms[0],&sec,&u,5,0)); CHECK(write(command[1],"X",1)==1); int status; CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    int lost=0; for(int i=0;i<200;++i) { if(dm_ldn_state(c)==4) {lost=1;break;} pause_ms(5); } CHECK(lost && dm_ldn_reason(c)==6); CHECK(dm_ldn_get_network(c,&n,NULL,0)!=0);
    dm_ldn_stop(c); close(command[1]);close(result[0]);
}
#ifdef DM_NET_TEST
static _Atomic unsigned ldn_discovery_calls;
static unsigned ldn_discovery_error_at;
static long ldn_host_down(int fd,const void *bytes,size_t size,int flags,uint32_t address,uint16_t port) {
    (void)address;unsigned call=atomic_fetch_add(&ldn_discovery_calls,1);
    if(call==ldn_discovery_error_at) { errno=EHOSTDOWN;return -1; }
    struct sockaddr_in to={.sin_len=sizeof to,.sin_family=AF_INET,.sin_port=port,.sin_addr={htonl(INADDR_LOOPBACK)}};
    return sendto(fd,bytes,size,flags,(struct sockaddr *)&to,sizeof to);
}
static void automatic_discovery(void) {
    dm_net_set_multicast_allowed(0); unsetenv("DM_NET_DISCOVERY_PEER");
    // The only reachable loopback neighbor is 127.0.0.1. The validated adapter
    // fixture's /24 makes the worker send the unchanged scan via unicast.
    dm_net_test_interface adapter={htonl(0x7f000002),htonl(0xffffff00),if_nametoindex("lo0"),IFF_UP|IFF_BROADCAST,1};
    CHECK(adapter.index); dm_net_test_set_interface(&adapter);
    unsigned hp=free_port(SOCK_DGRAM),sp=free_port(SOCK_DGRAM);
    dm_ldn_transport ht=transport(hp,hp,1),st=transport(sp,hp,2);
    st.address=adapter.address;st.mask=adapter.mask;st.discovery=st.address|~st.mask;
    dm_ldn_context *host=dm_ldn_start(&ht,NULL,NULL),*station=dm_ldn_start(&st,NULL,NULL); CHECK(host && station);
    CHECK(!dm_ldn_open(host,1) && !dm_ldn_open(station,0));
    dm_ldn_network_config cfg=config();dm_ldn_security_config sec=security();dm_ldn_user owner=user("auto host");
    CHECK(!dm_ldn_create(host,&cfg,&sec,&owner,NULL,0,NULL));
    dm_ldn_filter filter={0};dm_ldn_network rooms[24];int count=0;
    CHECK(!dm_ldn_scan(station,rooms,&count,24,&filter,0));
    CHECK(count==1 && !strcmp(rooms[0].nodes[0].name,"auto host") && rooms[0].nodes[0].ip==INADDR_LOOPBACK);
    dm_ldn_stop(station);
    // Destination-specific EHOSTDOWN must be skipped both at the first
    // neighbor and after a successful send in the worker's sweep.
    for(unsigned fault=0;fault<2;++fault) {
        dm_net_set_multicast_allowed(0);station=dm_ldn_start(&st,NULL,NULL);CHECK(station);CHECK(!dm_ldn_open(station,0));
        ldn_discovery_error_at=fault;atomic_store(&ldn_discovery_calls,0);dm_net_test_set_discovery_sender(ldn_host_down);
        CHECK(!dm_ldn_scan(station,rooms,&count,24,&filter,0)&&count==1&&atomic_load(&ldn_discovery_calls)>fault+1);
        dm_net_test_set_discovery_sender(NULL);dm_ldn_stop(station);
    }
    // A capable native sender keeps its original destination and also probes
    // restricted peers. The logical loopback broadcast itself reaches no peer;
    // finding this real host therefore requires the complementary unicast.
    dm_net_set_multicast_allowed(1);station=dm_ldn_start(&st,NULL,NULL);CHECK(station);CHECK(!dm_ldn_open(station,0));
    CHECK(!dm_ldn_scan(station,rooms,&count,24,&filter,0));CHECK(count==1&&dm_net_multicast_allowed()==1);
    dm_ldn_stop(station);
    // The same error on the original native broadcast remains visible and
    // must not launch a complementary sweep to the reachable host.
    station=dm_ldn_start(&st,NULL,NULL);CHECK(station);CHECK(!dm_ldn_open(station,0));
    ldn_discovery_error_at=0;atomic_store(&ldn_discovery_calls,0);dm_net_test_set_discovery_sender(ldn_host_down);
    CHECK(dm_ldn_scan(station,rooms,&count,24,&filter,0)!=0&&count==0&&atomic_load(&ldn_discovery_calls)==1);
    dm_net_test_set_discovery_sender(NULL);dm_ldn_stop(station);
    // Create the actual native socket without SO_BROADCAST, then simulate a
    // claimed capability. The kernel refuses native broadcast: supplemental
    // unicast must not hide that error by discovering the otherwise reachable host.
    dm_net_set_multicast_allowed(0);st.discovery=htonl(UINT32_MAX);station=dm_ldn_start(&st,NULL,NULL);CHECK(station);CHECK(!dm_ldn_open(station,0));
    dm_net_set_multicast_allowed(1);CHECK(dm_ldn_scan(station,rooms,&count,24,&filter,0)!=0&&count==0);
    dm_ldn_stop(station);dm_ldn_stop(host);dm_net_test_set_interface(NULL);dm_net_set_multicast_allowed(1);
}
#endif
int main(void) {
#ifdef DM_LDN_NET_TEST
    setenv("DM_NETWORK","ldn",1); CHECK(dm_net_enabled());
#endif
    protocol(); local(); peer_fixture();
#ifdef DM_NET_TEST
    automatic_discovery();
#endif
    printf("LDN: %u checks passed (protocol, malformed streams, real two-process discovery/session).\n",checks); return 0;
}
