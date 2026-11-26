// The renderer NRO of EA SPORTS FC 27 (1.8d.6523 and 1.8d.f570) builds its NVN swapchain from the size its settings pick (a 1280x720 default when it picks none) and reads neither display call.
// The rewriter's cuts (dm/games/0100C49025D3E000/patches.py) make its swapchain constructor and resize take the size from the words 16 and 17 of the NRO's key page, as PC-relative
// loads: code cannot be written once it runs from the signed app on iOS, but the key page is data until the loader seals it. With Native resolution on (hle_display.c) the words hold
// the 16:9 picture that fits the display, so the swapchain, its window textures, the crop and the present all follow it; with it off, the Switch's docked 1920x1080.
#include "hle.h"

#define FC27_NRO "d924da0d0673a87b550159a839f5f807"
#define FC27_KEY_PAGE_OFF 0xfc000u
#define FC27_WORD_W 16u   // key page words (the rewriter keeps words 0-3 for its keys); the cuts load them at +0x80 and +0x88
#define FC27_WORD_H 17u
#define FC27_CUT_WIDTH 0x7a310u   // the first cut's offset; its word tells the rewritten NRO from the game's own

// Called by install_key_page for every module, before the page is made read-only: fills the words the swapchain cuts read
void nro_key_words(module_t *m, uint64_t *page) {
    if (!m->nro || strcmp(m->build_id, FC27_NRO)) return;
    if (m->key_page_off != FC27_KEY_PAGE_OFF || *(const uint32_t *)(m->base + FC27_CUT_WIDTH) != 0x1840eb88u) {
        LOG("[RED] %s (%s) is not the NRO the swapchain cuts were made for (key page %#llx); the game keeps its own swapchain size", m->name, m->build_id, (unsigned long long)m->key_page_off);
        return;
    }
    int w = 1920, h = 1080;
    int native = dm_display_target(&w, &h);   // also decides the display size before any game code runs
    page[FC27_WORD_W] = (uint64_t)w; page[FC27_WORD_H] = (uint64_t)h;
    LOG("main: the swapchain of %s (%s) is %dx%d%s", m->name, m->build_id, w, h, native ? "" : " (Native resolution is off: the Switch's size)");
}
