// Ocarina of Time 1.1.1 (v131072): the HUD and the menu bars at the edges of a screen taller than 16:9 (DM_SCREEN_ASPECT, patches.py screen_shape).
// With the screen-shape stubs the layout space is 1280 x H (H > 720) and every layout, authored in 3840x2160, sits in a centred 16:9 band. This moves the HUD out to the
// picture's top and bottom edges, as whole subtrees, so a widget's children (labels, counts, icons, the parts the game places at run time) never come apart.
//
// Where: the layout library computes each pane's global matrix in main+0x152caa0 (global = parent global x local; stored at pane+0x28 as four columns of three
// floats, translation y at +0x50). shape_calc runs at main+0x152ce50, after the new matrix is in v0-v3 and before it is stored, for panes with a parent and for root
// panes (main+0x152cda0 joins there). A child's matrix is computed later from its parent's stored one, so a move here carries down the subtree.
//
// What moves, by the layout's resource name [[[pane+0x18]+0x10]+0x18] (FNV-1a):
// 1. HUD layouts (hearts, buttons, minimap, action guides, timers, location names): the first pane of a chain whose translation is past the 16:9 edge region
//    (|y| >= 650 canvas units) moves out by the extra half-height, once, with its subtree.
// 2. Menu frames (MenuOverlay_00: the pause menus' top bar with tabs, hearts and rupees, node 5, and their guide row, node 31; SystemOverlay_00: the Load / Save /
//    Options title bar, nodes 2 and 4, and bottom bar and guide row, 3 and 7): those nodes move out with their subtrees, by node index ([layout+0x100], 0xb0 apart).
//    Everything else in a menu stays in the band, so lists and centred dialogs keep their layout.
// 3. MapMenu_00 holds both the HUD minimap and the map page: the page's subtrees (nodes 16, 81, 90, 95) are menu, the rest HUD.
//    The minimap's map art (nodes 71-76, at y -690) moves by the rule above, but two layers the game places at the minimap sit at (1490, -644), just inside the
//    edge region: node 64 and node 82, the icon layer (the player arrow, exits and markers are parts placed under it), which is under the map page's node 81 and
//    so carries the menu mark. On the field (x 1490) both move down with the minimap, whatever their mark; on the Minus map page the game puts node 82 at the
//    centre (x 0) and it stays with the page (measured with DM_PROBE in the stub, 2026-11-18).
// The state of a chain rides in the translation's z as a mark (UI panes have z 0, z passes to children unchanged, and the UI projection keeps a small negative z):
// -4/1024 inside a HUD layout, -5/1024 moved, -3/1024 inside a menu (left alone), -6/1024 the Load list window, -7/1024 a Load row put back.
// Nothing changes at 16:9.
// Build: clang -target arm64-apple-macos -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-jump-tables -fno-asynchronous-unwind-tables -mno-outline-atomics
//        -c shape_anchor.c, then the __text words (no relocations, no data, shape_calc at +0) as _shape_anchor in patches.py.
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;

static inline __attribute__((always_inline)) u32 name_hash(const u8 *s) {
    if (!s) return 0;
    u32 h = 0x811c9dc5u;
    for (int n = 0; n < 48; n++) {
        if (!s[n]) return h;
        h = (h ^ s[n]) * 0x01000193u;
    }
    return 0;
}

// 4: a HUD layout, 4 | 8: MapMenu_00 (by node), 3: a menu, 0: any other layout (left alone)
static inline __attribute__((always_inline)) int layout_kind(u32 h) {
    switch (h) {
    case 0xb363e973u:   // MapMenu_00
        return 4 | 8;
    case 0xb2fda57fu:   // PlayerStatus_00
    case 0x32aee6beu:   // PlayerStatus3D_00
    case 0xf169a3f6u:   // DoCommand_00
    case 0xcf610476u:   // ActionGuide_00
    case 0x42e418b4u:   // ActionGuideTips_00
    case 0x2b0845b8u:   // ActionGuideTutorial_00
    case 0xfa08d1d3u:   // Location_00
    case 0x98a44eb0u:   // EnemyInfo_00
    case 0x2fdaf306u:   // HorseGauge_00
    case 0x37274b22u:   // MiniGameTimer_00
    case 0x5fc8f2d4u:   // NumDisplay_00
    case 0x6fe2260bu:   // NumDisplayMiniGame_00
    case 0x6e37bbdcu:   // SimpleInfo_00
    case 0x9705d8beu:   // SaveLoadIcon_00
    case 0x1326e949u:   // CtrlGuide_00
    case 0x534b3709u:   // EventSkip_00
    case 0x82dbe1b1u:   // ChallengeAttention_00
        return 4;
    case 0x7f3777e7u:   // PlusMenu_00
    case 0x80cd2e96u:   // MenuBG_00
    case 0xd77fb7adu:   // ChallengeMenu_01
    case 0x9bf1e6e4u:   // ImportantMenu_01
    case 0xd5063d1eu:   // SystemOption_00
    case 0xce8fc7a3u:   // SystemActionGuide_00
    case 0x34cd7f95u:   // AppOptionIcon_00
    case 0x97805ff7u:   // MenuOverlay_00
    case 0xe3af508eu:   // SystemSaveLoad_00
    case 0xe15161c9u:   // SystemOverlay_00
        return 3;
    }
    return 0;
}

// The game allocates count * 0xb0 bytes at layout+0x100 (main+0x1525d60). Its layout resource (UIL) has
// panes' names at +0x20 and records at +0x28. Verify the pane's name, live class
// (bits 22..25 of pane+0xa8, copied from bits 7..10 of the authored flags at main+0x1529d9c), and the
// authored dimensions used by the edit before changing an indexed pane. An updated asset can move panes.
enum { PANE = 0, PICTURE = 1, CAPTURE = 5, LIST_WINDOW = 7 };
enum { LEAVE, UP, DOWN, SCREEN_HEIGHT, EXTEND_Y, LOAD_WINDOW, PAUSE_GRADIENT, MAP_PAGE, MAP_BACKDROP, MINIMAP };
struct pane_edit { u32 name, kind, action; float height, width; };

static inline __attribute__((always_inline)) struct pane_edit pane_edit(u32 hash, u32 i) {
    switch (hash) {
    case 0x7f3777e7u:   // PlusMenu_00
        switch (i) {
        case 2: return (struct pane_edit){0xdc1b1dfbu, CAPTURE, SCREEN_HEIGHT, 2000, 0};   // C_3DFade_00
        case 3: return (struct pane_edit){0xdb1b1c68u, CAPTURE, SCREEN_HEIGHT, 2000, 0};   // C_3DFade_01
        case 5: return (struct pane_edit){0xe89eeb2cu, PICTURE, SCREEN_HEIGHT, 2000, 0};   // P_3DFade_02
        case 6: return (struct pane_edit){0xeb9eefe5u, PICTURE, SCREEN_HEIGHT, 2000, 0};   // P_3DFade_01
        case 11: return (struct pane_edit){0x71990cb8u, PICTURE, PAUSE_GRADIENT, 1172, 1682};   // P_InfoBg_00
        case 12: return (struct pane_edit){0x72990e4bu, PICTURE, PAUSE_GRADIENT, 1172, 1600};   // P_InfoBg_01
        case 13: return (struct pane_edit){0x73990fdeu, PICTURE, PAUSE_GRADIENT, 1172, 1742};   // P_InfoBg_02
        case 14: return (struct pane_edit){0x74991171u, PICTURE, DOWN, 1172, 300};       // P_InfoBg_03
        case 20: return (struct pane_edit){0x9235f16bu, PICTURE, UP, 800, 0};            // P_HeaderSh_00
        }
        break;
    case 0xdcdc36ddu:   // SystemLoadList_00
        if (i == 5) return (struct pane_edit){0x14520d78u, LIST_WINDOW, LOAD_WINDOW, 1984, 0};   // N_SlideList_00
        break;
    case 0xb363e973u:   // MapMenu_00
        switch (i) {
        case 11: return (struct pane_edit){0xf1141733u, PICTURE, SCREEN_HEIGHT, 2160, 0};  // BG_00
        case 16: return (struct pane_edit){0x75313156u, PANE, MAP_PAGE, 0, 0};             // N_LocalMap_00
        case 17: return (struct pane_edit){0x481ab206u, PICTURE, SCREEN_HEIGHT, 2160, 0};  // P_NotOpen_00
        case 64: return (struct pane_edit){0x8beed9f8u, PANE, MINIMAP, 0, 0};              // N_Field_01
        case 81: return (struct pane_edit){0x0a7b0a44u, PANE, MAP_PAGE, 0, 0};             // N_Common_00
        case 82: return (struct pane_edit){0xae00ac0eu, PANE, MINIMAP, 0, 0};              // N_MapIcon_00
        case 90: return (struct pane_edit){0x4c9f7245u, PANE, MAP_PAGE, 0, 0};             // N_WorldMap_00
        case 93: return (struct pane_edit){0x2dc6857au, PICTURE, EXTEND_Y, 2160, 0};       // P_Vignette_L_00
        case 94: return (struct pane_edit){0x981170f0u, PICTURE, EXTEND_Y, 2160, 0};       // P_Vignette_R_00
        case 95: return (struct pane_edit){0xc5b5a795u, PICTURE, MAP_BACKDROP, 2160, 0};  // P_Overlay_00
        }
        break;
    case 0x97805ff7u:   // MenuOverlay_00
        if (i == 5) return (struct pane_edit){0x52b09ce4u, PANE, UP, 0, 0};               // N_Header_00
        if (i == 31) return (struct pane_edit){0xe52447f6u, PANE, DOWN, 0, 0};            // N_Footer_00
        break;
    case 0xe15161c9u:   // SystemOverlay_00
        switch (i) {
        case 2: return (struct pane_edit){0x94881d53u, PICTURE, UP, 0, 0};                 // P_UpSh_00
        case 3: return (struct pane_edit){0x799576cdu, PICTURE, DOWN, 0, 0};               // P_BottomSh_00
        case 4: return (struct pane_edit){0x52b09ce4u, PANE, UP, 0, 0};                    // N_Header_00
        case 7: return (struct pane_edit){0xe52447f6u, PANE, DOWN, 0, 0};                  // N_Footer_00
        }
        break;
    }
    return (struct pane_edit){0, 0, LEAVE, 0, 0};
}

static inline __attribute__((always_inline)) int matches_pane(const u8 *pane, const u8 *res, u32 i, struct pane_edit edit) {
    if (edit.action == LEAVE || ((*(const u32 *)(pane + 0xa8) >> 22) & 15) != edit.kind) return 0;
    const u8 *const *names = *(const u8 *const *const *)(res + 0x20);
    const u8 *const *records = *(const u8 *const *const *)(res + 0x28);
    if (!names || !records || name_hash(names[i]) != edit.name) return 0;
    const u8 *rec = records[i];
    return rec && ((*(const u32 *)(rec + 0x38) >> 7) & 15) == edit.kind &&
           (!edit.height || *(const float *)(rec + 0x30) == edit.height) &&
           (!edit.width || *(const float *)(rec + 0x2c) == edit.width);
}

#define UNIT (-1.0f / 1024.0f)

// pane: the pane being computed; m: its new matrix as four columns of four floats (translation at m[12..14]); st: the state page (+0x48 the output height,
// 0 = 16:9; +0x5c k = (16/9) / the screen's ratio, by which the layout ortho stub makes the canvas taller)
void shape_calc(const u8 *pane, float *m, const u8 *st) {
    if (!*(const u32 *)(st + 0x48)) return;
    float d = 1080.0f * (*(const float *)(st + 0x5c) - 1.0f);   // the extra half-height in canvas units: (2160 k - 2160) / 2
    if (!(d > 0.0f)) return;
    float *t = m + 12;
    float k = t[2] * -1024.0f, z = t[2]; int mark = 0;
    if (k >= 2.5f && k <= 7.5f) { mark = (int)(k + 0.5f); if ((float)mark != k) mark = 0; else z = 0; }   // only an exact mark counts
    if (mark == 5 || mark == 7) return;   // already moved, or a Load row already put back: the parent's move carries
    if (mark == 6) { t[1] -= 0.5f * d; t[2] = z + 7.0f * UNIT; return; }   // a direct child of the Load list window: back to its 16:9 place (the game
                                                                             // lays the rows from the window's top edge, so they come out d/2 high)
    const u8 *lay = *(const u8 *const *)(pane + 0x18);
    const u8 *res = lay ? *(const u8 *const *)(lay + 0x10) : 0;
    if (!res || *(const u32 *)res != 0x004c4955u) return;   // UIL resource
    u32 h = name_hash(*(const u8 *const *)(res + 0x18));
    u64 base = *(const u64 *)(lay + 0x100), at = (u64)pane;
    u64 bytes = at - base;
    u32 count = *(const u16 *)(lay + 0x13c);
    u32 i = base && at >= base && bytes < (u64)count * 0xb0 && bytes % 0xb0 == 0 ? (u32)(bytes / 0xb0) : count;
    struct pane_edit edit = pane_edit(h, i);
    u32 action = i < count ? edit.action : LEAVE;
    if (action != LEAVE && (*(const u32 *)(res + 0x38) != count || !matches_pane(pane, res, i, edit))) return;

    if (action == UP) t[1] += d;
    else if (action == DOWN) t[1] -= d;
    else if (action == PAUSE_GRADIENT) {
        // Widen each bottom gradient about the centre of its page and move it to the new bottom edge.
        float c = i == 11 ? 1079.0f : i == 12 ? 94.0f : -1100.0f;
        float f = 3840.0f / edit.width;
        t[0] += c * m[0]; t[1] += c * m[1] - d;
        m[0] *= f; m[1] *= f; m[2] *= f;
    }
    else if (action == LOAD_WINDOW) {
        // Keep the window's top edge. Mark its direct children so their rows start where they did before.
        float *height = (float *)(pane + 0x5c);
        if (*height == edit.height) *height = edit.height + d;
        if (*height == edit.height + d) { t[1] -= 0.5f * d; t[2] = z + 6.0f * UNIT; }
        return;
    }
    else if (action == SCREEN_HEIGHT || action == EXTEND_Y || action == MAP_BACKDROP) {
        // Preserve the sign of the scale and account for any Y scaling from the full-screen pane hook.
        float sx = m[0] < 0.0f ? -m[0] : m[0], sy = m[5] < 0.0f ? -m[5] : m[5];
        float want = (action == EXTEND_Y ? edit.height + 2.0f * d : 2160.0f + 2.0f * d) / edit.height;
        float ratio = sx > 1e-3f ? sy / sx : 0.0f;
        if (ratio > 1e-3f) {
            float scale = want / ratio;
            m[4] *= scale; m[5] *= scale; m[6] *= scale;
        }
    }
    else if (action == MINIMAP && t[0] > 1000.0f) {
        t[1] -= d; t[2] = z + 5.0f * UNIT;
        return;
    }

    int kind = mark == 4 ? 4 : mark == 3 ? 3 : layout_kind(h);
    if (!kind) return;
    if (kind & 8) kind = 4;
    if (kind == 3 || action == MAP_PAGE || action == MAP_BACKDROP) {
        t[2] = z + 3.0f * UNIT;
        return;
    }
    mark = 4;
    if (t[1] >= 650.0f) { t[1] += d; mark = 5; }
    else if (t[1] <= -650.0f) { t[1] -= d; mark = 5; }
    t[2] = z + (float)mark * UNIT;
}
