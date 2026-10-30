// Retiring a pool after protect's lookup must leave its storage writable.
static void protect_lookup_hook(void);
#define NVN_DIRTY_PROTECT_LOOKUP_HOOK() protect_lookup_hook()
#include "../nvn/nvn_dirty.c"
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>

module_t *g_mod;
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }

static void *page;
static _Atomic int go, removing, removed;
static void protect_lookup_hook(void) {
    atomic_store(&go, 1);
    while (!atomic_load(&removing)) usleep(100);
    // Give the remover the same lookup-to-state window as the failing control.
    // With the fix it blocks on the slot mutex until protection finishes.
    usleep(100000);
}
static void *retire(void *unused) {
    (void)unused;
    while (!atomic_load(&go)) usleep(100);
    atomic_store(&removing, 1);
    nvn_dirty_remove_range(page);
    atomic_store(&removed, 1);
    return NULL;
}
int main(void) {
    size_t size = (size_t)getpagesize();
    page = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED) return 2;
    nvn_dirty_add_range(page, size);
    pthread_t worker;
    if (pthread_create(&worker, NULL, retire, NULL)) return 2;
    nvn_dirty_protect(page, size);
    pthread_join(worker, NULL);
    int fd = open("/dev/zero", O_RDONLY);
    if (fd < 0) return 2;
    ssize_t bytes = read(fd, page, 16);
    int read_error = bytes < 0 ? errno : 0;
    int untracked = !nvn_dirty_tracked(page, size);
    printf("retirement completed: %d; untracked: %d; write into retired pool: %zd bytes (expected 16), errno=%d\n",
           atomic_load(&removed), untracked, bytes, read_error);
    close(fd);
    // Also let the negative control exit cleanly instead of faulting on cleanup.
    mprotect(page, size, PROT_READ | PROT_WRITE);
    munmap(page, size);
    return bytes == 16 && atomic_load(&removed) && untracked ? 0 : 1;
}
