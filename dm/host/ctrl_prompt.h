// The controller applet's prompt (front.h front_ctrl_support; hle_hid.c ctrl_support; GitHub issue #1640). A game that opens nn::hid::ShowControllerSupport wanting more players
// than it sees gets, on a Switch, the system's player screen, which stays open until enough controllers are there or the player closes it. With a window the front end shows a
// prompt over the game and the game's thread waits for it: a controller that joins meanwhile (its first button with Press a button to join, its connection without) is a player
// at once. B or Escape closes it (canceled). With at least the minimum in, it is done when a player presses A (or Return), when the maximum has joined, or when no connected
// controller is left to join. No time limit and no close on a focus loss: a Switch has neither. Pure, so tests/t_ctrl_prompt.c checks it without a controller or a window.
#pragma once
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    CP_IDLE,     // nothing shown, the game has its input
    CP_OPEN,     // the prompt is open: the game's thread waits, the game gets no controller or keyboard input
    CP_SETTLE,   // the prompt has its answer and is gone: the game gets controller input again once A and B are up, so the A or B that closed it never reaches the game
} cp_phase;

typedef struct {
    cp_phase phase;
    int min, max;     // the game's playerCountMin and playerCountMax (max <= 0: none)
    int joined;       // the players the game sees (dm_players_reported) at the last step
    int a_up, b_up;   // A and B have been up since the prompt opened or a player last joined: a button held then does not count
    int ok;           // the answer once settled: 1 done, 0 canceled
} ctrl_prompt;

// Opens the prompt with the buttons as they are now (pressed on any controller)
static inline void cp_open(ctrl_prompt *q, int min, int max, int joined, int a, int b) {
    *q = (ctrl_prompt){.phase = CP_OPEN, .min = min, .max = max, .joined = joined, .a_up = !a, .b_up = !b};
}

// One step: joined = the players the game sees, waiting = connected controllers that are no player yet, a / b = A / B pressed on any controller (a press that makes a controller
// join changes joined, so it does not count), confirm / cancel = a key pressed (Return, Escape) or a touch on the prompt's buttons since the last step
static inline cp_phase cp_step(ctrl_prompt *q, int joined, int waiting, int a, int b, int confirm, int cancel) {
    switch (q->phase) {
    case CP_IDLE:
        break;
    case CP_OPEN:
        if (joined != q->joined) { q->joined = joined; q->a_up = !a; q->b_up = !b; }
        if (!a) q->a_up = 1;
        if (!b) q->b_up = 1;
        if ((b && q->b_up) || cancel) { q->phase = CP_SETTLE; q->ok = 0; }   // with A and B at once the safe one wins
        else if (joined >= q->min && ((a && q->a_up) || confirm || (q->max > 0 && joined >= q->max) || !waiting)) { q->phase = CP_SETTLE; q->ok = 1; }
        break;
    case CP_SETTLE:
        if (!a && !b) q->phase = CP_IDLE;
        break;
    }
    return q->phase;
}

// The game gets no controller input in these phases
static inline int cp_blocks_input(cp_phase p) { return p != CP_IDLE; }
// Whether A would finish it now (the prompt shows its action enabled)
static inline int cp_can_confirm(const ctrl_prompt *q) { return q->joined >= q->min; }

// The texts, in this order, one per line of DM_CTRL_PROMPT_TEXT: the title, the hint, the count, the done button, the cancel button. {min}, {max} and {joined} in a text are
// replaced by the numbers. An empty or missing line (or no text) keeps the English one
enum { CP_TEXT_TITLE, CP_TEXT_HINT, CP_TEXT_COUNT, CP_TEXT_DONE, CP_TEXT_CANCEL, CP_TEXTS };
static const char *const cp_text_default[CP_TEXTS] = {"{min} players wanted", "Press a button on each controller to join.", "Players: {joined} of {min}", "Done", "Cancel"};
// Text i of text (DM_CTRL_PROMPT_TEXT, NULL when unset) into out, with the numbers in
static inline void cp_text(const char *text, int i, const ctrl_prompt *q, char *out, size_t cap) {
    const char *s = text; size_t n = 0;
    for (int k = 0; s && k < i; k++) { s = strchr(s, '\n'); if (s) s++; }
    if (s) { const char *e = strchr(s, '\n'); n = e ? (size_t)(e - s) : strlen(s); }
    if (!n) { s = cp_text_default[i]; n = strlen(s); }
    size_t o = 0;
    if (!cap) return;
    for (size_t k = 0; k < n && o + 1 < cap;) {
        static const char *const key[3] = {"{min}", "{max}", "{joined}"};
        int v = -1; size_t kl = 0;
        for (int j = 0; j < 3; j++) { size_t l = strlen(key[j]); if (k + l <= n && !memcmp(s + k, key[j], l)) { v = j == 0 ? q->min : j == 1 ? q->max : q->joined; kl = l; break; } }
        if (kl) { int w = snprintf(out + o, cap - o, "%d", v); o = w < 0 ? o : o + (size_t)w < cap ? o + (size_t)w : cap - 1; k += kl; }
        else out[o++] = s[k++];
    }
    out[o] = 0;
}
