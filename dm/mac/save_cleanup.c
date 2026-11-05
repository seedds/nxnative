// A separate, UI-free invocation watches the owner, not a pipe's lifetime: a
// closed descriptor (or a crash with descendants still holding it) is not death.
#include "save_cleanup.h"
#include "../host/save_sync.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static pthread_mutex_t start_lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t started;

static int number(const char *s, long min, long max, long *out) {
    char *end; errno = 0;
    long n = strtol(s, &end, 10);
    if (errno || !*s || *end || n < min || n > max) return 0;
    *out = n; return 1;
}

int dm_save_cleanup_main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "--dm-save-cleanup")) return -1;
    long owner, ready;
    if (argc != 5 || !number(argv[2], 2, INT_MAX, &owner) ||
        !number(argv[3], 0, INT_MAX, &ready)) return 1;
    int fd = (int)ready, one = 1;
    // A failed/abandoned handshake must not kill us with SIGPIPE or send saves.
    if (fcntl(fd, F_SETNOSIGPIPE, one) < 0) { close(fd); return 1; }
    int q = kqueue(), error = q < 0 ? errno : 0;
    struct kevent event;
    if (!error) {
        EV_SET(&event, (uintptr_t)owner, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
        if (kevent(q, &event, 1, NULL, 0, NULL) < 0) error = errno;
    }
    ssize_t n;
    do { n = write(fd, &error, sizeof error); } while (n < 0 && errno == EINTR);
    close(fd);
    if (error || n != sizeof error) { if (q >= 0) close(q); return 1; }
    do { n = kevent(q, NULL, 0, &event, 1, NULL); } while (n < 0 && errno == EINTR);
    close(q);
    if (n != 1 || event.filter != EVFILT_PROC || event.ident != (uintptr_t)owner ||
        (event.flags & EV_ERROR) || !(event.fflags & NOTE_EXIT)) return 1;
    return save_sync_after_exit(getenv("DM_SAVE"), argv[4]) ? 0 : 1;
}

int dm_save_cleanup_start(const char *executable, const char *title) {
    const char *cloud = getenv("DM_SAVE_SYNC");
    if (!cloud || !*cloud) return 0;
    if (!executable || !*executable || !title || !*title) return EINVAL;
    pthread_mutex_lock(&start_lock);
    if (started == getpid()) { pthread_mutex_unlock(&start_lock); return 0; }
    int p[2], error = 0;
    if (pipe(p) < 0) { error = errno; goto done; }
    if (fcntl(p[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(p[1], F_SETFD, FD_CLOEXEC) < 0) {
        error = errno; goto close_pipe;
    }
    posix_spawn_file_actions_t actions;
    error = posix_spawn_file_actions_init(&actions);
    if (error) goto close_pipe;
    posix_spawnattr_t attributes;
    error = posix_spawnattr_init(&attributes);
    if (error) { posix_spawn_file_actions_destroy(&actions); goto close_pipe; }
    // Survive signals sent to the owner's foreground process group.
    error = posix_spawnattr_setpgroup(&attributes, 0);
    if (!error) error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    // Use the read end's number in the child: dup2 also clears CLOEXEC.
    if (!error) error = posix_spawn_file_actions_adddup2(&actions, p[1], p[0]);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, p[1]);
    char owner[32], ready[32];
    snprintf(owner, sizeof owner, "%d", getpid());
    snprintf(ready, sizeof ready, "%d", p[0]);
    char *args[] = {(char *)executable, "--dm-save-cleanup", owner, ready, (char *)title, NULL};
    pid_t child = -1;
    if (!error) error = posix_spawn(&child, executable, &actions, &attributes, args, environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (!error) {
        close(p[1]); p[1] = -1;
        int response; size_t got = 0;
        while (got < sizeof response) {
            ssize_t n = read(p[0], (char *)&response + got, sizeof response - got);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { error = n < 0 ? errno : EIO; break; }
            got += (size_t)n;
        }
        if (!error) error = response;
        if (!error) started = getpid();
        else while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
    }
close_pipe:
    close(p[0]); if (p[1] >= 0) close(p[1]);
done:
    pthread_mutex_unlock(&start_lock);
    return error;
}
