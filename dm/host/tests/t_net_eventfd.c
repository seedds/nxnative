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
void *guest_sym(const char *name) { (void)name; return NULL; }   // no game: a failed socket call keeps only the host's errno (hle_net.c dm_net_guest_errno)
// hle_net.c sits on the socket backend (hle_net_backend.c, which brackets its host waits) and its HLE_FORCE_WAIT entries branch to entry.S's
// dm_hle_wait_enter. The test calls the hooks directly, offline, so the backend's waits need no guest bookkeeping and no guest call reaches the entry.
void dm_guest_host_wait_begin(void) {}
void dm_guest_host_wait_end(void) {}
void dm_hle_wait_enter(void) { abort(); }

static void *writer(void *fd) { dm_guest_sleep_ns(20000000); assert(h_sk_EventFdWrite((int)(intptr_t)fd, 1) == 0); return NULL; }
static void *closer(void *fd) { dm_guest_sleep_ns(20000000); assert(h_sk_Close((int)(intptr_t)fd) == 0); return NULL; }

int main(void) {
    int fd = h_sk_EventFd(0, 6);   // the flags gRPC creates its descriptor with: never blocks
    assert(fd == 0);   // the bsd service's numbers: the lowest free one, from 0
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

    // Select is Poll over its sets (Minecraft's NetherNet sets its descriptor's bit in a 1024-bit fd_set on the stack and waits on it this way)
    uint64_t rs[16] = {0}, ws[16] = {0}; int64_t tv[2] = {0, 20000};
    rs[fd >> 6] |= 1ull << (fd & 63);
    t0 = guest_ns();
    assert(h_sk_Select(fd + 1, rs, NULL, NULL, tv) == 0 && !(rs[fd >> 6] >> (fd & 63) & 1));   // nothing written: the timeout passes and the set comes back empty
    assert(guest_ns() - t0 >= 15000000);
    assert(h_sk_EventFdWrite(fd, 1) == 0);
    rs[fd >> 6] |= 1ull << (fd & 63); ws[fd >> 6] |= 1ull << (fd & 63); ws[0] |= 1ull << 40;   // with a descriptor the host never made beside it: not ready
    assert(h_sk_Select(41, rs, ws, NULL, tv) == 2 && rs[fd >> 6] >> (fd & 63) & 1 && ws[fd >> 6] >> (fd & 63) & 1 && !(ws[0] >> 40 & 1));
    assert(h_sk_EventFdRead(fd, &v) == 0 && v == 1);
    uint64_t only[16] = {[15] = 1ull << 63};   // only a descriptor the host never made (1023): nothing ready at the timeout, as Poll
    t0 = guest_ns();
    assert(h_sk_Select(1024, only, NULL, NULL, tv) == 0 && only[15] == 0 && guest_ns() - t0 >= 15000000);
    assert(h_sk_Select(0, NULL, NULL, NULL, tv) == 0);

    int sem = h_sk_EventFd(2, 1 | 2);   // one at a time
    assert(sem == 1);
    assert(h_sk_EventFdRead(sem, &v) == 0 && v == 1 && h_sk_EventFdRead(sem, &v) == 0 && v == 1 && h_sk_EventFdRead(sem, &v) == -1);

    assert(h_nsk_Close(fd) == 0 && h_sk_EventFd(0, 6) == 0 && h_nsk_Close(0) == 0);   // the lowest free number is taken again
    // Closed during a Select: EBADF, as select
    uint64_t cs[16] = {0}; int64_t tv1[2] = {5, 0};
    cs[0] = 1ull << sem;
    pthread_create(&th, NULL, closer, (void *)(intptr_t)sem);
    assert(h_sk_Select(sem + 1, cs, NULL, NULL, tv1) == -1 && h_sk_GetLastError() == SK_EBADF);
    pthread_join(th, NULL);
    assert(h_nsk_Close(fd) == -1);   // closed already
    assert(h_sk_EventFdWrite(fd, 1) == -1 && h_sk_GetLastError() == SK_EBADF);
    assert(h_sk_EventFdRead(fd, &v) == -1 && h_sk_GetLastError() == SK_EBADF);

    // The table is reused and bounded
    int all[EFD_MAX];
    for (int i = 0; i < EFD_MAX; i++) { all[i] = h_sk_EventFd(0, 6); assert(all[i] == i); }
    assert(h_sk_EventFd(0, 6) == -1 && h_sk_GetLastError() == 24);
    for (int i = 0; i < EFD_MAX; i++) assert(h_sk_Close(all[i]) == 0);

    // What needs the network stays offline
    assert(h_nsk_Socket(2, 1, 0) == -1 && h_sk_GetLastError() == SK_ENETDOWN);
    sk_pollfd bad = {EFD_MAX, SK_POLLIN, 0};
    assert(h_nsk_Poll(&bad, 1, 0) == 0 && bad.revents == 0);   // one the host never made: nothing ready, as a socket with nothing to say
    sk_pollfd none = {-1, SK_POLLIN, 0x7f};
    assert(h_nsk_Poll(&none, 1, 1) == 0 && none.revents == 0);
    puts("ok t_net_eventfd");
    return 0;
}
