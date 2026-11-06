// nn::socket::EventFd offline (hle_net.c): a descriptor that works with no network, which gRPC's poll engine needs at startup, and that Poll waits on. Socket and the rest stay offline
#include "../hle_net.c"
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

void dm_guest_sleep_ns(int64_t ns) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; nanosleep(&ts, 0); }
void dm_guest_cond_wait_ns(pthread_cond_t *c, pthread_mutex_t *m, int64_t ns) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; pthread_cond_timedwait_relative_np(c, m, &ts); }
uint64_t guest_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec; }
int dm_detclock_enabled(void) { return 0; }

static void *writer(void *fd) { dm_guest_sleep_ns(20000000); assert(h_sk_EventFdWrite((int)(intptr_t)fd, 1) == 0); return NULL; }

int main(void) {
    int fd = h_sk_EventFd(0, 6);   // the flags gRPC creates its descriptor with: never blocks
    assert(fd > 0);
    unsigned long v = 99;
    assert(h_sk_EventFdRead(fd, &v) == -1 && h_sk_GetLastError() == SK_EAGAIN && v == 99);

    sk_pollfd p = {fd, SK_POLLIN, 0x7f};
    uint64_t t0 = guest_ns();
    assert(h_nsk_Poll(&p, 1, 20) == 0 && p.revents == 0);   // nothing written: the timeout passes
    assert(guest_ns() - t0 >= 15000000);

    assert(h_sk_EventFdWrite(fd, 2) == 0 && h_sk_EventFdWrite(fd, 3) == 0);
    assert(h_sk_Poll(&p, 1, 1000) == 1 && p.revents == SK_POLLIN);
    assert(h_sk_EventFdRead(fd, &v) == 0 && v == 5);   // the counter, whole, then empty
    assert(h_sk_EventFdRead(fd, &v) == -1);

    // A write from another thread ends a wait at once, and an endless wait comes back with 0 now and then, as with nothing to wait on
    pthread_t th; pthread_create(&th, NULL, writer, (void *)(intptr_t)fd);
    t0 = guest_ns();
    assert(h_sk_Poll(&p, 1, 10000) == 1 && p.revents == SK_POLLIN);
    assert(guest_ns() - t0 < 5000000000u);
    pthread_join(th, NULL);
    assert(h_sk_EventFdRead(fd, &v) == 0 && v == 1);
    t0 = guest_ns();
    assert(h_sk_Poll(&p, 1, -1) == 0 && guest_ns() - t0 < 1000000000u);

    sk_pollfd w = {fd, SK_POLLOUT, 0};   // writable whenever it is open
    assert(h_sk_Poll(&w, 1, 0) == 1 && w.revents == SK_POLLOUT);

    int sem = h_sk_EventFd(2, 1 | 2);   // one at a time
    assert(sem > 0 && sem != fd);
    assert(h_sk_EventFdRead(sem, &v) == 0 && v == 1 && h_sk_EventFdRead(sem, &v) == 0 && v == 1 && h_sk_EventFdRead(sem, &v) == -1);

    assert(h_nsk_Close(fd) == 0 && h_sk_Close(sem) == 0);
    assert(h_nsk_Close(fd) == -1);   // closed already
    assert(h_sk_EventFdWrite(fd, 1) == -1 && h_sk_GetLastError() == SK_EBADF);
    assert(h_sk_EventFdRead(fd, &v) == -1 && h_sk_GetLastError() == SK_EBADF);

    // The table is reused and bounded
    int all[EFD_MAX];
    for (int i = 0; i < EFD_MAX; i++) { all[i] = h_sk_EventFd(0, 6); assert(all[i] > 0); }
    assert(h_sk_EventFd(0, 6) == -1 && h_sk_GetLastError() == 24);
    for (int i = 0; i < EFD_MAX; i++) assert(h_sk_Close(all[i]) == 0);

    // What needs the network stays offline
    assert(h_nsk_Socket(2, 1, 0) == -1 && h_sk_GetLastError() == SK_ENETDOWN);
    sk_pollfd bad = {5, SK_POLLIN, 0};
    assert(h_nsk_Poll(&bad, 1, 0) == 0 && bad.revents == 0);   // one the host never made: nothing ready, as a socket with nothing to say
    sk_pollfd none = {-1, SK_POLLIN, 0x7f};
    assert(h_nsk_Poll(&none, 1, 1) == 0 && none.revents == 0);
    puts("ok t_net_eventfd");
    return 0;
}
