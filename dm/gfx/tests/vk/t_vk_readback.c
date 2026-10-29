// NVN's readback contract over the Vulkan backend: the Vulkan port of dm/gfx/tests/t_readback.m, recorded through the frontend's command buffer entry points as a game records.
// A pending clear reaches a GPU copy into a buffer (row stride 64: the row padding untouched) and a CPU read (nvnTextureReadTexels, from the image through a staging buffer it
// releases); GPU copies into packaged textures, a copy from one with a GPU write pending, a copy from a buffer a queued copy writes, a format-converting copy (RGBA8 into
// RGBA8 sRGB: converted, not raw bytes) from a texture the CPU filled, and a copy from a texture the GPU cleared, all read back by ReadTexels. The Vulkan backend reads a GPU-owned
// texture from its image (vk_texture.c), so the Metal test's mirror of GPU writes into storage has no counterpart; nor has its count of Metal buffer allocations (here: the
// device's live allocations are the same before and after a ReadTexels). Checks: the texture and command buffer initializations, the fence wait, each readback, validation
#include "t_vk_harness.h"

static int same(const uint8_t *p, const uint8_t want[4], int n, int tol) {
    int bad = 0;
    for (int i = 0; i < n; i++) for (int k = 0; k < 4; k++) bad += abs((int)p[i * 4 + k] - want[k]) > tol;
    return bad;
}

int main(void) {
    hv_up("t_vk_readback");
    hv_device(0);
    NVNqueue queue; NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&queue, &qb);
    hv_pool pool; if (!hv_pool_make(&pool, 524288)) { t("memory pool allocated", 0); return hv_finish(); }
    uint8_t *mem = pool.mem; memset(mem, 0, 524288);
    NVNtextureBuilder tb; NVNtexture tex; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetDevice(&tb, &g_dev); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D);
    h_nvnTextureBuilderSetWidth(&tb, 8); h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, &pool.pool, 131072); t("texture init", h_nvnTextureInitialize(&tex, &tb));
    NVNcommandBuffer cmd; t("command init", h_nvnCommandBufferInitialize(&cmd, &g_dev));
    h_nvnCommandBufferAddCommandMemory(&cmd, &pool.pool, 0, 65536);
    h_nvnCommandBufferBeginRecording(&cmd);
    const NVNtexture *targets[] = {&tex}; const NVNtextureView *views[] = {NULL};
    h_nvnCommandBufferSetRenderTargets(&cmd, 1, targets, views, NULL, NULL);
    float color[] = {0.2f, 0.4f, 0.6f, 1.f}; h_nvnCommandBufferClearColor(&cmd, 0, color, 15);
    NVNcopyRegion region = {0, 0, 0, 8, 4, 1}; uint8_t *out = mem + 196608; memset(out, 0xEE, 256);
    h_nvnCommandBufferSetCopyRowStride(&cmd, 64);
    h_nvnCommandBufferCopyTextureToBuffer(&cmd, &tex, NULL, &region, (NVNbufferAddress)(uintptr_t)out, 0);
    h_nvnCommandBufferSetCopyRowStride(&cmd, 0);
    // packaged textures (block-linear storage the host can fill): a GPU copy into one; a copy from it while that write is pending
    NVNpackagedTextureLayout lay = {{1, 0, 0, 0, 7, 0, 1, 0}}; h_nvnTextureBuilderSetPackagedTextureLayout(&tb, &lay);
    NVNtexture pk, pk2, pk3, pk4, pk5, pk6, pk7;
    h_nvnTextureBuilderSetStorage(&tb, &pool.pool, 163840); t("packaged texture init", h_nvnTextureInitialize(&pk, &tb));
    h_nvnCommandBufferCopyTextureToTexture(&cmd, &tex, NULL, &region, &pk, NULL, &region, 0);
    h_nvnTextureBuilderSetStorage(&tb, &pool.pool, 262144); t("packaged texture 2 init", h_nvnTextureInitialize(&pk2, &tb));
    h_nvnCommandBufferCopyTextureToTexture(&cmd, &pk, NULL, &region, &pk2, NULL, &region, 0);
    // a buffer a copy queued in this submission writes (out → src2 on the GPU) holds stale bytes on the CPU: the copy from it into a packaged texture reads the GPU's
    uint8_t *src2 = mem + 200704; memset(src2, 0xEE, 256);
    h_nvnCommandBufferCopyBufferToBuffer(&cmd, (NVNbufferAddress)(uintptr_t)out, (NVNbufferAddress)(uintptr_t)src2, 256, 0);
    h_nvnTextureBuilderSetStorage(&tb, &pool.pool, 294912); t("packaged texture 4 init", h_nvnTextureInitialize(&pk4, &tb));
    h_nvnCommandBufferSetCopyRowStride(&cmd, 64);
    h_nvnCommandBufferCopyBufferToTexture(&cmd, (NVNbufferAddress)(uintptr_t)src2, &pk4, NULL, &region, 0);
    h_nvnCommandBufferSetCopyRowStride(&cmd, 0);
    // a format-converting copy (RGBA8 → RGBA8 sRGB) from a packaged texture the CPU wrote (every texel the same, whatever the block-linear order)
    static const uint8_t expected[] = {51, 102, 153, 255};
    h_nvnTextureBuilderSetStorage(&tb, &pool.pool, 425984); t("packaged texture 7 init", h_nvnTextureInitialize(&pk7, &tb));
    for (int i = 0; i < 1024; i++) memcpy(mem + 425984 + i * 4, expected, 4);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8_SRGB);
    h_nvnTextureBuilderSetStorage(&tb, &pool.pool, 327680); t("packaged sRGB texture init", h_nvnTextureInitialize(&pk3, &tb));
    h_nvnCommandBufferCopyTextureToTexture(&cmd, &pk7, NULL, &region, &pk3, NULL, &region, 0);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    // a packaged texture the GPU cleared: a copy from it reads what the GPU wrote, not its storage
    h_nvnTextureBuilderSetStorage(&tb, &pool.pool, 360448); t("packaged texture 5 init", h_nvnTextureInitialize(&pk5, &tb));
    h_nvnTextureBuilderSetStorage(&tb, &pool.pool, 393216); t("packaged texture 6 init", h_nvnTextureInitialize(&pk6, &tb));
    float color2[] = {0.8f, 0.6f, 0.4f, 1.f}; h_nvnCommandBufferClearTexture(&cmd, &pk5, NULL, &region, color2, 15);
    h_nvnCommandBufferCopyTextureToTexture(&cmd, &pk5, NULL, &region, &pk6, NULL, &region, 0);
    NVNsync sync; h_nvnSyncInitialize(&sync, &g_dev);
    h_nvnCommandBufferFenceSync(&cmd, &sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    NVNcommandHandle h = h_nvnCommandBufferEndRecording(&cmd);
    h_nvnQueueSubmitCommands(&queue, 1, &h);
    int wait = h_nvnSyncWait(&sync, 5000000000ull); t("recorded fence wait", wait == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || wait == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED);
    int bad = 0;
    for (int y = 0; y < 4; y++) { for (int x = 0; x < 8; x++) bad += memcmp(out + y * 64 + x * 4, expected, 4) != 0; for (int x = 32; x < 64; x++) bad += out[y * 64 + x] != 0xEE; }
    t("pending clear -> GPU copy, literal pixels and untouched row padding", !bad);
    uint8_t cpu[128];
    uint32_t live = hvk_device()->allocations;
    memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&tex, NULL, &region, cpu);
    t("GPU clear -> CPU ReadTexels, 32 literal pixels", !same(cpu, expected, 32, 0));
    t("ReadTexels releases its staging buffer (the device's live allocations as before)", hvk_device()->allocations == live);
    memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk, NULL, &region, cpu);
    t("GPU copy into a packaged texture -> ReadTexels, 32 literal pixels", !same(cpu, expected, 32, 0));
    memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk2, NULL, &region, cpu);
    t("copy from a packaged texture with a GPU write pending -> ReadTexels, 32 literal pixels", !same(cpu, expected, 32, 0));
    memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk4, NULL, &region, cpu);
    t("copy from a buffer a queued copy writes -> ReadTexels, 32 literal pixels", !same(cpu, expected, 32, 0));
    static const uint8_t expected_srgb[] = {124, 170, 203, 255};   // 0.2, 0.4, 0.6 sRGB-encoded (within 1: the GPU's curve), not the source's 51, 102, 153
    memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk3, NULL, &region, cpu);
    t("format-converting copy from a CPU-written packaged texture -> converted bytes, 32 pixels within 1", !same(cpu, expected_srgb, 32, 1));
    static const uint8_t expected2[] = {204, 153, 102, 255}; memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk6, NULL, &region, cpu);
    t("copy from a packaged texture the GPU cleared -> ReadTexels, 32 literal pixels", !same(cpu, expected2, 32, 0));
    h_nvnTextureFinalize(&pk6); h_nvnTextureFinalize(&pk5); h_nvnTextureFinalize(&pk3); h_nvnTextureFinalize(&pk7); h_nvnTextureFinalize(&pk4); h_nvnTextureFinalize(&pk2);
    h_nvnTextureFinalize(&pk); h_nvnTextureFinalize(&tex); h_nvnCommandBufferFinalize(&cmd);
    hv_pool_free(&pool);
    return hv_finish();
}
