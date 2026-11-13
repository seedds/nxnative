// Opt-in DSU phone motion. One connected UDP socket per configured player.
// DM_DSU_1..8: IPv4,port,slot,x,y,z,biasX,biasY,biasZ. Signed axes 1..3
// map DSU gyro to Switch axes. Bias is degrees/s. Accelerometer mapping uses
// the determinant too: gyro is axial, acceleration is a polar vector.
#include "dm.h"
#include "dm_input.h"
#include "dsu_motion.h"
#include "dsu_protocol.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
static struct { int fd,slot,axes[3],have,det,buttons; float bias[3]; uint32_t server,sequence; uint64_t timestamp,arrival,local; } peers[DM_PLAYERS];
static pthread_t worker;
static _Atomic int active;
static int launched;
static uint32_t client_id;
static pthread_mutex_t report_mu=PTHREAD_MUTEX_INITIALIZER;
static struct { int configured,slot,port; char host[64]; uint64_t packets,last; float gyro[3]; } report[DM_PLAYERS];
void dm_dsu_report(char *out,size_t size) {
    size_t used=0;uint64_t now=now_ns();pthread_mutex_lock(&report_mu);
    if(size) out[0]=0;
    for(int i=0;i<DM_PLAYERS && used<size;i++) {
        int n=snprintf(out+used,size-used,"Player %d: %s\nEndpoint %s:%d | slot %d\n%llu valid packets | gyro %+.3f %+.3f %+.3f rad/s\n\n",i+1,!report[i].configured ? "not configured" : !report[i].last ? "waiting for phone" : now-report[i].last>300000000ull ? "signal stale" : "receiving in GAME HOST",report[i].host,report[i].port,report[i].slot,(unsigned long long)report[i].packets,report[i].gyro[0],report[i].gyro[1],report[i].gyro[2]);
        if(n<0)break;used+=(size_t)n<size-used ? (size_t)n:size-used;
    }
    pthread_mutex_unlock(&report_mu);
}
// Match motion_set_to: macOS CLOCK_MONOTONIC and mach_absolute_time are
// distinct clock domains. Using the former makes fresh packets appear stale.
static uint64_t dsu_now(void) { return now_ns(); }
static void *dsu_run(void *unused) {
    (void)unused;uint64_t sent=0;
    while (atomic_load(&active)) {
        uint64_t now=dsu_now();
        if (now-sent>=1000000000ull) { for (int i=0;i<DM_PLAYERS;i++) if (peers[i].fd>=0) { uint8_t req[28];dsu_request(req,client_id,peers[i].slot);send(peers[i].fd,req,sizeof req,0); }sent=now; }
        struct pollfd fds[DM_PLAYERS];for (int i=0;i<DM_PLAYERS;i++) fds[i]=(struct pollfd){peers[i].fd,POLLIN,0};
        if (poll(fds,DM_PLAYERS,100)<=0) continue;
        for (int i=0;i<DM_PLAYERS;i++) if (fds[i].revents&POLLERR) {
            int error=0;socklen_t length=sizeof error;getsockopt(peers[i].fd,SOL_SOCKET,SO_ERROR,&error,&length);
        }
        for (int i=0;i<DM_PLAYERS;i++) if (fds[i].revents&POLLIN) {
            uint8_t bytes[2048];ssize_t n=recv(peers[i].fd,bytes,sizeof bytes,0);dsu_sample s;
            if (n<0 || !dsu_decode(bytes,(size_t)n,&s) || s.slot!=peers[i].slot) continue;
            now=dsu_now();
            if (peers[i].have && now-peers[i].arrival<300000000ull && s.server==peers[i].server) {
                uint32_t diff=s.sequence-peers[i].sequence;
                if (!diff || diff>=0x80000000u || s.timestamp_us<=peers[i].timestamp) continue;
                uint64_t dt=s.timestamp_us-peers[i].timestamp;
                if (dt>300000) continue;
                peers[i].local+=dt*1000;
                if (peers[i].local>now) peers[i].local=now;
            } else peers[i].local=now;
            peers[i].have=1;peers[i].server=s.server;peers[i].sequence=s.sequence;peers[i].timestamp=s.timestamp_us;peers[i].arrival=now;
            float acc[3],rot[3];
            for (int a=0;a<3;a++) { int k=abs(peers[i].axes[a])-1;float sign=peers[i].axes[a]<0 ? -1:1;acc[a]=s.acceleration[k]*sign*peers[i].det;rot[a]=(s.degrees[k]-peers[i].bias[k])*sign*(float)(3.141592653589793/180.0); }
            pthread_mutex_lock(&report_mu);report[i].packets++;report[i].last=now;memcpy(report[i].gyro,rot,sizeof rot);pthread_mutex_unlock(&report_mu);
            if(peers[i].buttons)dm_dsu_input(i,s.buttons,s.sticks[0],s.sticks[1],s.sticks[2],s.sticks[3],now);
            dm_motion_set_dsu(i,acc,rot,peers[i].local);
        }
    }
    return NULL;
}
void dm_dsu_start(void) {
    if (launched) return;
#ifdef __APPLE__
    extern void dm_dsu_load_configuration(void) __attribute__((weak_import));
    if(dm_dsu_load_configuration)dm_dsu_load_configuration();
#endif
    memset(peers,0,sizeof peers);
    pthread_mutex_lock(&report_mu);memset(report,0,sizeof report);pthread_mutex_unlock(&report_mu);
    for (int i=0;i<DM_PLAYERS;i++) peers[i].fd=-1;
    int count=0;
    for (int i=0;i<DM_PLAYERS;i++) {
        char key[24],host[64],extra;int port,slot,x,y,z;float bx,by,bz;
        snprintf(key,sizeof key,"DM_DSU_%d",i+1);const char *value=getenv(key);if (!value || !*value) continue;
        if (sscanf(value,"%63[^,],%d,%d,%d,%d,%d,%f,%f,%f%c",host,&port,&slot,&x,&y,&z,&bx,&by,&bz,&extra)!=9 || port<1 || port>65535 || slot<0 || slot>3 || x < -3 || x > 3 || y < -3 || y > 3 || z < -3 || z > 3 || x==0 || y==0 || z==0 || abs(x)==abs(y) || abs(x)==abs(z) || abs(y)==abs(z) || !isfinite(bx) || !isfinite(by) || !isfinite(bz)) { LOG("[RED] DSU player %d: invalid configuration",i+1);continue; }
        struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_port=htons((uint16_t)port);
        if (inet_pton(AF_INET,host,&addr.sin_addr)!=1) { LOG("[RED] DSU player %d: numeric IPv4 required",i+1);continue; }
        int fd=socket(AF_INET,SOCK_DGRAM,0);if (fd<0) continue;
        if (connect(fd,(struct sockaddr *)&addr,sizeof addr)) { close(fd);continue; }
        snprintf(key,sizeof key,"DM_DSU_BUTTONS_%d",i+1);peers[i].buttons=getenv(key)&&atoi(getenv(key));
        peers[i].fd=fd;peers[i].slot=slot;peers[i].axes[0]=x;peers[i].axes[1]=y;peers[i].axes[2]=z;peers[i].bias[0]=bx;peers[i].bias[1]=by;peers[i].bias[2]=bz;
        int a=abs(x)-1,b=abs(y)-1,c=abs(z)-1;int inversions=(a>b)+(a>c)+(b>c);
        peers[i].det=((inversions%2) ? -1:1)*(x<0?-1:1)*(y<0?-1:1)*(z<0?-1:1);count++;
        pthread_mutex_lock(&report_mu);report[i].configured=1;report[i].slot=slot;report[i].port=port;snprintf(report[i].host,sizeof report[i].host,"%s",host);pthread_mutex_unlock(&report_mu);
        LOG("DSU player %d: configured slot %d (motion only, physical buttons unchanged)",i+1,slot);
    }
    if (!count) return;
    client_id=arc4random();
    atomic_store(&active,1);
    if (pthread_create(&worker,NULL,dsu_run,NULL)) { atomic_store(&active,0);for(int i=0;i<DM_PLAYERS;i++) if(peers[i].fd>=0) close(peers[i].fd);return; }
    launched=1;
}
void dm_dsu_stop(void) {
    if (!launched) return;
    atomic_store(&active,0);pthread_join(worker,NULL);
    for(int i=0;i<DM_PLAYERS;i++) if(peers[i].fd>=0) { close(peers[i].fd);peers[i].fd=-1; }
    for(int i=0;i<DM_PLAYERS;i++)dm_dsu_input(i,0,0,0,0,0,0);
    launched=0;
}
