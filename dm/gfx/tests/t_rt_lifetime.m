// Queue bindings outlive completed command recordings. Exercise the public NVN
// interface and verify pixels after the caller reuses the recording's memory.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
#define P(name) ((NVNT_##name)nvn_lookup(#name))
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failures;
static NVNdevice device;
static NVNqueue queues[2];
static NVNcommandBuffer command;
static NVNmemoryPool command_pool, texture_pool;
static uint8_t command_bytes[8192] __attribute__((aligned(16384)));
static uint8_t texture_bytes[12 * 16384] __attribute__((aligned(16384)));
static const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1}, blue[4] = {0, 0, 1, 1};
static const uint8_t red_pixel[4] = {255, 0, 0, 255}, green_pixel[4] = {0, 255, 0, 255}, blue_pixel[4] = {0, 0, 255, 255};

static void require(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "setup failed: %s\n", what); exit(2); }
}
static void pool_init(NVNmemoryPool *pool, void *memory, size_t bytes) {
    NVNmemoryPoolBuilder builder;
    P(nvnMemoryPoolBuilderSetDefaults)(&builder);
    P(nvnMemoryPoolBuilderSetDevice)(&builder, &device);
    P(nvnMemoryPoolBuilderSetStorage)(&builder, memory, bytes);
    require(P(nvnMemoryPoolInitialize)(pool, &builder), "memory pool");
}
static void texture_init(NVNtexture *texture, int slot, NVNformat format, int layers, int levels) {
    NVNtextureBuilder builder;
    P(nvnTextureBuilderSetDefaults)(&builder); P(nvnTextureBuilderSetDevice)(&builder, &device);
    P(nvnTextureBuilderSetSize2D)(&builder, 8, 8); P(nvnTextureBuilderSetFormat)(&builder, format);
    if (layers > 1) { P(nvnTextureBuilderSetTarget)(&builder, NVN_TEXTURE_TARGET_2D_ARRAY); P(nvnTextureBuilderSetDepth)(&builder, layers); }
    P(nvnTextureBuilderSetLevels)(&builder, levels);
    require(P(nvnTextureBuilderGetStorageSize)(&builder) <= 16384, "texture fits its storage slot");
    P(nvnTextureBuilderSetStorage)(&builder, &texture_pool, slot * 16384);
    require(P(nvnTextureInitialize)(texture, &builder), "texture");
}
static void begin_reusing_commands(void) {
    // Every prior submission has finished. These bytes no longer belong to a
    // live command list, even though the queue still has a render target bound.
    memset(command_bytes, 0xa5, sizeof command_bytes);
    P(nvnCommandBufferAddCommandMemory)(&command, &command_pool, 0, sizeof command_bytes);
    P(nvnCommandBufferBeginRecording)(&command);
}
static void submit(NVNqueue *queue) {
    NVNcommandHandle handle = P(nvnCommandBufferEndRecording)(&command);
    P(nvnQueueSubmitCommands)(queue, 1, &handle);
    P(nvnQueueFinish)(queue);
}
static void expect_color(const char *name, const NVNtexture *texture, const NVNtextureView *view, int size, const uint8_t expected[4]) {
    NVNcopyRegion region = {0, 0, 0, size, size, 1};
    uint8_t pixels[8 * 8 * 4]; memset(pixels, 0xee, sizeof pixels);
    P(nvnTextureReadTexels)(texture, view, &region, pixels);
    int matches = 0, count = size * size;
    for (int i = 0; i < count; i++) matches += !memcmp(pixels + i * 4, expected, 4);
    checks++; failures += matches != count;
    printf("%s: %d/%d expected pixels %s\n", name, matches, count, matches == count ? "PASS" : "FAIL");
}
int main(void) { @autoreleasepool {
    setvbuf(stdout, NULL, _IOLBF, 0);
    NVNdeviceBuilder db;
    P(nvnDeviceBuilderSetDefaults)(&db);
    require(P(nvnDeviceInitialize)(&device, &db), "device");
    NVNqueueBuilder qb;
    P(nvnQueueBuilderSetDefaults)(&qb); P(nvnQueueBuilderSetDevice)(&qb, &device);
    for (int i = 0; i < 2; i++) require(P(nvnQueueInitialize)(&queues[i], &qb), "queue");
    pool_init(&command_pool, command_bytes, sizeof command_bytes);
    pool_init(&texture_pool, texture_bytes, sizeof texture_bytes);
    require(P(nvnCommandBufferInitialize)(&command, &device), "command buffer");
    NVNtexture targets[2];
    for (int i = 0; i < 2; i++) texture_init(&targets[i], i, NVN_FORMAT_RGBA8, 1, 1);
    const NVNtexture *first[] = {&targets[0]}, *second[] = {&targets[1]};
    begin_reusing_commands();
    P(nvnCommandBufferSetRenderTargets)(&command, 1, first, NULL, NULL, NULL);
    P(nvnCommandBufferClearColor)(&command, 0, red, 15);
    submit(&queues[0]);
    begin_reusing_commands();
    P(nvnCommandBufferClearColor)(&command, 0, green, 15);
    submit(&queues[0]);
    expect_color("completed recording reused without rebinding", &targets[0], NULL, 8, green_pixel);

    begin_reusing_commands();
    P(nvnCommandBufferSetRenderTargets)(&command, 1, second, NULL, NULL, NULL);
    P(nvnCommandBufferClearColor)(&command, 0, blue, 15);
    submit(&queues[1]);
    begin_reusing_commands();
    P(nvnCommandBufferClearColor)(&command, 0, red, 15);
    submit(&queues[0]);
    expect_color("first queue keeps its own target", &targets[0], NULL, 8, red_pixel);
    expect_color("first queue does not clear the second target", &targets[1], NULL, 8, blue_pixel);
    begin_reusing_commands();
    P(nvnCommandBufferClearColor)(&command, 0, green, 15);
    submit(&queues[1]);
    expect_color("second queue restores its target", &targets[1], NULL, 8, green_pixel);

    // A nested list's target binding survives reuse of both child and parent.
    begin_reusing_commands();
    P(nvnCommandBufferSetRenderTargets)(&command, 1, first, NULL, NULL, NULL);
    NVNcommandHandle child = P(nvnCommandBufferEndRecording)(&command);
    P(nvnCommandBufferAddCommandMemory)(&command, &command_pool, 4096, 4096);
    P(nvnCommandBufferBeginRecording)(&command);
    P(nvnCommandBufferCallCommands)(&command, 1, &child);
    P(nvnCommandBufferClearColor)(&command, 0, red, 15);
    submit(&queues[0]);
    begin_reusing_commands();
    P(nvnCommandBufferClearColor)(&command, 0, blue, 15);
    submit(&queues[0]);
    expect_color("nested recording reused after submission", &targets[0], NULL, 8, blue_pixel);

    NVNtexture layered; texture_init(&layered, 2, NVN_FORMAT_RGBA8, 2, 2);
    NVNtextureView view;
    P(nvnTextureViewSetDefaults)(&view); P(nvnTextureViewSetLevels)(&view, 1, 1); P(nvnTextureViewSetLayers)(&view, 1, 1);
    const NVNtexture *layer_target[] = {&layered}; const NVNtextureView *views[] = {&view};
    begin_reusing_commands();
    P(nvnCommandBufferSetRenderTargets)(&command, 1, layer_target, views, NULL, NULL);
    P(nvnCommandBufferClearColor)(&command, 0, red, 15);
    submit(&queues[0]);
    begin_reusing_commands();
    P(nvnCommandBufferClearColor)(&command, 0, green, 15);
    submit(&queues[0]);
    expect_color("retained view keeps its mip and layer", &layered, &view, 4, green_pixel);

    NVNtexture depth; texture_init(&depth, 3, NVN_FORMAT_DEPTH32F, 1, 1);
    begin_reusing_commands();
    P(nvnCommandBufferSetRenderTargets)(&command, 0, NULL, NULL, &depth, NULL);
    P(nvnCommandBufferClearDepthStencil)(&command, 0.25f, 1, 0, 0);
    submit(&queues[0]);
    begin_reusing_commands();
    P(nvnCommandBufferClearDepthStencil)(&command, 0.75f, 1, 0, 0);
    submit(&queues[0]);
    NVNcopyRegion region = {0, 0, 0, 8, 8, 1}; float depth_values[64] = {0};
    P(nvnTextureReadTexels)(&depth, NULL, &region, depth_values);
    int matches = 0; for (int i = 0; i < 64; i++) matches += depth_values[i] == 0.75f;
    checks++; failures += matches != 64;
    printf("retained depth target: %d/64 expected values %s\n", matches, matches == 64 ? "PASS" : "FAIL");

    NVNtexture colors[8]; const NVNtexture *all_colors[8];
    for (int i = 0; i < 8; i++) { texture_init(&colors[i], i + 4, NVN_FORMAT_RGBA8, 1, 1); all_colors[i] = &colors[i]; }
    begin_reusing_commands();
    P(nvnCommandBufferSetRenderTargets)(&command, 8, all_colors, NULL, NULL, NULL);
    for (int i = 0; i < 8; i++) P(nvnCommandBufferClearColor)(&command, i, red, 15);
    submit(&queues[0]);
    begin_reusing_commands();
    P(nvnCommandBufferClearColor)(&command, 7, green, 15);
    submit(&queues[0]);
    expect_color("last color attachment survives reuse", &colors[7], NULL, 8, green_pixel);
    expect_color("clearing the last attachment preserves the first", &colors[0], NULL, 8, red_pixel);

    for (int i = 0; i < 2; i++) P(nvnQueueFinalize)(&queues[i]);
    for (int i = 0; i < 8; i++) P(nvnTextureFinalize)(&colors[i]);
    P(nvnTextureFinalize)(&depth); P(nvnTextureFinalize)(&layered);
    for (int i = 0; i < 2; i++) P(nvnTextureFinalize)(&targets[i]);
    P(nvnCommandBufferFinalize)(&command);
    P(nvnMemoryPoolFinalize)(&texture_pool); P(nvnMemoryPoolFinalize)(&command_pool);
    printf("render target lifetime: %d checks, %d failed\n", checks, failures);
    return failures != 0;
} }
