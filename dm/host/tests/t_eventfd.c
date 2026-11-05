// NPLN/gRPC needs a working local wakeup descriptor while the network is off.
// Exercise the exported HLE ABI and real blocking producer/consumer threads.
#include "../hle_net.c"
#include <mach-o/getsect.h>
#include <unistd.h>

static int checks, fails;
static _Thread_local int guest_errno;
static _Atomic unsigned waits, resumed;
static int *errno_location(void) { return &guest_errno; }
void *guest_sym(const char *name) { return !strcmp(name, "__errno_location") ? errno_location : NULL; }
void dm_guest_sleep_ns(int64_t ns) { struct timespec t = {ns / 1000000000, ns % 1000000000}; nanosleep(&t, NULL); }
void dm_thread_native_wait(int entering) { atomic_fetch_add(entering ? &waits : &resumed, 1); }
extern const struct mach_header_64 _mh_execute_header;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static void *hook(const char *name) {
    unsigned long size = 0; unsigned found = 0; void *fn = NULL;
    const hle_force_ent *e = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &size);
    for (size_t i = 0; e && i < size / sizeof *e; i++) if (!strcmp(e[i].name, name)) { found++; fn = e[i].fn; }
    CHECK(found == 1 && fn, name); return fn;
}
typedef struct { int fd, kind, result; uint64_t value; dm_socket_pollfd poll; int error; } worker;
static void *run_worker(void *arg) {
    worker *w = arg;
    if (w->kind == 0) w->result = h_sk_EventFdRead(w->fd, &w->value);
    else if (w->kind == 1) w->result = h_sk_EventFdWrite(w->fd, w->value);
    else { w->poll = (dm_socket_pollfd){w->fd, 1, 0}; w->result = dm_sk_poll(&w->poll, 1, -1); }
    w->error = guest_errno;
    return NULL;
}
static void blocked(unsigned previous) {
    uint64_t end = clock_gettime_nsec_np(CLOCK_MONOTONIC) + UINT64_C(5000000000);
    while (atomic_load(&waits) == previous && clock_gettime_nsec_np(CLOCK_MONOTONIC) < end) usleep(100);
    CHECK(atomic_load(&waits) > previous, "worker entered a native wait");
}
int main(void) {
    int (*create)(uint64_t, unsigned) = hook("_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE");
    int (*read_counter)(int, uint64_t *) = hook("_ZN2nn6socket11EventFdReadEiPm");
    int (*write_counter)(int, uint64_t) = hook("nnsocketEventFdWrite");
    int (*close_counter)(int) = hook("nnsocketClose");
    long (*read_bytes)(int, void *, unsigned long) = hook("nnsocketRead");
    long (*write_bytes)(int, const void *, unsigned long) = hook("_ZN2nn6socket5WriteEiPKvm");
    if (!create || !read_counter || !write_counter || !close_counter || !read_bytes || !write_bytes) return 1;
    CHECK(create(UINT64_MAX, 0) == -1 && guest_errno == 22, "reject initial UINT64_MAX");
    CHECK(create(0, 8) == -1 && guest_errno == 22, "reject unknown flags");
    int fd = create(0, 6); // exact gRPC wakeup flags: close-on-exec + nonblocking
    CHECK(fd >= 3, "create gRPC wakeup descriptor");
    uint64_t value = 123;
    CHECK(read_counter(fd, &value) == -1 && value == 123 && guest_errno == 11 && h_sk_GetLastError() == 11, "empty read is EAGAIN in guest errno and socket error");
    CHECK(write_counter(fd, 2) == 0 && write_counter(fd, 3) == 0, "accumulate writes");
    dm_socket_pollfd poll[] = {{fd, 0x145, 0}, {-1, 1, 123}};
    CHECK(dm_sk_poll(poll, 2, 0) == 1 && poll[0].revents == 0x145 && !poll[1].revents, "poll read/write aliases and ignored negative descriptor");
    CHECK(read_counter(fd, &value) == 0 && value == 5, "ordinary read drains entire counter");
    poll[0].events = 1;
    CHECK(dm_sk_poll(poll, 1, 0) == 0 && !poll[0].revents, "drained counter is not readable");
    CHECK(write_counter(fd, UINT64_MAX) == -1 && guest_errno == 22, "UINT64_MAX write is invalid");
    CHECK(write_counter(fd, UINT64_MAX - 1) == 0, "maximum counter value");
    poll[0].events = 5;
    CHECK(dm_sk_poll(poll, 1, 0) == 1 && poll[0].revents == 1, "full counter is readable but not writable");
    CHECK(write_counter(fd, 1) == -1 && guest_errno == 11, "overflow is EAGAIN, not a wrapped counter");
    CHECK(write_counter(fd, 0) == 0, "zero write to full counter succeeds");
    CHECK(read_counter(fd, &value) == 0 && value == UINT64_MAX - 1, "failed overflowing write leaves value intact");
    uint8_t bytes[16]; memset(bytes, 0xa5, sizeof bytes); value = 9;
    CHECK(write_bytes(fd, &value, 8) == 8, "generic write returns byte count");
    CHECK(read_bytes(fd, bytes, 7) == -1 && guest_errno == 22, "short read rejected without consuming counter");
    CHECK(read_bytes(fd, bytes + 1, 15) == 8, "generic read permits unaligned and oversized buffer");
    memcpy(&value, bytes + 1, 8);
    CHECK(value == 9 && bytes[0] == 0xa5 && bytes[9] == 0xa5, "generic read writes exactly eight bytes");
    uint64_t r[16] = {0}, w[16] = {0}, e[16] = {0}; int64_t timeout[2] = {0};
    r[fd / 64] = w[fd / 64] = e[fd / 64] = UINT64_C(1) << (fd & 63);
    CHECK(dm_sk_select(fd + 1, r, w, e, timeout) == 1 && !r[fd / 64] && w[fd / 64] && !e[fd / 64], "select writable counter and clear unready sets");
    poll[0].events = 1; uint64_t begin = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    CHECK(dm_sk_poll(poll, 1, 10) == 0 && clock_gettime_nsec_np(CLOCK_MONOTONIC) - begin >= 9000000, "empty poll waits for its timeout");
    CHECK(close_counter(fd) == 0, "close counter");
    CHECK(close_counter(fd) == -1 && guest_errno == 9, "double close reports EBADF");
    CHECK(dm_sk_poll(poll, 1, -1) == 1 && poll[0].revents == 0x20, "closed poll descriptor returns POLLNVAL immediately");
    r[fd / 64] = UINT64_C(1) << (fd & 63);
    CHECK(dm_sk_select(fd + 1, r, NULL, NULL, timeout) == -1 && guest_errno == 9 && r[fd / 64], "select invalid descriptor returns EBADF with sets intact");
    fd = create(2, 5);
    CHECK(read_counter(fd, &value) == 0 && value == 1 && read_counter(fd, &value) == 0 && value == 1, "semaphore reads consume one each");
    CHECK(read_counter(fd, &value) == -1 && guest_errno == 11, "empty semaphore is EAGAIN");
    close_counter(fd);
    fd = create(0, 0); pthread_t thread;
    worker job = {.fd = fd, .kind = 0}; unsigned previous = atomic_load(&waits);
    pthread_create(&thread, NULL, run_worker, &job); blocked(previous);
    CHECK(write_counter(fd, 7) == 0, "wake blocked reader"); pthread_join(thread, NULL);
    CHECK(job.result == 0 && job.value == 7, "blocked reader receives producer's value");
    write_counter(fd, UINT64_MAX - 1);
    job = (worker){.fd = fd, .kind = 1, .value = 4}; previous = atomic_load(&waits);
    pthread_create(&thread, NULL, run_worker, &job); blocked(previous);
    CHECK(read_counter(fd, &value) == 0 && value == UINT64_MAX - 1, "draining wakes blocked writer"); pthread_join(thread, NULL);
    CHECK(job.result == 0 && read_counter(fd, &value) == 0 && value == 4, "blocked writer preserves its value");
    job = (worker){.fd = fd, .kind = 2}; previous = atomic_load(&waits);
    pthread_create(&thread, NULL, run_worker, &job); blocked(previous);
    write_counter(fd, 1); pthread_join(thread, NULL);
    CHECK(job.result == 1 && job.poll.revents == 1 && read_counter(fd, &value) == 0 && value == 1, "write wakes infinite poll without consuming value");
    job = (worker){.fd = fd, .kind = 2}; previous = atomic_load(&waits);
    pthread_create(&thread, NULL, run_worker, &job); blocked(previous);
    close_counter(fd); int replacement = create(12, 4); pthread_join(thread, NULL);
    CHECK(replacement == fd && job.result == 1 && job.poll.revents == 0x20, "close/reuse cannot redirect an existing poll");
    CHECK(read_counter(replacement, &value) == 0 && value == 12, "replacement counter is untouched");
    close_counter(replacement);
    CHECK(atomic_load(&waits) == atomic_load(&resumed), "all native waits balance on return");
    CHECK(dm_sk_poll(NULL, 1, 0) == -1 && guest_errno == 14, "poll null array is EFAULT");
    CHECK(dm_sk_poll(NULL, DM_SOCKET_FDS + 1, 0) == -1 && guest_errno == 22, "poll rejects excessive count");
    printf("eventfd: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
