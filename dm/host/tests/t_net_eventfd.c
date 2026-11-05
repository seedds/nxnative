// Horizon eventfd is local process synchronization, so it remains available while network sockets are offline.
// Exercise the real hle_net.c implementation without registering its hooks in this unit test.
#include "../hle.h"
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include "../hle_net.c"
#include <assert.h>

static int sleeps;
void dm_guest_sleep_ns(int64_t ns) { assert(ns > 0); sleeps++; }

int main(void) {
    int probe = h_sk_EventFd(0, 0);
    assert(probe >= 0 && h_sk_Close(probe) == 0);     // the SDK/gRPC capability probe

    sk_pollfd unavailable[] = {{ .fd = 42, .events = SK_POLLIN }, { .fd = -1, .events = SK_POLLIN }};
    assert(dm_sk_poll(unavailable, 2, 1) == 0 && unavailable[0].revents == 0 && unavailable[1].revents == 0);

    int fd = h_sk_EventFd(2, SK_EFD_SEMAPHORE | SK_EFD_NONBLOCK | SK_EFD_CLOEXEC);
    assert(fd >= 0);
    sk_pollfd p = { .fd = fd, .events = SK_POLLIN };
    assert(dm_sk_poll(&p, 1, 0) == 1 && p.revents == SK_POLLIN);
    unsigned long value = 0;
    assert(h_sk_EventFdRead(fd, &value) == 0 && value == 1);
    assert(h_sk_EventFdRead(fd, &value) == 0 && value == 1);
    assert(h_sk_EventFdRead(fd, &value) == -1 && t_skerr == SK_EAGAIN);
    assert(dm_sk_poll(&p, 1, 2) == 0 && sleeps == 3);
    assert(h_sk_EventFdWrite(fd, 3) == 0);
    assert(dm_sk_poll(&p, 1, 0) == 1);
    sk_pollfd mixed[] = {{ .fd = fd, .events = SK_POLLIN }, { .fd = 42, .events = SK_POLLIN }};
    assert(dm_sk_poll(mixed, 2, 0) == 1 && mixed[0].revents == SK_POLLIN && mixed[1].revents == 0);
    assert(h_nsk_Close(fd) == 0);
    assert(h_sk_EventFdWrite(fd, 1) == -1 && t_skerr == SK_EBADF);

    int replacement = h_sk_EventFd(0, SK_EFD_NONBLOCK);
    assert(replacement >= 0 && replacement != fd);   // the generation rejects the stale descriptor
    assert(h_sk_EventFdRead(fd, &value) == -1 && t_skerr == SK_EBADF);
    assert(h_sk_Close(replacement) == 0);
    puts("eventfd: capability probe, counter, semaphore, poll, close and stale handles pass");
    return 0;
}
