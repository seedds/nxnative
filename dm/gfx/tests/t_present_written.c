// The present-drop latch of DM_NVN_PRESENT_DRAWN (nvn_written_presents.h): a serial's first present goes through,
// a present with no stored pass since the serial's own previous present counts as unchanged, and a stored pass
// between two presents makes the next one fresh. No GPU or game data.
#include "../nvn/nvn_written_presents.h"
#include <pthread.h>
#include <stdio.h>

static nvn_written_presents table = NVN_WRITTEN_PRESENTS_INIT;
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static void *store_marks(void *arg) {
    (void)arg;
    for (int i = 0; i < 20000; i++) nvn_written_stored(&table, 5);
    return NULL;
}

int main(void) {
    // The switch's rule: the first present of a serial goes through whatever the texture holds, the next one re-shows
    // it unchanged unless a pass stored in between, and each store is consumed by the present that showed it.
    nvn_written_stored(&table, 1);
    CHECK(!nvn_written_presented(&table, 1), "a serial's first present goes through whatever it holds");
    CHECK(nvn_written_presented(&table, 1), "a present with no stored pass since the previous one is unchanged");
    nvn_written_stored(&table, 1);
    CHECK(!nvn_written_presented(&table, 1), "a stored pass between two presents makes the next one fresh");
    CHECK(nvn_written_presented(&table, 1), "that store is consumed by the present that showed it");

    // Serials are independent: the latch of one texture does not leak onto another.
    nvn_written_stored(&table, 2);
    CHECK(!nvn_written_presented(&table, 2), "a second serial has a first present of its own");
    CHECK(nvn_written_presented(&table, 2), "the second serial repeats with nothing stored");
    CHECK(!nvn_written_presented(&table, 3), "a serial the game never presented stays fresh");

    // Serial 0 is never marked, so a caller that passes it drops nothing.
    CHECK(!nvn_written_presented(&table, 0), "serial 0 counts as fresh");
    nvn_written_stored(&table, 0);
    CHECK(!nvn_written_presented(&table, 0), "serial 0 takes no store");

    // Serials that hash to the same slot stay apart.
    uint64_t one = NVN_WRITTEN_TABLE, two = 2 * (uint64_t)NVN_WRITTEN_TABLE;
    nvn_written_stored(&table, one);
    nvn_written_stored(&table, two);
    CHECK(!nvn_written_presented(&table, one), "a collided serial's first present goes through");
    CHECK(!nvn_written_presented(&table, two), "the serial it collided with has a first present of its own");
    CHECK(nvn_written_presented(&table, one), "the collided serial repeats unchanged");

    // Stores that pile up between two presents count once: absent keeps the drop, present keeps the show.
    nvn_written_stored(&table, 9);
    nvn_written_stored(&table, 9);
    nvn_written_stored(&table, 9);
    CHECK(!nvn_written_presented(&table, 9), "several stores between presents keep the next present");
    CHECK(nvn_written_presented(&table, 9), "and the repeat after them drops again");

    // Store marks from another thread while this one presents: under the lock's contention a serial that nothing
    // stores still repeats unchanged, exactly as one thread alone would have it.
    pthread_t th;
    CHECK(pthread_create(&th, NULL, store_marks, NULL) == 0, "the marking thread starts");
    CHECK(!nvn_written_presented(&table, 7), "a serial shared with another thread's marks has a first present");
    for (int i = 0; i < 400; i++) CHECK(nvn_written_presented(&table, 7), "a serial nothing stored repeats unchanged under the lock's contention");
    CHECK(pthread_join(th, NULL) == 0, "the marking thread ends");

    // A latch without room for a serial drops none of its presents: the serial counts as fresh (see the header).
    for (uint64_t s = 0x100; s < 0x100 + NVN_WRITTEN_TABLE; s++) nvn_written_stored(&table, s);
    for (int i = 0; i < NVN_WRITTEN_TABLE; i++)
        if (table.slot[i].serial == 0x100) { CHECK(table.slot[i].stored == 1, "a filled table keeps its counts"); break; }
    CHECK(!nvn_written_presented(&table, 0x7777755555555ull), "a serial the table could not hold counts as fresh");
    CHECK(!nvn_written_presented(&table, 0x7777755555555ull), "and a latch without room drops no repeat");

    printf("%s: %d checks, %d failed\n", failed ? "FAIL" : "PASS", checks, failed);
    return failed != 0;
}
