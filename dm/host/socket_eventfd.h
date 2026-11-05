// BSD-local event counters. Return a descriptor/count/0 on success, or a
// negative Horizon errno (not the host's errno) on failure.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { DM_SOCKET_FDS = 1024, DM_EFD_SEMAPHORE = 1, DM_EFD_CLOEXEC = 2, DM_EFD_NONBLOCK = 4 };
typedef struct { int32_t fd; int16_t events, revents; } dm_socket_pollfd;
int dm_eventfd_create(uint64_t initial, unsigned flags);
int dm_eventfd_close(int fd);
int dm_eventfd_read(int fd, uint64_t *value);
int dm_eventfd_write(int fd, uint64_t value);
int dm_eventfd_poll(dm_socket_pollfd *fds, size_t count, int64_t timeout_ns);
int dm_eventfd_select(int nfds, uint64_t *read_set, uint64_t *write_set, uint64_t *error_set, const int64_t *timeval);
