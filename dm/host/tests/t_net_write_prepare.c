// Original socket-copyout fixture with the real dirty-page tracker; no GPU, game or external peer.
#include <sys/socket.h>
#include <sys/mman.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include "../../gfx/nvn/nvn_dirty.h"
static void (*prepare_hook)(void*,size_t);static unsigned preps,checks,failures;
static void *last_prep;static size_t last_len;
void dm_fault_hook_register(int (*fn)(void*,int,void*)) {(void)fn;}
void dm_guest_write_prep_register(void (*fn)(void*,size_t)) {assert(fn&&!prepare_hook);prepare_hook=fn;}
void dm_guest_write_prepare(void *p,size_t n) {++preps;last_prep=p;last_len=n;if(prepare_hook)prepare_hook(p,n);}
void dm_guest_host_wait_begin(void) {} void dm_guest_host_wait_end(void) {}
static int wait_sender=-1;static void *wait_destination;static size_t wait_size;static int wait_hits;
static struct sockaddr_in wait_address;static int wait_udp;
static const char *wait_bytes="DATA";static size_t wait_count=4;
static int fixture_poll(struct pollfd *p,nfds_t n,int ms) {
 if(wait_sender>=0){int s=wait_sender;wait_sender=-1;++wait_hits;nvn_dirty_protect(wait_destination,wait_size);
  ssize_t sent=wait_udp?sendto(s,wait_bytes,wait_count,0,(void*)&wait_address,16):send(s,wait_bytes,wait_count,0);assert(sent==(ssize_t)wait_count);}
 return poll(p,n,ms);
}
#define poll fixture_poll
#include "../hle_net_backend.c"
#undef poll
#define CHECK(x) do{++checks;if(!(x)){++failures;fprintf(stderr,"FAIL %d: %s (guest errno%d)\n",__LINE__,#x,dm_net_errno());}}while(0)
static nx_addr address(uint16_t port){return (nx_addr){.len=16,.family=2,.port=port,.address=htonl(INADDR_LOOPBACK)};}
static void udp_pair(int *guest,int *peer,struct sockaddr_in *back) {
 *guest=dm_net_socket(2,2,17);assert(*guest>=0);nx_addr a=address(0);assert(!dm_net_bind(*guest,&a,16));unsigned len=16;assert(!dm_net_name(*guest,&a,&len,0));
 *peer=socket(AF_INET,SOCK_DGRAM,0);assert(*peer>=0);*back=(struct sockaddr_in){.sin_len=16,.sin_family=AF_INET,.sin_port=a.port,.sin_addr={htonl(INADDR_LOOPBACK)}};
}
static void tcp_pair(int *guest,int *peer) {
 int server=socket(AF_INET,SOCK_STREAM,0);assert(server>=0);struct sockaddr_in a={.sin_len=16,.sin_family=AF_INET,.sin_addr={htonl(INADDR_LOOPBACK)}};assert(!bind(server,(void*)&a,16)&&!listen(server,1));socklen_t z=16;assert(!getsockname(server,(void*)&a,&z));
 *guest=dm_net_socket(2,1,6);assert(*guest>=0);nx_addr n=address(a.sin_port);assert(!dm_net_connect(*guest,&n,16));*peer=accept(server,NULL,NULL);assert(*peer>=0);close(server);
}
static void trial(int udp,int delayed,int invalid) {
 size_t page=(size_t)getpagesize();void *dst=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(dst!=MAP_FAILED);memset(dst,0xa5,page);
 if(!invalid)nvn_dirty_add_range(dst,page);
 if(invalid)assert(!mprotect(dst,page,PROT_NONE));else nvn_dirty_protect(dst,page);
 uint64_t stamp=nvn_dirty_now();unsigned before=preps;int guest,peer;struct sockaddr_in back;
 if(udp)udp_pair(&guest,&peer,&back);else tcp_pair(&guest,&peer);
 if(delayed){wait_sender=peer;wait_destination=dst;wait_size=page;wait_udp=udp;if(udp)wait_address=back;}
 else {ssize_t sent=udp?sendto(peer,"DATA",4,0,(void*)&back,16):send(peer,"DATA",4,0);assert(sent==4);}
 dm_net_set_errno(77);long result=dm_net_recv(guest,dst,4,0,NULL,NULL);
 if(invalid)CHECK(result==-1&&dm_net_errno()==14&&!nvn_dirty_tracked(dst,page));
 else {CHECK(result==4&&dm_net_errno()==77);if(result==4)CHECK(!memcmp(dst,"DATA",4));CHECK(nvn_dirty_since(dst,page,stamp));}
 printf("%s %s %s: result%ld errno%d preps%u\n",udp?"UDP":"TCP",delayed?"reprotected during wait":"already protected",invalid?"invalid":"tracked",result,dm_net_errno(),preps-before);
 if(delayed)CHECK(wait_sender==-1&&wait_hits>0);
 if(!invalid)nvn_dirty_remove_range(dst);CHECK(!munmap(dst,page));CHECK(!dm_net_close(guest));close(peer);
}
static void stream_partial(void);
static void invalid_and_empty(void);
static void socket_options(void);
int main(void) {setenv("DM_NETWORK","auto",1);
 for(int udp=0;udp<=1;++udp){trial(udp,0,0);trial(udp,1,0);trial(udp,0,1);}
 stream_partial();invalid_and_empty();socket_options();
 printf("socket write prepare: %u checks,%u failed\n",checks,failures);return failures!=0;
}
static void stream_partial(void) {
 size_t page=(size_t)getpagesize();void *dst=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(dst!=MAP_FAILED);nvn_dirty_add_range(dst,page);nvn_dirty_protect(dst,page);
 int guest,peer;tcp_pair(&guest,&peer);assert(send(peer,"AB",2,0)==2);int native=dm_net_dup_native(guest);struct pollfd ready={native,POLLIN,0};assert(poll(&ready,1,100)==1);close(native);
 wait_sender=peer;wait_destination=dst;wait_size=page;wait_udp=0;wait_bytes="CD";wait_count=2;unsigned before=preps;dm_net_set_errno(77);
 CHECK(dm_net_recv(guest,dst,4,0x40,NULL,NULL)==4&&dm_net_errno()==77);CHECK(!memcmp(dst,"ABCD",4));CHECK(preps>=before+3&&last_prep==(char*)dst+2&&last_len==2);CHECK(wait_sender==-1);
 nvn_dirty_remove_range(dst);CHECK(!munmap(dst,page));CHECK(!dm_net_close(guest));close(peer);wait_bytes="DATA";wait_count=4;
}
static void invalid_and_empty(void) {
 size_t page=(size_t)getpagesize();void *ro=mmap(NULL,page,PROT_READ,MAP_PRIVATE|MAP_ANON,-1,0);assert(ro!=MAP_FAILED);
 int guest,peer;struct sockaddr_in back;udp_pair(&guest,&peer,&back);assert(sendto(peer,"DATA",4,0,(void*)&back,16)==4);unsigned before=preps;
 CHECK(dm_net_recv(guest,ro,4,0,NULL,NULL)==-1&&dm_net_errno()==14&&!nvn_dirty_tracked(ro,page));CHECK(preps>before);CHECK(!munmap(ro,page));
 before=preps;CHECK(dm_net_recv(guest,NULL,1,0,NULL,NULL)==-1&&dm_net_errno()==14&&preps==before);
 CHECK(dm_net_recv(guest,(void*)1,4,0x10000,NULL,NULL)==-1&&dm_net_errno()==95&&preps==before);
 CHECK(dm_net_recv(-1,(void*)1,4,0,NULL,NULL)==-1&&dm_net_errno()==9&&preps==before);
 assert(sendto(peer,"DATA",4,0,(void*)&back,16)==4);CHECK(dm_net_recv(guest,(void*)1,4,0,NULL,NULL)==-1&&dm_net_errno()==14&&!nvn_dirty_tracked((void*)1,4));
 char drain[8];while(dm_net_recv(guest,drain,sizeof drain,0x80,NULL,NULL)>=0){}
 assert(sendto(peer,NULL,0,0,(void*)&back,16)==0);CHECK(dm_net_recv(guest,NULL,0,0,NULL,NULL)==0);
 CHECK(!dm_net_close(guest));close(peer);
}

// A getsockopt copyout is not a CPU write fault either. Exercise the exact
// validated option span, including a result crossing two protected pages.
static void socket_option_output(int kind) {
 size_t page=(size_t)getpagesize();uint8_t *mem=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(mem!=MAP_FAILED);memset(mem,0xa5,page*2);
 nvn_dirty_add_range(mem,page*2);nvn_dirty_protect(mem,page*2);uint64_t stamp=nvn_dirty_now();
 unsigned size=kind==2?8:4,option=kind==0?0x1008:kind==1?0x1007:128;
 uint8_t *value=kind==2?mem+page-4:mem+32;unsigned len=32,before=preps;
 int guest=dm_net_socket(2,kind==0?2:1,kind==0?17:6);assert(guest>=0);dm_net_set_errno(77);
 int result=dm_net_sockopt(guest,0xffff,option,value,&len,0);
 CHECK(result==0&&dm_net_errno()==77);CHECK(len==size);CHECK(preps==before+1&&last_prep==value&&last_len==size);
 int expected[2]={kind==0?SOCK_DGRAM:0,0};CHECK(!memcmp(value,expected,size));CHECK(value[-1]==0xa5&&value[size]==0xa5);
 CHECK(nvn_dirty_since(value,size,stamp));
 if(kind!=2)CHECK(!nvn_dirty_since(mem+page,page,stamp));
 else CHECK(nvn_dirty_since(mem,page,stamp)&&nvn_dirty_since(mem+page,page,stamp));
 printf("getsockopt kind%d: result%d errno%d preps%u\n",kind,result,dm_net_errno(),preps-before);
 nvn_dirty_remove_range(mem);CHECK(!munmap(mem,page*2));CHECK(!dm_net_close(guest));
}
static void socket_option_invalid(void) {
 size_t page=(size_t)getpagesize();int guest=dm_net_socket(2,1,6);assert(guest>=0);
 for(int inaccessible=0;inaccessible<2;inaccessible++) {
  void *mem=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(mem!=MAP_FAILED);assert(!mprotect(mem,page,inaccessible?PROT_NONE:PROT_READ));
  unsigned len=4;CHECK(dm_net_sockopt(guest,0xffff,0x1008,mem,&len,0)==-1&&dm_net_errno()==14);
  CHECK(len==4&&!nvn_dirty_tracked(mem,page));CHECK(!munmap(mem,page));
 }
 unsigned len=4;CHECK(dm_net_sockopt(guest,0xffff,0x1008,(void*)1,&len,0)==-1&&dm_net_errno()==14&&!nvn_dirty_tracked((void*)1,4));
 unsigned before=preps;CHECK(dm_net_sockopt(guest,0xffff,0x1008,NULL,&len,0)==-1&&dm_net_errno()==14&&preps==before);
 CHECK(dm_net_sockopt(guest,0xffff,0x1008,(void*)1,NULL,0)==-1&&dm_net_errno()==14&&preps==before);
 CHECK(dm_net_sockopt(-1,0xffff,0x1008,(void*)1,&len,0)==-1&&dm_net_errno()==9&&preps==before);
 CHECK(dm_net_sockopt(guest,0xffff,0x9999,(void*)1,&len,0)==-1&&dm_net_errno()==92&&preps==before);
 len=2;CHECK(dm_net_sockopt(guest,0xffff,0x1008,(void*)1,&len,0)==-1&&dm_net_errno()==22&&preps==before);CHECK(!dm_net_close(guest));
}
static void socket_option_input(void) {
 size_t page=(size_t)getpagesize();int *value=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(value!=MAP_FAILED);*value=1;
 nvn_dirty_add_range(value,page);nvn_dirty_protect(value,page);uint64_t stamp=nvn_dirty_now();
 int guest=dm_net_socket(2,1,6);assert(guest>=0);unsigned len=4,before=preps;dm_net_set_errno(77);
 CHECK(dm_net_sockopt(guest,6,1,value,&len,1)==0&&dm_net_errno()==77);CHECK(preps==before&&!nvn_dirty_since(value,page,stamp)&&*value==1);
 nvn_dirty_remove_range(value);CHECK(!munmap(value,page));CHECK(!dm_net_close(guest));
}
static void socket_options(void) {
 for(int kind=0;kind<3;kind++)socket_option_output(kind);
 socket_option_invalid();socket_option_input();
}
