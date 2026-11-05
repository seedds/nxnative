// The mode devices of audio_sys.h, implemented on AVAudioConverter (mode 1) and the spatial mixer (modes 2/3).
// Everything a device needs is built at open time; the per-callback calls only copy and convert, so a call
// never allocates on the real-time thread. Whether the spatial mixer exists at all is a per-OS question: an
// open that fails is answered by the caller's fallback to its own fold, not by an error to the player.
#import "audio_sys.h"
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAXSLICE 4096          // the largest slice a caller may ask for; hle_audio.c's fold blocks stay under it
static char g_err[192];
const char *dm_arm_last_err(void) { return g_err; }
#define ARMERR(...) do { snprintf(g_err, sizeof g_err, __VA_ARGS__); } while (0)

// Channel labels in the fixed role order of audio_sys.h: front-left, front-right, centre, LFE, back-left, back-right
static const AudioChannelLabel LB[6] = { kAudioChannelLabel_Left, kAudioChannelLabel_Right, kAudioChannelLabel_Center,
                                         kAudioChannelLabel_LFE2, kAudioChannelLabel_LeftSurround, kAudioChannelLabel_RightSurround };
static const AudioChannelLayoutTag TAG[4] = { kAudioChannelLayoutTag_MPEG_5_1_A, kAudioChannelLayoutTag_MPEG_5_1_B,
                                              kAudioChannelLayoutTag_MPEG_5_1_C, kAudioChannelLayoutTag_MPEG_5_1_D };
static size_t aclSize(const AudioChannelLayout *a) { return sizeof *a + sizeof(AudioChannelDescription) * (a->mNumberChannelDescriptions ? a->mNumberChannelDescriptions - 1 : 0); }
// A standard order declares itself with its tag; any other order names its channels slot by slot. Both devices
// follow the roles the declarations carry, so a different order only changes the declaration, not the code path.
static AudioChannelLayout *makeACL(int tag, const int *role) {
    if (tag >= 0 && tag < 4) { AudioChannelLayout *a = calloc(1, sizeof *a); a->mChannelLayoutTag = TAG[tag]; return a; }
    AudioChannelLayout *a = calloc(1, sizeof *a + sizeof(AudioChannelDescription) * 5);
    a->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions; a->mNumberChannelDescriptions = 6;
    for (int s = 0; s < 6; s++) for (int r = 0; r < 6; r++) if (role[r] == s) { a->mChannelDescriptions[s].mChannelLabel = LB[r]; a->mChannelDescriptions[s].mChannelFlags = 0; }
    return a;
}

// ---------- mode 1: AVAudioConverter with an explicit downmix ----------
typedef struct { AVAudioConverter *cv; AVAudioFormat *inF, *outF; AVAudioPCMBuffer *ib, *ob; } conv;
void *dm_conv_open(const int *role, int tag, int rate) {
    AudioChannelLayout *acl = makeACL(tag, role);
    AVAudioChannelLayout *inL = [[AVAudioChannelLayout alloc] initWithLayout:acl];
    BOOL wantDesc = (tag < 0); free(acl);
    if (!inL) { ARMERR("the input channel layout did not build (%s declaration)", wantDesc ? "description list" : "standard tag"); return NULL; }
    // More than two channels needs a layout: a bare common format with 6 channels comes back nil, so the
    // layout-carrying form is the only road in.
    AVAudioFormat *inF = [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatInt16 sampleRate:rate interleaved:YES channelLayout:inL];
    AVAudioFormat *outF = [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatInt16 sampleRate:rate interleaved:YES
                                                      channelLayout:[[AVAudioChannelLayout alloc] initWithLayoutTag:kAudioChannelLayoutTag_Stereo]];
    if (!inF || !outF) { ARMERR("the formats did not build (input %d output %d)", inF != nil, outF != nil); return NULL; }
    AVAudioConverter *cv = [[AVAudioConverter alloc] initFromFormat:inF toFormat:outF];
    if (!cv) { ARMERR("the converter did not build"); return NULL; }
    cv.downmix = YES;                       // explicit: the default NO picks the front pair by role and drops the centre
    conv *c = calloc(1, sizeof *c);
    c->cv = cv; c->inF = inF; c->outF = outF;
    c->ib = [[AVAudioPCMBuffer alloc] initWithPCMFormat:inF frameCapacity:MAXSLICE];
    c->ob = [[AVAudioPCMBuffer alloc] initWithPCMFormat:outF frameCapacity:MAXSLICE];
    if (!c->ib || !c->ob) { ARMERR("the converter's buffers did not build"); dm_conv_close(c); return NULL; }
    return c;
}
int dm_conv_run(void *h, const int16_t *in, unsigned frames, int16_t *out) {
    conv *c = h; if (!c || frames > MAXSLICE) return 0;
    memcpy(c->ib.audioBufferList->mBuffers[0].mData, in, (size_t)frames * 12);
    c->ib.frameLength = (AVAudioFrameCount)frames; c->ob.frameLength = 0;
    NSError *err = nil;
    if (![c->cv convertToBuffer:c->ob fromBuffer:c->ib error:&err]) { ARMERR("the conversion failed (code %ld)", err ? (long)err.code : 0); return 0; }
    unsigned got = (unsigned)c->ob.frameLength; if (!got) return 0;
    memcpy(out, c->ob.audioBufferList->mBuffers[0].mData, (size_t)got * 4);
    return (int)got;
}
// free() runs no member cleanup, and this file compiles with ARC: the struct's five strong members release
// only when overwritten, so nil them out before the free - otherwise every close of a mode-1 device (and the
// open-failure path in dm_conv_open) leaks the converter, the two formats and the two buffers.
void dm_conv_close(void *h) {
    conv *c = h; if (!c) return;
    c->cv = nil; c->inF = nil; c->outF = nil; c->ib = nil; c->ob = nil;
    free(c);
}

// ---------- modes 2/3: the spatial mixer ----------
typedef struct {
    AudioUnit au; unsigned slice, inPos; Float64 seq;
    float *fbuf[6], *obuf[2]; AudioBufferList *obl;
} ausm;
// The mixer pulls its input through this callback; it may ask in pieces, so feeding resumes at the position
// already consumed instead of repeating a stretch.
static OSStatus ausm_input(void *ref, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *io) {
    (void)fl; (void)ts; (void)bus; ausm *a = ref;
    UInt32 n = io->mBuffers[0].mDataByteSize / (UInt32)sizeof(float);
    if (n > frames) n = frames;
    if (n > a->slice) n = a->slice;
    if (a->inPos + n > a->slice) n = a->slice > a->inPos ? a->slice - a->inPos : 0;
    for (UInt32 i = 0; i < io->mNumberBuffers && i < 6; i++) memcpy(io->mBuffers[i].mData, a->fbuf[i] + a->inPos, (size_t)n * sizeof(float));
    a->inPos += n;
    return noErr;
}
void *dm_ausm_open(const int *role, int tag, int binaural, unsigned slice) {
    AudioComponentDescription cd = { kAudioUnitType_Mixer, kAudioUnitSubType_SpatialMixer, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = AudioComponentFindNext(NULL, &cd);
    if (!comp) { ARMERR("the spatial mixer component is missing on this OS"); return NULL; }
    ausm *a = calloc(1, sizeof *a); AudioUnit au = NULL;
    if (AudioComponentInstanceNew(comp, &au)) { free(a); ARMERR("the spatial mixer instance did not build"); return NULL; }
    a->au = au; a->slice = slice > MAXSLICE ? MAXSLICE : (slice ? slice : 1024);
    for (int i = 0; i < 6; i++) a->fbuf[i] = calloc(a->slice, sizeof(float));
    for (int i = 0; i < 2; i++) a->obuf[i] = calloc(a->slice, sizeof(float));
    a->obl = malloc(sizeof(AudioBufferList) + sizeof(AudioBuffer) * 2); a->obl->mNumberBuffers = 2;
    for (int i = 0; i < 2; i++) { a->obl->mBuffers[i].mNumberChannels = 1; a->obl->mBuffers[i].mDataByteSize = (UInt32)(a->slice * sizeof(float)); a->obl->mBuffers[i].mData = a->obuf[i]; }
    OSStatus e; UInt32 one = 1, mf = (UInt32)a->slice;
    UInt32 alg = binaural ? kSpatializationAlgorithm_HRTF : kSpatializationAlgorithm_EqualPowerPanning;
    UInt32 src = kSpatialMixerSourceMode_AmbienceBed;                     // explicit: the default leaves a multi-channel bed unspatialized
    UInt32 outType = binaural ? kSpatialMixerOutputType_Headphones : kSpatialMixerOutputType_BuiltInSpeakers;
    AudioStreamBasicDescription s = { .mSampleRate = 48000, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsNonInterleaved,
        .mFramesPerPacket = 1, .mBytesPerFrame = sizeof(float), .mBytesPerPacket = sizeof(float), .mBitsPerChannel = 32 };
    AudioChannelLayout *acl = makeACL(tag, role); size_t aclSz = aclSize(acl);
#define SET(prop, scope, elem, val, sz) do { if ((e = AudioUnitSetProperty(au, prop, scope, elem, val, sz))) { ARMERR("property " #prop " refused (code %d, scope %d)", (int)e, (int)scope); goto bad; } } while (0)
    SET(kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0, &one, sizeof one);
    SET(kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &mf, sizeof mf);   // one fixed slice for the instance's life: the caller banks whole slices around it
    s.mChannelsPerFrame = 2; SET(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &s, sizeof s);
    AudioChannelLayout st = { .mChannelLayoutTag = kAudioChannelLayoutTag_Stereo };
    SET(kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Output, 0, &st, sizeof st);        // the output is stereo either way; the binaural label describes the render, it does not request more channels
    s.mChannelsPerFrame = 6; SET(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &s, sizeof s);
    SET(kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Input, 0, acl, (UInt32)aclSz);
    SET(kAudioUnitProperty_SpatializationAlgorithm, kAudioUnitScope_Input, 0, &alg, sizeof alg);
    SET(kAudioUnitProperty_SpatialMixerSourceMode, kAudioUnitScope_Input, 0, &src, sizeof src);
    SET(kAudioUnitProperty_SpatialMixerOutputType, kAudioUnitScope_Global, 0, &outType, sizeof outType);
    AURenderCallbackStruct rcs = { ausm_input, a };
    SET(kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &rcs, sizeof rcs);
    free(acl); acl = NULL;
    if ((e = AudioUnitInitialize(au))) { ARMERR("the spatial mixer did not initialize (code %d)", (int)e); goto bad; }
#undef SET
    return a;
bad:
    free(acl); if (au) AudioComponentInstanceDispose(au);
    for (int i = 0; i < 6; i++) free(a->fbuf[i]); for (int i = 0; i < 2; i++) free(a->obuf[i]); free(a->obl); free(a);
    return NULL;
}
int dm_ausm_run(void *h, const int16_t *in6, unsigned frames, int16_t *out2) {
    ausm *a = h; if (!a || frames != a->slice) return 0;                 // one fixed slice: a different length produces nothing
    a->inPos = 0;
    for (unsigned f = 0; f < frames; f++) for (int c = 0; c < 6; c++) a->fbuf[c][f] = (float)in6[f * 6 + c] * (1.0f / 32768.0f);
    AudioUnitRenderActionFlags fl = 0; AudioTimeStamp ts; memset(&ts, 0, sizeof ts);
    ts.mSampleTime = a->seq; ts.mFlags = kAudioTimeStampSampleTimeValid; a->seq += frames;
    OSStatus e = AudioUnitRender(a->au, &fl, &ts, 0, (UInt32)frames, a->obl);
    if (e) { ARMERR("the render failed (code %d)", (int)e); return 0; }
    for (unsigned f = 0; f < frames; f++) for (int i = 0; i < 2; i++) {
        long x = lrintf(a->obuf[i][f] * 32768.0f);
        out2[f * 2 + i] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); }
    return (int)frames;
}
void dm_ausm_close(void *h) {
    ausm *a = h; if (!a) return;
    if (a->au) { AudioUnitSetProperty(a->au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &(AURenderCallbackStruct){ NULL, NULL }, sizeof(AURenderCallbackStruct)); AudioUnitUninitialize(a->au); AudioComponentInstanceDispose(a->au); }
    for (int i = 0; i < 6; i++) free(a->fbuf[i]); for (int i = 0; i < 2; i++) free(a->obuf[i]); free(a->obl); free(a);
}
