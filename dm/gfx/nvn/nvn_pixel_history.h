// Opt-in, one-frame render-pass pixel history for the traditional Metal backend.
// Included after geometry_diag_frame; no shader variants or CPU/GPU waits.
// DM_NVN_PIXEL_HISTORY="u,v;u,v;u,v" selects up to three normalized points.
// A changed geometry.capture token arms the same single frame as geometry logs.
#pragma once

typedef struct {
    __strong id<MTLTexture> texture;
    NSUInteger level, slice, plane;
    uint64_t serial;
    unsigned bpp; int slot, depth;
} ph_attachment;
typedef struct {
    uint64_t frame, pass, serial;
    uint32_t format, level, slice, plane, width, height, x, y, point, bpp, offset;
    int32_t slot, depth;
} ph_pixel;
static struct {
    int initialized, points, active, attachments, passes, draws;
    double xy[3][2]; uint64_t frame, pass;
    ph_attachment att[9];
    char first_vs[20], first_fs[20], last_vs[20], last_fs[20];
} g_pixel_history;

static int pixel_history_configured(void) {
    if (!g_pixel_history.initialized) {
        g_pixel_history.initialized = 1;
        const char *p = getenv("DM_NVN_PIXEL_HISTORY");
        while (p && *p && g_pixel_history.points < 3) {
            char *end; double u = strtod(p, &end); if (end == p || *end != ',') break;
            p = end + 1; double v = strtod(p, &end); if (end == p) break;
            if (!isfinite(u) || !isfinite(v) || u < 0 || u > 1 || v < 0 || v > 1) break;
            int i = g_pixel_history.points++; g_pixel_history.xy[i][0] = u; g_pixel_history.xy[i][1] = v;
            if (*end != ';') break; p = end + 1;
        }
        if (g_pixel_history.points) MLOG("[pixel-history] configured %d points, max 128 passes per captured frame; raw stored channel values, no CPU wait", g_pixel_history.points);
    }
    return g_pixel_history.points != 0;
}
// Explicit whitelist: unsupported / block-compressed formats are never copied.
static unsigned pixel_history_bytes(MTLPixelFormat f, int depth) {
    if (depth) {
        if (f == MTLPixelFormatDepth16Unorm) return 2;
        if (f == MTLPixelFormatDepth32Float || f == MTLPixelFormatDepth32Float_Stencil8) return 4;
        return 0;
    }
    switch (f) {
        case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Uint: return 1;
        case MTLPixelFormatR16Unorm: case MTLPixelFormatR16Float: case MTLPixelFormatR16Uint: case MTLPixelFormatRG8Unorm: return 2;
        case MTLPixelFormatR32Float: case MTLPixelFormatR32Uint: case MTLPixelFormatRG16Float:
        case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB:
        case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatBGRA8Unorm_sRGB:
        case MTLPixelFormatRGB10A2Unorm: case MTLPixelFormatRG11B10Float: return 4;
        case MTLPixelFormatRG32Float: case MTLPixelFormatRGBA16Float: return 8;
        case MTLPixelFormatRGBA32Float: return 16;
        default: return 0;
    }
}
static void pixel_history_add(MTLRenderPassAttachmentDescriptor *a, int slot, int depth, uint64_t serial) {
    id<MTLTexture> t = a.texture; if (!t || g_pixel_history.attachments >= 9) return;
    const char *skip = NULL; unsigned bpp = pixel_history_bytes(t.pixelFormat, depth);
    if (t.storageMode == MTLStorageModeMemoryless) skip = "memoryless";
    else if (t.framebufferOnly) skip = "framebuffer-only";
    else if (t.sampleCount != 1) skip = "multisampled";
    else if (!bpp) skip = "unsupported-format";
    else if (t.textureType != MTLTextureType2D && t.textureType != MTLTextureType2DArray &&
             t.textureType != MTLTextureTypeCube && t.textureType != MTLTextureTypeCubeArray && t.textureType != MTLTextureType3D) skip = "unsupported-type";
    else if (a.level >= t.mipmapLevelCount) skip = "invalid-level";
    if (skip) {
        MLOG("[pixel-history-skip] frame=%llu pass=%llu slot=%d depth=%d format=%lu type=%lu reason=%s",
             (unsigned long long)g_pixel_history.frame, (unsigned long long)g_pixel_history.pass, slot, depth,
             (unsigned long)t.pixelFormat, (unsigned long)t.textureType, skip); return;
    }
    NSUInteger slices = t.arrayLength * (t.textureType == MTLTextureTypeCube || t.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    if ((t.textureType == MTLTextureType3D && a.depthPlane >= MAX(t.depth >> a.level, 1ul)) ||
        (t.textureType != MTLTextureType3D && a.slice >= slices)) {
        MLOG("[pixel-history-skip] frame=%llu pass=%llu slot=%d reason=invalid-slice", (unsigned long long)g_pixel_history.frame, (unsigned long long)g_pixel_history.pass, slot); return;
    }
    ph_attachment *p = &g_pixel_history.att[g_pixel_history.attachments++];
    p->texture = t; p->level = a.level; p->slice = a.slice; p->plane = a.depthPlane;
    p->serial = serial; p->slot = slot; p->depth = depth; p->bpp = bpp;
}
static void pixel_history_begin(MTLRenderPassDescriptor *pd, NSUInteger nc) {
    g_pixel_history.active = 0;
    if (!pixel_history_configured() || !geometry_diag_frame()) return;
    uint64_t frame = atomic_load(&g_cur_frame);
    if (g_pixel_history.frame != frame) { g_pixel_history.frame = frame; g_pixel_history.passes = 0; }
    if (g_pixel_history.passes >= 128) return;
    g_pixel_history.passes++; g_pixel_history.pass = g_stat_pass + 1;
    g_pixel_history.attachments = g_pixel_history.draws = 0;
    g_pixel_history.first_vs[0] = g_pixel_history.first_fs[0] = g_pixel_history.last_vs[0] = g_pixel_history.last_fs[0] = 0;
    for (int i = 0; i < 9; i++) g_pixel_history.att[i].texture = nil;
    for (NSUInteger i = 0; i < nc && i < 8; i++)
        pixel_history_add(pd.colorAttachments[i], (int)i, 0, R.rt.color[i] ? R.rt.color[i]->serial : 0);
    pixel_history_add(pd.depthAttachment, -1, 1, R.rt.depth ? R.rt.depth->serial : 0);
    g_pixel_history.active = g_pixel_history.attachments > 0;
}
static void pixel_history_draw(const char *vs, const char *fs) {
    if (!g_pixel_history.active) return;
    if (!g_pixel_history.draws++) {
        snprintf(g_pixel_history.first_vs, 20, "%s", vs); snprintf(g_pixel_history.first_fs, 20, "%s", fs);
    }
    snprintf(g_pixel_history.last_vs, 20, "%s", vs); snprintf(g_pixel_history.last_fs, 20, "%s", fs);
}
static void pixel_history_store(void) {
    if (!g_pixel_history.active || !tbdr_on()) return; // sd_finish already stores every ordinary attachment when TBDR is off.
    for (int i = 0; i < g_pixel_history.attachments; i++) {
        const ph_attachment *a = &g_pixel_history.att[i];
        if (a->depth) [g_enc setDepthStoreAction:MTLStoreActionStore];
        else [g_enc setColorStoreAction:MTLStoreActionStore atIndex:(NSUInteger)a->slot];
        MLOG("[pixel-history-store] frame=%llu pass=%llu slot=%d diagnostic-only Store overrides deferred policy",
             (unsigned long long)g_pixel_history.frame, (unsigned long long)g_pixel_history.pass, a->slot);
    }
}
static float pixel_history_ufloat(unsigned x, unsigned bits) {
    unsigned m = x & ((1u << bits) - 1), e = x >> bits;
    if (e == 31) return m ? NAN : INFINITY;
    return e ? ldexpf(1.0f + (float)m / (float)(1u << bits), (int)e - 15) : ldexpf((float)m, 1 - 15 - (int)bits);
}
static void pixel_history_rgba(const uint8_t *b, MTLPixelFormat f, int depth, float out[4]) {
    uint16_t h[4] = {0}; uint32_t w[4] = {0}; memcpy(h, b, 8); memcpy(w, b, 16);
    out[0] = out[1] = out[2] = 0; out[3] = 1;
    if (depth) { if (f == MTLPixelFormatDepth16Unorm) out[0] = h[0] / 65535.0f; else memcpy(out, b, 4); return; }
    switch (f) {
        case MTLPixelFormatR8Unorm: out[0] = b[0] / 255.0f; break;
        case MTLPixelFormatR8Uint: out[0] = b[0]; break;
        case MTLPixelFormatR16Unorm: out[0] = h[0] / 65535.0f; break;
        case MTLPixelFormatR16Uint: out[0] = h[0]; break;
        case MTLPixelFormatRG8Unorm: out[0] = b[0] / 255.0f; out[1] = b[1] / 255.0f; break;
        case MTLPixelFormatR32Uint: out[0] = (float)w[0]; break;
        case MTLPixelFormatR32Float: memcpy(out, b, 4); break;
        case MTLPixelFormatRG32Float: memcpy(out, b, 8); break;
        case MTLPixelFormatRGBA32Float: memcpy(out, b, 16); break;
        case MTLPixelFormatR16Float: case MTLPixelFormatRG16Float: case MTLPixelFormatRGBA16Float: {
            int n = f == MTLPixelFormatR16Float ? 1 : f == MTLPixelFormatRG16Float ? 2 : 4;
            for (int i = 0; i < n; i++) { _Float16 v; memcpy(&v, h + i, 2); out[i] = (float)v; } break; }
        case MTLPixelFormatRGB10A2Unorm: out[0] = (w[0] & 1023) / 1023.0f; out[1] = (w[0] >> 10 & 1023) / 1023.0f; out[2] = (w[0] >> 20 & 1023) / 1023.0f; out[3] = (w[0] >> 30) / 3.0f; break;
        case MTLPixelFormatRG11B10Float: out[0] = pixel_history_ufloat(w[0] & 2047, 6); out[1] = pixel_history_ufloat(w[0] >> 11 & 2047, 6); out[2] = pixel_history_ufloat(w[0] >> 22, 5); break;
        default: {
            int bgra = f == MTLPixelFormatBGRA8Unorm || f == MTLPixelFormatBGRA8Unorm_sRGB;
            out[0] = b[bgra ? 2 : 0] / 255.0f; out[1] = b[1] / 255.0f; out[2] = b[bgra ? 0 : 2] / 255.0f; out[3] = b[3] / 255.0f; break; }
    }
}
static void pixel_history_end(void) {
    if (!g_pixel_history.active) return;
    g_pixel_history.active = 0;
    if (!g_cb) return;
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    ph_pixel records[27]; int n = 0;
    // This tiny diagnostic buffer is owned by its completion callback, rather
    // than reused through a frame arena while a CPU logger is still reading it.
    id<MTLBuffer> buffer = [g_dev newBufferWithLength:(size_t)g_pixel_history.attachments * g_pixel_history.points * 256 options:MTLResourceStorageModeShared];
    if (!buffer) {
        MLOG("[pixel-history] readback allocation failed; pass omitted");
        for (int i = 0; i < g_pixel_history.attachments; i++) g_pixel_history.att[i].texture = nil;
        return;
    }
    size_t off = 0;
    memset(buffer.contents, 0, buffer.length);
    NSMutableArray *sources = [NSMutableArray array]; id<MTLBlitCommandEncoder> bl = enc_b_begin(g_cb);
    for (int i = 0; i < g_pixel_history.attachments; i++) {
        ph_attachment *a = &g_pixel_history.att[i]; id<MTLTexture> t = a->texture; [sources addObject:t];
        NSUInteger width = MAX(t.width >> a->level, 1ul), height = MAX(t.height >> a->level, 1ul);
        for (int k = 0; k < g_pixel_history.points; k++) {
            NSUInteger x = MIN((NSUInteger)(g_pixel_history.xy[k][0] * width), width - 1), y = MIN((NSUInteger)(g_pixel_history.xy[k][1] * height), height - 1);
            ph_pixel *p = &records[n]; *p = (ph_pixel){.frame = g_pixel_history.frame, .pass = g_pixel_history.pass, .serial = a->serial,
                .format = (uint32_t)t.pixelFormat, .level = (uint32_t)a->level, .slice = (uint32_t)a->slice, .plane = (uint32_t)a->plane,
                .width = (uint32_t)width, .height = (uint32_t)height, .x = (uint32_t)x, .y = (uint32_t)y, .point = (uint32_t)k,
                .bpp = a->bpp, .offset = (uint32_t)(n * 256), .slot = a->slot, .depth = a->depth};
            MTLBlitOption options = a->depth && t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone;
            [bl copyFromTexture:t sourceSlice:t.textureType == MTLTextureType3D ? 0 : a->slice sourceLevel:a->level
                sourceOrigin:MTLOriginMake(x, y, t.textureType == MTLTextureType3D ? a->plane : 0) sourceSize:MTLSizeMake(1, 1, 1)
                toBuffer:buffer destinationOffset:off + p->offset destinationBytesPerRow:256 destinationBytesPerImage:256 options:options];
            n++;
        }
        a->texture = nil;
    }
    enc_b_end(bl);
    NSData *metadata = [NSData dataWithBytes:records length:(size_t)n * sizeof *records];
    NSString *draws = [NSString stringWithFormat:@"draws=%d first_vs=%s first_fs=%s last_vs=%s last_fs=%s", g_pixel_history.draws,
        g_pixel_history.first_vs, g_pixel_history.first_fs, g_pixel_history.last_vs, g_pixel_history.last_fs];
    __block NSArray *source_refs = [sources copy]; id<MTLBuffer> keep = buffer;
    [g_cb addCompletedHandler:^(id<MTLCommandBuffer> cb) {
        if (cb.error) { MLOG("[pixel-history] GPU command failed; captured values omitted"); source_refs = nil; return; }
        const ph_pixel *r = metadata.bytes;
        for (int i = 0; i < n; i++) {
            const uint8_t *bytes = (const uint8_t *)keep.contents + off + r[i].offset;
            uint32_t raw[4] = {0}; memcpy(raw, bytes, r[i].bpp); float rgba[4]; pixel_history_rgba(bytes, (MTLPixelFormat)r[i].format, r[i].depth, rgba);
            MLOG("[pixel-history] frame=%llu pass=%llu slot=%d depth=%d serial=%llu format=%u level=%u slice=%u plane=%u size=%ux%u point=%u xy=%u,%u bytes=%u raw=%08x,%08x,%08x,%08x rgba_stored=%g,%g,%g,%g %s",
                (unsigned long long)r[i].frame, (unsigned long long)r[i].pass, r[i].slot, r[i].depth, (unsigned long long)r[i].serial,
                r[i].format, r[i].level, r[i].slice, r[i].plane, r[i].width, r[i].height, r[i].point, r[i].x, r[i].y, r[i].bpp,
                raw[0], raw[1], raw[2], raw[3], rgba[0], rgba[1], rgba[2], rgba[3], draws.UTF8String);
        }
        source_refs = nil;
    }];
}
