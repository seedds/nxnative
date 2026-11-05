// The Mac output route's decisions, with no audio hardware anywhere: the forced verdict (DM_TEST_AUDIO_ROUTE)
// drives the switch path, dm_audio_set_mode / dm_audio_mode / dm_audio_mode_name are stubs that record what
// the route commands, and the CoreAudio queries themselves run for real - they answer with no device at all
// (the default output is then 0 and the forced verdict decides), so the test runs on a machine with no
// speakers as well. The unit of truth is evaluate(): dm_audio_route_init registers the real listeners and
// makes the first verdict on its serial queue, so the test waits that one out and drives the rest directly.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include "../hle.h"

int dm_audio_mode(void);
void dm_audio_set_mode(int m);
const char *dm_audio_mode_name(void);

static int checks, failures;
#define CHECK(v, why) do { checks++; if (!(v)) { failures++; printf("FAIL: %s\n", why); } } while (0)

static int mode_now = -1, set_calls, last_set;
int dm_audio_mode(void) { return mode_now; }
void dm_audio_set_mode(int m) { set_calls++; last_set = m; mode_now = m; }
const char *dm_audio_mode_name(void) {
    static const char *N[] = {"original matrix", "system downmix", "wide stereo", "binaural"};
    return mode_now >= 0 && mode_now < DM_AM_N ? N[mode_now] : "?";
}

#include "../audio_route_mac.m"   // the route's own statics (g_hp) and evaluate() come with it

int main(void) {
    mode_now = DM_AM_WIDE;
    setenv("DM_TEST_AUDIO_ROUTE", "headphone", 1);   // the verdict is forced throughout; no hardware decides
    dm_audio_route_init();   // the real listeners, plus the first verdict on the route's serial queue
    // Wait for the whole startup evaluate (it records the verdict before it may switch, so g_hp alone would
    // race the switch): the test's startup mode is Wide, which the forced headphones must switch.
    for (int i = 0; i < 2000 && set_calls != 1; i++) usleep(1000);
    CHECK(atomic_load(&g_hp) == 1, "the forced startup verdict is recorded");
    CHECK(set_calls == 1 && last_set == DM_AM_BINAURAL && mode_now == DM_AM_BINAURAL, "headphones already in at startup switch wide stereo -> binaural");
    evaluate("again");
    CHECK(set_calls == 1, "an unchanged verdict switches nothing");
    setenv("DM_TEST_AUDIO_ROUTE", "speaker", 1);
    evaluate("test");
    CHECK(set_calls == 2 && last_set == DM_AM_WIDE && mode_now == DM_AM_WIDE, "speakers switch binaural -> wide stereo");
    setenv("DM_TEST_AUDIO_ROUTE", "headphone", 1);
    mode_now = DM_AM_MATRIX;
    evaluate("test");
    CHECK(set_calls == 2, "a mode the player picked outside the two is never overridden");
    mode_now = DM_AM_SYSCONV;
    setenv("DM_TEST_AUDIO_ROUTE", "speaker", 1);
    evaluate("test");
    CHECK(set_calls == 2, "system downmix is not the auto-switch's to move");
    setenv("DM_AUDIO_HP_AUTO", "0", 1);
    mode_now = DM_AM_WIDE;
    setenv("DM_TEST_AUDIO_ROUTE", "headphone", 1);
    evaluate("test");
    CHECK(set_calls == 2 && atomic_load(&g_hp) == 1, "Headphone auto-switch off records the verdict without switching");
    unsetenv("DM_AUDIO_HP_AUTO");
    setenv("DM_TEST_AUDIO_ROUTE", "speaker", 1);
    mode_now = DM_AM_BINAURAL;
    evaluate("test");
    CHECK(set_calls == 3 && last_set == DM_AM_WIDE && mode_now == DM_AM_WIDE, "auto-switch back on: speakers return binaural to wide stereo");

    printf("audio route: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
