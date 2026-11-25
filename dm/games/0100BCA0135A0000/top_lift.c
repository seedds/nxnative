// The edge-anchored UI on a taller screen (DM_SCREEN_ASPECT, Taiko no Tatsujin: Rhythm Festival 7.0.0). The 2D scenes are drawn by orthographic cameras whose
// height patches.py widens to the screen's shape (540 x k world units each side of the middle), so a header placed at the 16:9 screen's top (y 540) sits below the
// taller screen's top edge, and an item at its bottom above the bottom edge. Most screens keep these items as children of the scene controller's GameObject (the
// Entrance screens' title bar panel_title, its text Text_title and the Drum Lvl panel denden, the options' title, the results' song_info...), placed by the asset and
// moved by no script, so top_lift moves them by the extra half-height once, when the controller initializes. The song select footer (a plain Transform Root of
// sprites under the unwidened helper bar camera) is scaled by 1/k about that camera's centre instead, from FooterController.OnInitialize.
// The popups (the Presenter windows: Controller Settings...) are drawn by normally widened cameras, and their full-width footer and the help band on it move down by
// the same, from PresenterWindowControllerBase.Initialized. The story scenes' dimming sprite (one 16:9 screen) is scaled to cover, from LetterBoxCamera.Start.
// In gameplay the top band (a DonBG scene's pattern, from EnsoPartsBase.OnInitialize) is stretched to the taller screen's edge and the stage below the lanes (EnsoSingleBackground)
// grows by k about the middle, its strip and dancers moved down to the bottom edge.
// It calls the game's IL2CPP methods (this first, the MethodInfo* last, NULL for these): t holds their offsets from main's base, then the rules. A rule: the move (int8:
// the extra half-height times it, SCALE, GROW, BAND, FIT or COVER), the guard (0 none, 1 the original local y >= g, 2 <= g, 3 within 16 of g: also once only where a
// controller can initialize twice), g (int16, little-endian), then the path, NUL ended: an
// optional "Controller:" (the controller GameObject's name), then names separated by '/' from a direct child of the controller down (every match at each level, * any
// name). The first one-name rule that moves a child wins; an empty path ends the rules. st: the state page (+0x48 the output's height, 0 = 16:9). Compiled to position-independent code with no
// data (constants are immediates)
typedef unsigned int u32;
typedef unsigned short u16;
typedef struct { float x, y, z; } v3;
typedef void *(*fn_obj)(void *, void *);
typedef int (*fn_int)(void *, void *);
typedef void *(*fn_child)(void *, int, void *);
typedef v3 (*fn_getv)(void *, void *);
typedef void (*fn_setv)(void *, v3, void *);

#define SCALE 0x40
#define COVER 0x41
#define GROW 0x42
#define BAND 0x43
#define FIT 0x44

// the Il2CppString s (length at +0x10, UTF-16 at +0x14) is the n chars at p, or they are *
__attribute__((always_inline)) static int same(const char *s, const char *p, int n) {
    if (s && n == 1 && *p == '*') return 1;
    if (!s || *(const int *)(s + 0x10) != n) return 0;
    const u16 *c = (const u16 *)(s + 0x14);
    for (int i = 0; i < n; i++) if (c[i] != (unsigned char)p[i]) return 0;
    return 1;
}

__attribute__((always_inline)) static int apply(void *c, const unsigned char *r, const char *base, const u32 *t, float d, float f, float cy) {
    if (r[0] == COVER) {   // a sprite of one 16:9 screen: its scale x k, once (while it is 1)
        v3 s = ((fn_getv)(base + t[6]))(c, 0);
        if (!(s.y > 0.99f && s.y < 1.01f)) return 0;
        s.x /= f; s.y /= f;
        ((fn_setv)(base + t[7]))(c, s, 0);
        return 1;
    }
    v3 v = ((fn_getv)(base + t[4]))(c, 0);   // Transform.get_localPosition
    int g = (short)(r[2] | r[3] << 8);
    if (r[1] == 1 && !(v.y >= g)) return 0;
    if (r[1] == 2 && !(v.y <= g)) return 0;
    if (r[1] == 3 && !(v.y >= g - 16 && v.y <= g + 16)) return 0;
    if (r[0] == SCALE || r[0] == GROW) {   // position and scale x 1/k (SCALE) or x k (GROW): scaled about the parent's origin
        float m = r[0] == SCALE ? f : 1.0f / f;
        v.x *= m; v.y *= m;
        ((fn_setv)(base + t[5]))(c, v, 0);   // Transform.set_localPosition
        v3 s = ((fn_getv)(base + t[6]))(c, 0);   // Transform.get_localScale
        s.x *= m; s.y *= m;
        ((fn_setv)(base + t[7]))(c, s, 0);   // Transform.set_localScale
        return 1;
    }
    if (r[0] == BAND) {   // a band g high each side of c's origin, stretched in y about its inner edge so its outer one (on the controller's side of 0) moves by d, once
        v3 s = ((fn_getv)(base + t[6]))(c, 0);
        if (!(s.y > 0.99f && s.y < 1.01f)) return 0;
        s.y *= 1.0f + d / (2 * g);
        ((fn_setv)(base + t[7]))(c, s, 0);
        v.y += cy < 0 ? -0.5f * d : 0.5f * d;
        ((fn_setv)(base + t[5]))(c, v, 0);
        return 1;
    }
    if (r[0] == FIT) {   // a renderer under the helper bar's canvas (g: its parent's y there) drawn by a widened camera: back to its 16:9 size and place by the bar, once
        v3 s = ((fn_getv)(base + t[6]))(c, 0);
        if (!(s.y > 0.99f && s.y < 1.01f)) return 0;
        float k = 1.0f / f;
        v.x *= k; v.y = k * (g + v.y - d) - g;
        ((fn_setv)(base + t[5]))(c, v, 0);
        s.x *= k; s.y *= k;
        ((fn_setv)(base + t[7]))(c, s, 0);
        return 1;
    }
    v.y += d * (signed char)r[0];
    ((fn_setv)(base + t[5]))(c, v, 0);
    return 1;
}

// c's name matched the name at p: apply the rule to it, or to every descendant matching the next names (depth first, without recursion: the words carry no relocation)
__attribute__((always_inline)) static void walk(void *c, const char *p, const char *e, const unsigned char *r, const char *base, const u32 *t, float d, float f, float cy) {
    void *node[8]; const char *at[8]; int idx[8];
    int k = 0; node[0] = c; at[0] = p; idx[0] = 0;
    while (k >= 0) {
        const char *q = at[k];
        while (q < e && *q != '/') q++;
        if (q == e) { apply(node[k], r, base, t, d, f, cy); k--; continue; }
        const char *qe = ++q;
        while (qe < e && *qe != '/') qe++;
        if (idx[k] >= ((fn_int)(base + t[1]))(node[k], 0)) { k--; continue; }   // Transform.get_childCount
        void *g = ((fn_child)(base + t[2]))(node[k], idx[k]++, 0);   // Transform.GetChild
        if (g && k < 7 && same(((fn_obj)(base + t[3]))(g, 0), q, qe - q)) { k++; node[k] = g; at[k] = q; idx[k] = 0; }
    }
}

void top_lift(void *ctl, const char *base, const u32 *t, const char *st) {
    int h = *(const int *)(st + 0x48);
    if (h <= 1080) return;
    float d = (h - 1080) * 0.5f;   // the extra half-height in world units (the width stays 1920: 1200 → 60)
    float f = 1080.0f / h;   // 1/k
    void *tr = ((fn_obj)(base + t[0]))(ctl, 0);   // Component.get_transform
    if (!tr) return;
    float cy = ((fn_getv)(base + t[4]))(tr, 0).y;   // the controller's own local y (BAND: which edge its band is at)
    const char *cn = ((fn_obj)(base + t[3]))(ctl, 0);   // Object.get_name: the controller GameObject's
    int n = ((fn_int)(base + t[1]))(tr, 0);   // Transform.get_childCount
    for (int i = 0; i < n; i++) {
        void *c = ((fn_child)(base + t[2]))(tr, i, 0);   // Transform.GetChild
        if (!c) continue;
        const char *s = ((fn_obj)(base + t[3]))(c, 0);
        if (!s) continue;
        for (const unsigned char *r = (const unsigned char *)(t + 8); r[4]; ) {
            const char *p = (const char *)r + 4, *e = p;
            while (*e) e++;
            const unsigned char *next = (const unsigned char *)e + 1;
            const char *q = p;
            while (q < e && *q != ':') q++;
            if (q < e) {
                if (!same(cn, p, q - p)) { r = next; continue; }
                p = q + 1;
            }
            for (q = p; q < e && *q != '/'; q++) ;
            if (q == e) {
                if (same(s, p, e - p) && apply(c, r, base, t, d, f, cy)) break;
            } else if (same(s, p, q - p)) walk(c, p, e, r, base, t, d, f, cy);
            r = next;
        }
    }
}
