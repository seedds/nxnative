// Ocarina of Time 2.0.0 (v131072): the HUD and the menu bars at the edges of a screen taller than 16:9 (DM_SCREEN_ASPECT, patches.py screen_shape).
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
// -4/1024 inside a HUD layout, -5/1024 moved, -3/1024 inside a menu (left alone). Nothing changes at 16:9.
// Build: clang -target arm64-apple-macos -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-jump-tables -fno-asynchronous-unwind-tables -mno-outline-atomics
//        -c shape_anchor.c, then the __text words (no relocations, no data, shape_calc at +0) as _shape_anchor in patches.py.
typedef unsigned int u32; typedef unsigned char u8; typedef unsigned long u64;

static inline __attribute__((always_inline)) u32 layout_hash(const u8 *pane) {
    const u8 *lay = *(const u8 *const *)(pane + 0x18); if (!lay) return 0;
    const u8 *res = *(const u8 *const *)(lay + 0x10); if (!res) return 0;
    const u8 *s = *(const u8 *const *)(res + 0x18); if (!s) return 0;
    u32 h = 0x811c9dc5u; int n = 0;
    while (s[n] && n < 48) { h = (h ^ s[n]) * 0x01000193u; n++; }
    return h;
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

static inline __attribute__((always_inline)) long node_index(const u8 *pane) {
    const u8 *lay = *(const u8 *const *)(pane + 0x18); if (!lay) return -1;
    u64 base = *(const u64 *)(lay + 0x100), at = (u64)pane; if (at < base || (at - base) % 0xb0) return -1;
    return (long)((at - base) / 0xb0);
}

// MapMenu_00: the map page's subtree roots (16, 81, 90, 95) are menu
static inline __attribute__((always_inline)) int map_page(const u8 *pane) {
    long i = node_index(pane);
    return i == 16 || i == 81 || i == 90 || i == 95;
}

// the menu bars of a layout: bit i of the low word = node i moves up, of the high word = node i moves down
static inline __attribute__((always_inline)) u64 edge_mask(u32 h) {
    if (h == 0x97805ff7u) return 0x8000000000000020ull;   // MenuOverlay_00: up 5, down 31
    if (h == 0xe15161c9u) return 0x0000008800000014ull;   // SystemOverlay_00: up 2, 4, down 3, 7
    return 0;
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
    if (k >= 2.5f && k <= 5.5f) { mark = (int)(k + 0.5f); if ((float)mark != k) mark = 0; else z = 0; }   // only an exact mark counts
    if (mark == 5) return;   // already moved: the parent's move carries
    u32 h = layout_hash(pane);
    if (h == 0xb363e973u && t[0] > 1000.0f) {   // MapMenu_00 at the minimap: its layers at (1490, -644) (3.)
        long i = node_index(pane);
        if (i == 64 || i == 82) { t[1] -= d; t[2] = z + 5.0f * UNIT; return; }
    }
    if (mark != 4) {   // the first pane of a chain, or a pane inside a menu: which layout it belongs to
        int kind = mark == 3 ? 3 : layout_kind(h);
        if (!kind) return;
        if (kind & 8) kind = map_page(pane) ? 3 : 4;
        if (kind == 3) {   // a menu: only its bar nodes move (they sit below the layout's root, so they come here with the menu mark)
            u64 em = edge_mask(h); long i = em ? node_index(pane) : -1;
            if (i >= 0 && i < 32) { if ((em >> (u32)i) & 1) t[1] += d; else if ((em >> (32 + (u32)i)) & 1) t[1] -= d; }
            t[2] = z + 3.0f * UNIT; return;
        }
        mark = 4;
    }
    else if ((layout_kind(h) & 8) && map_page(pane)) { t[2] = z + 3.0f * UNIT; return; }   // inside MapMenu_00: the map page
    if (t[1] >= 650.0f) { t[1] += d; mark = 5; }
    else if (t[1] <= -650.0f) { t[1] -= d; mark = 5; }
    t[2] = z + (float)mark * UNIT;
}
