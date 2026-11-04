// Optional local-only Apple API smoke; never part of the offline CI suite.
#include "../net_socket.h"
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned waits, balance;
void dm_guest_host_wait_begin(void) { ++waits; ++balance; }
void dm_guest_host_wait_end(void) { --balance; }
int main(void) {
    // This fixture watchdog only detects a broken smoke; the production query
    // uses Apple's own kDNSServiceFlagsTimeout deadline without an app timer.
    alarm(45);
    setenv("DM_NETWORK","lan",1);
    typedef struct { char *name, **aliases; int family, length; char **addresses; } hostent;
    hostent *result=dm_net_gethostbyname_cancel("localhost",dm_net_request_cancel_handle());
    if (!result) { fprintf(stderr,"real Apple localhost lookup unavailable: errno=%d\n",dm_net_errno()); return 1; }
    unsigned count=0;
    while (result->addresses[count]) {
        uint32_t address; memcpy(&address,result->addresses[count],4);
        if ((ntohl(address)>>24)!=127 || count>=64) return 1;
        ++count;
    }
    dm_net_finalize(); alarm(0);
    if (!count || result->family!=2 || result->length!=4 || !waits || balance) return 1;
    printf("real Apple DNSService localhost: %u loopback answers, callbacks completed, finalize PASS\n",count);
    return 0;
}
