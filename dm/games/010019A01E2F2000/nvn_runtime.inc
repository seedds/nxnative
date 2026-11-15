// MP4 main v1.1.0 private runtime-patch ABI, not an NVN extension advertised
// to games. Included only by hle_nvn.c after CopyTextureToTexture. Fixed engine
// offsets are checked against original instructions by patches.py and the full
// build ID is required by runtime_patches.py. No third-party patch data.
// The engine's temporary view stays owned by its original shared_ptr. Only its
// non-owning prefix is borrowed here, until main+336d00 replaces x28.
#include "quality_policy.inc"
static pthread_once_t mp4_quality_once=PTHREAD_ONCE_INIT;
// The full-output choices (runtime_patches.py FULL_CHOICES), 16:9 and their 16:10 variants
static const struct { const char *name;unsigned w,h; } mp4_sizes[]={
    {"native_full",1600,900},{"1080p_full",1920,1080},{"1440p_full",2560,1440},{"2160p_full",3840,2160},{"2880p_full",5120,2880},
    {"native_full_1610",1600,900},{"1080p_full_1610",1920,1080},{"1440p_full_1610",2560,1440},{"2160p_full_1610",3840,2160},{"2880p_full_1610",5120,2880}};
static int mp4_quality_dynamic,mp4_quality_ui_fit;
static unsigned mp4_quality_max_width,mp4_quality_max_height;
static void mp4_quality_init(void) {
    const char *d=getenv("DM_DYNAMIC_RESOLUTION"),*mode=getenv("DM_INTERNAL_RESOLUTION");
    if(d && strcmp(d,"0") && strcmp(d,"1")) LOG("[RED] MP4 dynamic resolution must be 0 or 1; keeping fixed scene size");
    mp4_quality_dynamic=d && !strcmp(d,"1");
    // The choice loader.c applied: <mode>_<DM_SCREEN_RATIO> when the manifest has it (16:10 presentation crop), else <mode>
    const char *ratio=getenv("DM_SCREEN_RATIO"); char variant[64]="";
    if(mode && ratio && *ratio && strcmp(ratio,"default")) snprintf(variant,sizeof variant,"%s_%s",mode,ratio);
    for(unsigned i=0;i<sizeof mp4_sizes/sizeof *mp4_sizes;i++) if(variant[0] && !strcmp(variant,mp4_sizes[i].name)) {
        mp4_quality_max_width=mp4_sizes[i].w;mp4_quality_max_height=mp4_sizes[i].h;
        mp4_quality_ui_fit=strstr(mp4_sizes[i].name,"_1610")!=NULL;
    }
    for(unsigned i=0;i<sizeof mp4_sizes/sizeof *mp4_sizes && !mp4_quality_max_width;i++) if(mode && !strcmp(mode,mp4_sizes[i].name)) {
        mp4_quality_max_width=mp4_sizes[i].w;mp4_quality_max_height=mp4_sizes[i].h;
        mp4_quality_ui_fit=strstr(mp4_sizes[i].name,"_1610")!=NULL;
    }
    // Retain the world/scan camera on a square-pixel 16:9 canvas and crop once.
    // The matching manifest fits the native UI camera uniformly into that crop,
    // before the game calculates its inverse. No pass-level viewport/scissor
    // changes, stack classification or menu-state guesses.
    if(mp4_quality_ui_fit) {
        mtl_present_center_crop(16,10);
        LOG("MP4 16:10: centered uniform crop of %ux%u; world 5%% cropped per side; native UI camera fit 90%%, square pixels",mp4_quality_max_width,mp4_quality_max_height);
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
        || (width == 3200 && height == 1800) || (width == 3840 && height == 2160) || (width == 5120 && height == 2880)
        || (width == mp4_quality_max_width && height == mp4_quality_max_height && width)
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

// Initialize presentation before the first window pass (including the title movie).
// All guest viewport/scissor and draw commands otherwise retain native semantics.
static void nvn_profile_set_rt(const NVNcommandBuffer *c, int n, const NVNtexture *const *ct, uint64_t caller) {
    (void)c; (void)n; (void)ct; (void)caller;
    if(!strcmp(dm_main_build(),"8275fbe1ec5191c279f3a943ad8524f4")) pthread_once(&mp4_quality_once,mp4_quality_init);
}

// Screen-ratio layout remains separate from world resolution and animation.
// The rewriter invokes this through main.hud.runtime.tsv for DM_SCREEN_RATIO=1610.
#include "hud_layout.inc"
static void mp4_hud_layout_v1(const void *properties,void *transform) {
    if(strcmp(dm_main_build(),"8275fbe1ec5191c279f3a943ad8524f4") || !properties || !transform) return;
    const char *ratio=getenv("DM_SCREEN_RATIO"),*mode=getenv("DM_INTERNAL_RESOLUTION");
    if(!ratio || strcmp(ratio,"1610") || !mode) return;
    int full=0,wide=ratio && !strcmp(ratio,"1610");
    for(unsigned i=0;i<sizeof mp4_sizes/sizeof *mp4_sizes;i++) if(!strcmp(mode,mp4_sizes[i].name)) {full=1;wide|=strstr(mode,"_1610")!=NULL;break;}
    if(!full || !wide) return; // baseline, supersampling and 16:9 stay native
    // Only the constructor's private stack record changes. Properties, original
    // files, source template root, world entities and animation curves are not written.
    int applied=mp4_hud_apply(properties,transform);
    if(!applied) return;
    int group=applied-1;
    static _Atomic unsigned seen;
    unsigned bit=1u<<group;
    if(!(atomic_fetch_or(&seen,bit)&bit)) LOG("MP4 HUD layout: group %d, %s/%s anchor, scale 85%%; native template root retained (experimental)",group,mp4_hud_groups[group].side<0?"left":mp4_hud_groups[group].side>0?"right":"center",mp4_hud_groups[group].anchor>0?"top":"bottom");
}

// The 16:10 pause-menu layout. Owner identity is checked in the
// original ROOM's template namespace, never inferred from frame history/draws.
#include "pause_ui.inc"
#include "menu_trace.inc"
#include "menu_visual.inc"
static pthread_mutex_t mp4_menu_visual_lock=PTHREAD_MUTEX_INITIALIZER;
static mp4_menu_binding mp4_menu_visual_binding;
#include "menu_provenance.inc"
static void mp4_menu_trace(const void *component,const void *node) {
    const char *trace=getenv("DM_MENU_TRACE");if(!trace || strcmp(trace,"1")) return;
    mp4_menu_snapshot s;if(!mp4_menu_snapshot_read(component,node,&s)) return;
    // At most 16 active caption instances, one sample each per 120 presents,
    // 64 samples total. Mutex bounds concurrent component update callbacks.
    static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    static uintptr_t panes[16];static uint64_t last[16];static mp4_menu_snapshot previous[16];static unsigned count;
    uint64_t frame=atomic_load(&g_nvn_presents);
    pthread_mutex_lock(&lock);
    unsigned i;for(i=0;i<16 && panes[i] && panes[i]!=s.pane;i++);
    if(i==16 || count>=64 || (panes[i] && frame-last[i]<120)) {pthread_mutex_unlock(&lock);return;}
    if(panes[i] && !memcmp(previous[i].gui,s.gui,sizeof s.gui) && !memcmp(previous[i].world,s.world,sizeof s.world) && !memcmp(previous[i].source,s.source,sizeof s.source)) {pthread_mutex_unlock(&lock);return;}
    panes[i]=s.pane;last[i]=frame;previous[i]=s;count++;
    LOG("MP4 menu trace: F%llu pane=%p source=(%.6g %.6g %.6g) world=(%.6g %.6g %.6g) scale=(%.6g %.6g %.6g)",
        (unsigned long long)frame,component,s.source[0],s.source[1],s.source[2],s.world[3],s.world[7],s.world[11],s.world[12],s.world[13],s.world[14]);
    LOG("MP4 menu trace: F%llu pane=%p GUI=(%.6g %.6g %.6g %.6g; %.6g %.6g %.6g %.6g; %.6g %.6g %.6g %.6g)",
        (unsigned long long)frame,component,s.gui[0],s.gui[1],s.gui[2],s.gui[3],s.gui[4],s.gui[5],s.gui[6],s.gui[7],s.gui[8],s.gui[9],s.gui[10],s.gui[11]);
    pthread_mutex_unlock(&lock);
}
static void mp4_pause_ui_v2(const void *component,void *payload,unsigned event) {
    if(strcmp(dm_main_build(),"8275fbe1ec5191c279f3a943ad8524f4")) return;
    const char *mode=getenv("DM_INTERNAL_RESOLUTION"),*ratio=getenv("DM_SCREEN_RATIO");
    if(!mode || !ratio || strcmp(ratio,"1610")) return;
    int full=0,wide=ratio && !strcmp(ratio,"1610");
    for(unsigned i=0;i<sizeof mp4_sizes/sizeof *mp4_sizes;i++) if(!strcmp(mode,mp4_sizes[i].name)) {full=1;wide|=strstr(mode,"_1610")!=NULL;break;}
    if(!full || !wide) return;
    if(event==2) {mp4_menu_trace(component,payload);return;}
    if(!mp4_pause_apply(component,payload,event)) return;
    if(event==0) {
        pthread_mutex_lock(&mp4_menu_visual_lock);
        int bound=mp4_menu_bind(&mp4_menu_visual_binding,component);
        pthread_mutex_unlock(&mp4_menu_visual_lock);
        if(bound) LOG("MP4 menu visuals: bound exact pause-room GUI frame");
    }
    static _Atomic unsigned seen;unsigned bit=1u<<event;
    if(!(atomic_fetch_or(&seen,bit)&bit)) LOG("MP4 pause UI: %s; exact pause-template match (candidate)",event?"perspective Camera fit 90%":"native 2D bounds restored");
}

// Serialization follows all native animation/component updates. The camera
// projection stays native; the exact backdrop is excluded by provenance.
static void mp4_menu_visual_v2(const void *node,void *packet,unsigned event) {
    if(strcmp(dm_main_build(),"8275fbe1ec5191c279f3a943ad8524f4")) return;
    const char *mode=getenv("DM_INTERNAL_RESOLUTION"),*ratio=getenv("DM_SCREEN_RATIO");
    if(!mode || !ratio || strcmp(ratio,"1610")) return;
    int full=0,wide=ratio && !strcmp(ratio,"1610");
    for(unsigned i=0;i<sizeof mp4_sizes/sizeof *mp4_sizes;i++) if(!strcmp(mode,mp4_sizes[i].name)){full=1;wide|=strstr(mode,"_1610")!=NULL;break;}
    if(!full || !wide) return;
    if(event==2){if(!mp4_menu_scope_push(node))LOG("[RED] MP4 menu provenance scope allocation failed");return;}
    if(event==3){mp4_menu_scope_pop();return;}
    pthread_mutex_lock(&mp4_menu_visual_lock);
    int applied=0,scoped=0;
    if(event==1) mp4_menu_unbind(&mp4_menu_visual_binding,node);
    else if(event==4) {
        if(!mp4_menu_node_register(node))LOG("[RED] MP4 menu GUI provenance allocation failed");
    } else if(event==5) {
        mp4_menu_node_forget(node);
    } else if(event==6) {
        applied=mp4_menu_compass_projection(&mp4_menu_visual_binding,packet);scoped=2;
    } else if(event==0) {
        mp4_menu_node_entry *p=*mp4_menu_node_slot(node);scoped=p!=NULL;
        applied=mp4_menu_visual_scoped(&mp4_menu_visual_binding,node,packet,p?&p->scope:NULL);
    }
    pthread_mutex_unlock(&mp4_menu_visual_lock);
    if(applied) {
        static _Atomic unsigned seen;unsigned bit=scoped==2?4:scoped?2:1;
        if(!(atomic_fetch_or(&seen,bit)&bit)) LOG("MP4 menu visuals: %s fit 90%% applied; native source retained (candidate)",scoped==2?"map compass projection":scoped?"room-provenance GUI":"room-frame GUI");
    }
}

// Profile-provided private entry lookup; no game IDs or engine offsets in the
// shared NVN implementation. The rewriter emits this name only for main 1.1.0.
static void *nvn_profile_runtime_lookup(const char *name) {
    if(strcmp(dm_main_build(),"8275fbe1ec5191c279f3a943ad8524f4")) return NULL;
    if(!strcmp(name,"nxnativeMp4ScanSourceV1")) return (void *)mp4_scan_source_v1;
    if(!strcmp(name,"nxnativeMp4SceneSizeV1")) return (void *)mp4_scene_size_v1;
    if(!strcmp(name,"nxnativeMp4MenuVisualV2") || !strcmp(name,"nxnativeMp4MenuVisualV1")) return (void *)mp4_menu_visual_v2;
    if(!strcmp(name,"nxnativeMp4PauseUiV2")) return (void *)mp4_pause_ui_v2;
    if(!strcmp(name,"nxnativeMp4HudLayoutV1")) return (void *)mp4_hud_layout_v1;
    return NULL;
}
