// The runtime NRO patch table: the MOVZ encoder's words, the four FC 27 swapchain size instructions rewritten for each launcher size, and the
// refusal to write when the build or the offline bytes are not the ones the table expects.
#define DM_NRO_TEST 1
#include "../hle_nro.c"
#include <assert.h>
#include <sys/mman.h>

static module_t test_modules[1];
module_t *g_mod = test_modules;
int g_nmod;

static uint32_t get(uint8_t *b, uint32_t off) { return *(uint32_t *)(b + off); }
static void put(uint8_t *b, uint32_t off, uint32_t v) { *(uint32_t *)(b + off) = v; }

static uint8_t *fresh(const char *build) {
    uint8_t *b = mmap(NULL, 0x80000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(b != MAP_FAILED);
    memset(test_modules, 0, sizeof test_modules);
    strcpy(test_modules[0].name, "nro0"); test_modules[0].base = b; test_modules[0].size = 0x80000; test_modules[0].text_end = 0x80000; test_modules[0].nro = 1;
    strcpy(test_modules[0].build_id, build);
    put(b, 0x7a310, 0x52814008); put(b, 0x7a314, 0xb9000e68); put(b, 0x7a318, 0x5280b408); put(b, 0x7a31c, 0xb9001268);
    put(b, 0x7ad00, 0xd503201f); put(b, 0x7ad18, 0xd280b414); put(b, 0x7ad1c, 0xd503201f); put(b, 0x7ad24, 0xd2814015); put(b, 0x7ad3c, 0xd503201f);
    g_nmod = 1;
    return b;
}

static void expect_sizes(uint8_t *b, int w, int h) {
    assert(get(b, 0x7a310) == movz(0, 8, (unsigned)w));
    assert(get(b, 0x7a318) == movz(0, 8, (unsigned)h));
    assert(get(b, 0x7ad18) == movz(1, 20, (unsigned)h));
    assert(get(b, 0x7ad24) == movz(1, 21, (unsigned)w));
}

static void unchanged(uint8_t *b) {
    assert(get(b, 0x7a310) == 0x52814008 && get(b, 0x7a318) == 0x5280b408 && get(b, 0x7ad18) == 0xd280b414 && get(b, 0x7ad24) == 0xd2814015);
}

static void restored(uint8_t *b) {
    assert(get(b, 0x7a310) == 0x34000408 && get(b, 0x7a314) == 0xb9401268 && get(b, 0x7a318) == 0x34000448 && get(b, 0x7a31c) == 0xaa1303e0 &&
           get(b, 0x7ad00) == 0xb40009c1 && get(b, 0x7ad18) == 0xaa0203f4 && get(b, 0x7ad1c) == 0xb4000862 && get(b, 0x7ad24) == 0xaa0103f5 &&
           get(b, 0x7ad3c) == 0x54000760);
}

int main(void) {
    // The encoder reproduces the offline 1440p cuts exactly.
    assert(movz(0, 8, 2560) == 0x52814008); assert(movz(0, 8, 1440) == 0x5280b408);
    assert(movz(1, 20, 1440) == 0xd280b414); assert(movz(1, 21, 2560) == 0xd2814015);

    uint8_t *b = fresh("d924da0d0673a87b550159a839f5f807");
    setenv("DM_DISPLAY_SIZE", "1920x1080", 1); nro_runtime_patch(); expect_sizes(b, 1920, 1080);
    munmap(b, 0x80000);

    b = fresh("d924da0d0673a87b550159a839f5f807");
    setenv("DM_DISPLAY_SIZE", "1280x720", 1); nro_runtime_patch(); expect_sizes(b, 1280, 720);
    munmap(b, 0x80000);

    b = fresh("d924da0d0673a87b550159a839f5f807");
    setenv("DM_DISPLAY_SIZE", "3840x2160", 1); nro_runtime_patch(); expect_sizes(b, 3840, 2160);
    munmap(b, 0x80000);

    b = fresh("d924da0d0673a87b550159a839f5f807");
    setenv("DM_DISPLAY_SIZE", "2560x1440", 1); nro_runtime_patch(); unchanged(b);   // the offline cuts already hold it
    munmap(b, 0x80000);

    b = fresh("d924da0d0673a87b550159a839f5f807");
    put(b, 0x7ad24, 0xd2814016);   // one row is not the byte the table expects: no partial write
    setenv("DM_DISPLAY_SIZE", "1920x1080", 1); nro_runtime_patch();
    assert(get(b, 0x7a310) == 0x52814008 && get(b, 0x7a318) == 0x5280b408 && get(b, 0x7ad18) == 0xd280b414 && get(b, 0x7ad24) == 0xd2814016);
    munmap(b, 0x80000);

    b = fresh("00000000000000000000000000000000");   // another build: untouched
    setenv("DM_DISPLAY_SIZE", "1920x1080", 1); nro_runtime_patch(); unchanged(b);
    munmap(b, 0x80000);

    b = fresh("d924da0d0673a87b550159a839f5f807");   // iOS with no DM_DISPLAY_SIZE: restore the original swapchain code
    unsetenv("DM_DISPLAY_SIZE"); nro_runtime_patch(); restored(b);
    munmap(b, 0x80000);

    b = fresh("d924da0d0673a87b550159a839f5f807");
    nro_restore_original(); restored(b);
    munmap(b, 0x80000);

    b = fresh("d924da0d0673a87b550159a839f5f807");
    put(b, 0x7ad3c, 0xd503201e);   // one cut word differs: no partial restore
    nro_restore_original();
    assert(get(b, 0x7a310) == 0x52814008 && get(b, 0x7a314) == 0xb9000e68 && get(b, 0x7a318) == 0x5280b408 && get(b, 0x7a31c) == 0xb9001268 &&
           get(b, 0x7ad00) == 0xd503201f && get(b, 0x7ad18) == 0xd280b414 && get(b, 0x7ad1c) == 0xd503201f && get(b, 0x7ad24) == 0xd2814015 &&
           get(b, 0x7ad3c) == 0xd503201e);
    munmap(b, 0x80000);

    puts("t_nro_patch: OK");
    return 0;
}
