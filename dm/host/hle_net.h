// Offline BSD descriptors: local UDP and EventFd share the service's small fd namespace. In network mode (net_socket.h) every call below goes to the host's sockets
// for the descriptors that transport owns.
#pragma once
#include <stdint.h>
int dm_sk_socket(int family, int type, int protocol);
int dm_sk_bind(int fd, const void *addr, unsigned size);
int dm_sk_connect(int fd, const void *addr, unsigned size);
int dm_sk_close(int fd);
int dm_sk_shutdown(int fd, int how);
int dm_sk_name(int fd, void *addr, unsigned *size, int peer);
int dm_sk_getopt(int fd, int level, int option, void *value, unsigned *size);
int dm_sk_setopt(int fd, int level, int option, const void *value, unsigned size);
int dm_sk_fcntl(int fd, int command, int value);
long dm_sk_recvfrom(int fd, void *data, unsigned long size, int flags, void *addr, unsigned *addr_size);
long dm_sk_sendto(int fd, const void *data, unsigned long size, int flags, const void *addr, unsigned addr_size);
long dm_sk_read(int fd, void *data, unsigned long size);
long dm_sk_write(int fd, const void *data, unsigned long size);
int dm_sk_poll(void *fds, uint64_t count, int milliseconds);
int dm_sk_select(int nfds, uint64_t *read, uint64_t *write, uint64_t *except, const int64_t *timeout);
int dm_sk_listen(int fd, int backlog);   // offline: -1, ENETDOWN
int dm_sk_accept(int fd, void *addr, unsigned *size);   // offline: -1, ENETDOWN
int dm_sk_errno(void);
void dm_sk_set_errno(int error);   // the transport in hle_net_backend.c reports through the same per-thread value
long dm_sk_ret(long result);   // a hook's result; a failure in network mode also sets the game's own errno (hle_net.c). Call on the game's thread only
int dm_sk_last_errno(void);   // GetLastErrno: the game's errno in network mode, the host's copy otherwise
