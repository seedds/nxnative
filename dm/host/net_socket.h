// Network-mode nn::socket transport (hle_net_backend.c): real Darwin IPv4 TCP/UDP sockets behind the guest's BSD ABI.
// Off by default. DM_NETWORK=lan turns it on; with it off, hle_net.c's offline local UDP/EventFd service answers every call exactly as before.
// The guest's descriptors here start at DM_NET_FD_BASE, above the offline service's range (0..63), so both fit one fd_set namespace. In network mode the event descriptors
// are this table's too (dm_net_eventfd), so one host poll waits on sockets and event descriptors together.
#pragma once
#include <stdint.h>

enum { DM_NET_FD_BASE = 64, DM_NET_FD_END = 1024 };   // fd_set holds 1024 descriptors
int dm_net_enabled(void);                  // DM_NETWORK=lan, read once
static inline int dm_net_owns(int fd) { return fd >= DM_NET_FD_BASE && fd < DM_NET_FD_END; }

// Each call sets the guest's last socket error (Horizon's Linux errno numbers) through dm_sk_set_errno on failure.
int dm_net_socket(int family, int type, int protocol);
int dm_net_close(int fd);
int dm_net_bind(int fd, const void *addr, unsigned len);
int dm_net_connect(int fd, const void *addr, unsigned len);
int dm_net_listen(int fd, int backlog);
int dm_net_accept(int fd, void *addr, unsigned *len);
int dm_net_name(int fd, void *addr, unsigned *len, int peer);
int dm_net_shutdown(int fd, int how);
int dm_net_fcntl(int fd, int cmd, int value);
int dm_net_sockopt(int fd, int level, int option, void *value, unsigned *len, int set);
long dm_net_recv(int fd, void *buf, unsigned long len, int flags, void *addr, unsigned *addrlen);
long dm_net_send(int fd, const void *buf, unsigned long len, int flags, const void *addr, unsigned addrlen);
// Event descriptors in network mode: Horizon's EventFd flags (bit 0 semaphore, bits 1 and 2 never block); a socket call on one fails with ENOTSOCK 88
int dm_net_eventfd(uint64_t initial, int flags);
int dm_net_eventfd_read(int fd, uint64_t *value);
int dm_net_eventfd_write(int fd, uint64_t value);
int dm_net_is_event(int fd);   // sets no errno
// Readiness of network descriptors only: entries outside this transport's range are left untouched (revents 0); the caller merges offline ones
int dm_net_poll(void *fds, uint64_t count, int timeout_ms);
// nifm: the IPv4 address and mask of the first up, running, broadcast-capable non-loopback interface. 0 on success
int dm_net_ipconfig(uint32_t *address, uint32_t *mask);
