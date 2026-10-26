// Asset-free per-encoder resource-usage union tests. No Metal/GPU required.
// clang -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
//   dm/gfx/tests/t_resource_usage.c -o /tmp/t_resource_usage
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned g_allocations, g_fail_next_allocation;
static void *test_realloc(void *p, size_t bytes) {
    g_allocations++;
    if (g_fail_next_allocation) { g_fail_next_allocation = 0; return NULL; }
    return realloc(p, bytes);
}
#define NVN_RESOURCE_USAGE_REALLOC test_realloc
#include "../nvn/nvn_resource_usage.h"
#undef NVN_RESOURCE_USAGE_REALLOC

enum { READ = 1, WRITE = 2, SAMPLE = 4, MAX_RESOURCES = 4096 };
typedef struct { uintptr_t resource; uint64_t usage; } reference_entry;
typedef struct {
    nvn_resource_usage_set actual;
    reference_entry reference[MAX_RESOURCES];
    size_t count;
} context;
static unsigned g_checks;
#define REQUIRE(condition, what) do { g_checks++; if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, what); exit(1); } } while (0)

static void begin(context *c) {
    nvn_resource_usage_begin(&c->actual); c->count = 0;
}
static void add(context *c, uintptr_t resource, uint64_t requested, int dedup) {
    size_t index = 0;
    while (index < c->count && c->reference[index].resource != resource) index++;
    uint64_t old = index < c->count ? c->reference[index].usage : 0;
    uint64_t expected = old | requested, combined = UINT64_MAX;
    int skip = -1;
    REQUIRE(nvn_resource_usage_add(&c->actual, resource, requested, dedup, &combined, &skip),
        "usage request succeeds when allocation is available");
    REQUIRE(combined == expected, "declaration contains the full encoder-lifetime access union");
    REQUIRE(skip == (dedup && old != 0 && (old & requested) == requested),
        "dedup skips only a declaration already covered by the existing union");
    if (index == c->count) {
        REQUIRE(c->count < MAX_RESOURCES, "reference oracle capacity");
        c->reference[c->count++] = (reference_entry){resource, 0};
    }
    c->reference[index].usage = expected;
}
static uintptr_t crowded_key(size_t index) {
    // These aligned fake objects share a fast-table start in the published
    // 512-slot scheme. More than 512 distinct keys also exceed total fast
    // capacity even if a future implementation changes its hash mixing.
    return (uintptr_t)0x100000 + (uintptr_t)index * 8192;
}
static void ordinary_aliases_and_epochs(void) {
    context c = {0}; begin(&c);
    const uintptr_t input = 0x1110, pool = 0x2220, independent = 0x3330;
    add(&c, input, READ, 1); add(&c, input, READ, 1);
    add(&c, pool, READ, 1);              // UBO view of a shared pool buffer
    add(&c, pool, READ | WRITE, 1);      // writable SBUF in that same buffer
    add(&c, pool, READ, 1);
    add(&c, input, READ, 1);            // a different input stays read-only
    add(&c, independent, WRITE, 1); add(&c, independent, READ, 1);
    begin(&c);
    add(&c, pool, READ | WRITE, 1);      // reverse SBUF -> UBO call order
    add(&c, pool, READ, 1); add(&c, pool, SAMPLE, 1);
    add(&c, pool, READ, 0);              // dedup off still sends the full union
    add(&c, pool, WRITE, 0); add(&c, pool, SAMPLE, 0);
    begin(&c);
    add(&c, pool, READ | WRITE, 0); add(&c, pool, READ, 0);
    add(&c, pool, READ, 0); add(&c, input, READ, 0);
    puts("RO input, UBO/SBUF aliases in both orders, and dedup-off unions: PASS");

    // The allocator may reuse both encoder and ObjC resource addresses. Every
    // explicit begin represents a new encoder even when those addresses match.
    for (unsigned i = 0; i < 128; i++) {
        begin(&c); add(&c, pool, i & 1 ? READ : READ | WRITE, 1);
        add(&c, pool, READ, 1);
    }
    // Exercise the epoch rollover without waiting for 2^64 encoder creations.
    c.actual.epoch = UINT64_MAX; begin(&c);
    // independent was written only in epoch 1. Its old epoch value now matches
    // again, so failing to clear stale fast entries would resurrect Write.
    add(&c, independent, READ, 1); add(&c, pool, READ, 1);
    nvn_resource_usage_dispose(&c.actual);
    REQUIRE(c.actual.overflow == NULL && c.actual.epoch == 0, "dispose releases storage and resets state");
    puts("same resource/encoder identities across fresh epochs and rollover: PASS");
}
static void collisions_and_capacity(void) {
    context c = {0}; begin(&c);
    enum { COUNT = 2048 };
    for (size_t i = 0; i < COUNT; i++) add(&c, crowded_key(i), WRITE, (int)(i & 1));
    // Revisit from both ends after overflowing every possible fast-table slot.
    // An evicting implementation would incorrectly forget earlier writes.
    for (size_t i = COUNT; i-- > 0;) add(&c, crowded_key(i), READ, 1);
    for (size_t i = 0; i < COUNT; i++) {
        add(&c, crowded_key(i), READ, 0); add(&c, crowded_key(i), WRITE, 1);
    }
    begin(&c);
    for (size_t i = 0; i < COUNT; i++) add(&c, crowded_key(i), READ, 1);
    nvn_resource_usage_dispose(&c.actual);
    puts("collisions, capacity overflow, revisit order, and overflow epoch reset: PASS");
}
static size_t fill_until_allocation_failure(context *c, size_t first) {
    g_fail_next_allocation = 1; unsigned before = g_allocations;
    for (size_t i = first; i < MAX_RESOURCES; i++) {
        uint64_t combined; int skip;
        if (!nvn_resource_usage_add(&c->actual, crowded_key(i), WRITE, 1, &combined, &skip)) {
            REQUIRE(!g_fail_next_allocation && g_allocations == before + 1,
                "OOM was injected at a real overflow allocation");
            return i;
        }
        // Record the successful request through the independent oracle. Its
        // second insertion must be a covered request, preserving Write.
        REQUIRE(c->count < MAX_RESOURCES, "OOM oracle capacity");
        c->reference[c->count++] = (reference_entry){crowded_key(i), WRITE};
        add(c, crowded_key(i), WRITE, 1);
    }
    REQUIRE(0, "dynamic overflow storage reaches the injected allocator failure");
    return 0;
}
static void allocation_failure_preserves_history(void) {
    context c = {0}; begin(&c);
    size_t failed = fill_until_allocation_failure(&c, 0);
    REQUIRE(failed > 0, "failure occurs after prior resource declarations exist");
    for (size_t i = 0; i < failed; i++) add(&c, crowded_key(i), READ, 1);
    add(&c, crowded_key(failed), WRITE, 1); // failed insertion is retryable
    failed = fill_until_allocation_failure(&c, failed + 1); // fail a later vector growth
    for (size_t i = failed; i-- > 0;) add(&c, crowded_key(i), READ, 0);
    add(&c, crowded_key(failed), WRITE, 1);
    add(&c, crowded_key(failed), READ, 0);
    nvn_resource_usage_dispose(&c.actual);
    puts("initial/growth OOM returns failure while preserving previous Write history: PASS");
}
int main(void) {
    ordinary_aliases_and_epochs(); collisions_and_capacity(); allocation_failure_preserves_history();
    printf("Resource-usage union: PASS (%u checks, fake resource identities, no GPU)\n", g_checks);
    return 0;
}
