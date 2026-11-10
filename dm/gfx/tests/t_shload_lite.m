// DM_NVN_SHLOAD_LITE (shload_lite): a shader load with no argument-encoder reflection must give the argument length the encoder gave, keep the encoding path on
// (encode_args_ runs on arg_len), and leave the layer output to the reflection json as before
#include "../nvn/nvn_mtl.m"

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int checks, failed;
#define EXPECT(name, want, got) do { uint64_t w_ = (want), g_ = (got); checks++; \
    if (w_ != g_) { fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w_, (unsigned long long)g_); failed++; } } while (0)

static int fixtures(NSString *dir) {
    // Both libraries hold the same functions: a layer-writing vertex shader (layer_out in its reflection) and a fragment shader; each reads
    // a three-entry argument buffer at buffer 0 (route B: arg_id 0, 1, 2 → 24 bytes)
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct A { constant float4 *c [[id(0)]]; device float4 *s [[id(1)]]; texture2d<float> t [[id(2)]]; };\n"
        "struct VO { float4 p [[position]]; uint layer [[render_target_array_index]]; };\n"
        "vertex VO vs(uint id [[vertex_id]], constant A &a [[buffer(0)]]) { VO o; o.p = a.c[id] + a.s[0]; o.layer = 1; return o; }\n"
        "fragment float4 fs(float4 p [[position]], constant A &a [[buffer(0)]]) { constexpr sampler sm; return a.c[0] + a.s[0] + a.t.sample(sm, p.xy); }\n";
    NSString *metal = [dir stringByAppendingPathComponent:@"fixture.metal"], *library = [dir stringByAppendingPathComponent:@"lite-vs_b.metallib"];
    if (![source writeToFile:metal atomically:YES encoding:NSUTF8StringEncoding error:nil]) return 0;
    NSTask *compiler = [NSTask new]; compiler.executableURL = [NSURL fileURLWithPath:@"/usr/bin/xcrun"];
    compiler.arguments = @[@"-sdk", @"macosx", @"metal", metal, @"-o", library];
    NSError *error = nil;
    if (![compiler launchAndReturnError:&error]) { fprintf(stderr, "fixture compiler: %s\n", error.localizedDescription.UTF8String); return 0; }
    [compiler waitUntilExit]; if (compiler.terminationStatus) return 0;
    if (![NSFileManager.defaultManager copyItemAtPath:library toPath:[dir stringByAppendingPathComponent:@"lite-fs_b.metallib"] error:nil]) return 0;
    NSArray *bindings = @[@{@"kind": @"cbuf", @"reg": @0, @"arg_id": @0, @"msl_name": @"c", @"msl_type": @"constant float4*"},
                          @{@"kind": @"sbuf", @"reg": @1, @"arg_id": @1, @"msl_name": @"s", @"msl_type": @"device float4*", @"rw": @YES},
                          @{@"kind": @"tex", @"reg": @128, @"arg_id": @2, @"msl_name": @"t", @"msl_type": @"texture2d<float>"}];
    for (NSString *stage in @[@"vs", @"fs"]) {
        NSDictionary *reflection = @{@"chain": @DMSH_CHAIN, @"route": @"B", @"entry": stage, @"entry_z": stage, @"bindings": bindings, @"vertex_inputs": @[], @"layer_out": @([stage isEqualToString:@"vs"])};
        NSData *json = [NSJSONSerialization dataWithJSONObject:reflection options:0 error:nil];
        if (![json writeToFile:[dir stringByAppendingPathComponent:[NSString stringWithFormat:@"lite-%@_b.json", stage]] atomically:YES]) return 0;
    }
    g_msl_dir = dir;
    return 1;
}

int main(void) { @autoreleasepool {
    g_dev = MTLCreateSystemDefaultDevice(); if (!g_dev) { puts("t_shload_lite: no Metal device, skipped"); return 0; }
    NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"t_shload_lite_%d", getpid()]];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    if (!fixtures(dir)) { fprintf(stderr, "t_shload_lite: fixtures failed\n"); return 1; }
    NvShader *full[2], *lite[2];
    for (int mode = 0; mode < 2; mode++) {
        g_shload_lite = mode;
        for (int st = 0; st < 2; st++) {
            NvShader *s = shader_load(st ? @"lite-fs_b" : @"lite-vs_b", st, 0);
            if (!s) { fprintf(stderr, "t_shload_lite: shader_load failed (mode %d stage %d)\n", mode, st); return 1; }
            (mode ? lite : full)[st] = s;
        }
    }
    EXPECT("the reflection's bindings", 3, (uint64_t)full[0]->rf.nb);
    for (int st = 0; st < 2; st++) {
        EXPECT("the old path makes an argument encoder", 1, full[st]->ae != nil);
        EXPECT("the lite path makes none", 1, lite[st]->ae == nil);
        EXPECT("the lite argument length equals the encoder's", full[st]->arg_len, lite[st]->arg_len);
        EXPECT("the encoder's length (8 bytes per argument ID)", 24, full[st]->arg_len);
    }
    EXPECT("the old path: a layer-writing vertex shader", 1, full[0]->layer_out);
    EXPECT("the lite path: the same layer output", 1, lite[0]->layer_out);
    // encode_args_ with an arg_len and no encoder writes the argument buffer: a nil encoder must not read as "no bindings"
    EXPECT("encode_args_ gate: lite shader has bindings", 1, !(!lite[1]->ae && !lite[1]->arg_len));
    [NSFileManager.defaultManager removeItemAtPath:dir error:nil];
    printf("t_shload_lite: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
