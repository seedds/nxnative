#pragma once
#include "nvn_zero_image_shadow.h"
static __strong id<MTLBuffer> g_zero_image_buffer;
static __strong id<MTLTexture> g_zero_image_texture;
static __strong NvShader *g_zero_image_shader;
static uint32_t g_zero_image_layout[6];
static uint64_t g_zero_image_serial;
static int g_zero_image_tid;

static int zero_image_alias(const tex_t *a, const tex_t *b) {
    if (!a || !b) return 0;
    if (a->serial == b->serial) return 1;
    uint64_t an = a->L.size, bn = b->L.size;
    if (a->pool && a->pool == b->pool) {
        if (!an || !bn) return 1;
        if (a->off <= b->off ? b->off - a->off < an : a->off - b->off < bn) return 1;
    }
    if (a->mem && b->mem) {
        uint64_t ap = (uintptr_t)a->mem, bp = (uintptr_t)b->mem;
        if (!an || !bn) return ap == bp;
        if (ap <= bp ? bp - ap < an : ap - bp < bn) return 1;
    }
    return 0;
}

static void zero_image_sync_end(void) {
    if (!g_zero_image_buffer) return;
    id<MTLBuffer> buffer = g_zero_image_buffer;
    id<MTLTexture> texture = g_zero_image_texture;
    g_zero_image_buffer = nil; g_zero_image_texture = nil; g_zero_image_shader = nil;
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTLBlitCommandEncoder> blit = enc_b_begin(g_cb);
    nvn_zero_image_copy(blit, texture, buffer, g_zero_image_layout, YES);
    enc_b_end(blit);
    defer_obj_release(buffer);
    static int notes;
    if (notes++ < 8) MLOG("Zero-image shadow synchronized: %ux%ux%u format=%lu row=%u bytes; uncovered pixels preserved",
        g_zero_image_layout[0], g_zero_image_layout[1], g_zero_image_layout[2],
        (unsigned long)texture.pixelFormat, g_zero_image_layout[4] * 4);
}

static int zero_image_prepare(NvProg *program, const p_draw *dw, const p_draw_idx *di, int merged) {
    NvShader *fragment = program ? program->sh[1] : nil;
    if (g_zero_image_buffer) rp_end();
    if (!fragment || !fragment->zero_image_variant) return 1;
    // Preserve the original attachments, tests, raster bounds and visibility.
    // Only non-aliasing attachments are allowed while the image is shadowed.
    int unsupported = !program->sh[0] || merged ||
        (dw && dw->ind) || (di && di->ind);
    for (int stage = 2; stage < 6; stage++) unsupported |= program->sh[stage] != nil;
    if (program->sh[0]) {
        unsupported |= program->sh[0]->rf.nv != 0;
        for (int i = 0; i < program->sh[0]->rf.nb; i++) unsupported |= program->sh[0]->rf.b[i].kind != NVN_RB_DRV0;
    }
    if (unsupported) {
        MLOG("[RED] Zero-image compatibility requires unmerged direct draw and driver-only vertex inputs (fs=%s)", fragment->key);
        return 0;
    }
    rp_end();
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    int tid = (int)(S.img[1][fragment->zero_image_reg] & 0xfffff);
    pthread_mutex_lock(&g_rmu);
    void *view = NULL; (void)tex_fast_rid(tid, &view);
    id<MTLTexture> texture = (__bridge id<MTLTexture>)view;
    NSUInteger bytes = 0; uint32_t layout[6];
    int valid = tid > 0 && tid < g_ntreg && g_treg[tid].ok &&
        nvn_zero_image_layout(texture, layout, &bytes) && layout[2] >= 3 &&
        texture.storageMode != MTLStorageModeMemoryless;
    const tex_t *destination = valid ? &g_treg[tid].t : NULL;
    if (valid) {
        for (int i = 0; i < R.rt.n; i++) valid &= !zero_image_alias(destination, R.rt.color[i]);
        valid &= !zero_image_alias(destination, R.rt.depth);
    }
    uint64_t serial = valid ? g_treg[tid].t.serial : 0;
    pthread_mutex_unlock(&g_rmu);
    if (!valid) {
        MLOG("[RED] Zero-image compatibility unsupported destination: tid=%d type=%lu format=%lu dimensions=%lux%lux%lu",
            tid, (unsigned long)texture.textureType, (unsigned long)texture.pixelFormat,
            (unsigned long)texture.width, (unsigned long)texture.height, (unsigned long)texture.arrayLength);
        return 0;
    }
    id<MTLBuffer> buffer = [g_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    if (!buffer) { MLOG("[RED] Zero-image shadow allocation failed: %lu bytes", (unsigned long)bytes); return 0; }
    res_add(buffer);
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTLBlitCommandEncoder> blit = enc_b_begin(g_cb);
    nvn_zero_image_copy(blit, texture, buffer, layout, NO);
    enc_b_end(blit);
    g_zero_image_buffer = buffer; g_zero_image_texture = texture; g_zero_image_shader = fragment;
    memcpy(g_zero_image_layout, layout, sizeof layout);
    g_zero_image_serial = serial; g_zero_image_tid = tid;
    args_dirty_all();
    return 1;
}
