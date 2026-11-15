/* Anamorphic presentation and centred 16:9 UI compensation.
 * UI squeeze adapted from MacNativeGames' totk_ui_squeeze.h / nvn_mtl.m,
 * Copyright (c) 2026 MacNativeGames contributors, MIT license (https://github.com/ly05010419/MacNativeGames).
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#pragma once
#include <math.h>
#include <string.h>

typedef struct { double x, y, w, h; } nvn_aspect_rect;

/* The caller supplies the final canvas's Metal format classification. */
static inline int nvn_aspect_canvas_candidate(double w, double h, unsigned level, int depth, int format_ok) {
    return format_ok && w >= 640 && h > 0 && fabs(w / h * 9 / 16 - 1) < 0.01 && level == 0 && !depth;
}

/* Classify immutable shader identity once at setup. Eight-character prefixes
 * intentionally accept translation variants and different trailing key bits.
 * An empty profile never classifies a shader as a float-canvas layout. */
static inline int nvn_aspect_ui_prefix(const char *vertex, const char prefixes[][9], unsigned count) {
    if (!vertex) return 0;
    for (unsigned i = 0; i < count; i++)
        if (!strncmp(vertex, prefixes[i], 8)) return 1;
    return 0;
}

/* Packed canvases keep their shader-independent rule; floating-point canvases
 * require the profile's cached layout classification. */
static inline int nvn_aspect_ui_shader(int float_canvas, int layout_shader) {
    return !float_canvas || layout_shader;
}

/* Zero effective aspect preserves the game's valid source rectangle's aspect. */
static inline nvn_aspect_rect nvn_aspect_present(double cw, double ch, double dw, double dh, double effective) {
    if (!(effective > 0) || !isfinite(effective)) {
        double s = fmin(dw / cw, dh / ch);
        if (fabs((dw / dh) / (cw / ch) - 1) < 0.01) return (nvn_aspect_rect){0, 0, dw, dh};
        return (nvn_aspect_rect){floor((dw - cw * s) / 2), floor((dh - ch * s) / 2), round(cw * s), round(ch * s)};
    }
    double a = effective;
    if (fabs((dw / dh) / a - 1) < 0.01) return (nvn_aspect_rect){0, 0, dw, dh};
    double w = fmin(dw, dh * a), h = w / a;
    return (nvn_aspect_rect){floor((dw - w) / 2), floor((dh - h) / 2), round(w), round(h)};
}

static inline void nvn_aspect_ui_scale(double effective, double *x, double *y) {
    double f = effective > 0 && isfinite(effective) ? effective / (16.0 / 9.0) : 1;
    *x = f > 1.001 ? 1 / f : 1;
    *y = f < 0.999 ? f : 1;
}

static inline nvn_aspect_rect nvn_aspect_ui_rect(nvn_aspect_rect r, double tw, double th, double x, double y) {
    return (nvn_aspect_rect){tw / 2 + (r.x - tw / 2) * x, th / 2 + (r.y - th / 2) * y, r.w * x, r.h * y};
}

/* Canvas quads are layouts, except GPU-rendered scene pictures. Scene
 * upscale/fullscreen triangles (3 vertices) and world geometry stay untouched. */
static inline int nvn_aspect_ui_draw(int canvas, unsigned count, int scene_picture) {
    return canvas && count == 6 && !scene_picture;
}


// Bounds of a unit layout quad under its size, model and projection transforms.
// Profiles opt shaders into this uniform-layout contract; unknown shaders never use it.
static inline int nvn_aspect_layout_bounds(const float u[140], nvn_aspect_rect *out) {
    double xmin = INFINITY, ymin = INFINITY, xmax = -INFINITY, ymax = -INFINITY;
    for (unsigned corner = 0; corner < 4; corner++) {
        double x = (corner & 1) * u[132] + u[134];
        double y = -(double)(corner >> 1) * u[133] + u[135];
        double world[4] = {x*u[16] + y*u[17] + u[19],
                           x*u[20] + y*u[21] + u[23],
                           x*u[24] + y*u[25] + u[27], 1};
        double clip[4] = {0};
        for (unsigned row = 0; row < 4; row++)
            for (unsigned k = 0; k < 4; k++) clip[row] += u[row*4+k] * world[k];
        if (!isfinite(clip[3]) || clip[3] <= 0) return 0;
        x = clip[0] / clip[3]; y = clip[1] / clip[3];
        if (!isfinite(x) || !isfinite(y)) return 0;
        xmin = fmin(xmin,x); xmax = fmax(xmax,x);
        ymin = fmin(ymin,y); ymax = fmax(ymax,y);
    }
    *out = (nvn_aspect_rect){xmin,ymin,xmax-xmin,ymax-ymin};
    return 1;
}
static inline int nvn_aspect_full_width(nvn_aspect_rect r) {
    return r.x <= -0.995 && r.x + r.w >= 0.995;
}

/* Stretching a textured map backing independently of its markers changes the
 * coordinate system. Only untextured backing/fade rectangles may expand. */
static inline int nvn_aspect_expand_backing(int full_width, int textured) {
    return full_width && !textured;
}
