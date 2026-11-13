// The controller applet's prompt (ctrl_prompt.h, GitHub issue #1640): waiting below the minimum, a joining press that is not an A, A and B, the maximum, nobody left to join,
// the keys, a button held when it opens, the settle phase that keeps the closing press from the game, and a front end's texts with the numbers in
#include "../ctrl_prompt.h"
#include <stdio.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

int main(void) {
    ctrl_prompt q;
    cp_open(&q, 2, 2, 1, 0, 0);
    CHECK(cp_blocks_input(q.phase), "open: the game has no input");
    CHECK(cp_step(&q, 1, 0, 0, 0, 0, 0) == CP_OPEN, "below the minimum with nobody waiting: it stays open (a controller may connect)");
    CHECK(cp_step(&q, 1, 1, 1, 0, 1, 0) == CP_OPEN, "below the minimum: A and Return do nothing");
    CHECK(!cp_can_confirm(&q), "below the minimum: done is not offered");
    CHECK(cp_step(&q, 2, 0, 0, 0, 0, 0) == CP_SETTLE && q.ok, "the second player joins: the maximum is in, done");
    CHECK(cp_blocks_input(q.phase), "settling: still no input");
    CHECK(cp_step(&q, 2, 0, 0, 0, 0, 0) == CP_IDLE, "all up: the game has its input");

    cp_open(&q, 2, 4, 1, 0, 0);
    CHECK(cp_step(&q, 2, 1, 1, 0, 0, 0) == CP_OPEN, "an A that makes a controller join does not finish it");
    CHECK(cp_can_confirm(&q), "the minimum is in: done is offered");
    CHECK(cp_step(&q, 2, 1, 1, 0, 0, 0) == CP_OPEN, "that A still held: nothing");
    CHECK(cp_step(&q, 2, 1, 0, 0, 0, 0) == CP_OPEN, "released");
    CHECK(cp_step(&q, 2, 1, 1, 0, 0, 0) == CP_SETTLE && q.ok, "a new A with the minimum in: done");
    CHECK(cp_step(&q, 2, 1, 1, 0, 0, 0) == CP_SETTLE, "A still down: still settling");
    CHECK(cp_step(&q, 2, 1, 0, 0, 0, 0) == CP_IDLE, "A up: idle");

    cp_open(&q, 2, 4, 1, 0, 0);
    CHECK(cp_step(&q, 3, 0, 0, 0, 0, 0) == CP_SETTLE && q.ok, "the minimum in and no controller left to join: done");

    cp_open(&q, 2, 4, 2, 0, 0);
    CHECK(cp_step(&q, 2, 2, 0, 0, 1, 0) == CP_SETTLE && q.ok, "Return with the minimum in: done");

    cp_open(&q, 2, 0, 2, 0, 0);
    CHECK(cp_step(&q, 2, 1, 0, 0, 0, 0) == CP_OPEN, "no maximum: a waiting controller keeps it open");

    cp_open(&q, 3, 4, 1, 0, 0);
    CHECK(cp_step(&q, 1, 1, 0, 1, 0, 0) == CP_SETTLE && !q.ok, "B cancels");
    cp_open(&q, 3, 4, 1, 0, 0);
    CHECK(cp_step(&q, 1, 1, 0, 0, 0, 1) == CP_SETTLE && !q.ok, "Escape cancels");
    cp_open(&q, 2, 4, 2, 0, 0);
    CHECK(cp_step(&q, 2, 1, 1, 1, 0, 0) == CP_SETTLE && !q.ok, "A and B at once: canceled");

    cp_open(&q, 2, 4, 1, 0, 1);
    CHECK(cp_step(&q, 1, 1, 0, 1, 0, 0) == CP_OPEN, "a B held when it opens does not cancel");
    CHECK(cp_step(&q, 1, 1, 0, 0, 0, 0) == CP_OPEN, "released");
    CHECK(cp_step(&q, 2, 1, 0, 1, 0, 0) == CP_OPEN, "a B that makes a controller join does not cancel");
    CHECK(cp_step(&q, 2, 1, 0, 0, 0, 0) == CP_OPEN && cp_step(&q, 2, 1, 0, 1, 0, 0) == CP_SETTLE && !q.ok, "a new B cancels");

    q = (ctrl_prompt){0};
    CHECK(cp_step(&q, 1, 1, 1, 1, 1, 1) == CP_IDLE && !cp_blocks_input(q.phase), "idle: nothing opens it but cp_open");

    char out[64];
    cp_open(&q, 2, 4, 1, 0, 0);
    cp_text(NULL, CP_TEXT_TITLE, &q, out, sizeof out); CHECK(!strcmp(out, "2 players wanted"), "the English title with the minimum");
    cp_text(NULL, CP_TEXT_COUNT, &q, out, sizeof out); CHECK(!strcmp(out, "Players: 1 of 2"), "the English count");
    cp_text("T\n\n{joined}/{min}-{max}", CP_TEXT_COUNT, &q, out, sizeof out); CHECK(!strcmp(out, "1/2-4"), "a front end's count with the numbers in");
    cp_text("T\n\n{joined}/{min}-{max}", CP_TEXT_HINT, &q, out, sizeof out); CHECK(!strcmp(out, "Press a button on each controller to join."), "an empty line keeps the English text");
    cp_text("T", CP_TEXT_CANCEL, &q, out, sizeof out); CHECK(!strcmp(out, "Cancel"), "a missing line keeps the English text");
    cp_text("{min", CP_TEXT_TITLE, &q, out, sizeof out); CHECK(!strcmp(out, "{min"), "an unfinished key stays as written");
    cp_text("{joined}{joined}{joined}", CP_TEXT_TITLE, &q, out, 3); CHECK(!strcmp(out, "11"), "cut at the buffer's end");

    printf("t_ctrl_prompt: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
