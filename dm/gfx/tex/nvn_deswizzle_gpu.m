#import "nvn_deswizzle_gpu.h"
#import "../nvn/host_msl.h"
#include <stdatomic.h>
#include <stdio.h>

static id<MTLComputePipelineState> g_nvn_deswz_pipeline, g_nvn_deswz_direct;
// The last 64 deswizzle jobs encoded (the GPU's upload batches fault or hang with no shader to name: PRAGMATA's tower ring), dumped at the first upload command buffer
// error (nvn_deswz_ring_dump). And bounds: a job whose source or destination range would leave its buffer, or whose grid is out of all proportion, is refused before the GPU
// sees it ([RED], return -2: the caller skips that upload), and the kernels check every access again (P / D src_len, dst_len)
typedef struct { uint32_t w, h, dpt, levels, fmt, target, level, layer, gx, gy, gz, kind; uint64_t src_gpu, src_off, src_len, base, dst_len, so_max, do_max; } dz_job;
static dz_job g_dz[64]; static _Atomic uint32_t g_dz_n; static _Atomic uint64_t g_dz_refused;
static void dz_note(const dz_job *j) { uint32_t i = atomic_fetch_add(&g_dz_n, 1) % 64; g_dz[i] = *j; }
void nvn_deswz_ring_dump(void) {
    uint32_t n = atomic_load(&g_dz_n), m = n < 64 ? n : 64;
    fprintf(stderr, "[dm] [nvn-mtl] [RED] deswizzle: the last %u of %u jobs (oldest first), %llu refused by the bounds check\n", m, n, (unsigned long long)atomic_load(&g_dz_refused));
    for (uint32_t k = 0; k < m; k++) { const dz_job *j = &g_dz[(n - m + k) % 64];
        fprintf(stderr, "[dm] [nvn-mtl] [deswz] %s %ux%ux%u levels %u format %#x target %u level %u layer %u grid %ux%ux%u src gpu %#llx+%#llx len %#llx base %#llx max read %#llx dst len %#llx max write %#llx\n",
                j->kind ? "direct" : "staged", j->w, j->h, j->dpt, j->levels, j->fmt, j->target, j->level, j->layer, j->gx, j->gy, j->gz, (unsigned long long)j->src_gpu, (unsigned long long)j->src_off,
                (unsigned long long)j->src_len, (unsigned long long)j->base, (unsigned long long)j->so_max, (unsigned long long)j->dst_len, (unsigned long long)j->do_max); }
}
static int dz_refuse(const dz_job *j, const char *why) {
    static _Atomic int lines; atomic_fetch_add(&g_dz_refused, 1);
    if (atomic_fetch_add(&lines, 1) < 32) fprintf(stderr, "[dm] [nvn-mtl] [RED] deswizzle job refused (%s): %ux%ux%u levels %u format %#x target %u level %u layer %u grid %ux%ux%u, source +%#llx..%#llx of %#llx, destination to %#llx of %#llx\n", why,
        j->w, j->h, j->dpt, j->levels, j->fmt, j->target, j->level, j->layer, j->gx, j->gy, j->gz, (unsigned long long)j->src_off, (unsigned long long)j->so_max, (unsigned long long)j->src_len, (unsigned long long)j->do_max, (unsigned long long)j->dst_len);
    return -2;
}

// The kernels deswz and deswz_direct: dm/gfx/msl/deswz.metal

int nvn_deswz_gpu_init(id<MTLDevice> dev) {
    if (g_nvn_deswz_pipeline) return 0;
    if (!dev) return -1;
    NSError *error = nil;
    id<MTLLibrary> library = host_msl_lib(dev, nil, "deswz", &error);   // compiled when the host is built (dm/gfx/msl/deswz.metal)
    if (!library) return -1;
    id<MTLFunction> function = [library newFunctionWithName:@"deswz"];
    if (!function) return -1;
    g_nvn_deswz_pipeline = [dev newComputePipelineStateWithFunction:function error:&error];
    id<MTLFunction> direct = [library newFunctionWithName:@"deswz_direct"];
    if (direct) g_nvn_deswz_direct = [dev newComputePipelineStateWithFunction:direct error:&error];
    return g_nvn_deswz_pipeline ? 0 : -1;
}

int nvn_deswz_gpu_direct_supported(const nvn_texdesc *d) {
    // Prefer retained compression for large images. This path targets many
    // small array layers where per-face dispatch/copy overhead dominates.
    return g_nvn_deswz_direct && d && d->format == 0x29 && (d->target == 4 || d->target == 9) &&
        !(d->flags & 0x38) && d->samples <= 1 && d->width && d->height && d->width <= 64 && d->height <= 64 && d->depth >= 32;
}
int nvn_deswz_gpu_direct_m4(id<MTL4ComputeCommandEncoder> enc, id<MTL4ArgumentTable> table,
                            id<MTLBuffer> params, size_t params_off, id<MTLBuffer> src, uint64_t src_off,
                            const nvn_texdesc *d, unsigned level, id<MTLTexture> view) {
    if (!nvn_deswz_gpu_direct_supported(d) || !enc || !table || !params || !src || !view ||
        view.pixelFormat != MTLPixelFormatRGBA16Uint || view.textureType != MTLTextureType2DArray ||
        level >= view.mipmapLevelCount || (src_off & 7) || params_off + 64 > params.length) return -1;
    nvn_texlayout layout; nvn_tex_layout(d, &layout);
    uint32_t row,rows,slices,bpb;uint64_t base;unsigned th,td;
    if (nvn_deswizzle_geom(d,level,0,&row,&rows,&slices,&base,&th,&td,&bpb) || bpb != 8 || slices != 1 || td ||
        layout.size > src.length || src_off > src.length-layout.size || layout.layers != view.arrayLength) return -1;
    struct {uint64_t src_off,base,layer_stride;uint32_t width,height,layers,level,th,gobs_x,gob2;uint64_t src_len;} p = {
        src_off,base,layout.layer_size,MAX(d->width>>level,1u),MAX(d->height>>level,1u),layout.layers,level,th,MAX(nvn_tex_level_gobs(d,level),(row+63u)/64u),(uint32_t)nvn_gob_switch2,src.length};
    dz_job j = {d->width, d->height, d->depth, d->levels, d->format, (uint32_t)d->target, level, 0, (p.width+7)/8, (p.height+3)/4, p.layers, 1, src.gpuAddress, src_off, src.length, base, 0,
                src_off + base + (uint64_t)(p.layers ? p.layers - 1 : 0) * layout.layer_size + layout.layer_size, 0};
    dz_note(&j);
    if (j.so_max > src.length) return dz_refuse(&j, "source past its buffer");
    if ((uint64_t)j.gx * j.gy * j.gz > (1u << 22)) return dz_refuse(&j, "grid");
    memcpy((uint8_t*)params.contents+params_off,&p,sizeof p);
    [enc setArgumentTable:table];[enc setComputePipelineState:g_nvn_deswz_direct];
    [table setAddress:src.gpuAddress atIndex:0];[table setAddress:params.gpuAddress+params_off atIndex:2];[table setTexture:view.gpuResourceID atIndex:0];
    [enc dispatchThreadgroups:MTLSizeMake((p.width+7)/8,(p.height+3)/4,p.layers) threadsPerThreadgroup:MTLSizeMake(8,4,1)];
    return 0;
}

// The staged deswizzle's bounds (deswz.metal's addressing): the largest source and destination byte its grid reaches, against the buffers. 0: fine, -2: refused (logged)
int nvn_deswz_check(const nvn_texdesc *d, unsigned level, unsigned layer, uint64_t src_gpu, uint64_t src_off, uint64_t src_len, uint64_t dst_off, uint64_t dst_len, size_t dst_pitch) {
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) return -1;
    uint32_t gobs_x = MAX(nvn_tex_level_gobs(d, level), (row_bytes + 63u) >> 6);
    uint32_t bh = 1u << th, bd = 1u << td, gx = (row_bytes + 63u) >> 6, gy = (rows + 7u) >> 3, gz = slices, blocks_y = (rows + 8u * bh - 1u) >> (3u + th);
    uint64_t block = 512ull * bh * bd, gib = ((uint64_t)(bd - 1u) << th) + (bh - 1u);
    uint64_t so_max = src_off + base + ((uint64_t)((gz ? gz - 1 : 0) >> td) * gobs_x * blocks_y + (uint64_t)((gy ? gy - 1 : 0) >> th) * gobs_x + (gx ? gx - 1 : 0)) * block + gib * 512ull + 31ull * 16ull + 16ull;
    uint64_t do_max = dst_off + ((uint64_t)(gz ? gz - 1 : 0) * rows + (rows ? rows - 1 : 0)) * dst_pitch + row_bytes;
    dz_job j = {d->width, d->height, d->depth, d->levels, d->format, (uint32_t)d->target, level, layer, gx, gy, gz, 0, src_gpu, src_off, src_len, base, dst_len, so_max, do_max};
    dz_note(&j);
    if (!row_bytes || !rows || !slices || (uint64_t)gx * gy * gz > (1u << 22) || th > 5 || td > 5) return dz_refuse(&j, "grid or block size");
    if (so_max > src_len) return dz_refuse(&j, "source past its buffer");
    if (do_max > dst_len) return dz_refuse(&j, "destination past its buffer");
    return 0;
}

int nvn_deswz_gpu_encode(id<MTLComputeCommandEncoder> enc, id<MTLBuffer> src, uint64_t src_off,
                         const nvn_texdesc *d, unsigned level, unsigned layer,
                         id<MTLBuffer> dst, uint64_t dst_off, size_t dst_pitch) {
    if (!g_nvn_deswz_pipeline || !enc || !src || !dst || !d) return -1;
    uint32_t row_bytes, rows, slices, bpb;
    uint64_t base;
    unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb) || dst_pitch < row_bytes) return -1;
    struct { uint64_t src_off, dst_off, base, pitch; uint32_t row_bytes, rows, slices, th, td, gobs_x, gob2; uint64_t src_len, dst_len; } p = {
        src_off, dst_off, base, dst_pitch, row_bytes, rows, slices, th, td, MAX(nvn_tex_level_gobs(d, level), (row_bytes + 63u) >> 6), (uint32_t)nvn_gob_switch2, src.length, dst.length   // GOB columns as laid out (nvn_tex_level_gobs)
    };
    { int rc = nvn_deswz_check(d, level, layer, src.gpuAddress, src_off, src.length, dst_off, dst.length, dst_pitch); if (rc) return rc; }
    [enc setComputePipelineState:g_nvn_deswz_pipeline];
    [enc setBuffer:src offset:0 atIndex:0];
    [enc setBuffer:dst offset:0 atIndex:1];
    [enc setBytes:&p length:sizeof p atIndex:2];
    NSUInteger gobs_x = (row_bytes + 63u) >> 6;
    NSUInteger gobs_y = (rows + 7u) >> 3;
    [enc dispatchThreadgroups:MTLSizeMake(gobs_x, gobs_y, slices) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    return 0;
}

int nvn_deswz_gpu_encode_m4(id<MTL4ComputeCommandEncoder> enc, id<MTL4ArgumentTable> table,
                            id<MTLBuffer> params, size_t params_off, id<MTLBuffer> src, uint64_t src_off,
                            const nvn_texdesc *d, unsigned level, unsigned layer,
                            id<MTLBuffer> dst, uint64_t dst_off, size_t dst_pitch) {
    if (!g_nvn_deswz_pipeline || !enc || !table || !params || !src || !dst || !d) return -1;
    uint32_t row_bytes, rows, slices, bpb;
    uint64_t base;
    unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb) || dst_pitch < row_bytes) return -1;
    struct { uint64_t src_off, dst_off, base, pitch; uint32_t row_bytes, rows, slices, th, td, gobs_x, gob2; uint64_t src_len, dst_len; } p = {
        src_off, dst_off, base, dst_pitch, row_bytes, rows, slices, th, td, MAX(nvn_tex_level_gobs(d, level), (row_bytes + 63u) >> 6), (uint32_t)nvn_gob_switch2, src.length, dst.length   // GOB columns as laid out (nvn_tex_level_gobs)
    };
    { int rc = nvn_deswz_check(d, level, layer, src.gpuAddress, src_off, src.length, dst_off, dst.length, dst_pitch); if (rc) return rc; }
    memcpy((uint8_t *)params.contents + params_off, &p, sizeof p);
    [enc setArgumentTable:table];
    [enc setComputePipelineState:g_nvn_deswz_pipeline];
    [table setAddress:src.gpuAddress atIndex:0];
    [table setAddress:dst.gpuAddress atIndex:1];
    [table setAddress:params.gpuAddress + params_off atIndex:2];
    NSUInteger gobs_x = (row_bytes + 63u) >> 6;
    NSUInteger gobs_y = (rows + 7u) >> 3;
    [enc dispatchThreadgroups:MTLSizeMake(gobs_x, gobs_y, slices) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    return 0;
}
