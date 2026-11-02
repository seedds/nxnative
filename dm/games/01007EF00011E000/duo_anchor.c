// The iPhone Duo's taller layout space (Breath of the Wild 1.9.0). Entry points into the layout library (nn::ui2d-like; pane:
// parent +0x18, translation +0x30, size +0x50, flags +0x58, global matrix 3x4 row-major +0x70 (scale +0x70 / +0x84, translation +0x7c / +0x8c), name +0xb8):
// - duo_calc, from Pane::Calculate (main+0x276a0) once a pane's global matrix has been computed and before its children's are (main+0x27ae0): a HUD widget at the 720
//   space's top or bottom edge, or one of the HUD's corner clusters, moves out by the extra half-height with everything below it, text included (text boxes draw from
//   their global matrices; their LoadMtx is an empty override), so it sits at the screen's edge again. The library recomputes a global matrix only when the pane or a
//   parent changed (flags +0x58 bit 4), so the shift is added once per computation. A full-screen background is stretched to the taller screen there too.
// - duo_size, at the entry of the two capture panes' Calculate (main+0xf60cc0, main+0xf59b2c; they draw their children, or the screen behind them, into a texture of
//   their own size through an orthographic view of that size, main+0xf5a028): a full-screen capture of children is made as tall as the screen, so it takes in the
//   taller content (the map's frame, moved to the edges) instead of a 720 band.
// Inside a full-screen cut-out (the map: its tiles, icons and the bounding pane its tiles are drawn through, which scroll and zoom) nothing changes.
// st: the parameters (state page + 0x80): st[0] the shift (layout half-height - 360; 0 = off), st[1] the vertical stretch (layout height / 720), st[2] the edge rule's
// distance, st[3] the listed widgets' distance, st[4] / st[5] a full-screen pane's width / height, st[6] the tallest pane still stretched. vt_*: the vtables of the
// game's capture panes (main+0x1d0d7a8, main+0x1d0cec8), cut-out pane (main+0x1d0e788) and the library's bounding pane (main+0x1d0de38). Compiled to
// position-independent code with no data (constants are immediates)
typedef unsigned int u32;
static inline float fab(float x) { return x < 0 ? -x : x; }
static inline float f(const char *p, int off) { return *(const float *)(p + off); }
static inline const char *parent(const char *p) { return *(const char *const *)(p + 0x18); }
static inline const void *vtable(const char *p) { return *(const void *const *)p; }
static inline __attribute__((always_inline)) u32 fnv(const char *s) { u32 h = 2166136261u; for (int i = 0; i < 24 && s[i]; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; } return h; }
static inline __attribute__((always_inline)) int has4(const char *s, u32 w) { for (int i = 0; i + 4 <= 24 && s[i]; i++) { u32 v; __builtin_memcpy(&v, s + i, 4); if (v == w) return 1; } return 0; }   // name contains 4 letters
static inline __attribute__((always_inline)) int pre7(const char *s, u32 a, u32 b) { u32 x, y; __builtin_memcpy(&x, s, 4); __builtin_memcpy(&y, s + 4, 4); return x == a && (y & 0xffffffu) == b; }   // name starts with 7 letters
static inline int moved(const char *p) { float tx = f(p, 0x30), ty = f(p, 0x34); return tx * tx + ty * ty > 0.25f; }
static inline int full_screen(const char *p, const float *st) { return fab(f(p, 0x50) * f(p, 0x70)) >= st[4] && fab(f(p, 0x54) * f(p, 0x84)) >= st[5]; }
// the map's cut-outs (N_Cut_00, 1500 x 720; N_Cut_01, 1500 x 1500): wider than the screen, holding what scrolls and zooms. A cut-out as wide as the screen is a page's
// fixed clip (the adventure log's Null_01, 1280 x 720) and is made as tall as the screen instead (duo_calc)
static inline int map_cut(const char *p, const float *st) { return full_screen(p, st) && fab(f(p, 0x50) * f(p, 0x70)) >= 1400; }
// inside the map's cut-outs. The HUD's minimap is the same layout drawn small: its cut-outs are not full-screen there
static inline __attribute__((always_inline)) int in_cut(const char *p, const float *st, const void *vt_cut) {
    for (int i = 0; i < 24 && (p = parent(p)); i++) if (vtable(p) == vt_cut && map_cut(p, st)) return 1;
    return 0;
}

void duo_calc(char *pane, const float *st, const void *vt_cut, const void *vt_cap_a, const void *vt_cap_b, const void *vt_bound, const void *vt_window) {
    float d = st[0]; if (d == 0) return;
    float *g = (float *)(pane + 0x70);
    // a full-width window centred on the screen (a menu page's frame and lines: the inventory's W_BlackBG_00, W_Line_00 / 02): made taller by the shift at both ends
    // instead of stretched, so its frame keeps its thickness and its edges follow the title and guide rows that moved out (stretched, the title band's lower edge
    // stood at 316 while the line under it stayed at 253). The size at +0x54 is in local units (x the global scale +0x84) and stays grown while the matrix is computed
    // again, so it grows once: the grown size is a whole number plus 1/1024, which a layout file's size is not (Mac, a Master Mode save: the map's background shadow, 608
    // tall, had grown twice to 960), and only while shorter than the screen's full height plus the growth (710 + 2 x 88 = 886). The map's shadow over its terrain
    // (W_Shadow_00, 1500 x 608, a child of its cut-out N_Cut_01) grows too: the map's inside is left alone, but the shadow does not scroll, and left at 608 the
    // terrain the moved title band uncovered above its top edge stood out bright
    // the camera rune's shutter: its blades close inside N_Scissor_00, 1280 x 608 between the Sheikah Slate's rows (AppCamera_00; the dungeon map's is 1100 x 720),
    // scaled about the screen's centre with the blades so they close over the 784 between the moved rows (a Master Mode save on the Mac: the scene showed in strips
    // above and below the closed shutter)
    // It is a cut-out (checked before the screen-wide ones below). The clip is the pane's own size, not its matrix (scaled alone, the blades grew and the strips stayed): the size grows by twice the shift once (only while
    // it is the file's 608) and the matrix takes the same ratio
    if (fnv(pane + 0xb8) == 0x06266ad5u && fab(f(pane, 0x50) * g[0]) >= st[4] && fab(g[7]) < 20 && g[5] != 0) {
        float *ph = (float *)(pane + 0x54);
        if (*ph >= 600 && *ph <= 616) *ph += 2 * d;
        if (*ph >= 600 + 2 * d && *ph <= 616 + 2 * d) { float k = *ph / (*ph - 2 * d); for (int i = 0; i < 8; i++) g[i] *= k; }
        return;
    }
    // a cut-out as wide as the screen and as tall as the 720 space (the adventure log's Null_01 around its page mask P_CaptureMask_00): its clip made as tall as the
    // screen (the size, set rather than added, so it holds while the matrix is computed again); left at 720 it cut the mask's frame 88 short of the page lines
    // that moved out with the title and guide rows (Mac, a Master Mode save: two lines at the page's bottom)
    if (vtable(pane) == vt_cut && fab(f(pane, 0x50) * g[0]) >= st[4] && !map_cut(pane, st) && fab(g[3]) < 40 && fab(g[7]) < 40 && g[5] != 0) {
        float *ph = (float *)(pane + 0x54), gh = fab(*ph * g[5]);
        if (gh >= 700 && gh <= 730) *ph = 720 * st[1] / fab(g[5]);
        return;
    }
    const char *pp = parent(pane);
    if (vtable(pane) == vt_window && fab(f(pane, 0x50) * g[0]) >= st[4] && fab(g[3]) < 40 && fab(g[7]) < 40 &&
        (!in_cut(pane, st, vt_cut) || (pp && vtable(pp) == vt_cut && map_cut(pp, st) && !in_cut(pp, st, vt_cut)))) {
        float *ph = (float *)(pane + 0x54), gh = fab(*ph * g[5]), fr = *ph - (float)(int)*ph;
        if (gh >= 540 && gh < st[5] + 2 * d && g[5] != 0 && !(fr > 0.0005f && fr < 0.0015f)) *ph = (float)(int)(*ph + 2 * d / fab(g[5]) + 0.5f) + 1.0f / 1024;
        if (gh >= 540) return;
    }
    // a full-width pane centred on the screen, at least 540 tall (backgrounds, dimming, the screenshots behind menus, the Sheikah Slate pages' scan lines): stretched
    // in its global matrix (row 2 at +0x80), so every way of drawing it follows (pictures of the game's own classes draw from the global matrix, not through LoadMtx).
    // One as tall as the screen or taller takes the screen's stretch; a shorter one takes the stretch that moves its edges out by the shift like the title and guide
    // rows (the map's, album's and compendium's P_ScanningLine_00, 608 tall: its faded edges at 304 had come out from under the moved title band). Captures are made
    // taller instead (duo_size), cut-outs and bounding panes are not drawn, and the map's inside stays
    float gh = fab(f(pane, 0x54) * g[5]);
    const void *vt = vtable(pane);
    int leaf = *(char *const *)(pane + 0x28) == pane + 0x20;   // children would inherit a stretch (a menu page's full-screen parts): only a leaf (the child list at +0x20 empty)
    int pic = leaf && vt != vt_window && vt != vt_cap_a && vt != vt_cap_b && vt != vt_cut && vt != vt_bound && g[5] != 0;
    // the title screen's illustrations (Title_00: TitleBG*, P_Image*, P_MainV*, the patterned P_BackGround_00; BootUp_00's P_Image_00; the names are used nowhere
    // else) are scaled about the screen's centre as wide as tall, their sides cut: stretched they were drawn 1.24 times too tall (Mac: the title's Link with his bow;
    // the 1440 x 810 key art left the taller screen's top uncovered)
    if (pic && (pre7(pane + 0xb8, 0x6c746954u, 0x474265u) || pre7(pane + 0xb8, 0x6d495f50u, 0x656761u) || pre7(pane + 0xb8, 0x614d5f50u, 0x566e69u) ||
                pre7(pane + 0xb8, 0x61425f50u, 0x476b63u))) {   // "TitleBG", "P_Image", "P_MainV", "P_BackG"
        for (int i = 0; i < 8; i++) g[i] *= st[1];
        return;
    }
    if (fab(f(pane, 0x50) * g[0]) >= st[4] && fab(g[3]) < 40 && fab(g[7]) < 40 && gh >= 540 && gh <= st[6]) {
        if (leaf && vt != vt_cap_a && vt != vt_cap_b && vt != vt_cut && vt != vt_bound && !in_cut(pane, st, vt_cut)) {
            float k = gh < 720 ? (gh + 2 * d) / gh : st[1]; g[4] *= k; g[5] *= k; g[6] *= k; g[7] *= k;
        }
        return;
    }
    // narrower pictures (leaves, outside the map's cut-out) that reach the 720 space's edges, found by going through every layout file (a pass over every layout file of the game's archives):
    float gw = fab(f(pane, 0x50) * g[0]);
    // - as tall as the screen band (608 between the Sheikah Slate's title and guide rows, or 720) and centred on it: one against a side (the scope's masks with the
    //   circle's edge, P_CameraPointerL / R_00, 320 x 608; the cold screen's frost; the adventure log's watermark) is scaled about the screen's centre, as wide as it
    //   is tall, so the circle stays round and reaches the moved rows as at 16:9 (an iPhone Duo: the scope's black masks stopped 88 short of the title and guide
    //   rows, the view uncovered above and below); a full-height panel in the middle is stretched (the shop list's capture masks), an illustration scaled too. Not
    //   the map's slides (P_CaptureSlideL / R, 110 x 720 beyond the screen's sides: copies of the 720 rows they slide over). The title's menu backing, two 480 x 720
    //   halves side by side (FBLayout_00 / 01 from -320 to 160, FBLayout_02 / 03 from 160 to the right edge), is stretched as a middle panel though one half is
    //   against the side: scaled, that half's inner edge had moved out to 199 and left a band 39 wide without the backing (an iPhone Duo, the title screen)
    if (pic && gw < st[4] && fab(g[7]) < 20 && gh >= 600 && gh <= 730 && !has4(pane + 0xb8, 0x64696c53u) && !in_cut(pane, st, vt_cut)) {   // "Slid"
        int side = fab(g[3]) + gw / 2 >= 630 && !has4(pane + 0xb8, 0x614c4246u);   // "FBLa"
        if (side || gh >= 700) {
            float k = gh < 720 ? (gh + 2 * d) / gh : st[1];
            if (side || has4(pane + 0xb8, 0x756c6c49u)) for (int i = 0; i < 8; i++) g[i] *= k;   // "Illu"
            else { g[4] *= k; g[5] *= k; g[6] *= k; g[7] *= k; }
            return;
        }
    }
    // - a full-width background whose top or bottom edge lies under the title or guide row (300..372 from the middle) and that the edge rule leaves (its centre
    //   nearer than 280): carried on toward that edge by the shift, its inner edge kept (the map's sensor panel and the Sheikah Slate's confirmation windows,
    //   1280 / 1880 x 276: their lower edges had come out from under the moved guide row with a dark band below). Inside the map's cut-outs too: N_Cut_01 holds
    //   those panels beside its scrolling icons, and nothing that scrolls is a full-width band. A band taller than a bar that the edge rule would move (the loading
    //   screen's header, Fade_00 P_Capture_00, 1280 x 275 hanging from the top edge) is carried on the same way instead: moved, it had left the tip's title, dark
    //   letters meant for the band, on the black below it (Mac, a Master Mode save). The rect's centre follows the pane's origin (+0x5b bits 2-3: 1 top,
    //   2 bottom, the rect hanging below the origin from its top, main+0x27430); scaled about the origin, the origin moves so the inner edge stays
    if (pic && gw >= st[4] && fab(g[3]) < 40 && gh > 40 && gh < 540) {
        int vo = (*(const unsigned char *)(pane + 0x5b) >> 2) & 3;
        float cy = g[7] + (vo == 1 ? -gh / 2 : vo == 2 ? gh / 2 : 0), e = fab(cy) + gh / 2;
        if (e >= 300 && e <= 372 && (fab(g[7]) < st[2] || gh > 100)) {
            float s = (gh + d) / gh, yin = cy > 0 ? cy - gh / 2 : cy + gh / 2;
            g[4] *= s; g[5] *= s; g[6] *= s; g[7] += (1 - s) * (yin - g[7]);
            return;
        }
    }
    if (!moved(pane)) return;   // a widget's top is the pane nearest the root (below it) that has a translation
    const char *p = parent(pane);
    if (!p) return;
    int bar = fab(f(pane, 0x50) * f(pane, 0x70)) >= st[4] && fab(f(pane, 0x54) * f(pane, 0x84)) <= 100;   // a full-width bar
    for (int i = 0; i < 24; i++) {
        const char *par = parent(p);
        if (!par) break;   // p is the root
        if (moved(p)) return;   // a parent has one: the widget's top is further up
        if (vtable(p) == vt_cut && map_cut(p, st) && !bar) return;   // the map's tiles and icons scroll: no edge rule (its black bars at the top and bottom do)
        p = par;
    }
    float *gy = (float *)(pane + 0x8c), ay = fab(*gy);
    int on = ay >= st[2];   // at the 720 space's edge
    if (!on && ay >= st[3]) { u32 h = fnv(pane + 0xb8); on = h == 0x9c431f43u || h == 0x96fab8b6u || h == 0x005606c5u || h == 0x3a148876u || h == 0x558e57abu || h == 0xebb4fd5bu || h == 0xc336c016u || h == 0xe163b81au || h == 0xc08d06eeu || h == 0xe87aa827u || h == 0xe77aa694u || h == 0x33a5fc10u || h == 0x77dfe84fu || h == 0x76dfe6bcu || h == 0x79dfeb75u; }   // a HUD widget in a corner cluster (N_InOut_00: the weather; Pa_SinJu_00..02: the champions' abilities)
    // widgets nearer the middle that belong to an edge all the same (listed from the layout files by a script, by name and place, none of them
    // matching another layout's widget there): the dialog box with its speaker's name and choices (Message_00, at -235 / -174; its A prompt at -292 already moved
    // and stood apart from it), the special and cutscene messages, the location and boss names, the loading screen's logo, the title's and boot screen's logos and
    // Master Mode text, the map's clock, temperature and sensor icon, the scope's pins, the camera's saved-photo prompt, the horse's spurs; captures (N_Capture*) are left, the pictures
    // that show them move
    if (!on) {
        float tx = g[3], ty = *gy; u32 h = fnv(pane + 0xb8);
        on = (h == 0x3e1237edu && fab(ty -230) < 3 && tx > 463 && tx < 469) ||   // Logo_W_00 Title_00
            (h == 0x89ce4be9u && fab(ty -240) < 3 && tx > 433 && tx < 439) ||   // Logo_W_00_JPja Title_00
            (h == 0x89ce4be9u && fab(ty -250) < 3 && tx > 415 && tx < 421) ||   // Logo_W_00_JPja Title_00
            (h == 0xa65b154cu && fab(ty -240) < 3 && tx > 433 && tx < 439) ||   // Logo_W_00_JPja_02 BootUp_00
            (h == 0xa65b154cu && fab(ty -250) < 3 && tx > 415 && tx < 421) ||   // Logo_W_00_JPja_02 BootUp_00
            (h == 0x3b123334u && fab(ty -230) < 3 && tx > 463 && tx < 469) ||   // Logo_W_03 BootUp_00
            (h == 0x69daddfau && fab(ty -251) < 3 && tx > -534 && tx < -528) ||   // N_All_00 PaAppMapSensor_00
            (h == 0x28321543u && fab(ty +0) < 3 && tx > 437 && tx < 443) ||   // N_BtnNum_00 Message_00
            (h == 0xec9a337bu && fab(ty +0) < 3 && tx > -67 && tx < -61) ||   // N_ExtraIn_01 MainHorse_00
            (h == 0x1e7be272u && fab(ty +254) < 3 && tx > 309 && tx < 315) ||   // N_GuidePos_01 AppMapDungeon_00
            (h == 0x523be9a3u && fab(ty -16) < 3 && tx > -3 && tx < 3) ||   // N_HardModePos_00 SystemWindow_01
            (h == 0xb9be1638u && fab(ty -16) < 3 && tx > -3 && tx < 3) ||   // N_HardMode_03 Title_00
            (h == 0x33a5fc10u && fab(ty -16) < 3 && tx > -3 && tx < 3) ||   // N_InOut_00 HardModeTextDLC_00
            (h == 0xc08d06eeu && fab(ty +272) < 3 && tx > -266 && tx < -260) ||   // N_In_00 PaLocationNameS_00
            (h == 0xc08d06eeu && fab(ty +236) < 3 && tx > -263 && tx < -257) ||   // N_In_00 PaLocationNameHokoraS_00
            (h == 0xc08d06eeu && fab(ty +220) < 3 && tx > -3 && tx < 3) ||   // N_In_00 DemoNameEnemy_00
            (h == 0xc18d0881u && fab(ty +260) < 3 && tx > 394 && tx < 400) ||   // N_In_01 GameTitle_00
            (h == 0xb1f0f48bu && fab(ty +251) < 3 && tx > 528 && tx < 534) ||   // N_MapPos_00 AppPictureBook_00
            (h == 0x363f2668u && fab(ty +0) < 3 && tx > -67 && tx < -61) ||   // N_NumEx_00 MainHorse_00
            (h == 0x5c3f9a71u && fab(ty -16) < 3 && tx > -3 && tx < 3) ||   // N_RegionTypePos_00 Fade_00
            (h == 0xd6f15fd0u && fab(ty +212) < 3 && tx > 427 && tx < 433) ||   // N_Region_01 GameTitle_00
            (h == 0x268648a5u && fab(ty +235) < 3 && tx > -3 && tx < 3) ||   // N_SunLight_00 MessageSp_00
            (h == 0x3e1168cau && fab(ty +235) < 3 && tx > -281 && tx < -275) ||   // Nt_DecoLineL_00 Message_00
            (h == 0x3f116a5du && fab(ty +235) < 3 && tx > -281 && tx < -275) ||   // Nt_DecoLineL_01 Message_00
            (h == 0xa9c69e60u && fab(ty +235) < 3 && tx > 275 && tx < 281) ||   // Nt_DecoLineR_00 Message_00
            (h == 0xaac69ff3u && fab(ty +235) < 3 && tx > 275 && tx < 281) ||   // Nt_DecoLineR_01 Message_00
            (h == 0x11d5842du && fab(ty +235) < 3 && tx > -287 && tx < -281) ||   // Nt_MsgDeco_00 Message_00
            (h == 0x10d5829au && fab(ty +235) < 3 && tx > 281 && tx < 287) ||   // Nt_MsgDeco_01 Message_00
            (h == 0x0fd58107u && fab(ty +235) < 3 && tx > 281 && tx < 287) ||   // Nt_MsgDeco_02 Message_00
            (h == 0x0ed57f74u && fab(ty +235) < 3 && tx > -287 && tx < -281) ||   // Nt_MsgDeco_03 Message_00
            (h == 0xb098a293u && fab(ty -251) < 3 && tx > -534 && tx < -528) ||   // P_Abstract_00 PaAppMapSensor_00
            (h == 0xaf98a100u && fab(ty -251) < 3 && tx > -534 && tx < -528) ||   // P_Abstract_01 PaAppMapSensor_00
            (h == 0x61103407u && fab(ty -251) < 3 && tx > -534 && tx < -528) ||   // P_BG_01 PaAppMapSensor_00
            (h == 0x4295d322u && fab(ty +250) < 3 && tx > 477 && tx < 483) ||   // P_BarUS_00 PaFadeBar_00
            (h == 0x4943c7aau && fab(ty -221) < 3 && tx > 401 && tx < 407) ||   // P_Blur_08 Title_00
            (h == 0x410a8b13u && fab(ty -251) < 3 && tx > -534 && tx < -528) ||   // P_Born_01 PaAppMapSensor_00
            (h == 0xbabd1602u && fab(ty -251) < 3 && tx > -534 && tx < -528) ||   // P_CaptureBorn_00 PaAppMapSensor_00
            (h == 0xacb52a28u && fab(ty +260) < 3 && tx > -3 && tx < 3) ||   // P_Complete_00 LastComplete_00
            (h == 0x2ddef4f3u && fab(ty -251) < 3 && tx > -534 && tx < -528) ||   // P_FBLayout_00 PaAppMapSensor_00
            (h == 0x085743f2u && fab(ty +235) < 3 && tx > -3 && tx < 3) ||   // P_GlowB_00 MessageSp_00
            (h == 0x34ce41dau && fab(ty +235) < 3 && tx > -3 && tx < 3) ||   // P_Glow_00 MessageSp_00
            (h == 0x04758f1au && fab(ty -256) < 3 && tx > -519 && tx < -513) ||   // P_Icon_00 HomeNixSign_00
            (h == 0x514622f4u && fab(ty +260) < 3 && tx > -3 && tx < 3) ||   // P_Sh_00 MessageDemo_00
            (h == 0x514622f4u && fab(ty +174) < 3 && tx > -160 && tx < -154) ||   // P_Sh_00 Message_00
            (h == 0x514622f4u && fab(ty -249) < 3 && tx > -3 && tx < 3) ||   // P_Sh_00 PaSystemWindow_02
            (h == 0x52462487u && fab(ty +235) < 3 && tx > -3 && tx < 3) ||   // P_Sh_01 MessageSp_00
            (h == 0x5346261au && fab(ty +235) < 3 && tx > -3 && tx < 3) ||   // P_Sh_02 MessageSp_00
            (h == 0xed93f428u && fab(ty +235) < 3 && tx > -3 && tx < 3) ||   // P_Text_00 MessageSp_00
            (h == 0xee93f5bbu && fab(ty +235) < 3 && tx > -3 && tx < 3) ||   // P_Text_01 MessageSp_00
            (h == 0xef93f74eu && fab(ty +235) < 3 && tx > -3 && tx < 3) ||   // P_Text_02 MessageSp_00
            (h == 0xe5e5eb22u && fab(ty +250) < 3 && tx > 477 && tx < 483) ||   // P_TitleLogo_00 PaFadeBar_00
            (h == 0xe0831f9fu && fab(ty +251) < 3 && tx > -196 && tx < -190) ||   // Pa_BtnL_01 PaSystemWindow_02
            (h == 0x19154191u && fab(ty +251) < 3 && tx > 190 && tx < 196) ||   // Pa_BtnR_01 PaSystemWindow_02
            (h == 0x005606c5u && fab(ty -220) < 3 && tx > -3 && tx < 3) ||   // Pa_HeartGauge_00 MainScreenHeartIchigekiDLC_00,MainScreenMS_00
            (h == 0xc911ab6au && fab(ty +251) < 3 && tx > -51 && tx < -45) ||   // Pa_MapDLCGaugeL_00 AppMap_00
            (h == 0x8ab7ca46u && fab(ty -268) < 3 && tx > 577 && tx < 583) ||   // Pa_Num_00 FadeStatus_00
            (h == 0x8bb7cbd9u && fab(ty -241) < 3 && tx > 577 && tx < 583) ||   // Pa_Num_01 FadeStatus_00
            (h == 0x88b7c720u && fab(ty -214) < 3 && tx > 577 && tx < 583) ||   // Pa_Num_02 FadeStatus_00
            (h == 0xd5581193u && fab(ty +255) < 3 && tx > -63 && tx < -57) ||   // Pa_Pin_00 PaCameraPointer_00
            (h == 0xd4581000u && fab(ty +255) < 3 && tx > -33 && tx < -27) ||   // Pa_Pin_01 PaCameraPointer_00
            (h == 0xd75814b9u && fab(ty +255) < 3 && tx > -3 && tx < 3) ||   // Pa_Pin_02 PaCameraPointer_00
            (h == 0xd6581326u && fab(ty +255) < 3 && tx > 27 && tx < 33) ||   // Pa_Pin_03 PaCameraPointer_00
            (h == 0xd95817dfu && fab(ty +255) < 3 && tx > 57 && tx < 63) ||   // Pa_Pin_04 PaCameraPointer_00
            (h == 0xb90d99a1u && fab(ty +251) < 3 && tx > 528 && tx < 534) ||   // Pa_SensorIcon_00 AppMapDungeon_00,AppMap_00
            (h == 0x0a217c55u && fab(ty +251) < 3 && tx > -612 && tx < -606) ||   // Pa_Temp_01 AppMap_00
            (h == 0x548e5618u && fab(ty +216) < 3 && tx > -612 && tx < -606) ||   // Pa_Time_01 AppMap_00
            (h == 0x73b7d5e4u && fab(ty +235) < 3 && tx > -3 && tx < 3) ||   // T_Message_00 Message_00
            (h == 0xa8b3a6d8u && fab(ty +235) < 3 && tx > -3 && tx < 3) ||   // T_Message_00_CN Message_00
            (h == 0x9596bc0eu && fab(ty +235) < 3 && tx > -3 && tx < 3) ||   // T_Message_00_JPja Message_00
            (h == 0xc4c75aa4u && fab(ty +235) < 3 && tx > -3 && tx < 3) ||   // T_Message_00_KR Message_00
            (h == 0xf0e1e365u && fab(ty +235) < 3 && tx > -3 && tx < 3) ||   // T_Message_00_TH Message_00
            (h == 0x01e1fe28u && fab(ty +235) < 3 && tx > -3 && tx < 3) ||   // T_Message_00_TW Message_00
            (h == 0x74b7d777u && fab(ty +260) < 3 && tx > -3 && tx < 3) ||   // T_Message_01 MessageDemo_00
            (h == 0x7aa586fdu && fab(ty +260) < 3 && tx > -3 && tx < 3) ||   // T_Message_01_CN MessageDemo_00
            (h == 0xd4f18b2bu && fab(ty +260) < 3 && tx > -3 && tx < 3) ||   // T_Message_01_JPja MessageDemo_00
            (h == 0x4e91ba01u && fab(ty +260) < 3 && tx > -3 && tx < 3) ||   // T_Message_01_KR MessageDemo_00
            (h == 0x3a805164u && fab(ty +260) < 3 && tx > -3 && tx < 3) ||   // T_Message_01_TH MessageDemo_00
            (h == 0x41805c69u && fab(ty +260) < 3 && tx > -3 && tx < 3) ||   // T_Message_01_TW MessageDemo_00
            (h == 0x217d8e30u && fab(ty +174) < 3 && tx > -251 && tx < -245) ||   // T_Name_00 Message_00
            (h == 0x7e12dc0au && fab(ty +174) < 3 && tx > -251 && tx < -245) ||   // T_Name_00_JPja Message_00
            (h == 0xb8a74a30u && fab(ty +174) < 3 && tx > -251 && tx < -245) ||   // T_Name_00_KR Message_00
            (h == 0xb8a499b7u && fab(ty -245) < 3 && tx > -3 && tx < 3) ||   // T_TextAlbum_00 PaSystemWindow_02
            (h == 0xba4b14cdu && fab(ty +229) < 3 && tx > -3 && tx < 3) ||   // T_TextDLC_00 PaDLCTipsBefore_00
            (h == 0x0d66a434u && fab(ty +220) < 3 && tx > -597 && tx < -591) ||   // T_Text_00 SystemWindowNoBtn_00
            (h == 0xbf5c91beu && fab(ty +220) < 3 && tx > -597 && tx < -591) ||   // T_Text_00_JPja SystemWindowNoBtn_00
            (h == 0xf7a26114u && fab(ty +220) < 3 && tx > -597 && tx < -591) ||   // T_Text_00_KR SystemWindowNoBtn_00
            (h == 0xa9217f83u && fab(ty +235) < 3 && tx > -3 && tx < 3) ||   // W_Base_00 Message_00
            (h == 0xa8217df0u && fab(ty +235) < 3 && tx > -3 && tx < 3) ||   // W_Base_01 MessageSp_00,Message_00
            (h == 0xaa218116u && fab(ty +220) < 3 && tx > -443 && tx < -437) ||   // W_Base_03 SystemWindowNoBtn_00
            (h == 0xad2185cfu && fab(ty +220) < 3 && tx > -443 && tx < -437) ||   // W_Base_04 SystemWindowNoBtn_00
            (h == 0xac21843cu && fab(ty +220) < 3 && tx > -443 && tx < -437) ||   // W_Base_05 SystemWindowNoBtn_00
            (h == 0x45aa27ccu && fab(ty -251) < 3 && tx > -534 && tx < -528) ||   // W_Shadow_00 PaAppMapSensor_00
            (h == 0x7f6a968eu && fab(ty +235) < 3 && tx > -3 && tx < 3) ||   // W_SysBase_00 Message_00
            (h == 0x806a9821u && fab(ty +235) < 3 && tx > -3 && tx < 3) ||   // W_SysBase_01 Message_00
            (h == 0x034fbbb7u && fab(ty -251) < 3 && tx > -534 && tx < -528);   // White8_00 PaAppMapSensor_00
    }
    if (on) *gy += *gy > 0 ? d : -d;
}

void duo_size(char *pane, const float *st, const void *vt_cut, int cls) {
    // a full-screen capture centred on the screen (a capture's own matrix is its capture space, so its size and translation decide), made as tall as the screen once:
    // one of its children (the map's frame, moved to the edges) so it takes in the taller content; one of the screen behind it (the menus' blurred backgrounds, the
    // weapon wheel's screenshot, the inventory's side panel) so it copies the whole taller screen (the copied rect and the texture follow the screen's layout height,
    // main+0xf5a0dc / main+0xf5a4b8) and, shown stretched to it or projected fitted to the screen's whole height (main+0x2df28), lines up. A copy of the first class
    // with no children made taller once squashed the inventory's Link to 0.72 of his height (an iPhone Duo) while the projection still fitted 720 rows; with the
    // projection following, it is made taller too. The map's capture, inside its full-screen cut-out, stays (made taller, its footprint composite was not drawn)
    float *ph = (float *)(pane + 0x54), k = st[1];
    (void)cls;
    if (st[0] == 0 || f(pane, 0x50) < st[4] || *ph < st[5] || *ph >= st[5] * k - 1 || fab(f(pane, 0x30)) >= 40 || fab(f(pane, 0x34)) >= 40 || in_cut(pane, st, vt_cut)) return;
    *ph *= k;
}
