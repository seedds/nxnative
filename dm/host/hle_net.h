// Offline BSD descriptors: local UDP and EventFd share the service's small fd namespace.
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
int dm_sk_errno(void);
