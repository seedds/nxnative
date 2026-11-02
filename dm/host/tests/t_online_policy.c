// Actual resolver admission: automatic local networking remains independent of
// an absent Nextendo profile. Only localhost/numeric loopback are resolved.
#include "../net_socket.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned calls, checks;
static int refuse(const char *name,char *out,size_t cap) { (void)name;(void)out;(void)cap;calls++;return -1; }
#define CHECK(c) do { checks++;assert(c); } while(0)
static int local(void) { dm_net_addrinfo hint={.family=2,.type=1};void *out=NULL;
    int result=dm_net_getaddrinfo("localhost","0",&hint,&out);int okay=!result&&out;
    if(out)dm_net_freeaddrinfo(out);return okay; }
int main(void) {
    dm_net_set_dns_provider(refuse);unsetenv("DM_NETWORK");unsetenv("DM_NEXTENDO_ROUTES");
    CHECK(!dm_net_enabled()&&!dm_net_nextendo_enabled()&&!local()&&!calls);
    setenv("DM_NETWORK","auto",1);CHECK(dm_net_enabled()&&!dm_net_nextendo_enabled()&&local()&&!calls);
    setenv("DM_NEXTENDO_ROUTES","",1);CHECK(!dm_net_nextendo_enabled()&&local()&&!calls);
    setenv("DM_NEXTENDO_ROUTES","explicit-but-invalid",1);dm_net_set_errno(113);
    CHECK(dm_net_nextendo_enabled()&&!local()&&calls==1&&dm_net_errno()==113);
    unsetenv("DM_NEXTENDO_ROUTES");setenv("DM_NETWORK","nextendo",1);
    CHECK(dm_net_nextendo_enabled()&&!local()&&calls==2);
    setenv("DM_NEXTENDO_ROUTES","explicit-but-invalid",1);setenv("DM_NETWORK","lan",1);
    CHECK(!dm_net_nextendo_enabled()&&local()&&calls==2);
    setenv("DM_NETWORK","ldn",1);CHECK(!dm_net_nextendo_enabled()&&local()&&calls==2);
    printf("Online policy: %u checks PASS; unconfigured automatic/local DNS preserved\n",checks);return 0;
}
void dm_guest_host_wait_begin(void) {} void dm_guest_host_wait_end(void) {}
