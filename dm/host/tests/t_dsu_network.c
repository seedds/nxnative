// Loopback server proves real subscription, decoding, motion callback and shutdown.
#include "../hle_dsu.c"
#include <assert.h>
static _Atomic int callbacks;
void dm_motion_set_dsu(int p,const float a[3],const float r[3],uint64_t t) { assert(p==0 && t>0);assert(fabsf(a[2]+1)<0.001f);assert(fabsf(r[0]-1.5707963f)<0.001f);atomic_fetch_add(&callbacks,1); }
int main(void) {
    int server=socket(AF_INET,SOCK_DGRAM,0);assert(server>=0);
    struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(!bind(server,(struct sockaddr*)&addr,sizeof addr));socklen_t len=sizeof addr;assert(!getsockname(server,(struct sockaddr*)&addr,&len));
    char config[128];snprintf(config,sizeof config,"127.0.0.1,%u,1,1,2,3,0,0,0",ntohs(addr.sin_port));setenv("DM_DSU_1",config,1);dm_dsu_start();
    struct pollfd fd={server,POLLIN,0};assert(poll(&fd,1,2000)>0);
    uint8_t req[64];struct sockaddr_in client;socklen_t cl=sizeof client;assert(recvfrom(server,req,sizeof req,0,(struct sockaddr*)&client,&cl)==28);assert(!memcmp(req,"DSUC",4) && req[21]==1);
    uint8_t packet[100]={0};memcpy(packet,"DSUS",4);packet[4]=0xe9;packet[5]=3;packet[6]=84;dsu_put(packet+16,0x100002);packet[20]=1;packet[21]=2;packet[31]=1;dsu_put(packet+32,1);dsu_put(packet+68,1000000);
    float g=-1,r=90;memcpy(packet+84,&g,4);memcpy(packet+88,&r,4);dsu_put(packet+8,dsu_crc(packet,100));assert(sendto(server,packet,100,0,(struct sockaddr*)&client,cl)==100);
    for(int i=0;i<100 && !atomic_load(&callbacks);i++) { struct timespec pause={0,10000000};nanosleep(&pause,NULL); }
    assert(atomic_load(&callbacks)==1);dm_dsu_stop();close(server);puts("DSU loopback subscription and radian conversion passed");
}
