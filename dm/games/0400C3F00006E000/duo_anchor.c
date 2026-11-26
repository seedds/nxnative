// The iPhone Duo's taller layout space (Mario Kart World 1.8.0, Donkey Kong Bananza v458752 and Tears of the Kingdom 1.4.3, the same nn::ui2d): Breath of the Wild 1.9.0's duo_anchor.c, its general rules only, for the newer nn::ui2d.
// Entry point duo_calc, from the base pane's global matrix calculation (Mario Kart World main+0x2f6520 at its matrix store main+0x2f6688 and after main+0x17f89f0 at +0x2f65c8; Bananza main+0x2f8420, Tears of the Kingdom main+0x593060: Pane vtable slot 26; TextBox's and Window's call it, Capture's after its own),
// once the matrix has been stored (Bananza main+0x2f8574, Tears of the Kingdom main+0x5931b0) and before the children's are computed: the library recomputes a matrix only when the pane or a parent changed,
// so a shift is added once per computation. Pane: parent +0x18, child list head +0x20 (first child +0x28), translation +0x30, size +0x50, flags +0x58, global matrix
// 3x4 row-major +0x70 (scale +0x70 / +0x84, translation +0x7c / +0x8c), name +0xb0. With the 2D band stubs the layout's 720 space sits in the middle of a 896 tall
// one; this moves a widget at the 720 space's top or bottom edge out by the extra half-height (88 at 1.43) with everything below it, so it sits at the screen's
// edge, and stretches a full-screen background to the taller screen.
// st (state page + 0x80, loader.c): st[0] the shift (layout half-height - 360; 0 = off), st[1] the vertical stretch (layout height / 720), st[2] the edge rule's
// distance, st[4] / st[5] a full-screen pane's width / height, st[6] the tallest pane still stretched. Compiled to position-independent code with no data
typedef unsigned int u32;
static inline float fab(float x) { return x < 0 ? -x : x; }
static inline float f(const char *p, int off) { return *(const float *)(p + off); }
static inline const char *parent(const char *p) { return *(const char *const *)(p + 0x18); }
static inline int moved(const char *p) { float tx = f(p, 0x30), ty = f(p, 0x34); return tx * tx + ty * ty > 0.25f; }

// Bananza lays its 2D out in a 1920x1080 space (the HUD's corners at x +-905..925, y +-470): st's 720-space values x 1.5
void duo_calc(char *pane, const float *st) {
    const float u = 1.5f;
    float d = st[0] * u; if (d == 0) return;
    float *g = (float *)(pane + 0x70);
    float gw = fab(f(pane, 0x50) * g[0]), gh = fab(f(pane, 0x54) * g[5]);
    int leaf = *(char *const *)(pane + 0x28) == pane + 0x20;   // no children: a stretch is not inherited
    // a full-screen background (as wide as the screen, centred, at least 810 tall): stretched to the taller screen
    if (gw >= st[4] * u && fab(g[3]) < 60 && fab(g[7]) < 60 && gh >= 810 && gh <= st[6] * u) {
        if (leaf && g[5] != 0) { float k = gh < 1080 ? (gh + 2 * d) / gh : st[1]; g[4] *= k; g[5] *= k; g[6] *= k; g[7] *= k; }
        return;
    }
    // the edge rule: the first pane on the way from the root whose position crosses the edge distance moves out by the extra half-height, everything below it
    // following (a child's matrix is computed from its parent's, after it: a child of a moved pane is past the edge already and is left as it is)
    // the edge rule: a pane past the edge distance moves out by the extra half-height unless its parent already has (everything below a moved pane follows: a
    // child's matrix is computed from its parent's). The moved panes are kept in a ring at state +0x1000 (256 pointers), so the rule holds whatever order the
    // panes are computed in and however the game places a layout's root (Tears of the Kingdom: the hearts' layout root at y 465, set by the game's UI code, its
    // panes recomputed long after that root; Bananza: a root moved past the 16:9 space's edge took its HUD out of view, so roots are never moved)
    const char *p = parent(pane);
    if (!p) return;
    const char **ring = (const char **)((char *)st - 0x80 + 0x1000);
    u32 *next = (u32 *)((char *)st - 0x80 + 0xff8);
    float edge = st[2] * u, ty = g[7];
    // a HUD widget under a centred container moves with its edge from 300 on, not only past the edge distance: Bananza's balloon,
    // bottle and chip rows (y 402 to 414) under the hearts (472) stayed behind them, a gap where the cluster was one (Breath of the Wild's listed widgets, by structure)
    if (fab(ty) < edge) {
        // not only layout parts (L_*): the HUD's item rows N_Item_00 (y 358, its balloon / bottle / chip rows at 402 to 414) and notice text N_Notify_00 (390),
        // under the HUD's centred N_All, stayed 132 behind the hearts
        if (fab(ty) < 300 || fab(f(p, 0x8c)) >= 60) return;
    }
    if (fab(ty) > 540) return;   // outside the 16:9 space: a pane waiting off the screen to slide in (Bananza's dialogue box at y -549.7: moved further out, it never
                                 // arrived and the dialogue never advanced), not one anchored at the edge
    // the button prompts (L_HardKey_*: the controller key a dialogue waits on, Bananza): left where the 16:9 layout has them (moved past the 1080 space's bottom
    // edge, the dialogue never advanced; Mac, the tunnel's opening dialogue)
    for (const char *a = pane; a; a = parent(a)) { u32 x, y; __builtin_memcpy(&x, a + 0xb0, 4); __builtin_memcpy(&y, a + 0xb4, 4); if (x == 0x61485f4cu && y == 0x654b6472u) return; }   // "L_HardKe"
    // map space (Tears of the Kingdom's minimap and map screen: icons under containers scaled below 0.3, rotated or not): left where the game puts them, on the map
    for (const char *a = p; a; a = parent(a)) { float m00 = f(a, 0x70), m10 = f(a, 0x80); if (m00 * m00 + m10 * m10 < 0.09f) return; }
    for (const char *a = p; a; a = parent(a)) for (int i = 0; i < 256; i++) if (ring[i] == a) return;   // an ancestor carries the shift (Breath of the Wild's
    // rule looks up the whole chain too: checking only the parent let each level of a widget move once more, the hearts 4 x 132 off the screen)
    g[7] = ty + (ty > 0 ? d : -d);
    for (int i = 0; i < 256; i++) if (ring[i] == pane) return;
    ring[*next & 255] = pane; *next += 1;
}
