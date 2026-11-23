// BSD descriptors and network helpers: local UDP/EventFd offline, plus real host TCP/UDP/DNS/TLS when DM_NEXTENDO=1.
#pragma once
#include <stdint.h>
int dm_sk_socket(int family, int type, int protocol);
int dm_sk_bind(int fd, const void *addr, unsigned size);
int dm_sk_connect(int fd, const void *addr, unsigned size);
int dm_sk_listen(int fd, int backlog);
int dm_sk_accept(int fd, void *addr, unsigned *size);
int dm_sk_close(int fd);
int dm_sk_shutdown(int fd, int how);
int dm_sk_name(int fd, void *addr, unsigned *size, int peer);
int dm_sk_getopt(int fd, int level, int option, void *value, unsigned *size);
int dm_sk_setopt(int fd, int level, int option, const void *value, unsigned size);
int dm_sk_fcntl(int fd, int command, int value);
long dm_sk_recvfrom(int fd, void *data, unsigned long size, int flags, void *addr, unsigned *addrlen);
long dm_sk_sendto(int fd, const void *data, unsigned long size, int flags, const void *addr, unsigned addrlen);
long dm_sk_sendmsg(int fd, const void *msg, int flags);
long dm_sk_recvmsg(int fd, void *msg, int flags);
long dm_sk_read(int fd, void *data, unsigned long size);
long dm_sk_write(int fd, const void *data, unsigned long size);
int dm_sk_poll(void *fds, uint64_t count, int milliseconds);
int dm_sk_select(int nfds, uint64_t *read, uint64_t *write, uint64_t *except, const int64_t *timeout);
int dm_sk_errno(void);
int dm_sk_host_fd(int fd);
int dm_sk_getaddrinfo(const char *host, const char *service, const void *hint, void **res);
int dm_sk_getaddrinfo_ex(const char *host, const char *service, const void *hint, void **res, int posix_layout);
void dm_sk_freeaddrinfo(void *ai);
void *dm_sk_gethostbyname(const char *name);
int dm_nifm_get_ip(uint32_t *ip_net_order);
int dm_nifm_get_ip_config(uint32_t *ip, uint32_t *mask, uint32_t *gw, uint32_t *dns1, uint32_t *dns2);
