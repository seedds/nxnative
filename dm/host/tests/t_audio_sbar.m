// The system renderer's set-up and enqueue path (audio_spatial_mac.m), with the real AVFoundation, without playback: the synchronizer's rate stays 0 throughout, so nothing is
// heard and its clock never runs. Checks: the class is there (route_can's AR_CAN_SYSTEM), a six-channel renderer enqueues 32-bit float at 48 kHz with the MPEG_5_1_A layout
// (what makes it multichannel content macOS spatializes), a stereo one the stereo layout, other counts are refused with a reason; 150 ms of 5 ms buffers enqueue (the
// pump's pre-roll) and read back as 150 ms of lead; the renderer has not failed; a stop flushes and starts the timestamps and the clock from 0 again (from a clock
// moved to 2 s, as after playing); destroy releases it all
#include "../audio_sbar.h"
#include <CoreAudioTypes/CoreAudioTypes.h>
#include <math.h>
#include <stdio.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void) {
    char why[200] = "";
    CHECK(dm_sbar_available());
    dm_sbar *s = dm_sbar_create(6, 48000, why, sizeof why);
    CHECK(s != NULL);
    if (!s) { printf("no renderer: %s\n", why); return 1; }
    int ch = 0, is_float = 0; uint32_t tag = 0; double rate = 0;
    CHECK(dm_sbar_describe(s, &ch, &tag, &rate, &is_float) == 0);
    CHECK(ch == 6 && tag == kAudioChannelLayoutTag_MPEG_5_1_A && rate == 48000 && is_float);
    printf("system renderer: %d channels, layout tag %#x (MPEG_5_1_A %#x), %.0f Hz, %s\n", ch, tag, kAudioChannelLayoutTag_MPEG_5_1_A, rate, is_float ? "float" : "integer");
    CHECK(fabs(dm_sbar_lead(s)) < 1e-9);
    static float block[240 * 6];
    for (int i = 0; i < 240 * 6; i++) block[i] = 0;   // silence: the pump's pre-roll, and nothing could be heard even if a clock ran
    int n = 0;
    while (dm_sbar_lead(s) < 0.150 && n < 100) { CHECK(dm_sbar_enqueue(s, block, 240) == 0); n++; }
    printf("system renderer: %d buffers of 5 ms enqueued, lead %.1f ms, status %d\n", n, dm_sbar_lead(s) * 1000, dm_sbar_status(s, why, sizeof why));
    CHECK(n == 30 && fabs(dm_sbar_lead(s) - 0.150) < 1e-6);
    CHECK(dm_sbar_status(s, why, sizeof why) == SBAR_OK);
    // As after 2 s of playing: the clock at 2 s (its rate still 0). A stop must put it back to 0 too, or the next pre-roll reads a lead of -1.85 s and fills 2 s of silence
    dm_sbar_test_set_time(s, 2.0);
    CHECK(fabs(dm_sbar_lead(s) - (0.150 - 2.0)) < 1e-3);
    dm_sbar_stop(s);
    CHECK(fabs(dm_sbar_lead(s)) < 1e-9);   // timestamps and the clock from 0 again
    CHECK(dm_sbar_enqueue(s, block, 240) == 0 && fabs(dm_sbar_lead(s) - 0.005) < 1e-6);
    CHECK(dm_sbar_status(s, why, sizeof why) == SBAR_OK);
    dm_sbar_destroy(s);
    dm_sbar *st = dm_sbar_create(2, 48000, why, sizeof why);
    CHECK(st != NULL);
    if (st) { CHECK(dm_sbar_describe(st, &ch, &tag, &rate, &is_float) == 0 && ch == 2 && tag == kAudioChannelLayoutTag_Stereo); dm_sbar_destroy(st); }
    why[0] = 0;
    CHECK(dm_sbar_create(4, 48000, why, sizeof why) == NULL && why[0]);
    printf("audio sbar: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
