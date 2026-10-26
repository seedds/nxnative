// Hardwareless main-queue retry-policy scenarios used by front_ios.m.
// clang -O1 -Wall -o /tmp/t_audio_session_retry dm/host/tests/t_audio_session_retry.c
#include "../../ios/Sources/DmAudioRetry.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    // The category experiment is opt-in; typoed values must not choose a policy.
    assert(dm_audio_session_category_parse(NULL) == DM_AUDIO_SESSION_SOLO);
    assert(dm_audio_session_category_parse("") == DM_AUDIO_SESSION_SOLO);
    assert(dm_audio_session_category_parse("solo") == DM_AUDIO_SESSION_SOLO);
    assert(dm_audio_session_category_parse("playback_mix") == DM_AUDIO_SESSION_PLAYBACK_MIX);
    assert(dm_audio_session_category_parse("playback") == -1);
    assert(dm_audio_session_category_parse("playback_mix ") == -1);
    assert(dm_audio_session_category_parse("SOLO") == -1);
    puts("category parser: SoloAmbient default, exact opt-in, invalid value rejected: PASS");

    dm_audio_retry r = {0}; uint64_t first, second, token; unsigned ms;
    // A session held for longer than seven seconds must still be able to recover.
    // Failed activation stays suspended, with rate capped at one attempt per 8 s.
    for (unsigned i = 0; i < 7; i++) {
        ms = dm_audio_retry_plan(&r, 0, 1, &token); assert(ms == (1000u << (i < 3 ? i : 3)));
        assert(!dm_audio_retry_plan(&r, 0, 1, &second)); // no duplicate timer
        assert(dm_audio_retry_fire(&r, token, 0, 1));
    }
    assert(r.attempts == 7); // success can still occur after the fourth/later attempt
    dm_audio_retry_reset(&r); assert(!dm_audio_retry_plan(&r, 0, 0, &token));
    puts("long-held session: retries 1/2/4/8 seconds, later success still possible: PASS");

    // An old callback must neither activate in the background nor cancel a new
    // foreground retry that happens to be pending when it eventually fires.
    dm_audio_retry_reset(&r); assert(dm_audio_retry_plan(&r, 0, 1, &first) == 1000);
    dm_audio_retry_reset(&r); assert(!dm_audio_retry_plan(&r, 1, 1, &token));
    assert(!dm_audio_retry_fire(&r, first, 1, 1));
    assert(dm_audio_retry_plan(&r, 0, 1, &second) == 1000);
    assert(!dm_audio_retry_fire(&r, first, 0, 1) && r.pending);
    assert(dm_audio_retry_fire(&r, second, 0, 1));
    puts("background/foreground generations: stale callback cannot activate or cancel newer retry: PASS");

    // Stage Manager resign-active and interruptions are blocked states even
    // when a scene stays visible and never receives didEnterBackground.
    dm_audio_retry_reset(&r); assert(dm_audio_retry_plan(&r, 0, 1, &token));
    assert(!dm_audio_retry_fire(&r, token, 1, 1));
    assert(!dm_audio_retry_plan(&r, 1, 1, &token));
    dm_audio_retry_reset(&r); assert(dm_audio_retry_plan(&r, 0, 1, &token));
    assert(!dm_audio_retry_fire(&r, token, 1, 1));
    puts("visible-inactive card and interruption: never reactivate while blocked: PASS");

    // Successful activation clears the backoff and invalidates any queued retry.
    dm_audio_retry_reset(&r); assert(dm_audio_retry_plan(&r, 0, 1, &token));
    dm_audio_retry_reset(&r);
    assert(!dm_audio_retry_fire(&r, token, 0, 0));
    assert(!dm_audio_retry_plan(&r, 0, 0, &token));
    assert(r.attempts == 0 && !r.pending);
    puts("success: pending retry invalidated, no restart while already active: PASS");

    // A category transition cancels retries from the preceding configuration.
    // Going inactive before either the old timer or a new one fires must prevent
    // activation, including a visible Stage Manager card that never backgrounds.
    assert(dm_audio_retry_plan(&r, 0, 1, &first));
    dm_audio_retry_reset(&r); // live category changed on the main queue
    assert(!dm_audio_retry_fire(&r, first, 0, 1));
    assert(dm_audio_retry_plan(&r, 0, 1, &second));
    dm_audio_retry_reset(&r); // resign-active cancels that category's retry too
    assert(!dm_audio_retry_fire(&r, second, 1, 1));
    assert(!dm_audio_retry_plan(&r, 1, 1, &token));
    puts("live category change followed by inactive: neither configuration can reactivate: PASS");
    return 0;
}
