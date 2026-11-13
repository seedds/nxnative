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

// A 16:10 output (1610_full): the HUD, menus and movies keep 16:9. Every pass the game begins on a window texture (the swap chain's; RGBA8_SRGB
// intermediates of the same size are left alone)
// through its pass-begin helper (main+0x13003c, binding at main+0x1300b8; DM_NVN_RT_TRACE: the HUD at main+0x12e0e8, the radar at main+0x60860, the
// front end and its movie at main+0x6c98c / 0x6cba0, the menus at main+0x58f520) is laid out in 16:9 and drawn into the middle band: viewports and
// scissors keep their x and width, their y and height are scaled by k = (output ratio) / (16:9) about the centre. The scene's composite into the window
// binds it elsewhere (main+0x145c8) and keeps the whole 16:10 picture.
// Two calls right after the HUD's pass begins draw full-screen images, not HUD: 0x131d64 draws layer 0x14 with the brightness (s, s, s, 1) (called at
// main+0x12e158: a glow over the whole picture, banded in the second 16:10 run it left the scene's glow outside the band), and the virtual call at
// main+0x12e180 (into main+0xd31fd8, the code that allocates the 1600x900 scene canvas at 0xd315a4) draws the finished scene as a picture. Draws under
// either keep the viewport the game set, so they fill the 16:10 window; everything else in the pass is banded.
static _Thread_local const NVNcommandBuffer *mp4_band_cb; static _Thread_local float mp4_band_h, mp4_band_vp[4], mp4_full_vp[4]; static _Thread_local int mp4_band_has, mp4_band_bypass;
static uintptr_t mp4_main_base(void) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) return (uintptr_t)g_mod[i].base; return 0; }
static float mp4_band_k(void) {
    pthread_once(&mp4_quality_once,mp4_quality_init);
    if (!mp4_quality_max_width || mp4_quality_max_width * 9 >= mp4_quality_max_height * 16) return 0;   // 16:9 or wider: nothing to do
    return (float)mp4_quality_max_width / (float)mp4_quality_max_height / (16.0f / 9.0f);
}
static int mp4_under(uintptr_t ra);
static _Atomic int mp4_menu_seen, mp4_menu_last;   // a menu item drawn in this / the last frame (mp4_hud_item_full)
static _Thread_local const NVNcommandBuffer *mp4_hud_cb;   // the HUD's pass, banded or not: its items are still looked at while a menu is open
// The scene's own passes on the window (its final image and post effects: main+0x332fa0 into 0xca8b0, main+0x338d48 into 0x1360c8, DM_NVN_RT_TRACE
// in the fourth 16:10 run) run under the frame's scene step, which returns to main+0x33194c; they keep the whole window. The HUD (main+0x12e0e8),
// radar, menus and loading screens begin theirs elsewhere.
static void nvn_profile_set_rt(const NVNcommandBuffer *c, int n, const NVNtexture *const *ct, uint64_t caller) {
    const tex_t *t = n > 0 && ct ? (const tex_t *)ct[0] : NULL; uintptr_t base = mp4_main_base();
    int band = base && mp4_band_k() > 0 && !strcmp(dm_main_build(),"8275fbe1ec5191c279f3a943ad8524f4") && caller == base + 0x1300b8
        && t && t->magic == MAGIC('T','E','X',0) && nvn_is_window_tex(ct[0]) && !mp4_under(base + 0x33194c)
        && !mp4_under(base + 0xe2acb8);   // the front end (title movie and its menus, main+0x6c98c / 0x6cba0 / 0x58f520): stretched to 16:10, the user's choice   // the window itself: the game renders intermediates of its size and format too
    mp4_band_has = 0;
    if (band && base && mp4_under(base + 0x12e0e8)) {   // the HUD's pass, once a frame: a menu drawn in the last frame turns the band off for this one
        int menu = atomic_exchange(&mp4_menu_seen, 0), was = atomic_exchange(&mp4_menu_last, menu);
        if (menu != was) LOG("MP4 16:10: %s", menu ? "a menu is open, the HUD is drawn over the whole window" : "no menu, the HUD is back in the 16:9 band");
        if (menu) band = 0;
        mp4_hud_cb = c;
    } else if (mp4_hud_cb == c) mp4_hud_cb = NULL;
    if (!band && !atomic_load(&mp4_menu_last) && base && caller == base + 0x1300b8 && t && t->magic == MAGIC('T','E','X',0) && t->d.format == NVN_FORMAT_RGBA8_SRGB && t->d.width == mp4_quality_max_width
        && t->d.height == mp4_quality_max_height && mp4_band_k() > 0) { static int once; if (!once++) LOG("MP4 16:10: a pass begins on an output-size RGBA8_SRGB target that is not the window; left whole"); }
    if (band) { mp4_band_cb = c; mp4_band_h = 0;   // the band's frame: the pass's first viewport (the window rectangle: 1920x1080, the window crop)
        static int once; if (!once++) LOG("MP4 16:10: HUD and menu passes drawn into the middle 16:9 band (y and height x %.3f)", mp4_band_k()); }
    else if (mp4_band_cb == c) mp4_band_cb = NULL;
}
static void nvn_profile_viewports(const NVNcommandBuffer *c, int n, float *v) {
    if (mp4_band_cb != c || mp4_band_bypass) return;
    if (!mp4_band_h) mp4_band_h = v[1] + v[3];
    float k = mp4_band_k(), H = mp4_band_h;
    memcpy(mp4_full_vp, v, sizeof mp4_full_vp);
    for (int i = 0; i < n; i++) { v[4 * i + 1] = H * (1 - k) / 2 + v[4 * i + 1] * k; v[4 * i + 3] *= k; }
    memcpy(mp4_band_vp, v, sizeof mp4_band_vp); mp4_band_has = 1;
}
static int mp4_under(uintptr_t ra) {   // whether the guest call chain of this draw passes return address ra (frame records: [fp] the caller's fp, [fp+8] its return address)
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0), top = fp + (8u << 20);
    for (int i = 0; i < 24 && fp && !(fp & 7) && fp < top; i++) {
        const uintptr_t *f = (const uintptr_t *)fp;
        if (f[1] == ra) return 1;
        if (f[0] <= fp) break;
        fp = f[0];
    }
    return 0;
}
// The HUD's render list (main+0x12f0c8 and main+0x12f074 call one item at a time) mixes 2D widgets with items drawn as 3D models through the scene's own
// model code (main+0xdf304 into 0xe2268, main+0x8e778 into 0x10f38: in the fifth 16:10 run the scan outline of the ship stayed in the band, off the
// ship): those fill the window, the rest stays in the band. An item is named by the return address inside it just below the list's call
// (DM_MP4_HUD_FULL=<hex main offsets, comma-separated> or "none" replaces the default list; each item seen is logged once with where it went).
// The pause menu's pages (map, inventory, ...) mix 3D tab frames with 2D icons, which the band pulled apart; stretched whole they looked right in the first
// 16:10 run. While a menu item (DM_MP4_HUD_MENU, default the map page's main+0xf75724, seventh run) was drawn in the last frame, the HUD's pass is not banded.
static uintptr_t mp4_hud_full[16], mp4_hud_menu[16]; static int mp4_hud_nfull = -1, mp4_hud_nmenu;
static int mp4_hex_list(const char *e, uintptr_t *out) {
    int n = 0; if (!strcmp(e, "none")) return 0;
    for (const char *p = e; *p && n < 16; ) { char *q; unsigned long v = strtoul(p, &q, 16); if (q == p) break; out[n++] = v; p = *q == ',' ? q + 1 : q; }
    return n;
}
static void mp4_hud_full_init(void) {
    const char *e = getenv("DM_MP4_HUD_FULL"), *m = getenv("DM_MP4_HUD_MENU");
    if (e) mp4_hud_nfull = mp4_hex_list(e, mp4_hud_full); else { mp4_hud_full[0] = 0xdf304; mp4_hud_full[1] = 0x8e778; mp4_hud_nfull = 2; }
    if (m) mp4_hud_nmenu = mp4_hex_list(m, mp4_hud_menu); else { mp4_hud_menu[0] = 0xf75724; mp4_hud_nmenu = 1; }
}
static int mp4_hud_item_full(uintptr_t base) {   // whether this draw belongs to a HUD item drawn over the whole window
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0), top = fp + (8u << 20), prev = 0;
    for (int i = 0; i < 24 && fp && !(fp & 7) && fp < top; i++) {
        const uintptr_t *f = (const uintptr_t *)fp; uintptr_t ra = f[1];
        if (ra == base + 0x12e15c || ra == base + 0x12e184) return 1;   // the glow and the scene picture
        if ((ra == base + 0x12f0cc || ra == base + 0x12f078) && prev) {
            if (mp4_hud_nfull < 0) mp4_hud_full_init();
            int full = 0; for (int k = 0; k < mp4_hud_nfull; k++) if (prev - base == mp4_hud_full[k]) full = 1;
            for (int k = 0; k < mp4_hud_nmenu; k++) if (prev - base == mp4_hud_menu[k]) atomic_store(&mp4_menu_seen, 1);
            static uintptr_t seen[64]; static int ns; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
            pthread_mutex_lock(&mu); int fresh = ns < 64; for (int k = 0; k < ns && fresh; k++) if (seen[k] == prev) fresh = 0; if (fresh) seen[ns++] = prev; pthread_mutex_unlock(&mu);
            if (fresh) LOG("MP4 16:10: HUD item main+%#lx (list call main+%#lx): %s", (unsigned long)(prev - base), (unsigned long)(ra - base - 4), full ? "whole window" : "16:9 band");
            return full;
        }
        if (f[0] <= fp) break;
        prev = ra; fp = f[0];
    }
    return 0;
}
static int nvn_profile_draw_begin(NVNcommandBuffer *c) {
    if (mp4_hud_cb == c && mp4_band_cb != c) { mp4_hud_item_full(mp4_main_base()); return 0; }   // a menu frame: only notice whether the menu is still drawn
    if (mp4_band_cb != c || !mp4_band_has) return 0;
    uintptr_t base = mp4_main_base();
    if (!mp4_hud_item_full(base)) return 0;
    mp4_band_bypass = 1; h_nvnCommandBufferSetViewports(c, 0, 1, mp4_full_vp); mp4_band_bypass = 0;
    return 1;
}
static void nvn_profile_draw_end(NVNcommandBuffer *c) { mp4_band_bypass = 1; h_nvnCommandBufferSetViewports(c, 0, 1, mp4_band_vp); mp4_band_bypass = 0; }
static void nvn_profile_scissor(const NVNcommandBuffer *c, int *x, int *y, int *w, int *h) {
    (void)x; (void)w;
    if (mp4_band_cb != c || *h >= 16384 || !mp4_band_h) return;   // the game's "no scissor" (16384x16384) stays
    float k = mp4_band_k(), H = mp4_band_h;
    int y0 = (int)floorf(H * (1 - k) / 2 + (float)*y * k), y1 = (int)ceilf(H * (1 - k) / 2 + (float)(*y + *h) * k);
    *y = y0; *h = y1 - y0;
}

// Profile-provided private entry lookup; no game IDs or engine offsets in the
// shared NVN implementation. The rewriter emits this name only for main 1.1.0.
static void *nvn_profile_runtime_lookup(const char *name) {
    if(strcmp(dm_main_build(),"8275fbe1ec5191c279f3a943ad8524f4")) return NULL;
    if(!strcmp(name,"nxnativeMp4ScanSourceV1")) return (void *)mp4_scan_source_v1;
    if(!strcmp(name,"nxnativeMp4SceneSizeV1")) return (void *)mp4_scene_size_v1;
    return NULL;
}
