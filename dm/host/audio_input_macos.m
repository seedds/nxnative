#include "audio_input.h"
#include "hle.h"
#include <AVFAudio/AVFAudio.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <TargetConditionals.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { INPUT_CHANNEL_MAX = 8, INPUT_FALLBACK_MAX_FRAMES = 4096 };

typedef struct {
    AudioUnit unit;
    AudioDeviceID device;
    AudioStreamBasicDescription format;
    AudioBufferList *buffers;
    float *planes[INPUT_CHANNEL_MAX];
    UInt32 max_frames;
    int channel_count, listener_installed, listener_devices, uses_default;
    char name[0x100], uid[0x100], selected_uid[0x100];
    _Atomic int running, failed;
    _Atomic uint32_t callbacks;
} input_device;

static input_device g_input;
static pthread_mutex_t g_input_lock = PTHREAD_MUTEX_INITIALIZER;
static OSStatus input_device_changed(AudioObjectID object, UInt32 count, const AudioObjectPropertyAddress addresses[], void *ctx);

static AudioObjectPropertyAddress default_input_address(void) {
    return (AudioObjectPropertyAddress){ kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
}

static AudioObjectPropertyAddress devices_address(void) {
    return (AudioObjectPropertyAddress){ kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
}

static AudioDeviceID default_input_device(void) {
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof device;
    AudioObjectPropertyAddress address = default_input_address();
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, &device) != noErr) return kAudioObjectUnknown;
    return device;
}

static int device_string(AudioDeviceID device, AudioObjectPropertySelector selector, char out[0x100]) {
    memset(out, 0, 0x100);
    AudioObjectPropertyAddress address = { selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef value = NULL; UInt32 size = sizeof value;
    if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &value) != noErr || !value) return 0;
    int ok = CFStringGetCString(value, out, 0x100, kCFStringEncodingUTF8);
    CFRelease(value);
    return ok && out[0];
}

static AudioDeviceID input_device_for_uid(const char *uid) {
    if (!uid || !*uid) return kAudioObjectUnknown;
    AudioObjectPropertyAddress address = devices_address(); UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, NULL, &size) != noErr || !size) return kAudioObjectUnknown;
    AudioDeviceID *devices = malloc(size);
    if (!devices) return kAudioObjectUnknown;
    AudioDeviceID found = kAudioObjectUnknown;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, devices) == noErr) {
        UInt32 count = size / sizeof *devices;
        for (UInt32 i = 0; i < count; i++) {
            char candidate[0x100], name[0x100];
            if (!device_string(devices[i], kAudioDevicePropertyDeviceUID, candidate) || strcmp(candidate, uid)) continue;
            if (!device_string(devices[i], kAudioObjectPropertyName, name)) continue;
            AudioObjectPropertyAddress inputs = { kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeInput, kAudioObjectPropertyElementMain };
            UInt32 channels_size = 0;
            if (AudioObjectGetPropertyDataSize(devices[i], &inputs, 0, NULL, &channels_size) == noErr && channels_size) found = devices[i];
            break;
        }
    }
    free(devices);
    return found;
}

static AudioDeviceID selected_input_device(int *uses_default, int *fell_back) {
    const char *uid = getenv("DM_MIC_DEVICE");
    *uses_default = !uid || !*uid || !strcmp(uid, "default");
    *fell_back = 0;
    if (*uses_default) return default_input_device();
    AudioDeviceID selected = input_device_for_uid(uid);
    if (selected != kAudioObjectUnknown) return selected;
    *fell_back = 1;
    return default_input_device();
}

static AVAudioApplicationRecordPermission microphone_permission_status(void) {
    return [AVAudioApplication.sharedInstance recordPermission];
}

static int microphone_permission_granted(void) {
    AVAudioApplicationRecordPermission permission = microphone_permission_status();
    if (permission == AVAudioApplicationRecordPermissionGranted) {
        fprintf(stderr, "audio input: microphone permission allowed in host\n");
        return 1;
    }
    const char *status = permission == AVAudioApplicationRecordPermissionDenied ? "denied" : "not asked";
    fprintf(stderr, "audio input: microphone permission %s in host; capture remains closed\n", status);
    return 0;
}

int dm_audio_input_platform_device_available(void) {
    int uses_default = 0, fell_back = 0;
    if (atomic_load_explicit(&g_input.failed, memory_order_acquire) || selected_input_device(&uses_default, &fell_back) == kAudioObjectUnknown) return 0;
    return microphone_permission_status() != AVAudioApplicationRecordPermissionDenied;
}

int dm_audio_input_platform_failed(void) { return atomic_load_explicit(&g_input.failed, memory_order_acquire); }

static void input_cleanup_locked(void) {
    if (!g_input.unit) return;
    if (atomic_exchange_explicit(&g_input.running, 0, memory_order_acq_rel)) AudioOutputUnitStop(g_input.unit);
    while (atomic_load_explicit(&g_input.callbacks, memory_order_acquire)) sched_yield();
    if (g_input.listener_installed) {
        AudioObjectPropertyAddress address = g_input.listener_devices ? devices_address() : default_input_address();
        AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &address, input_device_changed, &g_input);
        g_input.listener_installed = 0;
    }
    AudioUnitUninitialize(g_input.unit);
    AudioComponentInstanceDispose(g_input.unit);
    for (int i = 0; i < INPUT_CHANNEL_MAX; i++) { free(g_input.planes[i]); g_input.planes[i] = NULL; }
    free(g_input.buffers); g_input.buffers = NULL;
    g_input.unit = NULL; g_input.device = kAudioObjectUnknown; g_input.channel_count = 0; g_input.max_frames = 0;
    g_input.listener_devices = g_input.uses_default = 0;
    memset(g_input.name, 0, sizeof g_input.name); memset(g_input.uid, 0, sizeof g_input.uid); memset(g_input.selected_uid, 0, sizeof g_input.selected_uid);
    memset(&g_input.format, 0, sizeof g_input.format);
    atomic_store_explicit(&g_input.failed, 0, memory_order_release);
}

static OSStatus input_proc(void *ctx, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *time, UInt32 bus, UInt32 frames, AudioBufferList *io) {
    (void)io;
    input_device *d = ctx;
    atomic_fetch_add_explicit(&d->callbacks, 1, memory_order_acquire);
    if (!atomic_load_explicit(&d->running, memory_order_acquire) || atomic_load_explicit(&d->failed, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&d->callbacks, 1, memory_order_release);
        return noErr;
    }
    if (!d->buffers || frames > d->max_frames) {
        atomic_store_explicit(&d->failed, 1, memory_order_release);
        dm_audio_input_capture_failed();
        atomic_fetch_sub_explicit(&d->callbacks, 1, memory_order_release);
        return kAudio_ParamError;
    }
    for (int i = 0; i < d->channel_count; i++) d->buffers->mBuffers[i].mDataByteSize = frames * sizeof(float);
    OSStatus status = AudioUnitRender(d->unit, flags, time, 1, frames, d->buffers);
    if (status != noErr) {
        atomic_store_explicit(&d->failed, 1, memory_order_release);
        dm_audio_input_capture_failed();
        atomic_fetch_sub_explicit(&d->callbacks, 1, memory_order_release);
        return status;
    }
    const float *planes[INPUT_CHANNEL_MAX];
    for (int i = 0; i < d->channel_count; i++) planes[i] = d->planes[i];
    dm_audio_input_process(planes, d->channel_count, frames, d->format.mSampleRate);
    atomic_fetch_sub_explicit(&d->callbacks, 1, memory_order_release);
    return noErr;
}

static OSStatus input_device_changed(AudioObjectID object, UInt32 count, const AudioObjectPropertyAddress addresses[], void *ctx) {
    (void)object; (void)count; (void)addresses;
    input_device *d = ctx;
    int changed = d->uses_default ? default_input_device() != d->device : input_device_for_uid(d->selected_uid) != d->device;
    if (changed) {
        fprintf(stderr, "[dm] audio input: selected device disconnected or changed; current stream is closed and will use the selected/default device on its next open\n");
        atomic_store_explicit(&d->failed, 1, memory_order_release);
        dm_audio_input_capture_failed();
    }
    return noErr;
}

static int input_prepare_locked(void) {
    if (g_input.unit) return !atomic_load_explicit(&g_input.failed, memory_order_acquire);
    if (atomic_load_explicit(&g_input.failed, memory_order_acquire)) return 0;
    int uses_default = 0, fell_back = 0;
    AudioDeviceID device = selected_input_device(&uses_default, &fell_back);
    if (device == kAudioObjectUnknown || !microphone_permission_granted()) return 0;
    if (fell_back) fprintf(stderr, "[dm] audio input: selected UID is unavailable; falling back to the system default input\n");
    AudioComponentDescription desc = { kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent component = AudioComponentFindNext(NULL, &desc);
    if (!component) return 0;
    AudioUnit unit = NULL;
    OSStatus status = AudioComponentInstanceNew(component, &unit);
    UInt32 enabled = 1, disabled = 0;
    if (status == noErr) status = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &enabled, sizeof enabled);
    if (status == noErr) status = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &disabled, sizeof disabled);
    if (status == noErr) status = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device, sizeof device);

    AudioStreamBasicDescription device_format = {0};
    UInt32 size = sizeof device_format;
    if (status == noErr) status = AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1, &device_format, &size);
    if (status != noErr || device_format.mChannelsPerFrame < 1 || device_format.mChannelsPerFrame > INPUT_CHANNEL_MAX ||
        device_format.mSampleRate < 8000.0 || device_format.mSampleRate > 192000.0) {
        if (unit) AudioComponentInstanceDispose(unit);
        return 0;
    }

    AudioStreamBasicDescription client = {0};
    client.mSampleRate = device_format.mSampleRate;  // AUHAL does not rate-convert microphone input; the core resampler does.
    client.mFormatID = kAudioFormatLinearPCM;
    client.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    client.mBytesPerPacket = sizeof(float); client.mFramesPerPacket = 1; client.mBytesPerFrame = sizeof(float);
    client.mChannelsPerFrame = device_format.mChannelsPerFrame; client.mBitsPerChannel = 32;
    status = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &client, sizeof client);
    AURenderCallbackStruct callback = { input_proc, &g_input };
    if (status == noErr) status = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &callback, sizeof callback);
    if (status == noErr) status = AudioUnitInitialize(unit);
    UInt32 max_frames = 0; size = sizeof max_frames;
    if (status == noErr) status = AudioUnitGetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &max_frames, &size);
    if (status != noErr) {
        if (unit) { AudioUnitUninitialize(unit); AudioComponentInstanceDispose(unit); }
        return 0;
    }
    if (!max_frames) max_frames = INPUT_FALLBACK_MAX_FRAMES;
    UInt32 channels = client.mChannelsPerFrame;
    AudioBufferList *buffers = calloc(1, sizeof(AudioBufferList) + (channels - 1) * sizeof(AudioBuffer));
    if (!buffers) { AudioUnitUninitialize(unit); AudioComponentInstanceDispose(unit); return 0; }
    buffers->mNumberBuffers = channels;
    float *planes[INPUT_CHANNEL_MAX] = {0};
    for (UInt32 i = 0; i < channels; i++) {
        planes[i] = calloc(max_frames, sizeof(float));
        if (!planes[i]) { status = memFullErr; break; }
        buffers->mBuffers[i].mNumberChannels = 1;
        buffers->mBuffers[i].mDataByteSize = max_frames * sizeof(float);
        buffers->mBuffers[i].mData = planes[i];
    }
    if (status != noErr) {
        for (int i = 0; i < INPUT_CHANNEL_MAX; i++) free(planes[i]);
        free(buffers); AudioUnitUninitialize(unit); AudioComponentInstanceDispose(unit); return 0;
    }

    g_input.unit = unit; g_input.device = device; g_input.format = client; g_input.buffers = buffers;
    g_input.max_frames = max_frames; g_input.channel_count = (int)channels;
    g_input.uses_default = uses_default || fell_back;
    const char *selected_uid = getenv("DM_MIC_DEVICE");
    if (!uses_default && !fell_back && selected_uid) strlcpy(g_input.selected_uid, selected_uid, sizeof g_input.selected_uid);
    if (!device_string(device, kAudioObjectPropertyName, g_input.name)) strlcpy(g_input.name, "Unknown input device", sizeof g_input.name);
    if (!device_string(device, kAudioDevicePropertyDeviceUID, g_input.uid)) strlcpy(g_input.uid, "unknown", sizeof g_input.uid);
    fprintf(stderr, "[dm] audio input: opened device=\"%s\" uid=\"%s\" rate=%.0f channels=%u%s\n", g_input.name, g_input.uid,
            client.mSampleRate, (unsigned)channels, uses_default ? " (system default)" : fell_back ? " (fallback)" : " (selected)");
    for (int i = 0; i < INPUT_CHANNEL_MAX; i++) g_input.planes[i] = planes[i];
    atomic_store_explicit(&g_input.failed, 0, memory_order_release);
    AudioObjectPropertyAddress address = g_input.uses_default ? default_input_address() : devices_address();
    g_input.listener_devices = !g_input.uses_default;
    if (AudioObjectAddPropertyListener(kAudioObjectSystemObject, &address, input_device_changed, &g_input) == noErr) g_input.listener_installed = 1;
    return 1;
}

int dm_audio_input_platform_prepare(void) {
    pthread_mutex_lock(&g_input_lock);
    int ok = input_prepare_locked();
    pthread_mutex_unlock(&g_input_lock);
    return ok ? 0 : -1;
}

int dm_audio_input_platform_start(void) {
    pthread_mutex_lock(&g_input_lock);
    int ok = g_input.unit && !atomic_load_explicit(&g_input.failed, memory_order_acquire);
    if (ok && !atomic_load_explicit(&g_input.running, memory_order_relaxed)) {
        atomic_store_explicit(&g_input.running, 1, memory_order_release);
        OSStatus status = AudioOutputUnitStart(g_input.unit);
        if (status != noErr) {
            atomic_store_explicit(&g_input.running, 0, memory_order_release);
            atomic_store_explicit(&g_input.failed, 1, memory_order_release);
            dm_audio_input_capture_failed();
            LOG("[RED] audio input: AUHAL start failed (%d)", (int)status);
            ok = 0;
        }
    }
    pthread_mutex_unlock(&g_input_lock);
    return ok ? 0 : -1;
}

void dm_audio_input_platform_stop(void) {
    pthread_mutex_lock(&g_input_lock);
    if (g_input.unit && atomic_exchange_explicit(&g_input.running, 0, memory_order_acq_rel)) AudioOutputUnitStop(g_input.unit);
    pthread_mutex_unlock(&g_input_lock);
}

void dm_audio_input_platform_close(void) {
    pthread_mutex_lock(&g_input_lock);
    input_cleanup_locked();
    pthread_mutex_unlock(&g_input_lock);
}

void dm_audio_input_platform_synchronize(void) {
    while (atomic_load_explicit(&g_input.callbacks, memory_order_acquire)) sched_yield();
}

void dm_audio_input_platform_info(char name[0x100], char uid[0x100], double *sample_rate, int *channels) {
    pthread_mutex_lock(&g_input_lock);
    if (name) strlcpy(name, g_input.name[0] ? g_input.name : "unknown", 0x100);
    if (uid) strlcpy(uid, g_input.uid[0] ? g_input.uid : "unknown", 0x100);
    if (sample_rate) *sample_rate = g_input.format.mSampleRate;
    if (channels) *channels = g_input.channel_count;
    pthread_mutex_unlock(&g_input_lock);
}
