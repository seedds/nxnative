// Handle resolution of the NVN replay (see nvn_resolve.h). The code is nvn_mtl.m's as it was before the move (docs/design/vulkan-backend.md step M2b): the driver segment fill takes the
// buffer and the draw's values as arguments and returns what encode_args reported red, and what reads the Metal backend's own copies of uniform memory, its red line and its pool
// lookup became the hooks of nvn_resolve.h
#include "nvn_resolve.h"
#include "nvn_gpuwrite.h"   // nvn_gw_pend_hits, nvn_gw_pend_read: the inline updates queued in this submission

uint64_t nvn_c2_handle(int stage, unsigned b) {
    if (b >= C2_TEX && b < C2_SEPTEX && !((b - C2_TEX) & 7)) return nvn_tex_slot_handle(S.tex[stage], S.img[stage], (b - C2_TEX) / 8);
    if (b >= C2_SEPTEX && b < C2_SAMP && !((b - C2_SEPTEX) & 7)) return S.tex[stage][NVN_SEPTEX_SLOT0 + (b - C2_SEPTEX) / 8];
    return 0;
}

// Geometry's counter/output addresses are c0+0x410/0x430 (NieR:Automata's MOVC/IADDC pairs); they use the logical NVN stage even when Metal executes a compute kernel.
static int c0_ssbo_base(int stage) { return stage == 0 ? 0x110 : stage == 1 ? 0x510 : stage == 2 ? 0x410 : stage == 5 ? 0x310 : -1; }   // NVN stages 0 vertex, 1 pixel, 2 geometry, 5 compute; hull / domain never seen

// NVN driver constant segments, filled from the current bindings: c0 = storage buffer table (base + 0x10 r: address 64 bits, size 32 bits), c2 = the handle tables (C2_*), the values
// being the NVN handles the game gave at bind time
int nvn_fill_driver_bank(int stage, int bank, uint8_t *d, int dyn, uint32_t base_instance, const uint32_t dispatch_dim[3]) {
    int ok = 1;
    memset(d, 0, DRV_SIZE);
    if (bank == 0) {
        int base = c0_ssbo_base(stage);
        if (base < 0) ok = 0;   // the caller reports it (nvn_mtl.m encode_args: RED_ONCE 45)
        else for (int r = 0; r < 16; r++) { uint64_t a = S.ssbo[stage][r].addr; uint32_t n = (uint32_t)S.ssbo[stage][r].size; memcpy(d + base + 16 * r, &a, 8); memcpy(d + base + 16 * r + 8, &n, 4); }
        // Compute also has the dispatch size at 0x200 (x, y, z) and the uniform buffer table at 0x210 + 16 r (address 64 bits, size 32 bits), ending where the storage buffer table
        // starts (0x310). Measured on Link's Awakening's light culling (light_culling_compute.bfsha): tile index = workgroup y × c0[0x200] + workgroup x, and c0[0x280] is
        // the address of uniform buffer 7 (the light parameters, 144 bytes per light); Breath of the Wild's compute shaders read 0x280 / 0x290 / 0x2d0 of the same table
        // Graphics stages: 0x610, right after the pixel stage's storage buffer table, holds the largest point size, to which GLSLC clamps a shader-written
        // gl_PointSize (Super Mario Odyssey's agl point sprites, alPointSpriteShader: gl_PointSize = min(size attribute, c0[0x610])). It is not SetPointSize: the game
        // sets that to 1 and sizes its sprites itself (the fog splats ask for 3-18 px). Left at 0, every sprite had size 0, so the splats that push the Cap Kingdom
        // fog aside never reached its velocity fields and the fog did not part around Mario. 2047 is Maxwell's point size limit (Metal clamps to its own)
        if (stage != 5) { const float max_point = 2047.0f; memcpy(d + 0x610, &max_point, 4); }
        // Graphics stages: 0x644 holds the draw's base instance. Maxwell's instance ID leaves it out (GL's gl_InstanceID), and GLSLC adds this word where a shader needs the
        // absolute instance (gl_InstanceID + gl_BaseInstance): Pokémon Legends: Z-A's occlusion boxes (a793f436dcabd350, one box per draw at base instance 61, 62, …) pick their
        // transform this way, and 1,550 reads in 793 of its vertex shaders. Left at 0, every box was box 0: all six queries came back empty, and the game, taking its characters for
        // hidden, posed their legs straight back from the hips. 0x640 (read by 58 shaders, added to the vertex ID) stays 0: what the driver puts there is not established
        if (stage != 5) memcpy(d + 0x644, &base_instance, 4);
        if (stage == 5) {
            memcpy(d + 0x200, dispatch_dim, 12);   // an indirect dispatch copies its GPU-written counts over these (nvn_mtl.m g_c0_dim_buf)
            for (int r = 0; r < 16; r++) { uint64_t a = S.ubo[5][r].addr; uint32_t n = (uint32_t)S.ubo[5][r].size; memcpy(d + 0x210 + 16 * r, &a, 8); memcpy(d + 0x210 + 16 * r + 8, &n, 4); }
        }
    } else {
        // A shader with dynamic handle arrays (TEXD / SAMPD, profile key tex_dyn_slots) picks a table slot at run time and ORs a texture word with a sampler word into a handle,
        // whose texture / sampler IDs index the arrays. For it the tables hold slot indices instead of handles (texture slot r → r, and r << 20 when a combined handle carries
        // a sampler; sampler slot r → r << 20), and the arrays hold the bound slots, so the shader's own arithmetic picks the binding (Octopath Traveler 0: separate texture slots 0-10, samplers 0-4)
        const uint64_t *tx = S.tex[stage], *im = S.img[stage];
        for (int r = 0; r < C2_IMG_SLOTS; r++) { uint32_t ih = (uint32_t)im[r]; memcpy(d + C2_IMG + 8 * r, &ih, 4); }
        for (int r = 0; r < C2_TEX_SLOTS; r++) {   // a combined slot past 31 is written when it is bound and the image slot it shares is not (Hades II binds texture slot 34, no image)
            if (r >= 32 && (!tx[r] || (r - 32 < C2_IMG_SLOTS && im[r - 32]))) continue;
            uint32_t v = !dyn ? (uint32_t)tx[r] : !tx[r] ? 0 : (uint32_t)r | (tx[r] >> 20 ? (uint32_t)r << 20 : 0); memcpy(d + C2_TEX + 8 * r, &v, 4); }
        for (int r = 0; r < 128; r++) { uint64_t h = tx[NVN_SEPTEX_SLOT0 + r]; uint32_t v = !dyn ? (uint32_t)h : h ? (uint32_t)r : 0; memcpy(d + C2_SEPTEX + 8 * r, &v, 4); }
        for (int r = 0; r < 128; r++) { uint64_t h = S.samp[stage][r]; uint32_t v = !dyn ? (uint32_t)h : h ? (uint32_t)r << 20 : 0; memcpy(d + C2_SAMP + 8 * r, &v, 4); }
    }
    return ok;
}

void nvn_ubo_read(int stage, int reg, int off, size_t n, void *out) {
    memset(out, 0, n);
    if (stage < 0 || stage >= 6 || reg < 0 || reg >= 16) return;
    const p_bind_buf *u = &S.ubo[stage][reg];
    if (!u->addr || (uint64_t)off + n > u->size) return;
    if (nvn_resolve_hook_ubo_copy(u->addr + off, n, out)) return;   // the backend's copies: an inline update deferred in this pass, or memory and the inline updates queued over it
    if (nvn_gw_pend_hits(u->addr + off, n)) { int kind; if (nvn_gw_pend_read(u->addr + off, n, out, &kind)) return;   // written by an inline update queued in this submission: its data
        nvn_resolve_hook_ubo_stale(stage, reg, off, kind); }
    nvn_resolve_hook_mem_read(u->addr + off, n, out);
}
uint32_t nvn_ubo_handle(int stage, int reg, int off) { uint32_t h; nvn_ubo_read(stage, reg, off, 4, &h); return h; }
int nvn_tid_from_handle(uint32_t h) { int t = (int)(h & 0xfffff); return t ? t + S.tex_base : 0; }
int nvn_sid_from_handle(uint32_t h) { int s = (int)(h >> 20); return s ? s + S.samp_base : 0; }
