// MP4 main v1.1.0 private runtime-patch ABI, not an NVN extension advertised
// to games. Included only by hle_nvn.c after CopyTextureToTexture. Fixed engine
// offsets are checked against original instructions by patches.py and the full
// build ID is required by runtime_patches.py. No third-party patch data.
// The engine's temporary view stays owned by its original shared_ptr. Only its
// non-owning prefix is borrowed here, until main+336d00 replaces x28.
#include "quality_policy.inc"
static pthread_once_t mp4_quality_once=PTHREAD_ONCE_INIT;
static int mp4_quality_dynamic;
static unsigned mp4_quality_max_width,mp4_quality_max_height;
static void mp4_quality_init(void) {
    const char *d=getenv("DM_DYNAMIC_RESOLUTION"),*mode=getenv("DM_INTERNAL_RESOLUTION");
    if(d && strcmp(d,"0") && strcmp(d,"1")) LOG("[RED] MP4 dynamic resolution must be 0 or 1; keeping fixed scene size");
    mp4_quality_dynamic=d && !strcmp(d,"1");
    const struct { const char *name;unsigned w,h; } sizes[]={
        {"native_full",1600,900},{"1080p_full",1920,1080},{"1440p_full",2560,1440},
        {"2160p_full",3840,2160},{"2880p_full",5120,2880},{"1610_full",3200,2000}};
    for(unsigned i=0;i<sizeof sizes/sizeof *sizes;i++) if(mode && !strcmp(mode,sizes[i].name)) {
        mp4_quality_max_width=sizes[i].w;mp4_quality_max_height=sizes[i].h;
    }
    // A canvas of another shape than 16:9 (1610_full) is presented in its own shape, not fitted into the 16:9 layer (nvn_mtl.m present_shape)
    if(mp4_quality_max_width && mp4_quality_max_width*9!=mp4_quality_max_height*16) {
        extern void mtl_present_screen(uint32_t w,uint32_t h); mtl_present_screen(mp4_quality_max_width,mp4_quality_max_height);
        LOG("MP4 output shape %ux%u (%.3f), presented in its own shape",mp4_quality_max_width,mp4_quality_max_height,(double)mp4_quality_max_width/mp4_quality_max_height);
    }
    if(!mp4_quality_max_width) mp4_quality_dynamic=0; // legacy/reference choices retain their confirmed behavior
    LOG("MP4 scene policy: %s; selected maximum %ux%u",mp4_quality_dynamic?"native dynamic controller":"fixed",mp4_quality_max_width,mp4_quality_max_height);
}
static uint64_t mp4_scene_size_v1(uint64_t native,uint64_t selected,uint64_t base) {
    if(strcmp(dm_main_build(),"8275fbe1ec5191c279f3a943ad8524f4")) return selected;
    pthread_once(&mp4_quality_once,mp4_quality_init);
    uint64_t out;
    if(mp4_scene_policy(native,selected,base,mp4_quality_dynamic,&out)) return out;
    static _Atomic unsigned rejects;
    if(atomic_fetch_add(&rejects,1)<4) LOG("[RED] MP4 invalid scene policy extents; keeping selected fixed size");
    return selected;
}
typedef struct {
    _Alignas(16) uint8_t prefix[0x58];
    tex_t texture;
    unsigned reports;
} mp4_scan_source;
static pthread_key_t mp4_scan_key;
static pthread_once_t mp4_scan_once = PTHREAD_ONCE_INIT;
static void mp4_scan_destroy(void *p) {
    mp4_scan_source *s = p;
    if (s) { mtl_tex_release(&s->texture); free(s); }
}
static void mp4_scan_key_init(void) {
    if (pthread_key_create(&mp4_scan_key, mp4_scan_destroy)) die("MP4 scan-source TLS key failed");
}
static void *mp4_scan_source_v1(NVNcommandBuffer *commands, const void *view) {
    // The private entry can only operate on this build. Standard NVN behavior
    // and native/legacy1440p never call it; 1800p/2160p and full-output choices opt in.
    if (strcmp(dm_main_build(), "8275fbe1ec5191c279f3a943ad8524f4") || !view) return (void *)view;
    const uint8_t *in = view;
    uint32_t width, height, engine_format; const tex_t *src; view_t sv;
    memcpy(&width, in + 0x14, 4); memcpy(&height, in + 0x18, 4);
    if (width <= 1600 && height <= 900) return (void *)view;
    memcpy(&engine_format, in + 0xc, 4); memcpy(&src, in + 0x38, sizeof src); memcpy(&sv, in + 0x48, sizeof sv);
    // Narrow, explicit format/layout contract. The table at main+13c1af4 maps
    // engine 12->NVN D24S8, 13->D32F, 1f->R32F; no unknown integer format is
    // sent to the floating-point copy shader. Subresources must be the full
    // scene's base level and first layer. Refusal retains the game's size guard.
    int format_ok = src && ((engine_format == 0x12 && src->d.format == NVN_FORMAT_DEPTH24_STENCIL8)
                   || (engine_format == 0x13 && src->d.format == NVN_FORMAT_DEPTH32F)
                   || (engine_format == 0x1f && src->d.format == NVN_FORMAT_R32F));
    pthread_once(&mp4_quality_once,mp4_quality_init);
    int extent_ok = (width == 1920 && height == 1080) || (width == 2560 && height == 1440)
        || (width == 3200 && height == 1800) || (width == 3840 && height == 2160) || (width == 5120 && height == 2880) || (width == 3200 && height == 2000)
        || mp4_dynamic_extent(width,height,mp4_quality_max_width,mp4_quality_max_height,mp4_quality_dynamic);
    if (!commands || CB(commands)->magic != MAGIC('C','M','D','B') || !CB(commands)->recording
        || !src || src->magic != MAGIC('T','E','X',0) || !format_ok || !extent_ok
        || src->d.width != width || src->d.height != height || src->d.target != NVN_TEXTURE_TARGET_2D
        || src->d.depth != 1 || src->d.samples > 1 || !src->d.levels
        || (sv.set_levels && (sv.base_level || sv.nlevels != 1))
        || (sv.set_layers && (sv.min_layer || sv.nlayers != 1))
        || (sv.set_format && sv.format != src->d.format)
        || (sv.set_target && sv.target != NVN_TEXTURE_TARGET_2D)) {
        static _Atomic unsigned rejects;
        if (atomic_fetch_add(&rejects, 1) < 4) LOG("[RED] MP4 scan-source refused: %ux%u engine=%#x NVN=%#x (scene/query left unchanged)", width, height, engine_format, src ? src->d.format : 0);
        return (void *)view;
    }
    pthread_once(&mp4_scan_once, mp4_scan_key_init);
    mp4_scan_source *s = pthread_getspecific(mp4_scan_key);
    if (!s) {
        s = calloc(1, sizeof *s);
        if (!s || pthread_setspecific(mp4_scan_key, s)) die("MP4 scan-source TLS allocation failed");
    }
    if (!s->texture.serial || s->texture.d.format != src->d.format) {
        mtl_tex_release(&s->texture);
        s->texture = (tex_t){.magic = MAGIC('T','E','X',0), .serial = nvn_new_serial(), .readback_proxy = 1,
            .d = {.target = NVN_TEXTURE_TARGET_2D, .format = src->d.format, .width = 1600, .height = 900, .depth = 1, .levels = 1, .samples = 1}};
        nvn_tex_layout(&s->texture.d, &s->texture.L); mtl_tex_init(&s->texture);
    }
    const NVNcopyRegion sr = {.width = (int)width, .height = (int)height, .depth = 1}, dr = {.width = 1600, .height = 900, .depth = 1};
    // Record in the SAME command stream, after the scene writes and before its
    // ROI copy/fence. Snapshots survive this function and ordinary NVN replay
    // provides GPU ordering. No CPU memory fallback, extra wait or guest write.
    h_nvnCommandBufferCopyTextureToTexture(commands, (const NVNtexture *)src, NULL, &sr,
        (const NVNtexture *)&s->texture, NULL, &dr, 0); // nearest: IDs must never be interpolated
    memcpy(s->prefix, in, sizeof s->prefix);
    width = 1600; height = 900; src = &s->texture;
    memcpy(s->prefix + 0x14, &width, 4); memcpy(s->prefix + 0x18, &height, 4); memcpy(s->prefix + 0x38, &src, sizeof src);
    // The new texture is a one-level, one-layer full copy. Preserve the stencil
    // selection/swizzle/format of the game's view; its level/layer are already 0.
    if (s->reports++ < 3) LOG("MP4 scan-source: %dx%d -> 1600x900 nearest, engine=%#x NVN=%#x; main scene unchanged", sr.width, sr.height, engine_format, src->d.format);
    return s->prefix;
}

// Profile-provided private entry lookup; no game IDs or engine offsets in the
// shared NVN implementation. The rewriter emits this name only for main 1.1.0.
static void *nvn_profile_runtime_lookup(const char *name) {
    if(strcmp(dm_main_build(),"8275fbe1ec5191c279f3a943ad8524f4")) return NULL;
    if(!strcmp(name,"nxnativeMp4ScanSourceV1")) return (void *)mp4_scan_source_v1;
    if(!strcmp(name,"nxnativeMp4SceneSizeV1")) return (void *)mp4_scene_size_v1;
    return NULL;
}
