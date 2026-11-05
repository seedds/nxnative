// eventfd unit test (hle_net.c): the counter's read / write semantics, the semaphore flag, nonblocking reads and their errno (the socket error code and the C library's),
// poll and select readiness, a poll woken by a write from another thread, fcntl's O_NONBLOCK, close, and that other descriptors are offline sockets (a poll on one reports nothing ready).
// gRPC's poller (The Legend of Zelda: Ocarina of Time) checks for an eventfd, then drains one with EAGAIN and EINTR handled as on Linux. Expected values are written by hand.
#include "../hle_net.c"
void dm_guest_sleep_ns(int64_t ns) { struct timespec t = {ns / 1000000000, ns % 1000000000}; nanosleep(&t, NULL); }
static int g_errno;
static int *errno_loc(void) { return &g_errno; }   // the guest's __errno_location
void *guest_sym(const char *name) { return !strcmp(name, "__errno_location") ? (void *)errno_loc : NULL; }
static int checks, fails;
#define CHECK(c, s) do { checks++; int ok_ = !!(c); fails += !ok_; printf("%s %s\n", ok_ ? "✓" : "✗", s); } while (0)
static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static int g_wfd;
static void *late_write(void *p) { (void)p; dm_guest_sleep_ns(50000000); h_sk_EventFdWrite(g_wfd, 3); return NULL; }

int main(void) {
    uint64_t v = 0;
    // gRPC's availability check: EventFd(0, 0), then close
    int a = h_sk_EventFd(0, 0);
    CHECK(a >= 512 && a < 1024, "EventFd(0, 0) gives a descriptor below select's 1024");
    CHECK(h_nsk_Close(a) == 0, "nnsocketClose closes it");
    CHECK(h_sk_EventFdWrite(a, 1) == -1 && t_skerr == SK_EBADF && g_errno == LIBC_EBADF, "a closed eventfd: EBADF");

    // counter semantics, nonblocking (gRPC's flags 6)
    int fd = h_sk_EventFd(5, 6);
    CHECK(h_sk_EventFdRead(fd, &v) == 0 && v == 5, "read takes the initial value");
    g_errno = 0;
    CHECK(h_sk_EventFdRead(fd, &v) == -1 && t_skerr == SK_EAGAIN && g_errno == LIBC_EAGAIN, "an empty nonblocking read: EAGAIN (socket code 35, errno 11)");
    CHECK(h_sk_EventFdWrite(fd, 2) == 0 && h_sk_EventFdWrite(fd, 3) == 0, "writes add");
    CHECK(h_sk_EventFdRead(fd, &v) == 0 && v == 5, "read takes their sum and resets");
    CHECK(h_sk_EventFdWrite(fd, UINT64_MAX) == -1 && g_errno == LIBC_EINVAL, "writing 2^64-1: EINVAL");

    // semaphore flag: one at a time
    int sem = h_sk_EventFd(2, EFD_SEMAPHORE | EFD_NONBLOCK);
    CHECK(h_sk_EventFdRead(sem, &v) == 0 && v == 1 && h_sk_EventFdRead(sem, &v) == 0 && v == 1 && h_sk_EventFdRead(sem, &v) == -1, "semaphore: 1 per read, then EAGAIN");

    // poll: readable only while the counter is not zero; writable always
    sk_pollfd p[2] = {{fd, SK_POLLIN, 0}, {sem, SK_POLLIN | SK_POLLOUT, 0}};
    uint64_t t0 = now_ms();
    CHECK(dm_sk_poll(p, 2, 0) == 1 && p[0].revents == 0 && p[1].revents == SK_POLLOUT, "poll with timeout 0: nothing readable, one writable");
    CHECK(now_ms() - t0 < 50, "a zero timeout does not wait");
    p[1].events = SK_POLLIN;
    t0 = now_ms();
    CHECK(dm_sk_poll(p, 2, 30) == 0 && now_ms() - t0 >= 25, "nothing ready: the timeout passes, 0");
    g_wfd = fd; pthread_t th; pthread_create(&th, NULL, late_write, NULL);
    t0 = now_ms();
    int got = dm_sk_poll(p, 2, 2000);
    CHECK(got == 1 && p[0].revents == SK_POLLIN && now_ms() - t0 < 1000, "a write from another thread wakes the poll");
    pthread_join(th, NULL);
    CHECK(h_sk_EventFdRead(fd, &v) == 0 && v == 3, "and its value is read");

    // select on the same descriptors
    uint64_t rset[16] = {0}, wset[16] = {0}; int64_t tv[2] = {0, 0};
    rset[fd >> 6] |= 1ull << (fd & 63); wset[sem >> 6] |= 1ull << (sem & 63);
    h_sk_EventFdWrite(fd, 1);
    CHECK(dm_sk_select(1024, rset, wset, NULL, tv) == 2 && (rset[fd >> 6] >> (fd & 63) & 1) && (wset[sem >> 6] >> (sem & 63) & 1), "select: readable and writable set");
    h_sk_EventFdRead(fd, &v);
    memset(rset, 0, sizeof rset); rset[fd >> 6] |= 1ull << (fd & 63);
    CHECK(dm_sk_select(1024, rset, NULL, NULL, tv) == 0 && !(rset[fd >> 6] >> (fd & 63) & 1), "select: an empty eventfd is cleared from the read set");

    // fcntl: O_NONBLOCK on a blocking eventfd
    int b = h_sk_EventFd(0, 0);
    CHECK(h_nsk_Fcntl(b, SK_F_GETFL, 0) == 0, "F_GETFL of a blocking eventfd: 0");
    CHECK(h_nsk_Fcntl(b, SK_F_SETFL, SK_O_NONBLOCK) == 0 && h_nsk_Fcntl(b, SK_F_GETFL, 0) == SK_O_NONBLOCK, "F_SETFL O_NONBLOCK sticks");
    CHECK(h_sk_EventFdRead(b, &v) == -1 && g_errno == LIBC_EAGAIN, "and its empty read no longer blocks");

    // everything else stays offline
    CHECK(h_nsk_Socket(2, 1, 0) == -1 && t_skerr == SK_ENETDOWN, "socket(): still ENETDOWN");
    sk_pollfd other = {7, SK_POLLIN, 0};
    other.revents = -1;
    CHECK(dm_sk_poll(&other, 1, 0) == 0 && other.revents == 0, "poll on a descriptor the host never made: nothing ready at the timeout (Star Fox)");
    h_sk_EventFdWrite(fd, 1);
    sk_pollfd mixed[2] = {{7, SK_POLLIN, -1}, {fd, SK_POLLIN, 0}};
    CHECK(dm_sk_poll(mixed, 2, 0) == 1 && mixed[0].revents == 0 && mixed[1].revents == SK_POLLIN, "with an eventfd beside it: only the eventfd is ready");
    h_sk_EventFdRead(fd, &v);
    CHECK(h_sk_Close(7) == -1, "close of one: fails");
    h_sk_Close(fd); h_sk_Close(sem); h_sk_Close(b);
    printf("eventfd: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
