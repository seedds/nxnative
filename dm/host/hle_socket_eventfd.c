// Local BSD event counters for thread wakeups and descriptor readiness.
// These descriptors never refer to host file descriptors.
#include "hle.h"
#include "socket_eventfd.h"
#include <time.h>

typedef struct { uint64_t value, identity; unsigned flags; int open; } event_fd;
static event_fd events[DM_SOCKET_FDS];
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static uint64_t next_identity = 1;
static event_fd *lookup(int fd) { return fd >= 3 && fd < DM_SOCKET_FDS && events[fd].open ? &events[fd] : NULL; }
static uint64_t monotonic_ns(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }
static void wait_changed(int64_t ns) {
    dm_thread_native_wait(1);
    if (ns < 0) pthread_cond_wait(&changed, &mutex);
    else {
        struct timespec t = {.tv_sec = ns / 1000000000, .tv_nsec = ns % 1000000000};
        pthread_cond_timedwait_relative_np(&changed, &mutex, &t);
    }
    dm_thread_native_wait(0);
}
int dm_eventfd_create(uint64_t initial, unsigned flags) {
    if (initial == UINT64_MAX || flags > 7) return -22;
    pthread_mutex_lock(&mutex);
    int fd = 3;
    while (fd < DM_SOCKET_FDS && events[fd].open) fd++;
    if (fd == DM_SOCKET_FDS || !next_identity) { pthread_mutex_unlock(&mutex); return -24; }
    events[fd] = (event_fd){initial, next_identity++, flags, 1};
    pthread_mutex_unlock(&mutex);
    return fd;
}
int dm_eventfd_close(int fd) {
    pthread_mutex_lock(&mutex);
    event_fd *e = lookup(fd);
    if (e) { e->open = 0; pthread_cond_broadcast(&changed); }
    pthread_mutex_unlock(&mutex);
    return e ? 0 : -9;
}
int dm_eventfd_read(int fd, uint64_t *value) {
    if (!value) return -14;
    pthread_mutex_lock(&mutex);
    event_fd *e = lookup(fd);
    uint64_t identity = e ? e->identity : 0;
    int result = 0;
    for (;;) {
        if (!e || !e->open || e->identity != identity) { result = -9; break; }
        if (e->value) {
            *value = e->flags & DM_EFD_SEMAPHORE ? 1 : e->value;
            e->value -= *value;
            pthread_cond_broadcast(&changed);
            break;
        }
        if (e->flags & DM_EFD_NONBLOCK) { result = -11; break; }
        wait_changed(-1);
    }
    pthread_mutex_unlock(&mutex);
    return result;
}
int dm_eventfd_write(int fd, uint64_t value) {
    if (value == UINT64_MAX) return -22;
    pthread_mutex_lock(&mutex);
    event_fd *e = lookup(fd);
    uint64_t identity = e ? e->identity : 0;
    int result = 0;
    for (;;) {
        if (!e || !e->open || e->identity != identity) { result = -9; break; }
        if (value < UINT64_MAX - e->value) {
            e->value += value;
            pthread_cond_broadcast(&changed);
            break;
        }
        if (e->flags & DM_EFD_NONBLOCK) { result = -11; break; }
        wait_changed(-1);
    }
    pthread_mutex_unlock(&mutex);
    return result;
}
int dm_eventfd_poll(dm_socket_pollfd *fds, size_t count, int64_t timeout_ns) {
    if (count > DM_SOCKET_FDS) return -22;
    if (count && !fds) return -14;
    // Keep the identities across the wait. Close followed by reuse must not
    // turn an old poll into a wait on somebody else's counter.
    uint64_t *identities = calloc(count ? count : 1, sizeof *identities);
    if (!identities) return -12;
    uint64_t start = monotonic_ns();
    pthread_mutex_lock(&mutex);
    for (size_t i = 0; i < count; i++) {
        event_fd *e = lookup(fds[i].fd);
        if (e) identities[i] = e->identity;
    }
    int ready;
    for (;;) {
        ready = 0;
        for (size_t i = 0; i < count; i++) {
            dm_socket_pollfd *p = &fds[i]; p->revents = 0;
            if (p->fd < 0) continue;
            event_fd *e = lookup(p->fd);
            if (!e || e->identity != identities[i]) p->revents = 0x20; // POLLNVAL
            else {
                if (e->value) p->revents |= p->events & 0x41; // IN/RDNORM
                if (e->value < UINT64_MAX - 1) p->revents |= p->events & 0x104; // OUT/WRNORM
            }
            ready += p->revents != 0;
        }
        if (ready) break;
        int64_t remaining = -1;
        if (timeout_ns >= 0) {
            uint64_t elapsed = monotonic_ns() - start;
            if (elapsed >= (uint64_t)timeout_ns) break;
            remaining = timeout_ns - (int64_t)elapsed;
        }
        wait_changed(remaining);
    }
    pthread_mutex_unlock(&mutex);
    free(identities);
    return ready;
}
int dm_eventfd_select(int nfds, uint64_t *r, uint64_t *w, uint64_t *e, const int64_t *tv) {
    if (nfds < 0 || nfds > DM_SOCKET_FDS) return -22;
    int64_t ns = -1;
    if (tv) {
        if (tv[0] < 0 || tv[1] < 0 || tv[1] >= 1000000 || tv[0] > (INT64_MAX - tv[1] * 1000) / 1000000000) return -22;
        ns = tv[0] * 1000000000 + tv[1] * 1000;
    }
    dm_socket_pollfd *p = calloc(nfds ? (size_t)nfds : 1, sizeof *p);
    if (!p) return -12;
    size_t n = 0;
    for (int fd = 0; fd < nfds; fd++) {
        uint64_t bit = UINT64_C(1) << (fd & 63);
        int want_r = r && (r[fd / 64] & bit), want_w = w && (w[fd / 64] & bit), want_e = e && (e[fd / 64] & bit);
        if (want_r || want_w || want_e) p[n++] = (dm_socket_pollfd){fd, (int16_t)((want_r ? 1 : 0) | (want_w ? 4 : 0)), 0};
    }
    int result = dm_eventfd_poll(p, n, ns);
    for (size_t i = 0; result >= 0 && i < n; i++) if (p[i].revents & 0x20) result = -9;
    if (result >= 0) {
        if (r) memset(r, 0, DM_SOCKET_FDS / 8);
        if (w) memset(w, 0, DM_SOCKET_FDS / 8);
        if (e) memset(e, 0, DM_SOCKET_FDS / 8);
        result = 0;
        for (size_t i = 0; i < n; i++) {
            int fd = p[i].fd; uint64_t bit = UINT64_C(1) << (fd & 63);
            if (r && (p[i].revents & 1)) { r[fd / 64] |= bit; result++; }
            if (w && (p[i].revents & 4)) { w[fd / 64] |= bit; result++; }
        }
    }
    free(p);
    return result;
}
