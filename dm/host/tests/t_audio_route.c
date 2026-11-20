// The output route decision (audio_route.h): transport x data source x channels x DM_SPATIAL_AUDIO x what the host can make → the path, the channels the game is told and
// the channels a DeviceSink output pulls. Then live sequences over three outputs (AirPods Pro, MacBook Pro speakers, Studio Display), as their HAL
// reports them on an M5 MacBook Pro (macOS 27), with the setting checked unchanged after every step. Pure: no Core Audio
#include "../audio_route.h"
#include <stdio.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const uint32_t BLUE = AR_FOURCC('b', 'l', 'u', 'e'), BLEA = AR_FOURCC('b', 'l', 'e', 'a'), BLTN = AR_FOURCC('b', 'l', 't', 'n'), USB = AR_FOURCC('u', 's', 'b', ' '),
                      HDMI = AR_FOURCC('h', 'd', 'm', 'i'), DPRT = AR_FOURCC('d', 'p', 'r', 't'), VIRT = AR_FOURCC('v', 'i', 'r', 't'), AIRP = AR_FOURCC('a', 'i', 'r', 'p'),
                      AGGR = AR_FOURCC('a', 'g', 'g', 'r'), THUN = AR_FOURCC('t', 'h', 'u', 'n');
enum { ALL = AR_CAN_SYSTEM | AR_CAN_MIXER };

static void expect(int pref, uint32_t transport, uint32_t ds, int ch, unsigned can, int path, int report, int mixer) {
    audio_route_decision d = audio_route_decide(pref, audio_transport_of(transport), ds, ch, can);
    CHECK(d.path == path); CHECK(d.report == report); CHECK(d.pull == report); CHECK(d.mixer_type == mixer);
    if (d.path != path || d.report != report) printf("  pref %d transport %#x ds %#x ch %d can %u: got path %d report %d, want %d %d\n", pref, transport, ds, ch, can, d.path, d.report, path, report);
}

static void test_parse(void) {
    int bad;
    CHECK(spatial_pref_parse("auto", SPATIAL_PREF_OFF, &bad) == SPATIAL_PREF_AUTO && !bad);
    CHECK(spatial_pref_parse("0", SPATIAL_PREF_AUTO, &bad) == SPATIAL_PREF_OFF && !bad);
    CHECK(spatial_pref_parse(NULL, SPATIAL_PREF_AUTO, &bad) == SPATIAL_PREF_AUTO && !bad);   // unset: the profile's default (OoT: auto)
    CHECK(spatial_pref_parse("", SPATIAL_PREF_OFF, &bad) == SPATIAL_PREF_OFF && !bad);       // another game: 0
    const char *wrong[] = {"1", "on", "Auto", "off", "auto ", "2"};
    for (unsigned i = 0; i < sizeof wrong / sizeof *wrong; i++) {
        CHECK(spatial_pref_parse(wrong[i], SPATIAL_PREF_AUTO, &bad) == SPATIAL_PREF_AUTO && bad);
        CHECK(spatial_pref_parse(wrong[i], SPATIAL_PREF_OFF, &bad) == SPATIAL_PREF_OFF && bad);
    }
}

static void test_transports(void) {
    CHECK(audio_transport_of(0) == AT_UNKNOWN); CHECK(audio_transport_of(BLTN) == AT_BUILTIN);
    CHECK(audio_transport_of(BLUE) == AT_BLUETOOTH); CHECK(audio_transport_of(BLEA) == AT_BLUETOOTH);
    CHECK(audio_transport_of(USB) == AT_USB); CHECK(audio_transport_of(HDMI) == AT_HDMI); CHECK(audio_transport_of(DPRT) == AT_DISPLAYPORT);
    CHECK(audio_transport_of(THUN) == AT_THUNDERBOLT); CHECK(audio_transport_of(AIRP) == AT_AIRPLAY); CHECK(audio_transport_of(VIRT) == AT_VIRTUAL);
    CHECK(audio_transport_of(AGGR) == AT_AGGREGATE); CHECK(audio_transport_of(AR_FOURCC('x', 'y', 'z', 'w')) == AT_OTHER);
}

static void test_table(void) {
    const uint32_t HP = AR_DS_HEADPHONES, SPK = AR_DS_SPEAKERS;
    for (int pref = 0; pref <= 1; pref++) {
        // Six or more channels: 5.1 as it is, whatever the setting and transport (the Studio Display: USB, 8; HDMI 5.1; BlackHole 16ch)
        const uint32_t multi[] = {USB, HDMI, DPRT, VIRT, BLTN, 0};
        for (unsigned i = 0; i < sizeof multi / sizeof *multi; i++) for (int ch = 6; ch <= 16; ch += 2) for (unsigned can = 0; can <= ALL; can++)
            expect(pref, multi[i], 0, ch, can, AP_SURROUND, 6, 0);
        // Setting 0 on a stereo or mono device: the fold, as before the setting existed
        if (!pref) {
            const uint32_t all[] = {BLUE, BLEA, BLTN, USB, HDMI, VIRT, AIRP, 0};
            for (unsigned i = 0; i < sizeof all / sizeof *all; i++) for (int ch = 0; ch <= 5; ch++) for (unsigned can = 0; can <= ALL; can++) {
                expect(pref, all[i], 0, ch, can, AP_FOLD, 2, 0); expect(pref, all[i], HP, ch, can, AP_FOLD, 2, 0); expect(pref, all[i], SPK, ch, can, AP_FOLD, 2, 0);
            }
            continue;
        }
        // auto: AirPods / Bluetooth headphones, and the built-in headphone jack: the system renderer, else AUSpatialMixer for headphones, else the fold
        const uint32_t hp_t[] = {BLUE, BLEA};
        for (unsigned i = 0; i < 2; i++) {
            expect(pref, hp_t[i], 0, 2, ALL, AP_SYSTEM, 6, 0);
            expect(pref, hp_t[i], 0, 2, AR_CAN_SYSTEM, AP_SYSTEM, 6, 0);
            expect(pref, hp_t[i], 0, 2, AR_CAN_MIXER, AP_HEADPHONES, 6, AR_MIXER_HEADPHONES);
            expect(pref, hp_t[i], 0, 2, 0, AP_FOLD, 2, 0);
            expect(pref, hp_t[i], 0, 1, ALL, AP_FOLD, 2, 0);   // a headset in call mode (AirPods: 24 kHz mono while a microphone is open)
            expect(pref, hp_t[i], 0, 0, ALL, AP_FOLD, 2, 0);   // unknown channels
        }
        expect(pref, BLTN, HP, 2, ALL, AP_SYSTEM, 6, 0);
        expect(pref, BLTN, HP, 2, AR_CAN_MIXER, AP_HEADPHONES, 6, AR_MIXER_HEADPHONES);
        // The MacBook speakers ('bltn', data source 'ispk'; also when the device names no data source): AUSpatialMixer for the built-in speakers, never the system renderer
        expect(pref, BLTN, SPK, 2, ALL, AP_SPEAKERS, 6, AR_MIXER_BUILTIN_SPEAKERS);
        expect(pref, BLTN, 0, 2, ALL, AP_SPEAKERS, 6, AR_MIXER_BUILTIN_SPEAKERS);
        expect(pref, BLTN, SPK, 2, AR_CAN_SYSTEM, AP_FOLD, 2, 0);
        expect(pref, BLTN, SPK, 2, 0, AP_FOLD, 2, 0);
        expect(pref, BLTN, SPK, 1, ALL, AP_FOLD, 2, 0);
        // Everything else with two channels: USB / AirPlay / HDMI stereo, virtual devices (BlackHole 2ch, Microsoft Teams Audio), aggregates, an unknown transport: the fold
        const uint32_t other[] = {USB, AIRP, HDMI, DPRT, THUN, VIRT, AGGR, 0, AR_FOURCC('x', 'y', 'z', 'w')};
        for (unsigned i = 0; i < sizeof other / sizeof *other; i++) for (int ch = 0; ch <= 5; ch++) {
            expect(pref, other[i], 0, ch, ALL, AP_FOLD, 2, 0); expect(pref, other[i], HP, ch, ALL, AP_FOLD, 2, 0);
        }
    }
    // Every decision tells the game 6 exactly when a DeviceSink pulls the six, and only a mixer path names a mixer type
    for (int pref = 0; pref <= 1; pref++) for (int t = 0; t < 12; t++) for (int ch = 0; ch <= 8; ch++) for (unsigned can = 0; can <= ALL; can++) for (int h = 0; h < 3; h++) {
        uint32_t ds = h == 0 ? 0 : h == 1 ? AR_DS_HEADPHONES : AR_DS_SPEAKERS;
        audio_route_decision d = audio_route_decide(pref, t, ds, ch, can);
        CHECK(d.report == (d.path == AP_FOLD ? 2 : 6) && d.pull == d.report);
        CHECK((d.mixer_type != 0) == (d.path == AP_SPEAKERS || d.path == AP_HEADPHONES));
        CHECK(d.path != AP_SYSTEM || (can & AR_CAN_SYSTEM));
        CHECK((d.path != AP_SPEAKERS && d.path != AP_HEADPHONES) || (can & AR_CAN_MIXER));
        CHECK(pref == SPATIAL_PREF_AUTO || d.path == AP_FOLD || d.path == AP_SURROUND);
    }
}

// The outputs as their HAL reports them (AirPods: 2 ch Bluetooth; MacBook Pro: built-in 'ispk' 2 ch; Studio Display: USB 8 ch): transport, data source, channels
typedef struct { const char *name; uint32_t transport, ds; int ch; } device;
static const device AIRPODS = {"AirPods Pro", BLUE, 0, 2}, MACBOOK = {"MacBook Pro Speakers", BLTN, AR_DS_SPEAKERS, 2},
                    STUDIO = {"Studio Display Speakers", USB, 0, 8}, AIRPODS_CALL = {"AirPods Pro (microphone open)", BLUE, 0, 1},
                    JACK = {"External Headphones", BLTN, AR_DS_HEADPHONES, 2}, BLACKHOLE = {"BlackHole 2ch", VIRT, 0, 2};
static void test_sequences(void) {
    const device *seq[] = {&AIRPODS, &STUDIO, &MACBOOK, &AIRPODS, &MACBOOK, &STUDIO, &AIRPODS_CALL, &AIRPODS, &JACK, &BLACKHOLE, &MACBOOK};
    const int want_auto[] = {AP_SYSTEM, AP_SURROUND, AP_SPEAKERS, AP_SYSTEM, AP_SPEAKERS, AP_SURROUND, AP_FOLD, AP_SYSTEM, AP_SYSTEM, AP_FOLD, AP_SPEAKERS};
    const int told_auto[] = {6, 6, 6, 6, 6, 6, 2, 6, 6, 2, 6};
    for (int pref0 = 0; pref0 <= 1; pref0++) {
        const int pref = pref0;   // read once (hle_audio.c spatial_pref): no step may change it
        int told_before = 0, notifications = 0;
        for (unsigned i = 0; i < sizeof seq / sizeof *seq; i++) {
            const device *d = seq[i];
            audio_route_decision r = audio_route_decide(pref, audio_transport_of(d->transport), d->ds, d->ch, ALL);
            int want = pref ? want_auto[i] : (d->ch >= 6 ? AP_SURROUND : AP_FOLD), told = pref ? told_auto[i] : (d->ch >= 6 ? 6 : 2);
            CHECK(r.path == want); CHECK(r.report == told);
            if (r.path != want) printf("  step %u %s (setting %d): path %d, want %d\n", i, d->name, pref, r.path, want);
            if (told_before && told != told_before) notifications++;   // the game re-asks the count on its notification: a change of count is one re-mix
            told_before = told;
            CHECK(pref == pref0);
        }
        // auto: the game keeps mixing 5.1 across AirPods, the Studio Display and the MacBook; only the microphone-open step and BlackHole change its count (four re-mixes).
        // 0: every move between the Studio Display and a stereo device does (four too, at other steps)
        CHECK(notifications == 4);
    }
}

int main(void) {
    test_parse();
    test_transports();
    test_table();
    test_sequences();
    printf("audio route: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
