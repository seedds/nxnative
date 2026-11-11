#include "../nvn/nvn_flow_trace.h"
#include <string.h>
#include <assert.h>

static void *writer(void *arg) {
    uint64_t base = (uintptr_t)arg;
    for (uint64_t i = 0; i < 10000; i++)
        nvn_flow_event("concurrent", base + i, 1, base + i + 1, base + i + 1.25);
    return NULL;
}
int main(void) {
    if (!getenv("DM_NVN_FLOW")) {
        assert(!nvn_flow_on());
        nvn_flow_event("disabled", 1, 1, 1, 2);
        assert(!g_flow_entries);
        return 0;
    }
    if (getenv("DM_TEST_FLOW_WINDOW")) {
        nvn_flow_event("before", 1, 1, 83, 84);
        nvn_flow_event("cross lower", 2, 1, 83.9, 84.1);
        nvn_flow_event("inside", 3, 1, 90, 91);
        nvn_flow_event("cross upper", 4, 1, 105.9, 106.1);
        nvn_flow_event("upper boundary", 5, 0, 106, 106);
        nvn_flow_event("lower boundary", 6, 0, 84, 84);
        assert(atomic_load(&g_flow_count) == 4);
        assert(atomic_load(&g_flow_filtered) == 2);
        puts("flow: crossing intervals and boundary markers retained correctly");
        return 0;
    }
    pthread_t threads[2];
    assert(!pthread_create(&threads[0], NULL, writer, (void *)(uintptr_t)0));
    assert(!pthread_create(&threads[1], NULL, writer, (void *)(uintptr_t)20000));
    for (int i = 0; i < 2; i++) assert(!pthread_join(threads[i], NULL));
    nvn_flow_event("invalid", 1, 1, 0, 1);
    nvn_flow_event("invalid", 1, 1, 2, 1);
    assert(atomic_load(&g_flow_count) == 20000);
    unsigned seen[30000] = {0};
    for (int i = 0; i < 20000; i++) {
        nvn_flow_entry *e = &g_flow_entries[i];
        assert(atomic_load(&e->ready));
        assert(e->frame < 30000 && !seen[e->frame]++);
        assert(e->start == e->frame + 1 && e->end == e->frame + 1.25);
        assert(e->gpu == 1 && !strcmp(e->kind, "concurrent"));
    }
    nvn_flow_dump();
    FILE *f = fopen(getenv("DM_NVN_FLOW"), "r"); assert(f);
    char line[512]; int lines = 0;
    while (fgets(line, sizeof line, f)) lines++;
    fclose(f); assert(lines == 20002);
    puts("flow: 20000 concurrent intervals intact, invalid intervals rejected");
    return 0;
}
