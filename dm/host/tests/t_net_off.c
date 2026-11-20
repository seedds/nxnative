// The network transport as it is offline, for a host unit test that compiles hle_offline.c without the transport (t_server_environment): DM_NETWORK is
// offline, so hle_offline.c's socket calls go to the local service (dm_sk_*) and none of these is reached; each fails if one is. Weak, so a test's own wins.
#include "../hle.h"
#include "../net_socket.h"
#include "../hle_net.h"
#define W __attribute__((weak))
W dm_net_mode_t dm_net_mode(void) { return DM_NET_OFFLINE; }
W int dm_net_enabled(void) { return 0; }
W void *guest_sym(const char *name) { (void)name; return NULL; }
W void dm_hle_wait_enter(void) { abort(); }   // the HLE_FORCE_WAIT gate's entry (entry.S); no wait hook is called here
W void dm_nifm_submit_wait(void) {}
W int dm_sk_unavailable(void) { return -1; }
W int dm_net_accept(int fd, void *addr, unsigned *len) { (void)fd; (void)addr; (void)len; return -1; }
W int dm_net_bind(int fd, const void *addr, unsigned len) { (void)fd; (void)addr; (void)len; return -1; }
W int dm_net_connect(int fd, const void *addr, unsigned len) { (void)fd; (void)addr; (void)len; return -1; }
W int dm_net_fcntl(int fd, int cmd, int flags) { (void)fd; (void)cmd; (void)flags; return -1; }
W void dm_net_finalize(void) {}
W void dm_net_freeaddrinfo(void *result) { (void)result; }
W int dm_net_getaddrinfo(const char *host, const char *service, const void *hints, void **result) { (void)host; (void)service; (void)hints; if (result) *result = NULL; return -1; }
W int dm_net_getaddrinfo_legacy(const char *host, const char *service, const void *hints, void **result) { (void)host; (void)service; (void)hints; if (result) *result = NULL; return -1; }
W void *dm_net_gethostbyname(const char *name) { (void)name; return NULL; }
W long dm_net_guest_errno(long r) { return r; }
W int dm_net_guest_errno_get(void) { return 0; }
W int dm_net_ipconfig(uint32_t *address, uint32_t *mask, uint32_t *gateway, uint32_t *dns1, uint32_t *dns2) { (void)address; (void)mask; (void)gateway; (void)dns1; (void)dns2; return -1; }
W int dm_net_listen(int fd, int backlog) { (void)fd; (void)backlog; return -1; }
W int dm_net_name(int fd, void *addr, unsigned *len, int peer) { (void)fd; (void)addr; (void)len; (void)peer; return -1; }
W long dm_net_recv(int fd, void *buf, unsigned long len, int flags, void *addr, unsigned *addrlen) { (void)fd; (void)buf; (void)len; (void)flags; (void)addr; (void)addrlen; return -1; }
W long dm_net_send(int fd, const void *buf, unsigned long len, int flags, const void *addr, unsigned addrlen) { (void)fd; (void)buf; (void)len; (void)flags; (void)addr; (void)addrlen; return -1; }
W void dm_net_set_errno(int e) { (void)e; }
W int dm_net_shutdown(int fd, int how) { (void)fd; (void)how; return -1; }
W int dm_net_socket(int family, int type, int protocol) { (void)family; (void)type; (void)protocol; return -1; }
W int dm_net_socket_type(int fd) { (void)fd; return 0; }
W int dm_net_sockopt(int fd, int level, int option, void *value, unsigned *len, int set) { (void)fd; (void)level; (void)option; (void)value; (void)len; (void)set; return -1; }
