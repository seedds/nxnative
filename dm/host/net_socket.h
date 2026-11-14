// Horizon socket ABI, translated by hle_net_backend.c. Opt-in Apple IPv4 TCP/UDP.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include "net_discovery.h"

typedef struct { int32_t fd; int16_t events, revents; } dm_net_pollfd;
typedef struct { int32_t flags, family, type, protocol; uint32_t addrlen, pad; void *addr; char *canonname; void *next; } dm_net_addrinfo;
typedef struct { int32_t flags, family, type, protocol; uint32_t addrlen, pad; char *canonname; void *addr; void *next; } dm_net_addrinfo_legacy;
typedef enum { DM_NET_OFFLINE, DM_NET_LAN, DM_NET_LDN, DM_NET_NEXTENDO, DM_NET_AUTO } dm_net_mode_t;
dm_net_mode_t dm_net_mode(void);
// Automatic enables independent service capabilities, not a mutable global
// session type. The guest chooses sockets, LDN or online APIs from its menu.
static inline int dm_net_nextendo_enabled(void) {
    dm_net_mode_t mode = dm_net_mode();
    const char *profile = getenv("DM_NEXTENDO_ROUTES");
    // A missing profile leaves automatic local play unchanged. An explicitly
    // supplied but invalid profile still enables fail-closed service policy.
    return mode == DM_NET_NEXTENDO || (mode == DM_NET_AUTO && profile && *profile);
}
static inline int dm_net_ldn_enabled(void) {
    dm_net_mode_t mode = dm_net_mode();
    return mode == DM_NET_LDN || mode == DM_NET_AUTO;
}
int dm_net_enabled(void);
// VPN play, DM_NET_VPN=1 in the lan, ldn and auto modes: the adapter is the VPN's (net_vpn.h; hle_net_backend.c net_vpn_interface), never Wi-Fi, and discovery
// never relies on native broadcast, which a tunnel does not carry
static inline int dm_net_vpn(void) {
    const char *v = getenv("DM_NET_VPN"); dm_net_mode_t mode = dm_net_mode();
    return v && !strcmp(v, "1") && (mode == DM_NET_LAN || mode == DM_NET_LDN || mode == DM_NET_AUTO);
}
// The LDN room this device is in, as hle_ldn.c publishes it: room 0 when there is none, else the other members' addresses (network order, at most
// DM_NET_SESSION_MAX, none while the device is alone in it). In a room a game's broadcast goes to each member instead of a native broadcast or a subnet
// sweep, and alone it reaches nobody, as on an empty network; unless native broadcast is available off a VPN.
enum { DM_NET_SESSION_MAX = 7 };
void dm_net_set_session(int room, const uint32_t *members, unsigned count);
// Logs every IPv4 adapter's name, address, prefix, flags and functional type (LDN logs them as it starts)
void dm_net_log_adapters(void);
// Startup-only DNS policy: -1 refuses, 0 keeps the name, 1 supplies IPv4.
// Only explicit Nextendo/auto modes consult the provider.
typedef int (*dm_net_dns_provider)(const char *host, char *output, size_t capacity);
void dm_net_set_dns_provider(dm_net_dns_provider provider);
// Internal host API: caller owns the CLOEXEC, non-blocking duplicate. Closing or
// reusing the guest descriptor cannot attach the duplicate to another connection.
int dm_net_dup_native(int guest);
int dm_net_dup_native_generation(int guest, uint64_t *generation);
int dm_net_close_generation(int guest, uint64_t generation);
// Discovery uses native broadcast when permitted, an optional explicit peer, or
// a bounded unicast sweep of the actual adapter. Ordinary unicast is unchanged.
int dm_net_discovery_prepare(uint32_t destination, dm_net_discovery_plan *plan);
// For verified discovery queries only: 1 supplies a best-effort complementary
// unicast plan, 0 keeps native behavior. Does not alter the real capability,
// explicit peer choice, or the guest's last errno on native success.
int dm_net_discovery_supplement(uint32_t destination,dm_net_discovery_plan *plan);
// Internal native UDP sender shared by both discovery transports. Addresses
// and ports use network order. This does not apply a discovery policy itself.
long dm_net_discovery_send_native(int fd,const void *bytes,size_t size,int flags,
                                  uint32_t address,uint16_t port);
int dm_net_discovery_target(uint32_t broadcast, uint32_t *target);
void dm_net_set_multicast_allowed(int allowed);
int dm_net_multicast_allowed(void);
#ifdef DM_NET_TEST
// Test-only adapter snapshots let real loopback datagrams exercise the complete
// production send path without probing the user's LAN or enabling broadcast.
typedef struct { uint32_t address, mask; unsigned index, flags; int local_link; } dm_net_test_interface;
void dm_net_test_set_interface(const dm_net_test_interface *interface);
unsigned dm_net_test_discovery_pending(void);
typedef long (*dm_net_test_discovery_sender)(int fd,const void *bytes,size_t size,int flags,
                                             uint32_t address,uint16_t port);
void dm_net_test_set_discovery_sender(dm_net_test_discovery_sender sender);
#endif
int dm_net_errno(void);
void dm_net_set_errno(int e);
// A failed socket call also sets the game's own errno (hle_net.c): on Horizon nn::socket's errno is the C library's (detail::SetLastErrno writes
// *__errno_location()), and code such as gRPC's reads errno itself. Called by the hooks only, on the game's thread; returns r
long dm_net_guest_errno(long r);
int dm_net_guest_errno_get(void);   // the game's errno, or the host's when the game has no __errno_location
int dm_net_socket(int family, int type, int protocol);
int dm_net_close(int fd);
int dm_net_socket_type(int fd);   // 1 stream, 2 datagram, 0 none the host made; sets no errno
// Event descriptors in a network mode, in the socket table's numbering (hle_net_backend.c): Horizon's EventFd flags, Linux errno through dm_net_errno
int dm_net_eventfd(uint64_t initial, int flags);
int dm_net_eventfd_read(int fd, uint64_t *value);
int dm_net_eventfd_write(int fd, uint64_t value);
void dm_net_finalize(void);
int dm_net_bind(int fd, const void *addr, unsigned len);
int dm_net_connect(int fd, const void *addr, unsigned len);
int dm_net_listen(int fd, int backlog);
int dm_net_accept(int fd, void *addr, unsigned *len);
int dm_net_name(int fd, void *addr, unsigned *len, int peer);
int dm_net_shutdown(int fd, int how);
int dm_net_fcntl(int fd, int cmd, int flags);
int dm_net_sockopt(int fd, int level, int option, void *value, unsigned *len, int set);
long dm_net_recv(int fd, void *buf, unsigned long len, int flags, void *addr, unsigned *addrlen);
long dm_net_send(int fd, const void *buf, unsigned long len, int flags, const void *addr, unsigned addrlen);
int dm_net_poll(void *fds, uint64_t count, int timeout_ms);
int dm_net_idle(void);
int dm_net_select(int nfds, void *read, void *write, void *except, const int64_t *timeout);
int dm_net_getaddrinfo(const char *host, const char *service, const void *hints, void **result);
int dm_net_getaddrinfo_legacy(const char *host, const char *service, const void *hints, void **result);
void dm_net_freeaddrinfo(void *result);
void *dm_net_gethostbyname(const char *name);
// The cancellation identity owns no socket or registry slot until a query starts.
void *dm_net_gethostbyname_cancel(const char *name, uint32_t cancel_id);
uint32_t dm_net_request_cancel_handle(void);
int dm_net_cancel(uint32_t cancel_id);
int dm_net_ipconfig(uint32_t *address, uint32_t *mask, uint32_t *gateway, uint32_t *dns1, uint32_t *dns2);
int dm_nifm_available(void);
void dm_nifm_submit_wait(void);
