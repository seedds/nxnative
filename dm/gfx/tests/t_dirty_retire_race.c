// A pool retired while protect holds its descriptor must finish writable.
// Stage retirement after lookup, before the first protection-state update.
static void protect_lookup_hook(void);
#define NVN_DIRTY_PROTECT_LOOKUP_HOOK() protect_lookup_hook()
#include "../nvn/nvn_dirty.c"
#include <fcntl.h>
#include <stdio.h>

module_t g_mod[MAX_MOD];
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);

static void *page;
static _Atomic int go, removing, removed;
static void protect_lookup_hook(void) {
    atomic_store(&go, 1);
    while (!atomic_load(&removing)) usleep(100);
    // The unfixed implementation lets retirement complete in this window.
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
    pthread_create(&worker, NULL, retire, NULL);
    nvn_dirty_protect(page, size);
    pthread_join(worker, NULL);
    int fd = open("/dev/zero", O_RDONLY);
    ssize_t bytes = read(fd, page, 16);
    printf("retirement completed: %d; write into retired pool: %zd bytes (expected 16)\n", atomic_load(&removed), bytes);
    close(fd);
    // Restore permissions so the failing control exits without a second fault.
    mprotect(page, size, PROT_READ | PROT_WRITE);
    munmap(page, size);
    return bytes == 16 && atomic_load(&removed) ? 0 : 1;
}
