#include "native_display.h"
#include <math.h>
#include <pthread.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static dm_native_display_request requested;
static dm_native_display_render frozen;

int dm_native_display_publish(double width, double height, double scale) {
    // Reject incomplete scene transitions and nonsensical dimensions before conversion.
    if (!isfinite(width) || !isfinite(height) || !isfinite(scale) ||
        width < 1 || height < 1 || scale <= 0 || scale > 16) return 0;
    if (height > width) { double tmp = height; height = width; width = tmp; }
    if (width / height > 8 || width * scale > 65535 || height * scale > 65535 ||
        width * scale < 1 || height * scale < 1) return 0;
    pthread_mutex_lock(&lock);
    if (!requested.generation || requested.width_points != width ||
        requested.height_points != height || requested.scale != scale) {
        requested.width_points = width;
        requested.height_points = height;
        requested.scale = scale;
        requested.aspect = width / height;
        requested.pixel_width = (uint32_t)floor(width * scale + 0.5);
        requested.pixel_height = (uint32_t)floor(height * scale + 0.5);
        ++requested.generation;
        if (!requested.generation) ++requested.generation;
    }
    pthread_mutex_unlock(&lock);
    return 1;
}

int dm_native_display_snapshot(dm_native_display_request *out) {
    if (!out) return 0;
    pthread_mutex_lock(&lock);
    int valid = requested.generation != 0;
    if (valid) *out = requested;
    pthread_mutex_unlock(&lock);
    return valid;
}

int dm_native_display_freeze(uint32_t height, dm_native_display_render *out) {
    if (!out) return 0;
    pthread_mutex_lock(&lock);
    if (!frozen.scene.generation && requested.generation) {
        if (!height) height = DM_NATIVE_RENDER_HEIGHT_DEFAULT;
        if (height < DM_NATIVE_RENDER_HEIGHT_MIN) height = DM_NATIVE_RENDER_HEIGHT_MIN;
        if (height > DM_NATIVE_RENDER_HEIGHT_MAX) height = DM_NATIVE_RENDER_HEIGHT_MAX;
        frozen.scene = requested;
        frozen.height = height;
        frozen.width = (uint32_t)(2 * floor(height * requested.aspect / 2 + 0.5));
        frozen.aspect = (double)frozen.width / height;
    }
    int valid = frozen.scene.generation != 0;
    if (valid) *out = frozen;
    pthread_mutex_unlock(&lock);
    return valid;
}

int dm_native_display_frozen(dm_native_display_render *out) {
    if (!out) return 0;
    pthread_mutex_lock(&lock);
    int valid = frozen.scene.generation != 0;
    if (valid) *out = frozen;
    pthread_mutex_unlock(&lock);
    return valid;
}
