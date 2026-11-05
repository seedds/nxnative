// Thread-safe scene geometry publication; contains no Apple framework types.
#pragma once
#include <stdint.h>

#define DM_NATIVE_RENDER_HEIGHT_DEFAULT 720u
#define DM_NATIVE_RENDER_HEIGHT_MIN 360u
#define DM_NATIVE_RENDER_HEIGHT_MAX 2160u

typedef struct {
    double width_points, height_points, scale;
    double aspect;                  // Landscape width_points / height_points.
    uint32_t pixel_width, pixel_height;
    uint64_t generation;            // Changes only when valid scene geometry changes.
} dm_native_display_request;

typedef struct {
    dm_native_display_request scene;
    uint32_t width, height;         // Guest output dimensions, width rounded to nearest even pixel.
    double aspect;                 // Actual width / height after integer rounding.
} dm_native_display_render;

// Call from scene layout with bounds in points and the scene's screen scale.
// Portrait bounds are normalized to landscape. Invalid/transient geometry is ignored.
// Returns 1 for an accepted request (including unchanged geometry), 0 if invalid.
int dm_native_display_publish(double width_points, double height_points, double scale);
// Returns 0 until the first valid publication. out must be non-NULL.
int dm_native_display_snapshot(dm_native_display_request *out);
// Freeze once, after the window scene has published valid geometry, before game init.
// height == 0 selects 720; other heights are bounded to [360, 2160].
// Returns 0 if no valid geometry exists. Further calls return the original dimensions.
// Later scene changes remain visible via snapshot().generation, but cannot resize
// live game resources implicitly. The caller must handle future frame-boundary updates.
int dm_native_display_freeze(uint32_t height, dm_native_display_render *out);
int dm_native_display_frozen(dm_native_display_render *out);
