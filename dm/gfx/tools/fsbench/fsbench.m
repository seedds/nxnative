// fsbench: an offline benchmark of a translated pixel shader and hand-made variants of it.
//
//   fsbench stats <lib.metallib> [fn] [colour MTLPixelFormat]         Apple's compiler statistics of the fragment stage (registers, spills, instruction mix)
//   fsbench cstats <lib.metallib> [fn]                                 a compute kernel's statistics, threadgroup memory and threads a threadgroup
//   fsbench crun <scene.json> <out dir> <lib>...                       a compute kernel: written resources against the first library (in place, and as a multiset of words for append order), time a dispatch
//   fsbench run <scene.json> <out dir> <lib>...                        GPU time of each library's fragment function on one full-screen draw, and its output against the first
//
// The scene (written by fsbench.py from a pass dump and its uniform dumps) names the target size and formats, the shader's interpolants, and each argument of its
// argument buffer (spvDescriptorSet0 at buffer 0): a file or a fill for buffers, a shape and format for textures (random bytes), a state for samplers, and the pools
// ({uint n; ulong seen; T e[n]}) of bindless textures and samplers. The vertex function is generated: a full-screen triangle whose interpolants are linear in the
// screen position (scene "vary"), so texture coordinates sweep the textures as a real surface does.
// Timing: one draw a render pass (a pass with several opaque full-screen draws would leave all but the last to hidden-surface removal), P passes a command buffer, the
// libraries and a null shader (constant output: the pass and raster cost) in turn, one command buffer each, R rounds; a library's time is the median over the rounds of
// (GPU end − GPU start) / P, and "net" is that minus the null shader's, the pixel shader's own cost: what a knockout in the game takes away.
// The statistics come from a pipeline option the public headers do not name (1 << 18, MTLPipelineOptionPerformanceStatistics in Xcode's tools); the reflection's
// performanceStatistics then carries what Xcode's pipeline statistics show.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <CoreGraphics/CoreGraphics.h>
#import <objc/message.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define OPT_STATS ((MTLPipelineOption)(1u << 18))
static id<MTLDevice> D;

static NSString *vary_struct(NSString *msl) {   // the interpolants: struct main0_in of the shader's source
    NSRange r = [msl rangeOfString:@"struct main0_in\n{"];
    if (r.location == NSNotFound) return @"struct vout { float4 pos [[position]]; };";
    NSRange e = [msl rangeOfString:@"};" options:0 range:NSMakeRange(r.location, msl.length - r.location)];
    NSString *s = [msl substringWithRange:NSMakeRange(r.location, e.location + 2 - r.location)];
    s = [s stringByReplacingOccurrencesOfString:@"main0_in" withString:@"vout"];
    return [s stringByReplacingOccurrencesOfString:@"{" withString:@"{\n    float4 pos [[position]];"];
}
// A full-screen triangle; each interpolant m = o + u * du + v * dv (u, v in 0..1 across the target), from scene "vary" {name: [o4, du4, dv4]}, default u * 8, v * 8, u + v, 1
static id<MTLFunction> vertex_fn(NSString *msl, NSDictionary *vary) {
    NSString *st = vary_struct(msl);
    NSMutableString *body = [NSMutableString string];
    NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:@"(float[234]?|int[234]?|uint[234]?) (\\w+) \\[\\[user\\(" options:0 error:nil];
    for (NSTextCheckingResult *m in [re matchesInString:st options:0 range:NSMakeRange(0, st.length)]) {
        NSString *ty = [st substringWithRange:[m rangeAtIndex:1]], *nm = [st substringWithRange:[m rangeAtIndex:2]];
        NSArray *v = vary[nm];
        NSString *o = v ? [NSString stringWithFormat:@"float4(%@,%@,%@,%@)", v[0][0], v[0][1], v[0][2], v[0][3]] : @"float4(0,0,0,1)";
        NSString *du = v ? [NSString stringWithFormat:@"float4(%@,%@,%@,%@)", v[1][0], v[1][1], v[1][2], v[1][3]] : @"float4(8,0,1,0)";
        NSString *dv = v ? [NSString stringWithFormat:@"float4(%@,%@,%@,%@)", v[2][0], v[2][1], v[2][2], v[2][3]] : @"float4(0,8,1,0)";
        NSString *n = [ty hasPrefix:@"float"] ? (ty.length > 5 ? [ty substringFromIndex:5] : @"1") : [ty substringFromIndex:[ty hasPrefix:@"uint"] ? 4 : 3];
        NSString *sw = [@"xyzw" substringToIndex:n.length ? MAX(1, n.intValue) : 1];
        [body appendFormat:@"    o.%@ = %@((%@ + uv.x * %@ + uv.y * %@).%@);\n", nm, ty, o, du, dv, sw];
    }
    NSString *src = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n%@\nvertex vout vmain(uint vid [[vertex_id]]) {\n    vout o;\n"
        "    float2 p = float2((vid << 1) & 2, vid & 2);\n    o.pos = float4(p * 2 - 1, 0.5, 1);\n    float2 uv = p;   // 0..1 across the target\n%@    return o;\n}\n", st, body];
    NSError *e = nil;
    id<MTLLibrary> L = [D newLibraryWithSource:src options:nil error:&e];
    if (!L) { fprintf(stderr, "vertex function: %s\n%s\n", e.description.UTF8String, src.UTF8String); exit(1); }
    return [L newFunctionWithName:@"vmain"];
}
static NSString *g_null_src = @"#include <metal_stdlib>\nusing namespace metal;\n%@\nstruct o_t { float4 c [[color(0)]]; };\nfragment o_t fnull(vout in [[stage_in]]) { return o_t{float4(0.25, 0.5, 0.75, 1)}; }\n";

static id<MTLRenderPipelineState> pipeline(id<MTLFunction> vf, id<MTLFunction> ff, NSArray *fmts, NSDictionary **stats) {
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vf; pd.fragmentFunction = ff;
    for (NSUInteger i = 0; i < fmts.count; i++) pd.colorAttachments[i].pixelFormat = (MTLPixelFormat)[fmts[i] integerValue];
    NSError *e = nil; MTLAutoreleasedRenderPipelineReflection refl = nil;
    id<MTLRenderPipelineState> p = [D newRenderPipelineStateWithDescriptor:pd options:OPT_STATS reflection:&refl error:&e];
    if (!p) { fprintf(stderr, "pipeline: %s\n", e.description.UTF8String); exit(1); }
    if (stats) *stats = [(id)refl respondsToSelector:@selector(performanceStatistics)] ? ((id(*)(id, SEL))objc_msgSend)((id)refl, @selector(performanceStatistics))[@"Fragment Shader"] : nil;
    return p;
}
static const char *STAT_KEYS[] = {"Temporary register count", "Spilled bytes", "Instruction count", "ALU instruction count", "FP16 instruction count", "FP32 instruction count",
    "INT16 instruction count", "INT32 instruction count", "Texture reads instruction count", "Device load instruction count", "Device atomic instruction count", "Branch instruction count",
    "Wait instruction count", "Uniform register count", "Constant calculation temporary register count", "Threadgroup memory", 0};
static const char *STAT_SHORT[] = {"regs", "spill", "instr", "alu", "f16", "f32", "i16", "i32", "tex", "ld", "atom", "br", "wait", "ureg", "cregs", "tgmem", 0};
static NSString *stat_line(NSDictionary *s) {
    NSMutableString *o = [NSMutableString string];
    for (int i = 0; STAT_KEYS[i]; i++) [o appendFormat:@"%s=%@ ", STAT_SHORT[i], s[@(STAT_KEYS[i])] ?: @"?"];
    return o;
}

// The statistics dictionary inside a pipeline's performanceStatistics: a render pipeline nests it under the stage's name; a compute pipeline's nesting differs, so search for the
// dictionary that holds the first statistic. FSBENCH_STATSKEYS=1 prints the top level's shape (for a new layout).
static NSDictionary *find_stats(id o, int depth) {
    if (depth == 0 && getenv("FSBENCH_STATSKEYS")) fprintf(stderr, "performanceStatistics: %s\n", [[o description] substringToIndex:MIN((NSUInteger)2000, [[o description] length])].UTF8String);
    if ([o isKindOfClass:NSDictionary.class]) {
        if (((NSDictionary *)o)[@(STAT_KEYS[0])]) return o;
        for (id k in (NSDictionary *)o) { NSDictionary *r = find_stats(((NSDictionary *)o)[k], depth + 1); if (r) return r; }
    } else if ([o isKindOfClass:NSArray.class]) for (id x in (NSArray *)o) { NSDictionary *r = find_stats(x, depth + 1); if (r) return r; }
    return nil;
}

// Every statistics dictionary in a pipeline's performanceStatistics, with the path of keys that leads to it (a mesh pipeline: the object, mesh and fragment stages).
static void print_stages(const char *lib, id o, NSString *path) {
    if (![o isKindOfClass:NSDictionary.class]) return;
    if (((NSDictionary *)o)[@(STAT_KEYS[0])]) { printf("%s\t%s\t%s\n", lib, path.length ? path.UTF8String : "(top)", stat_line(o).UTF8String); return; }
    for (NSString *k in (NSDictionary *)o) print_stages(lib, ((NSDictionary *)o)[k], path.length ? [NSString stringWithFormat:@"%@/%@", path, k] : k);
}

// ---------- scene resources ----------
static NSMutableArray *g_res;   // every resource the argument buffers reach (useResource)
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }
static id<MTLBuffer> buffer_of(NSDictionary *a) {
    NSUInteger size = [a[@"size"] unsignedIntegerValue];
    NSData *file = a[@"file"] ? [NSData dataWithContentsOfFile:a[@"file"]] : nil;
    if (file && !size) size = file.length;
    size = MAX(size, (NSUInteger)16);
    id<MTLBuffer> b = [D newBufferWithLength:size options:MTLResourceStorageModeShared];
    memset(b.contents, 0, size);
    NSString *fill = a[@"fill"];
    if ([fill isEqualToString:@"random"]) for (NSUInteger i = 0; i < size / 4; i++) ((uint32_t *)b.contents)[i] = rnd();
    for (NSArray *w in a[@"words"] ?: @[]) {   // [first word, count, value (uint), step]: value + k * step for k < count
        uint32_t f = [w[0] unsignedIntValue], n = [w[1] unsignedIntValue], v = [w[2] unsignedIntValue], st = w.count > 3 ? [w[3] unsignedIntValue] : 0;
        for (uint32_t k = 0; k < n && (f + k) * 4 + 4 <= size; k++) ((uint32_t *)b.contents)[f + k] = v + k * st;
    }
    for (NSArray *w in a[@"floats"] ?: @[]) {   // [first word, count, value (float), step]
        uint32_t f = [w[0] unsignedIntValue], n = [w[1] unsignedIntValue]; float v = [w[2] floatValue], st = w.count > 3 ? [w[3] floatValue] : 0;
        for (uint32_t k = 0; k < n && (f + k) * 4 + 4 <= size; k++) ((float *)b.contents)[f + k] = v + k * st;
    }
    if (file) memcpy(b.contents, file.bytes, MIN(file.length, size));
    if (a[@"hex"]) { const char *h = [a[@"hex"] UTF8String]; for (NSUInteger i = 0; h[2 * i] && h[2 * i + 1] && i < size; i++) { unsigned x; sscanf(h + 2 * i, "%2x", &x); ((uint8_t *)b.contents)[i] = x; } }
    [g_res addObject:b];
    return b;
}
static id<MTLTexture> texture_of(NSDictionary *a) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    NSString *t = a[@"type"] ?: @"2d";
    td.textureType = [t isEqualToString:@"cube"] ? MTLTextureTypeCube : [t isEqualToString:@"3d"] ? MTLTextureType3D : [t isEqualToString:@"2da"] ? MTLTextureType2DArray : MTLTextureType2D;
    td.pixelFormat = (MTLPixelFormat)[a[@"fmt"] integerValue];
    td.width = [a[@"w"] unsignedIntegerValue]; td.height = [a[@"h"] unsignedIntegerValue];
    td.depth = td.textureType == MTLTextureType3D ? [a[@"d"] unsignedIntegerValue] : 1;
    td.arrayLength = td.textureType == MTLTextureType2DArray ? MAX(1u, [a[@"d"] unsignedIntValue]) : 1;
    NSUInteger maxm = (NSUInteger)floor(log2((double)MAX(td.width, td.height))) + 1;
    td.mipmapLevelCount = MIN(MAX(1u, [a[@"mips"] unsignedIntValue]), maxm);
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead | ([a[@"write"] boolValue] ? MTLTextureUsageShaderWrite : 0);   // write: a storage image the kernel writes
    id<MTLTexture> x = [D newTextureWithDescriptor:td];
    if (!x) { fprintf(stderr, "texture %s: format %ld %lux%lu not made\n", [a description].UTF8String, (long)td.pixelFormat, td.width, td.height); exit(1); }
    // random bytes, a block-compressed format's blocks too (the decode cost does not depend on the data); a float format gets finite values in 0..1: random bits in a
    // float format hold Inf and NaN (an exponent of all ones), which spread through the shader (raw random bits made 73 % of an RG11B10Float scene's pixels NaN)
    MTLPixelFormat pf = td.pixelFormat;
    BOOL isf = pf == MTLPixelFormatRGBA32Float || pf == MTLPixelFormatR32Float || pf == MTLPixelFormatRG32Float;
    BOOL ish = pf == MTLPixelFormatRGBA16Float || pf == MTLPixelFormatRG16Float || pf == MTLPixelFormatR16Float;
    NSUInteger faces = td.textureType == MTLTextureTypeCube ? 6 : td.arrayLength;
    for (NSUInteger lv = 0; lv < td.mipmapLevelCount; lv++) {
        NSUInteger w = MAX(1u, td.width >> lv), h = MAX(1u, td.height >> lv), d = MAX(1u, td.depth >> lv);
        NSUInteger bw = 1, bh = 1, bb = 0;
        switch (td.pixelFormat) {
            case MTLPixelFormatBC1_RGBA: case MTLPixelFormatBC1_RGBA_sRGB: case MTLPixelFormatBC4_RUnorm: case MTLPixelFormatBC4_RSnorm: bw = bh = 4; bb = 8; break;
            case MTLPixelFormatBC2_RGBA: case MTLPixelFormatBC3_RGBA: case MTLPixelFormatBC3_RGBA_sRGB: case MTLPixelFormatBC5_RGUnorm: case MTLPixelFormatBC5_RGSnorm:
            case MTLPixelFormatBC6H_RGBFloat: case MTLPixelFormatBC6H_RGBUfloat: case MTLPixelFormatBC7_RGBAUnorm: case MTLPixelFormatBC7_RGBAUnorm_sRGB: bw = bh = 4; bb = 16; break;
            case MTLPixelFormatASTC_4x4_LDR: case MTLPixelFormatASTC_4x4_sRGB: bw = 4; bh = 4; bb = 16; break;
            case MTLPixelFormatASTC_5x5_LDR: case MTLPixelFormatASTC_5x5_sRGB: bw = 5; bh = 5; bb = 16; break;
            case MTLPixelFormatASTC_6x6_LDR: case MTLPixelFormatASTC_6x6_sRGB: bw = 6; bh = 6; bb = 16; break;
            case MTLPixelFormatASTC_8x8_LDR: case MTLPixelFormatASTC_8x8_sRGB: bw = 8; bh = 8; bb = 16; break;
            case MTLPixelFormatRGBA32Float: bb = 16; break;
            case MTLPixelFormatRGBA16Float: case MTLPixelFormatRG32Float: bb = 8; break;
            case MTLPixelFormatR8Unorm: bb = 1; break;
            case MTLPixelFormatRG8Unorm: case MTLPixelFormatR16Float: bb = 2; break;
            default: bb = 4;
        }
        NSUInteger bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh, row = bx * bb, img = row * by;
        NSMutableData *m = [NSMutableData dataWithLength:img * d];
        uint32_t *p = m.mutableBytes;
        for (NSUInteger i = 0; i < img * d / 4; i++) p[i] = rnd();
        if (isf) { float *f = m.mutableBytes; for (NSUInteger i = 0; i < img * d / 4; i++) f[i] = (rnd() & 0xffff) / 65535.0f; }
        if (ish) { uint16_t *q = m.mutableBytes; for (NSUInteger i = 0; i < img * d / 2; i++) q[i] = (uint16_t)((rnd() % 15) << 10 | (rnd() & 0x3ff)); }   // halves below 1
        if (pf == MTLPixelFormatRG11B10Float) for (NSUInteger i = 0; i < img * d / 4; i++) p[i] &= ~(1u << 10 | 1u << 21 | 1u << 31);   // each exponent's top bit off: finite, below 2
        if ((td.pixelFormat >= MTLPixelFormatASTC_4x4_sRGB && td.pixelFormat <= MTLPixelFormatASTC_12x12_LDR)) {   // a valid block: void-extent LDR constant colour, random colour
            for (NSUInteger i = 0; i < img * d / 16; i++) { uint8_t *b = (uint8_t *)m.mutableBytes + 16 * i; uint8_t c[8]; for (int k = 0; k < 8; k++) c[k] = rnd(); memset(b, 0xff, 8); b[0] = 0xfc; b[1] = 0xfd; memcpy(b + 8, c, 8); }
        }
        for (NSUInteger s = 0; s < faces; s++)
            [x replaceRegion:MTLRegionMake3D(0, 0, 0, w, h, d) mipmapLevel:lv slice:s withBytes:m.bytes bytesPerRow:row bytesPerImage:td.textureType == MTLTextureType3D ? img : 0];
    }
    [g_res addObject:x];
    return x;
}
static id<MTLSamplerState> sampler_of(NSDictionary *a) {
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = [a[@"min"] intValue] ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.magFilter = [a[@"mag"] intValue] ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    int mip = [a[@"mip"] intValue]; sd.mipFilter = mip == 2 ? MTLSamplerMipFilterLinear : mip == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterNotMipmapped;
    NSArray *w = a[@"wrap"] ?: @[@0, @0, @0];
    MTLSamplerAddressMode am[] = {MTLSamplerAddressModeRepeat, MTLSamplerAddressModeMirrorRepeat, MTLSamplerAddressModeClampToEdge, MTLSamplerAddressModeClampToBorderColor, MTLSamplerAddressModeMirrorClampToEdge};
    sd.sAddressMode = am[[w[0] intValue]]; sd.tAddressMode = am[[w[1] intValue]]; sd.rAddressMode = am[[w[2] intValue]];
    sd.borderColor = MTLSamplerBorderColorOpaqueWhite;
    if (a[@"cmp"]) sd.compareFunction = (MTLCompareFunction)[a[@"cmp"] intValue];   // a depth-compare sampler (the shader's sample_compare / gather_compare)
    sd.maxAnisotropy = MAX(1, [a[@"aniso"] intValue]);
    sd.lodMinClamp = [a[@"lodmin"] floatValue]; sd.lodMaxClamp = a[@"lodmax"] ? [a[@"lodmax"] floatValue] : FLT_MAX;
    sd.supportArgumentBuffers = YES;
    return [D newSamplerStateWithDescriptor:sd];
}
// A pool: {uint n; ulong seen; T e[n]} with every entry the same texture or sampler; seen points at a bitmask the shader's dm_pool_mark sets
static id<MTLBuffer> pool_of(NSDictionary *a, BOOL tex) {
    uint32_t n = MAX(1u, [a[@"n"] unsignedIntValue]);
    id<MTLBuffer> b = [D newBufferWithLength:16 + 8 * n options:MTLResourceStorageModeShared];
    id<MTLBuffer> seen = [D newBufferWithLength:(1u << 20) / 8 options:MTLResourceStorageModeShared]; memset(seen.contents, 0, seen.length);
    *(uint32_t *)b.contents = n; *(uint64_t *)((char *)b.contents + 8) = seen.gpuAddress;
    if (tex) { id<MTLTexture> t = texture_of(a[@"tex"]); for (uint32_t i = 0; i < n; i++) ((MTLResourceID *)((char *)b.contents + 16))[i] = t.gpuResourceID; }
    else { id<MTLSamplerState> s = sampler_of(a[@"samp"]); for (uint32_t i = 0; i < n; i++) ((MTLResourceID *)((char *)b.contents + 16))[i] = s.gpuResourceID; }
    [g_res addObject:b]; [g_res addObject:seen];
    return b;
}

// ---------- output: read back, compare, PNG ----------
static float *read_rgba32(id<MTLTexture> t) {
    float *p = malloc(t.width * t.height * 16);
    [t getBytes:p bytesPerRow:t.width * 16 fromRegion:MTLRegionMake2D(0, 0, t.width, t.height) mipmapLevel:0];
    return p;
}
static void write_png(NSString *path, const float *rgba, NSUInteger w, NSUInteger h, int diff_of_ref, const float *ref) {
    uint8_t *px = malloc(w * h * 4);
    for (NSUInteger i = 0; i < w * h; i++) for (int c = 0; c < 4; c++) {
        float v = rgba[4 * i + c];
        if (diff_of_ref) v = c == 3 ? 1 : fminf(1, fabsf(v - ref[4 * i + c]) * 64);   // ×64 so a small difference shows
        else v = c == 3 ? 1 : v / (1 + v);   // Reinhard, for an HDR target
        px[4 * i + c] = (uint8_t)(fmaxf(0, fminf(1, isnan(v) ? 1 : v)) * 255 + 0.5f);
    }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef cg = CGBitmapContextCreate(px, w, h, 8, w * 4, cs, kCGImageAlphaPremultipliedLast);
    CGImageRef im = CGBitmapContextCreateImage(cg);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], CFSTR("public.png"), 1, NULL);
    CGImageDestinationAddImage(dst, im, NULL); CGImageDestinationFinalize(dst);
    CFRelease(dst); CGImageRelease(im); CGContextRelease(cg); CGColorSpaceRelease(cs); free(px);
}

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv) { @autoreleasepool {
    D = MTLCreateSystemDefaultDevice(); g_res = [NSMutableArray array];
    if (argc >= 3 && !strcmp(argv[1], "stats")) {
        NSError *e = nil;
        id<MTLLibrary> L = [D newLibraryWithURL:[NSURL fileURLWithPath:@(argv[2])] error:&e]; if (!L) { fprintf(stderr, "%s\n", e.description.UTF8String); return 1; }
        id<MTLFunction> F = [L newFunctionWithName:argc > 3 ? @(argv[3]) : @"main0" constantValues:[MTLFunctionConstantValues new] error:nil];   // specialized with none set: a library with function constants (dm_border_off) refuses a plain function
        NSString *msl = [NSString stringWithContentsOfFile:[[@(argv[2]) stringByDeletingPathExtension] stringByAppendingPathExtension:@"metal"] encoding:NSUTF8StringEncoding error:nil] ?: @"";
        NSDictionary *st = nil; pipeline(vertex_fn(msl, @{}), F, @[@(argc > 4 ? atoi(argv[4]) : MTLPixelFormatRG11B10Float)], &st);
        printf("%s\t%s\n", argv[2], stat_line(st).UTF8String);
        if (getenv("FSBENCH_REMARKS")) printf("%s\n", [st[@"Remarks"] UTF8String]);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "cstats")) {   // cstats <lib> [fn]: a compute kernel's statistics, its threadgroup memory and the threads Metal allows a threadgroup at that register use
        NSError *e = nil; id<MTLLibrary> L = [D newLibraryWithURL:[NSURL fileURLWithPath:@(argv[2])] error:&e]; if (!L) { fprintf(stderr, "%s\n", e.description.UTF8String); return 1; }
        NSString *fn = argc > 3 ? @(argv[3]) : @"main0";
        id<MTLFunction> F = [L newFunctionWithName:fn constantValues:[MTLFunctionConstantValues new] error:&e]; if (!F) { fprintf(stderr, "%s: %s\n", argv[2], e.description.UTF8String); return 1; }
        MTLAutoreleasedComputePipelineReflection refl = nil;
        id<MTLComputePipelineState> p = [D newComputePipelineStateWithFunction:F options:OPT_STATS reflection:&refl error:&e];
        if (!p) { fprintf(stderr, "%s: %s\n", argv[2], e.description.UTF8String); return 1; }
        NSDictionary *st = [(id)refl respondsToSelector:@selector(performanceStatistics)] ? find_stats(((id(*)(id, SEL))objc_msgSend)((id)refl, @selector(performanceStatistics)), 0) : nil;
        printf("%s\t%sthreads=%lu width=%lu tgstatic=%lu\n", argv[2], stat_line(st).UTF8String, (unsigned long)p.maxTotalThreadsPerThreadgroup, (unsigned long)p.threadExecutionWidth, (unsigned long)p.staticThreadgroupMemoryLength);
        if (getenv("FSBENCH_REMARKS")) printf("%s\n", [st[@"Remarks"] description].UTF8String);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "mstats")) {   // mstats <mesh lib> <fragment lib> [object lib]: the mesh pipeline's statistics, each stage (entry main0 in each)
        NSError *e = nil; id<MTLFunction> f[3] = {nil, nil, nil};
        for (int i = 0; i < 3 && 2 + i < argc; i++) {
            id<MTLLibrary> L = [D newLibraryWithURL:[NSURL fileURLWithPath:@(argv[2 + i])] error:&e]; if (!L) { fprintf(stderr, "%s: %s\n", argv[2 + i], e.description.UTF8String); return 1; }
            NSError *fe = nil; f[i] = [L newFunctionWithName:@"main0" constantValues:[MTLFunctionConstantValues new] error:&fe]; if (!f[i]) { fprintf(stderr, "%s: no main0 (%s)\n", argv[2 + i], [L.functionNames componentsJoinedByString:@" "].UTF8String); return 1; }
        }
        MTLMeshRenderPipelineDescriptor *md = [MTLMeshRenderPipelineDescriptor new];
        md.meshFunction = f[0]; md.fragmentFunction = f[1]; md.objectFunction = f[2];
        md.colorAttachments[0].pixelFormat = MTLPixelFormatRG11B10Float; md.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        MTLAutoreleasedRenderPipelineReflection refl = nil;
        id p = [D newRenderPipelineStateWithMeshDescriptor:md options:OPT_STATS reflection:&refl error:&e];
        if (!p) { fprintf(stderr, "mesh pipeline: %s\n", e.description.UTF8String); return 1; }
        NSDictionary *st = ((id(*)(id, SEL))objc_msgSend)((id)refl, @selector(performanceStatistics));
        if (getenv("FSBENCH_STATSKEYS")) fprintf(stderr, "performanceStatistics keys: %s\n", [[st allKeys] componentsJoinedByString:@" | "].UTF8String);
        print_stages(argv[2], st, @"");
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "crun")) {   // crun <scene.json> <out dir> <lib>...: a compute kernel: outputs against the first library (fresh inputs from one seed each), time a dispatch
        NSDictionary *sc = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:@(argv[2])] options:0 error:nil];
        NSString *fn = sc[@"fn"] ?: @"main0"; NSArray *G = sc[@"groups"], *TG = sc[@"tg"];
        MTLSize grid = MTLSizeMake([G[0] unsignedIntegerValue], [G[1] unsignedIntegerValue], [G[2] unsignedIntegerValue]), tgs = MTLSizeMake([TG[0] unsignedIntegerValue], [TG[1] unsignedIntegerValue], [TG[2] unsignedIntegerValue]);
        int P = getenv("FSBENCH_P") ? atoi(getenv("FSBENCH_P")) : 20, R = getenv("FSBENCH_R") ? atoi(getenv("FSBENCH_R")) : 40, NL = argc - 4; NSError *e = nil;
        NSMutableArray *names = [NSMutableArray array], *pso = [NSMutableArray array], *fns = [NSMutableArray array];
        for (int i = 0; i < NL; i++) {
            id<MTLLibrary> L = [D newLibraryWithURL:[NSURL fileURLWithPath:@(argv[4 + i])] error:&e]; if (!L) { fprintf(stderr, "%s: %s\n", argv[4 + i], e.description.UTF8String); return 1; }
            id<MTLFunction> F = [L newFunctionWithName:fn constantValues:[MTLFunctionConstantValues new] error:&e]; if (!F) { fprintf(stderr, "%s: no %s\n", argv[4 + i], fn.UTF8String); return 1; }
            MTLAutoreleasedComputePipelineReflection refl = nil; id<MTLComputePipelineState> p = [D newComputePipelineStateWithFunction:F options:OPT_STATS reflection:&refl error:&e];
            if (!p) { fprintf(stderr, "%s: %s\n", argv[4 + i], e.description.UTF8String); return 1; }
            NSDictionary *st = find_stats(((id(*)(id, SEL))objc_msgSend)((id)refl, @selector(performanceStatistics)), 0);
            [names addObject:[[@(argv[4 + i]) lastPathComponent] stringByDeletingPathExtension]]; [pso addObject:p]; [fns addObject:F];
            printf("stats\t%s\t%sthreads=%lu\n", [names[i] UTF8String], stat_line(st).UTF8String, (unsigned long)p.maxTotalThreadsPerThreadgroup);
            if (p.maxTotalThreadsPerThreadgroup < tgs.width * tgs.height * tgs.depth) { printf("error\t%s: %lu threads a group allowed, the dispatch needs %lu\n", [names[i] UTF8String], (unsigned long)p.maxTotalThreadsPerThreadgroup, (unsigned long)(tgs.width * tgs.height * tgs.depth)); return 1; }
        }
        id<MTLFunction> nf = [[D newLibraryWithSource:@"#include <metal_stdlib>\nkernel void knull() {}\n" options:nil error:&e] newFunctionWithName:@"knull"]; [pso addObject:[D newComputePipelineStateWithFunction:nf error:&e]]; [names addObject:@"(null)"];
        // the argument buffer and its resources, from one seed: each library's check gets its own copy of the same inputs
        NSMutableArray *outs = [NSMutableArray array];
        id<MTLBuffer> (^encode)(void) = ^id<MTLBuffer>(void) {
            rng = 0x9E3779B97F4A7C15ull; [g_res removeAllObjects]; [outs removeAllObjects];
            id<MTLArgumentEncoder> enc = [fns[0] newArgumentEncoderWithBufferIndex:0]; id<MTLBuffer> ab = [D newBufferWithLength:MAX(enc.encodedLength, (NSUInteger)16) options:MTLResourceStorageModeShared]; [enc setArgumentBuffer:ab offset:0];
            for (NSDictionary *a in sc[@"args"]) {
                NSUInteger id_ = [a[@"id"] unsignedIntegerValue]; NSString *k = a[@"kind"]; id r = nil;
                if ([k isEqualToString:@"buffer"]) { r = buffer_of(a); [enc setBuffer:r offset:0 atIndex:id_]; }
                else if ([k isEqualToString:@"tex"]) { r = texture_of(a); [enc setTexture:r atIndex:id_]; }
                else if ([k isEqualToString:@"samp"]) [enc setSamplerState:sampler_of(a) atIndex:id_];
                else if ([k isEqualToString:@"texpool"]) [enc setBuffer:pool_of(a, YES) offset:0 atIndex:id_];
                else if ([k isEqualToString:@"samppool"]) [enc setBuffer:pool_of(a, NO) offset:0 atIndex:id_];
                if (r && [a[@"out"] boolValue]) [outs addObject:r];
            }
            return ab;
        };
        id<MTLCommandQueue> q = [D newCommandQueue];
        void (^disp)(id<MTLCommandBuffer>, id<MTLComputePipelineState>, id<MTLBuffer>) = ^(id<MTLCommandBuffer> cb, id<MTLComputePipelineState> p, id<MTLBuffer> ab) {
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder]; [ce setComputePipelineState:p]; [ce setBuffer:ab offset:0 atIndex:0];
            for (id r in g_res) [ce useResource:r usage:MTLResourceUsageRead | MTLResourceUsageWrite];
            [ce dispatchThreadgroups:grid threadsPerThreadgroup:tgs]; [ce endEncoding];
        };
        NSMutableArray *ref = nil;
        for (int i = 0; i < NL; i++) {
            id<MTLBuffer> ab = encode(); id<MTLCommandBuffer> cb = [q commandBuffer]; disp(cb, pso[i], ab); [cb commit]; [cb waitUntilCompleted];
            if (cb.error) { printf("error\t%s: %s\n", [names[i] UTF8String], cb.error.description.UTF8String); return 1; }
            NSMutableArray *got = [NSMutableArray array];
            for (id r in outs) {
                if ([r conformsToProtocol:@protocol(MTLBuffer)]) [got addObject:[NSData dataWithBytes:[(id<MTLBuffer>)r contents] length:[(id<MTLBuffer>)r length]]];
                else { id<MTLTexture> t = r; NSUInteger bpp = 16; NSMutableData *m = [NSMutableData dataWithLength:t.width * t.height * bpp * MAX(t.depth, t.arrayLength)];
                    for (NSUInteger s = 0; s < t.arrayLength; s++) [t getBytes:(uint8_t *)m.mutableBytes + s * t.width * t.height * bpp bytesPerRow:t.width * bpp bytesPerImage:t.width * t.height * bpp fromRegion:MTLRegionMake3D(0, 0, 0, t.width, t.height, t.depth) mipmapLevel:0 slice:s];
                    [got addObject:m]; }
            }
            if (!ref) { ref = got; printf("output\t%s\t%lu written resources\n", [names[i] UTF8String], (unsigned long)got.count); continue; }
            // A kernel can differ from itself between dispatches: records appended through an atomic counter land in an order the GPU's scheduling decides, and racing
            // writes or atomic accumulation give different values. Ocarina of Time's foliage kernels 510c344f and e855420a differ from copies of themselves in 8-17 words of
            // millions, as many by value as in place (races, not order; 11-12). So each resource is also compared as a multiset of its 32-bit words (sorted, merge walk):
            // "same as a multiset" is append order; a multiset count near the in-place one is changed values. Compare a variant's count with the base's against itself
            NSUInteger words = 0, diff = 0, udiff = 0;
            for (NSUInteger j = 0; j < got.count; j++) { NSData *a = ref[j], *b = got[j]; const uint32_t *x = a.bytes, *y = b.bytes; NSUInteger n = MIN(a.length, b.length) / 4, d = 0; words += n; for (NSUInteger w = 0; w < n; w++) d += x[w] != y[w];
                diff += d; if (!d) continue;
                uint32_t *sx = malloc(n * 4), *sy = malloc(n * 4); memcpy(sx, x, n * 4); memcpy(sy, y, n * 4);
                int (^cmp)(const void *, const void *) = ^int(const void *p, const void *q) { uint32_t u = *(const uint32_t *)p, v = *(const uint32_t *)q; return u < v ? -1 : u > v; };
                qsort_b(sx, n, 4, cmp); qsort_b(sy, n, 4, cmp);   // a merge walk: the words of a that b lacks (a value changed in one place shifts every later position, so not a positional compare)
                NSUInteger p = 0, q = 0, m = 0; while (p < n && q < n) { if (sx[p] == sy[q]) { m++; p++; q++; } else if (sx[p] < sy[q]) p++; else q++; } udiff += n - m; free(sx); free(sy); }
            printf("diff\t%s\t%s\twords differing %lu of %lu (%.4f%%)%s\n", [names[i] UTF8String], diff ? "not exact" : "bit-exact", (unsigned long)diff, (unsigned long)words, 100.0 * diff / MAX(words, (NSUInteger)1),
                   !diff ? "" : udiff ? [NSString stringWithFormat:@"; as a multiset of words: %lu differ", (unsigned long)udiff].UTF8String : "; same as a multiset of words (append order)");
        }
        id<MTLBuffer> ab = encode();   // timing: one set of resources, rounds rotated over the libraries and a null kernel, P dispatches a command buffer
        for (int w = 0; w < 3; w++) for (int i = 0; i <= NL; i++) { id<MTLCommandBuffer> cb = [q commandBuffer]; for (int p = 0; p < P; p++) disp(cb, pso[i], ab); [cb commit]; [cb waitUntilCompleted]; }
        double *t = calloc((NL + 1) * R, sizeof(double));
        for (int r = 0; r < R; r++) for (int i0 = 0; i0 <= NL; i0++) { int i = (i0 + r) % (NL + 1); id<MTLCommandBuffer> cb = [q commandBuffer]; for (int p = 0; p < P; p++) disp(cb, pso[i], ab); [cb commit]; [cb waitUntilCompleted]; t[i * R + r] = (cb.GPUEndTime - cb.GPUStartTime) * 1e3 / P; }
        double med[NL + 1]; for (int i = 0; i <= NL; i++) { qsort(t + i * R, R, sizeof(double), cmp_d); med[i] = t[i * R + R / 2]; }
        printf("time\tlibrary\tms a dispatch (median, IQR)\tnet ms (− null)\tnet vs first\n");
        for (int i = 0; i <= NL; i++) printf("time\t%s\t%.4f (%.4f–%.4f)\t%.4f\t%+.1f%%\n", [names[i] UTF8String], med[i], t[i * R + R / 4], t[i * R + 3 * R / 4], med[i] - med[NL], 100.0 * ((med[i] - med[NL]) / (med[0] - med[NL]) - 1));
        printf("setup\tgroups %lux%lux%lu of %lux%lux%lu, %d dispatches a command buffer, %d rounds, %s\n", (unsigned long)grid.width, (unsigned long)grid.height, (unsigned long)grid.depth, (unsigned long)tgs.width, (unsigned long)tgs.height, (unsigned long)tgs.depth, P, R, D.name.UTF8String);
        return 0;
    }
    if (argc < 5 || strcmp(argv[1], "run")) { fprintf(stderr, "usage: fsbench stats <lib> [fn] [format] | fsbench run <scene.json> <out dir> <lib>...\n"); return 2; }
    NSDictionary *sc = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:@(argv[2])] options:0 error:nil];
    NSString *out = @(argv[3]); [[NSFileManager defaultManager] createDirectoryAtPath:out withIntermediateDirectories:YES attributes:nil error:nil];
    NSUInteger W = [sc[@"w"] unsignedIntegerValue], H = [sc[@"h"] unsignedIntegerValue];
    NSArray *fmts = sc[@"color"] ?: @[@(MTLPixelFormatRG11B10Float)];
    NSString *fn = sc[@"fn"] ?: @"main0";
    int P = getenv("FSBENCH_P") ? atoi(getenv("FSBENCH_P")) : 20, R = getenv("FSBENCH_R") ? atoi(getenv("FSBENCH_R")) : 40;
    NSString *msl = [NSString stringWithContentsOfFile:sc[@"msl"] encoding:NSUTF8StringEncoding error:nil];
    id<MTLFunction> vf = vertex_fn(msl, sc[@"vary"] ?: @{});
    NSError *e = nil;
    id<MTLFunction> nullf = [[D newLibraryWithSource:[NSString stringWithFormat:g_null_src, vary_struct(msl)] options:nil error:&e] newFunctionWithName:@"fnull"];
    if (!nullf) { fprintf(stderr, "null shader: %s\n", e.description.UTF8String); return 1; }

    int NL = argc - 4;
    NSMutableArray *names = [NSMutableArray array], *pso = [NSMutableArray array], *pso32 = [NSMutableArray array], *args = [NSMutableArray array];
    id<MTLFunction> f0 = nil;
    for (int i = 0; i <= NL; i++) {
        id<MTLFunction> ff;
        if (i < NL) {
            id<MTLLibrary> L = [D newLibraryWithURL:[NSURL fileURLWithPath:@(argv[4 + i])] error:&e];
            if (!L) { fprintf(stderr, "%s: %s\n", argv[4 + i], e.description.UTF8String); return 1; }
            ff = [L newFunctionWithName:fn constantValues:[MTLFunctionConstantValues new] error:nil]; if (!ff) { fprintf(stderr, "%s: no %s\n", argv[4 + i], fn.UTF8String); return 1; }
            [names addObject:[[@(argv[4 + i]) lastPathComponent] stringByDeletingPathExtension]];
            if (!f0) f0 = ff;
        } else { ff = nullf; [names addObject:@"(null)"]; }
        NSDictionary *st = nil;
        [pso addObject:pipeline(vf, ff, fmts, &st)];
        NSMutableArray *f32 = [NSMutableArray array]; for (NSUInteger k = 0; k < fmts.count; k++) [f32 addObject:@(MTLPixelFormatRGBA32Float)];
        [pso32 addObject:pipeline(vf, ff, f32, NULL)];
        if (i < NL) printf("stats\t%s\t%s\n", [names[i] UTF8String], stat_line(st).UTF8String);
    }
    // the argument buffer, encoded once from the first library's function (the variants keep its arguments)
    id<MTLArgumentEncoder> enc = [f0 newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> ab = [D newBufferWithLength:enc.encodedLength options:MTLResourceStorageModeShared];
    [enc setArgumentBuffer:ab offset:0];
    for (NSDictionary *a in sc[@"args"]) {
        NSUInteger id_ = [a[@"id"] unsignedIntegerValue]; NSString *k = a[@"kind"];
        if ([k isEqualToString:@"buffer"]) [enc setBuffer:buffer_of(a) offset:0 atIndex:id_];
        else if ([k isEqualToString:@"tex"]) [enc setTexture:texture_of(a) atIndex:id_];
        else if ([k isEqualToString:@"samp"]) [enc setSamplerState:sampler_of(a) atIndex:id_];
        else if ([k isEqualToString:@"texpool"]) [enc setBuffer:pool_of(a, YES) offset:0 atIndex:id_];
        else if ([k isEqualToString:@"samppool"]) [enc setBuffer:pool_of(a, NO) offset:0 atIndex:id_];
    }
    [args addObject:ab];
    id<MTLCommandQueue> q = [D newCommandQueue];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG11B10Float width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
    NSMutableArray *rts = [NSMutableArray array], *rts32 = [NSMutableArray array];
    for (NSNumber *f in fmts) { td.pixelFormat = (MTLPixelFormat)f.integerValue; [rts addObject:[D newTextureWithDescriptor:td]]; }
    td.pixelFormat = MTLPixelFormatRGBA32Float; td.storageMode = MTLStorageModeShared;
    for (NSUInteger k = 0; k < fmts.count; k++) [rts32 addObject:[D newTextureWithDescriptor:td]];
    void (^draw)(id<MTLCommandBuffer>, id<MTLRenderPipelineState>, NSArray *, MTLLoadAction) = ^(id<MTLCommandBuffer> cb, id<MTLRenderPipelineState> p, NSArray *t, MTLLoadAction la) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        for (NSUInteger k = 0; k < t.count; k++) { rp.colorAttachments[k].texture = t[k]; rp.colorAttachments[k].loadAction = la; rp.colorAttachments[k].storeAction = MTLStoreActionStore; rp.colorAttachments[k].clearColor = MTLClearColorMake(0, 0, 0, 0); }
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
        [re setRenderPipelineState:p];
        [re setFragmentBuffer:ab offset:0 atIndex:0];
        for (id r in g_res) [re useResource:r usage:MTLResourceUsageRead | MTLResourceUsageWrite stages:MTLRenderStageFragment];
        [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [re endEncoding];
    };
    // the outputs, at float32, and the comparison with the first
    float *ref = NULL;
    for (int i = 0; i <= NL; i++) {
        id<MTLCommandBuffer> cb = [q commandBuffer]; draw(cb, pso32[i], rts32, MTLLoadActionClear); [cb commit]; [cb waitUntilCompleted];
        if (cb.error) { fprintf(stderr, "%s: %s\n", [names[i] UTF8String], cb.error.description.UTF8String); return 1; }
        if (i == NL) break;
        float *o = read_rgba32(rts32[0]);
        write_png([out stringByAppendingPathComponent:[names[i] stringByAppendingString:@".png"]], o, W, H, 0, NULL);
        [[NSData dataWithBytesNoCopy:o length:W * H * 16 freeWhenDone:NO] writeToFile:[out stringByAppendingPathComponent:[names[i] stringByAppendingString:@".rgba32f"]] atomically:NO];
        if (!ref) { ref = o; double s[3] = {0}; NSUInteger nan = 0; for (NSUInteger p = 0; p < W * H; p++) { for (int c = 0; c < 3; c++) { float v = o[4 * p + c]; if (isnan(v)) nan++; else s[c] += v; } }
            printf("output\t%s\tmean %.5g %.5g %.5g\tnan %lu\n", [names[i] UTF8String], s[0] / (W * H), s[1] / (W * H), s[2] / (W * H), (unsigned long)nan); continue; }
        double maxd = 0, sum = 0; NSUInteger ndiff = 0, nbig = 0, nan = 0; uint32_t exact = 1;
        for (NSUInteger p = 0; p < W * H; p++) { int pd = 0, pb = 0; for (int c = 0; c < 4; c++) {
            float a = ref[4 * p + c], b = o[4 * p + c];
            if (isnan(a) != isnan(b)) { nan++; pd = pb = 1; continue; }
            if (isnan(a)) continue;
            if (*(uint32_t *)&a != *(uint32_t *)&b) { exact = 0; pd = 1; }
            double d = fabs((double)a - b), rel = d / fmax(1e-3, fabs(a)); maxd = fmax(maxd, rel); sum += d; if (rel > 1.0 / 512) pb = 1; }
            ndiff += pd; nbig += pb; }
        printf("diff\t%s\t%s\tpixels differing %lu (%.3f%%), beyond 1/512 relative %lu (%.4f%%), max relative %.3g, mean abs %.3g, nan mismatch %lu\n", [names[i] UTF8String], exact ? "bit-exact" : "not exact",
            (unsigned long)ndiff, 100.0 * ndiff / (W * H), (unsigned long)nbig, 100.0 * nbig / (W * H), maxd, sum / (W * H * 4), (unsigned long)nan);
        write_png([out stringByAppendingPathComponent:[names[i] stringByAppendingString:@".diff.png"]], o, W, H, 1, ref);
        free(o);
    }
    // FSBENCH_CAPTURE=<path>.gputrace (with MTL_CAPTURE_ENABLED=1): one command buffer drawing each library once, for Xcode's shader profiler (per-line cost, occupancy).
    // The game's own captures hold mesh pipelines, which Xcode does not replay; this one holds only the fragment shaders under test
    if (getenv("FSBENCH_CAPTURE")) {
        MTLCaptureDescriptor *cd = [MTLCaptureDescriptor new]; cd.captureObject = q; cd.destination = MTLCaptureDestinationGPUTraceDocument;
        cd.outputURL = [NSURL fileURLWithPath:@(getenv("FSBENCH_CAPTURE"))];
        NSError *ce = nil;
        if (![[MTLCaptureManager sharedCaptureManager] startCaptureWithDescriptor:cd error:&ce]) printf("capture\tnot started: %s\n", ce.localizedDescription.UTF8String);
        else { id<MTLCommandBuffer> cb = [q commandBuffer]; for (int i = 0; i <= NL; i++) draw(cb, pso[i], rts, MTLLoadActionLoad); [cb commit]; [cb waitUntilCompleted];
               [[MTLCaptureManager sharedCaptureManager] stopCapture]; printf("capture\t%s\n", getenv("FSBENCH_CAPTURE")); }
    }
    // timing: round-robin over the libraries and the null shader, one command buffer each, P passes of one draw
    id perf = nil;
    if (getenv("FSBENCH_PERFSTATE")) {   // the GPU performance state Xcode's "consistent state" uses; fails without the entitlement on some systems
        NSError *pe = nil; perf = ((id(*)(id, SEL, long, NSError **))objc_msgSend)(D, @selector(newPerformanceStateAssertion:error:), atol(getenv("FSBENCH_PERFSTATE")), &pe);
        printf("perfstate\t%s\t%s\n", perf ? "asserted" : "not asserted", pe ? pe.localizedDescription.UTF8String : "");
    }
    for (int w = 0; w < 3; w++) for (int i = 0; i <= NL; i++) { id<MTLCommandBuffer> cb = [q commandBuffer]; for (int p = 0; p < P; p++) draw(cb, pso[i], rts, MTLLoadActionLoad); [cb commit]; [cb waitUntilCompleted]; }
    double *t = calloc((NL + 1) * R, sizeof(double));
    for (int r = 0; r < R; r++) for (int i0 = 0; i0 <= NL; i0++) {
        int i = (i0 + r) % (NL + 1);   // rotate the order each round
        id<MTLCommandBuffer> cb = [q commandBuffer];
        for (int p = 0; p < P; p++) draw(cb, pso[i], rts, MTLLoadActionLoad);
        [cb commit]; [cb waitUntilCompleted];
        t[i * R + r] = (cb.GPUEndTime - cb.GPUStartTime) * 1e3 / P;
    }
    if (perf) ((void(*)(id, SEL, id))objc_msgSend)(D, @selector(removePerformanceStateAssertion:), perf);
    double med[NL + 1], q1[NL + 1], q3[NL + 1];
    for (int i = 0; i <= NL; i++) { qsort(t + i * R, R, sizeof(double), cmp_d); med[i] = t[i * R + R / 2]; q1[i] = t[i * R + R / 4]; q3[i] = t[i * R + 3 * R / 4]; }
    printf("time\tlibrary\tms a pass (median, IQR)\tnet ms (− null)\tnet vs first\n");
    for (int i = 0; i <= NL; i++)
        printf("time\t%s\t%.4f (%.4f–%.4f)\t%.4f\t%+.1f%%\n", [names[i] UTF8String], med[i], q1[i], q3[i], med[i] - med[NL], 100.0 * ((med[i] - med[NL]) / (med[0] - med[NL]) - 1));
    printf("setup\t%lux%lu, %d passes a command buffer, %d rounds, %s\n", (unsigned long)W, (unsigned long)H, P, R, D.name.UTF8String);
    return 0;
} }
