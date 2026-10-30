// Pure presentation / UI geometry: no Metal device, shader compilation or game data.
#include "../nvn/nvn_aspect.h"
#include <stdio.h>

static int checks, fails;
#define CHECK(c, why) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", why); } } while (0)
#define NEAR(a, b) (fabs((a) - (b)) < 0.00001)

int main(void) {
    nvn_aspect_rect r = nvn_aspect_present(1920, 1080, 3440, 1440, 0);
    CHECK(r.x == 440 && r.y == 0 && r.w == 2560 && r.h == 1440, "unsupported/off retains 16:9 pillarbox");
    r = nvn_aspect_present(1920, 1080, 3440, 1440, 3440.0 / 1440);
    CHECK(r.x == 0 && r.y == 0 && r.w == 3440 && r.h == 1440, "acknowledged wide camera fills wide drawable");
    r = nvn_aspect_present(1920, 1080, 1920, 1080, 21.0 / 9);
    CHECK(r.x == 0 && r.y == 128 && r.w == 1920 && r.h == 823, "camera aspect, not destination shape, controls presentation");
    r = nvn_aspect_present(1280, 720, 1920, 1200, 0);
    CHECK(r.x == 0 && r.y == 60 && r.w == 1920 && r.h == 1080, "valid source crop controls normal aspect-fit");
    r = nvn_aspect_present(1280, 720, 3440, 1440, 3440.0 / 1440);
    CHECK(r.w == 3440 && r.h == 1440, "valid crop dimensions do not undo camera anamorphism");
    r = nvn_aspect_present(1952, 900, 2868, 1320, 0);
    CHECK(r.x == 0 && r.y == 0 && r.w == 2868 && r.h == 1320, "existing one-percent rounding tolerance");
    r = nvn_aspect_present(1280, 720, 2868, 1320, 2868.0 / 1320);
    CHECK(r.x == 0 && r.y == 0 && r.w == 2868 && r.h == 1320, "TOTK acknowledged iPhone camera uses actual drawable, not a named ultrawide preset");

    CHECK(nvn_aspect_canvas_candidate(1920, 1080, 0, 0, 1), "packed or float docked final canvas is eligible");
    CHECK(nvn_aspect_canvas_candidate(1280, 720, 0, 0, 1), "handheld float final canvas is eligible");
    CHECK(!nvn_aspect_canvas_candidate(480, 270, 0, 0, 1), "small rendered scene picture is not the final canvas");
    CHECK(!nvn_aspect_canvas_candidate(1600, 896, 0, 1, 1), "depth-bearing scene target is not the final canvas");
    CHECK(!nvn_aspect_canvas_candidate(1920, 1080, 1, 0, 1), "mip target is not the final canvas");
    CHECK(!nvn_aspect_canvas_candidate(1920, 1080, 0, 0, 0), "ordinary unorm target is not a packed/float final canvas");
    CHECK(!nvn_aspect_canvas_candidate(1920, 0, 0, 0, 1), "empty canvas is rejected");

    double x, y;
    nvn_aspect_ui_scale(0, &x, &y);
    CHECK(x == 1 && y == 1, "no acknowledged camera means no UI change");
    nvn_aspect_ui_scale(16.0 / 9 * 1.0005, &x, &y);
    CHECK(x == 1 && y == 1, "near-native UI tolerance");
    nvn_aspect_ui_scale(21.0 / 9, &x, &y);
    CHECK(NEAR(x, 16.0 / 21) && y == 1, "wide UI inverse horizontal stretch");
    r = nvn_aspect_ui_rect((nvn_aspect_rect){0, 0, 1920, 1080}, 1920, 1080, x, y);
    CHECK(NEAR(r.x + r.w / 2, 960) && r.y == 0 && r.h == 1080, "wide UI viewport remains centred");
    nvn_aspect_rect sc = nvn_aspect_ui_rect((nvn_aspect_rect){100, 200, 400, 300}, 1920, 1080, x, y);
    CHECK(NEAR(sc.x, r.x + 100 * x) && NEAR(sc.w, 400 * x) && sc.y == 200, "subrect scissors use the viewport's transform");
    nvn_aspect_ui_scale(16.0 / 10, &x, &y);
    CHECK(x == 1 && NEAR(y, 0.9), "taller effective picture inversely compresses UI vertically");
    CHECK(nvn_aspect_ui_draw(1, 6, 0), "canvas layout quad is compensated");
    CHECK(!nvn_aspect_ui_draw(0, 6, 0), "world quad outside canvas stays untouched");
    CHECK(!nvn_aspect_ui_draw(1, 3, 0), "scene upscale fullscreen triangle stays untouched");
    CHECK(!nvn_aspect_ui_draw(1, 6, 1), "blurred rendered scene picture stays untouched");
    CHECK(nvn_aspect_ui_shader(0, NULL), "BOTW packed-canvas classifier remains shader-independent");
    static const char *const totk_ui[] = {
        "7f2113e2d2db603f", "e9fd7b433dd1fe6c", "a662e313830baa26", "16684efcf1fa7d8d",
        "3e11cee56c2686bb", "48c18ae6", "35cbc686dd8461b8", "4872bec9a5567f8b",
        "9f47ed9055b2205a", "1a40e5f5ff257e13", "2117f58b153eef2c", "8d4e36d08baf8e73", "1469983f337e6bb2"
    };
    for (unsigned i = 0; i < sizeof totk_ui / sizeof *totk_ui; i++)
        CHECK(nvn_aspect_ui_shader(1, totk_ui[i]), "TOTK layout shader is compensated on the float canvas");
    CHECK(nvn_aspect_ui_shader(1, "7f2113e2d2db603f_b"), "translated shader variant keeps layout identity");
    CHECK(!nvn_aspect_ui_shader(1, NULL), "float canvas with no vertex shader stays untouched");
    CHECK(!nvn_aspect_ui_shader(1, "7d591779"), "inventory opaque scene quad stays untouched");
    CHECK(!nvn_aspect_ui_shader(1, "0ac9a549"), "scene compose stays untouched after UI has started");
    CHECK(!nvn_aspect_ui_shader(1, "7f2113e"), "partial shader prefix is not a layout identity");
    CHECK(!(nvn_aspect_ui_draw(1, 6, 1) && nvn_aspect_ui_shader(1, totk_ui[0])), "TOTK layout sampling a rendered scene keeps full width");
    printf("aspect: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
