// Exercise real fixups parsing and loader aspect state; synthetic pages, no game files.
#include "../loader.c"
#include <assert.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>

struct dm_flags g_flag;
uint64_t now_ns(void) { return 0; }
void gate_register(uint64_t a, const char *k, int n, uint64_t o) { (void)a; (void)k; (void)n; (void)o; }
void hlecount_seal(void) {}
void *hlecount_wrap(const char *kind, const char *name, void *fn) { (void)kind; (void)name; return fn; }
uint64_t resolve_import(module_t *m, const char *name) { (void)m; (void)name; return 0; }
int hle_implements(const char *name) { (void)name; return 0; }
int svc_nro_deactivate(module_t *m) { m->loaded = 0; return 1; }

static _Alignas(0x4000) uint8_t image[0xc000];
static module_t module;
static const char *verified = "cd57b23fa4bbad65803d9788c01821ee";

static void reset(void) {
    atomic_store(&g_aspect_state, NULL);
    g_aspect_enabled = 0; g_aspect_drawable = 0; dm_fps_state = NULL;
    g_screen_aspect_supported = 0; atomic_store(&g_aspect_tall_active, 0);
    memset(image, 0, sizeof image);
    module = (module_t){.base = image, .size = sizeof image, .fps_state_off = 0x8000,
                        .key_page_off = 0x4000, .key_page_size = 0x4000, .camera_aspect = 1};
    strcpy(module.name, "main"); strcpy(module.build_id, verified);
    unsetenv("DM_WIDESCREEN"); setenv("DM_FPS_TARGET", "0", 1); unsetenv("DM_RENDER_SCALE");
    unsetenv("DM_SCREEN_ASPECT"); unsetenv("DM_SCREEN_ASPECT_SITES");
    unsetenv("DM_SCREEN_FOV"); unsetenv("DM_SCREEN_HUD");
}
static float desired(void) {
    uint32_t bits = atomic_load((_Atomic uint32_t *)(void *)(image+0x8040));
    float f; memcpy(&f, &bits, sizeof f); return f;
}
// The execution regression runs the real guest writer; this host-only fixture
// emulates its release acknowledgement to test the consumer independently.
static void acknowledge(void) {
    uint32_t bits = atomic_load((_Atomic uint32_t *)(void *)(image+0x8040));
    atomic_store_explicit((_Atomic uint32_t *)(void *)(image+0x8044), bits, memory_order_release);
}
static void preload_ordering(void) {
    reset(); dm_game_aspect_drawable(2520, 1080);
    assert(dm_game_aspect() == 0);
    install_fps_state(&module);
    assert(desired() == 0 && dm_game_aspect() == 0); // preload must not read launcher settings
    dm_fps_state_apply();
    assert(desired() == 2520.0f/1080 && dm_game_aspect() == 0);
    acknowledge(); assert(dm_game_aspect() == desired());
    dm_game_aspect_drawable(3840, 1080);
    assert(desired() == 3840.0f/1080 && dm_game_aspect() == 2520.0f/1080);
    acknowledge(); assert(dm_game_aspect() == desired());
    dm_game_aspect_drawable(1920, 1080);
    assert(desired() == 0 && dm_game_aspect() == 3840.0f/1080);
    acknowledge(); assert(dm_game_aspect() == 0);

    // Opposite notification order also works, including an iOS opt-out set after preload.
    reset(); install_fps_state(&module); setenv("DM_WIDESCREEN", "0", 1);
    dm_fps_state_apply(); dm_game_aspect_drawable(2520, 1080);
    assert(desired() == 0 && dm_game_aspect() == 0);
    unsetenv("DM_WIDESCREEN"); dm_fps_state_apply();
    assert(desired() == 2520.0f/1080); acknowledge(); assert(dm_game_aspect() == desired());
    setenv("DM_WIDESCREEN", "0", 1); dm_fps_state_apply();
    assert(desired() == 0 && dm_game_aspect() == 2520.0f/1080);
    acknowledge(); assert(dm_game_aspect() == 0);
}
static void unsupported(void) {
    for (unsigned mode = 0; mode < 3; mode++) {
        reset();
        if (mode == 0) module.camera_aspect = 0; // old output, no hook
        if (mode == 1) strcpy(module.build_id, "00000000000000000000000000000000");
        if (mode == 2) strcpy(module.name, "sdk");
        install_fps_state(&module); dm_game_aspect_drawable(2520, 1080); dm_fps_state_apply();
        assert(desired() == 0 && dm_game_aspect() == 0);
    }
    reset(); install_fps_state(&module); dm_fps_state_apply();
    for (unsigned i = 0; i < 3; i++) {
        dm_game_aspect_drawable(i ? 1024 : 1920, i == 2 ? 1024 : 1080);
        assert(desired() == 0);
    }
    dm_game_aspect_drawable(2520, 1080);
    for (unsigned i = 0; i < 3; i++) dm_game_aspect_drawable(i ? NAN : 0, 1080);
    assert(desired() == 2520.0f/1080); // transient invalid drawable notifications do not disable
}
static float state_float(unsigned offset) {
    float f; memcpy(&f, image + 0x8000 + offset, sizeof f); return f;
}
static uint32_t state_word(unsigned offset) {
    uint32_t word; memcpy(&word, image + 0x8000 + offset, sizeof word); return word;
}
static void tall_parameters_disabled(void) {
    for (unsigned off = 0x48; off < 0x64; off += 4) assert(state_word(off) == 0);
    for (unsigned off = 0x74; off < 0x9c; off += 4) assert(state_word(off) == 0);
}
static void tall_precedence(void) {
    reset(); install_fps_state(&module); dm_game_aspect_drawable(2520, 1080);
    memset(image + 0x8064, 0xa5, 0x74 - 0x64);
    memset(image + 0x80fc, 0x5a, 0x200 - 0xfc);
    dm_fps_state_apply(); acknowledge();
    const int supported = DM_GAME_SCREEN_ASPECT && !strcmp(verified, "cd57b23fa4bbad65803d9788c01821ee");
    setenv("DM_SCREEN_ASPECT", "1.43", 1); dm_fps_state_apply();
    assert(desired() == (supported ? 0 : 2520.0f/1080));
    assert(dm_game_aspect() == (supported ? 0 : 2520.0f/1080));
    assert(state_float(0x44) == 2520.0f/1080); // host never manufactures a guest ACK
    assert(state_word(0x48) == (supported ? 1344 : 0));
    assert(state_word(0x4c) == (supported ? 896 : 0));
    assert(state_float(0x50) == (supported ? 896 : 0));
    assert(state_float(0x58) == (supported ? 1920.0f/1344 : 0));
    assert(state_float(0x98) == (supported ? 1300 : 0)); // last host-written tall field
    dm_game_aspect_drawable(3840, 1080);
    assert(desired() == (supported ? 0 : 3840.0f/1080));
    assert(dm_game_aspect() == (supported ? 0 : 2520.0f/1080));
    acknowledge(); assert(dm_game_aspect() == (supported ? 0 : 3840.0f/1080));
    unsetenv("DM_SCREEN_ASPECT"); dm_fps_state_apply();
    assert(desired() == 3840.0f/1080);
    assert(dm_game_aspect() == (supported ? 0 : 3840.0f/1080));
    tall_parameters_disabled();
    for (unsigned off = 0x64; off < 0x74; off += 4) assert(state_word(off) == 0xa5a5a5a5);
    for (unsigned off = 0xfc; off < 0x200; off += 4) assert(state_word(off) == 0x5a5a5a5a);
    acknowledge(); assert(dm_game_aspect() == desired());

    // A merely present, disabled or rejected setting must not block widening,
    // including after a supported active tall mode has populated the state.
    const char *inactive[] = {"", "0", "1.77", "1.2", "nan", "1.43junk"};
    for (unsigned i = 0; i < sizeof inactive / sizeof *inactive; i++) {
        setenv("DM_SCREEN_ASPECT", "1.43", 1); dm_fps_state_apply();
        setenv("DM_SCREEN_ASPECT", inactive[i], 1); dm_fps_state_apply();
        assert(desired() == 3840.0f/1080);
        tall_parameters_disabled();
    }
    // Upstream tall mode does not depend on the newer landscape capability tag.
    reset(); module.camera_aspect = 0; install_fps_state(&module);
    setenv("DM_SCREEN_ASPECT", "1.43", 1); dm_fps_state_apply();
    assert(state_word(0x48) == (supported ? 1344 : 0));
    assert(desired() == 0 && dm_game_aspect() == 0);
    // An unrelated build or non-main module cannot enable the BOTW tall stubs.
    for (unsigned mode = 0; mode < 2; mode++) {
        reset();
        if (mode) strcpy(module.name, "sdk");
        else strcpy(module.build_id, "00000000000000000000000000000000");
        install_fps_state(&module); setenv("DM_SCREEN_ASPECT", "1.43", 1);
        dm_fps_state_apply();
        tall_parameters_disabled();
    }
}
static void animal_crossing_tall(void) {
    reset(); module.camera_aspect = 0;
    strcpy(module.build_id, "ff1d1c05670db6021c85b624a710b963"); // upstream 3.0.3 main
    install_fps_state(&module); dm_game_aspect_drawable(2520, 1080);
    setenv("DM_WIDESCREEN", "0", 1); setenv("DM_SCREEN_ASPECT", "1.43", 1);
    setenv("DM_SCREEN_ASPECT_SITES", "0x1ff", 1);
    dm_fps_state_apply();
    assert(desired() == 0 && dm_game_aspect() == 0);
    assert(state_word(0x48) == (DM_GAME_SCREEN_ASPECT ? 1344 : 0));
    assert(state_word(0x4c) == (DM_GAME_SCREEN_ASPECT ? 896 : 0));
    assert(state_float(0x50) == (DM_GAME_SCREEN_ASPECT ? 896 : 0));
    assert(state_word(0x54) == (DM_GAME_SCREEN_ASPECT ? 0x1ff : 0));
    assert(state_float(0x58) == (DM_GAME_SCREEN_ASPECT ? 1920.0f/1344 : 0));
    assert(state_float(0x5c) == (DM_GAME_SCREEN_ASPECT ? (16.0f/9.0f)/(1920.0f/1344) : 0));
    assert(state_float(0x80) == (DM_GAME_SCREEN_ASPECT ? 88 : 0));
    assert(state_float(0x98) == (DM_GAME_SCREEN_ASPECT ? 1300 : 0));
    setenv("DM_SCREEN_ASPECT", "0", 1); dm_fps_state_apply();
    tall_parameters_disabled();
    assert(desired() == 0 && dm_game_aspect() == 0);
}
static void layout(void) {
    reset();
    char dir[] = "/tmp/dm-aspect-XXXXXX"; assert(mkdtemp(dir));
    char path[128]; snprintf(path, sizeof path, "%s/main.fixups.tsv", dir);
    FILE *f = fopen(path, "w"); assert(f);
    fprintf(f, "# PATCH_OFF\t0xc000\n# KEY_PAGE_OFF\t0x4000\n# KEY_PAGE_SIZE\t0x4000\n"
               "# KEY_K18_IDX\t0\n# KEY_KTLS_IDX\t1\n# KEY_KTLR_IDX\t2\n# KEY_KGATE_IDX\t3\n"
               "# FPS_STATE_OFF\t0x8000\n# CAMERA_ASPECT\t1\n# BUILD_ID\t%s\n", verified);
    fclose(f); module.camera_aspect = 0; module.fps_state_off = 0; module.build_id[0] = 0;
    read_layout(&module, dir);
    assert(module.camera_aspect && module.fps_state_off == 0x8000 && !strcmp(module.build_id, verified));
    unlink(path); rmdir(dir);
    module.size = 0x809c; install_fps_state(&module); // +0x98's four-byte value fits exactly
    image[0x809c] = 0x5a;
    setenv("DM_SCREEN_ASPECT", "1.43", 1); dm_fps_state_apply();
    assert(image[0x809c] == 0x5a);
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        struct rlimit lim = {0, 0}; setrlimit(RLIMIT_CORE, &lim);
        module.size--; install_fps_state(&module); _exit(0);
    }
    int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
static void *notifications(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 10000; i++) dm_game_aspect_drawable(i & 1 ? 2520 : 3840, 1080);
    return NULL;
}
static void *guest_acknowledgements(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 10000; i++) acknowledge();
    return NULL;
}
static void concurrency(void) {
    reset(); install_fps_state(&module); dm_fps_state_apply();
    pthread_t writer, guest;
    assert(!pthread_create(&writer, NULL, notifications, NULL));
    assert(!pthread_create(&guest, NULL, guest_acknowledgements, NULL));
    for (unsigned i = 0; i < 10000; i++) {
        float f = dm_game_aspect(); assert(f == 0 || f == 2520.0f/1080 || f == 3840.0f/1080);
    }
    assert(!pthread_join(writer, NULL)); assert(!pthread_join(guest, NULL));
    acknowledge(); assert(dm_game_aspect() == desired());
}
int main(void) {
    preload_ordering(); unsupported(); tall_precedence(); layout(); concurrency();
    verified = "277178b7dba1b6d4949a84778d4abc58";
    preload_ordering(); unsupported(); tall_precedence(); layout(); concurrency();
    animal_crossing_tall();
    puts("BOTW/TOTK camera aspect and ACNH tall shape: profile/build gates, tall precedence, transitions, bounds and concurrency: OK");
    return 0;
}
