// The iPhone Duo's taller layout space (DM_SCREEN_ASPECT, Animal Crossing: New Horizons 3.0.3). nn::ui2d's Pane: parent +0x18, child list +0x20 (first child
// at +0x28), translation +0x30, scale +0x48, size +0x50, flags +0x58, global matrix 3x4 row-major +0x70 (scale +0x70 / +0x84, translation +0x7c / +0x8c), name +0xb0.
// duo_calc runs from Pane::Calculate (main+0x27d4c0) right after a pane's global matrix has been computed (main+0x27d570) and before its children's are, so what it does to
// a pane's matrix its children and text follow. The library recomputes a global matrix only when the pane or a parent changed, so a shift is added once per computation:
// - a full-screen background (a leaf that draws, as wide and as tall as its layout, centred) is stretched to the taller screen;
// - a widget at the layout's top or bottom edge moves out by the extra half-height with everything in it, so it sits at the screen's edge again.
// Layouts placed by their root (balloons over characters, set at a point of the 3D scene) are left alone.
// The game's panes are its own classes (no RTTI): what draws is told by the vtable's DrawSelf (slot 21), which null panes, alignments, captures and parts keep from
// nn::ui2d::Pane (main+0x27e2c8, an empty function; fn_null_draw is its address).
// st: the parameters (state page + 0x80), in 720 units: st[0] the shift (layout half-height - 360; 0 = off), st[1] the vertical stretch (layout height / 720), st[2] / st[3]
// unused here, st[4] / st[5] a full-screen pane's width / height, st[6] the tallest pane still stretched. Compiled to position-independent code with no data (constants
// are immediates)
typedef unsigned int u32;
static inline float fab(float x) { return x < 0 ? -x : x; }
static inline float f(const char *p, int off) { return *(const float *)(p + off); }
static inline const char *parent(const char *p) { return *(const char *const *)(p + 0x18); }
static inline int draws(const char *p, const void *fn_null_draw) { return (*(const void *const *const *)p)[21] != fn_null_draw; }
static inline int moved(const char *p) { float tx = f(p, 0x30), ty = f(p, 0x34); return tx * tx + ty * ty > 0.25f; }
// a pane that only holds others: one that draws nothing and is smaller than 64 either way (the null panes the game's animations hang widgets on, 30 x 30 / 30 x 40, and
// alignments, 0 x 0), or one as large as the layout (Footer's part L_Footer, 3020 x 1208, whose bar sits at the bottom). A part that draws nothing itself but has a size
// (the main screen's road button, 360 x 360) is a widget of its own
static inline int container(const char *p, const void *fn_null_draw, const float *st, float u) {
    float w = fab(f(p, 0x50) * f(p, 0x70)), h = fab(f(p, 0x54) * f(p, 0x84));
    if (w >= st[4] * u && h >= st[5] * u) return 1;
    return !draws(p, fn_null_draw) && (w < 64 * u || h < 64 * u);
}

void duo_calc(char *pane, const float *st, const void *fn_null_draw) {
    if (st[0] == 0) return;
    const char *pp = parent(pane);
    if (!pp) return;   // a layout's root
    // The layout's space: most of the game's layouts are 1920x1080 (793 of 959), the rest 1280x720 (the root pane's size); the parameters are in 720 units
    const char *root = pp;
    for (int i = 0; i < 48 && parent(root); i++) root = parent(root);
    float u = f(root, 0x54) >= 1000 ? 1.5f : 1.0f, d = st[0] * u;
    float *g = (float *)(pane + 0x70);
    float gw = fab(f(pane, 0x50) * g[0]), gh = fab(f(pane, 0x54) * g[5]);
    int leaf = *(char *const *)(pane + 0x28) == pane + 0x20;   // the child list empty: a stretch would carry over to children (a menu page's whole content)
    if (gw >= st[4] * u && gh >= st[5] * u && gh <= st[6] * u && fab(g[3]) < 40 * u && fab(g[7]) < 40 * u) {
        if (leaf && draws(pane, fn_null_draw)) { g[4] *= st[1]; g[5] *= st[1]; g[6] *= st[1]; g[7] *= st[1]; }
        return;
    }
    // a background as tall as the layout but not as wide (the tailor's two halves, ShopBuyTailorBG's P_BG_03 / 04, 960 x 1080), centred up and down: stretched the same
    // way. One already taller than the taller screen (the catalog's patterns, 700 x 1370) covers it as it is
    if (leaf && gh >= st[5] * u && gh < 720 * u * st[1] - 2 && fab(g[7]) < 40 * u && draws(pane, fn_null_draw)) {
        g[4] *= st[1]; g[5] *= st[1]; g[6] *= st[1]; g[7] *= st[1];
        return;
    }
    // A widget is the topmost pane of its chain that is not a container. Null panes are where the game's animations hang widgets, anywhere: the version label sits under
    // a null pane at the bottom (Version's N_InOut_00, -430) and is placed 908 above it, so moving the top pane with a translation took the label down to the middle
    if (container(pane, fn_null_draw, st, u)) return;
    const char *p = pp;
    for (int i = 0; i < 48; i++) {
        const char *par = parent(p);
        if (!par) { if (moved(p)) return; break; }   // p is the root: a layout placed by its root (a balloon over a character) is not on the screen's edges
        if (!container(p, fn_null_draw, st, u)) return;   // a widget above: the widget's top is further up
        p = par;
    }
    float *gy = g + 7;
    // In 720 units (x 1.5 in the 1080 space): 230 for a widget towards a corner (|x| 400 or more: the top right's road and album buttons at 420 and 389, the bottom left's
    // clock at -416, the bottom right's minimap at -354, the title's version and settings lines), 270 for one nearer the middle (the bottom's button guides at -486 and
    // -540, the title's press-A line at -414, the timer at 450) so the title logo's upper line (+352) stays with the rest of the logo; not the room guide at the left
    // (256). Its own distances, not DM_SCREEN_HUD's (Breath of the Wild's 280 left the album button behind)
    float edge = (fab(g[3]) >= 400 * u ? 230 : 270) * u;
    if (fab(*gy) >= edge) *gy += *gy > 0 ? d : -d;
}
