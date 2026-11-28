// Multi-device NVN depth mode lifecycle and depth-keyed shader caching (issue #3382):
// - nvnDeviceInitialize resets depth mode to GL (near = -w) and clears the per-device vertex-shader
//   count without emitting "[RED] depth mode changed", so multi-NRO collections (launcher -> game)
//   re-initializing NVNdevice and selecting near = 0 stay quiet.
// - Depth-dependent stages (vertex, domain, mesh) are cached per depth convention so a second
//   device using the other convention loads its own entry point instead of a stale cached NvShader.
// - Changing depth mode after loading a vertex shader on the same device (including via a cache
//   hit on a new device) still emits the [RED] warning, while a stale-generation async load does not.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int checks, failed;
#define EXPECT(name, want, got) do { \
    long long w_ = (long long)(want), g_ = (long long)(got); checks++; \
    if (w_ != g_) { printf("FAIL %s: expected %lld, got %lld\n", name, w_, g_); failed++; } \
} while (0)

static NSString *capture_stderr(void (^block)(void)) {
    fflush(stderr);
    FILE *tmp = tmpfile();
    if (!tmp) return @"";
    int saved = dup(STDERR_FILENO);
    dup2(fileno(tmp), STDERR_FILENO);
    block();
    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    fseek(tmp, 0, SEEK_END);
    long n = ftell(tmp);
    rewind(tmp);
    NSMutableData *data = [NSMutableData dataWithLength:n > 0 ? (NSUInteger)n : 0];
    if (n > 0) (void)fread(data.mutableBytes, 1, (size_t)n, tmp);
    fclose(tmp);
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"";
}

// Compile real Metal functions online so the test runs without the optional MetalToolchain download.
@interface DepthFixtureDevice : NSObject {
@public id<MTLDevice> device; id<MTLLibrary> library;
}
@end
@implementation DepthFixtureDevice
- (id)forwardingTargetForSelector:(SEL)selector { return device; }
- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error { (void)data; (void)error; return library; }
@end

static id<MTLLibrary> make_fixtures(NSString *dir) {
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 vs_gl(uint id [[vertex_id]]) { return float4(0, 0, -1, 1); }\n"
        "vertex float4 vs_z(uint id [[vertex_id]]) { return float4(0, 0, 0, 1); }\n"
        "struct LayerOut { float4 pos [[position]]; uint layer [[render_target_array_index]]; };\n"
        "vertex LayerOut vs_l(uint id [[vertex_id]]) { return {float4(0, 0, -1, 1), 0}; }\n"
        "vertex LayerOut vs_lz(uint id [[vertex_id]]) { return {float4(0, 0, 0, 1), 0}; }\n"
        "fragment float4 fs_main() { return float4(1); }\n"
        "struct MeshVertex { float4 pos [[position]]; };\n"
        "using Tri = mesh<MeshVertex, void, 3, 1, topology::triangle>;\n"
        "struct Block { uint first; };\n"
        "[[object]] void ms_o(object_data Block &b [[payload]], mesh_grid_properties g) { b.first = 0; g.set_threadgroups_per_grid(uint3(1, 1, 1)); }\n"
        "[[mesh]] void ms_gl(Tri out, const object_data Block &b [[payload]]) { out.set_primitive_count(0); }\n"
        "[[mesh]] void ms_z(Tri out, const object_data Block &b [[payload]]) { out.set_primitive_count(0); }\n";
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:source options:nil error:&error];
    if (!lib) { fprintf(stderr, "newLibraryWithSource failed: %s\n", error.localizedDescription.UTF8String); return nil; }
    NSData *placeholder = [@"MTLB" dataUsingEncoding:NSUTF8StringEncoding];
    for (NSString *name in @[@"depth-vs", @"depth-fs", @"depth-ms"]) {
        if (![placeholder writeToFile:[dir stringByAppendingPathComponent:[name stringByAppendingString:@".metallib"]] atomically:YES]) return nil;
    }
    NSDictionary *vs_refl = @{
        @"chain": @DMSH_CHAIN, @"route": @"B",
        @"entry": @"vs_gl", @"entry_z": @"vs_z",
        @"entry_layer": @"vs_l", @"entry_layer_z": @"vs_lz",
        @"bindings": @[], @"vertex_inputs": @[]
    };
    NSDictionary *fs_refl = @{
        @"chain": @DMSH_CHAIN, @"route": @"B",
        @"entry": @"fs_main", @"bindings": @[], @"vertex_inputs": @[]
    };
    NSDictionary *ms_refl = @{
        @"chain": @DMSH_CHAIN, @"route": @"B",
        @"entry": @"ms_gl", @"entry_z": @"ms_z", @"entry_object": @"ms_o",
        @"mesh_stage": @{@"kind": @"ms", @"threads": @1},
        @"bindings": @[], @"vertex_inputs": @[]
    };
    NSDictionary *all = @{@"depth-vs": vs_refl, @"depth-fs": fs_refl, @"depth-ms": ms_refl};
    for (NSString *name in all) {
        NSData *json = [NSJSONSerialization dataWithJSONObject:all[name] options:0 error:nil];
        if (![json writeToFile:[dir stringByAppendingPathComponent:[name stringByAppendingString:@".json"]] atomically:YES]) return nil;
    }
    return lib;
}

int main(void) { @autoreleasepool {
    char tmpl[] = "/tmp/t_depth_mode.XXXXXX";
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 2; }
    NSString *dir = @(tmpl);
    mtl_init();
    id<MTLLibrary> lib = make_fixtures(dir);
    if (!lib) return 2;
    DepthFixtureDevice *fixture_dev = [DepthFixtureDevice new];
    fixture_dev->device = g_dev;
    fixture_dev->library = lib;
    g_dev = (id<MTLDevice>)fixture_dev;
    g_msl_dir = dir;
    g_shcache = nil;

    NVNT_nvnDeviceBuilderSetDefaults builder_defaults = nvn_lookup("nvnDeviceBuilderSetDefaults");
    NVNT_nvnDeviceInitialize dev_init = nvn_lookup("nvnDeviceInitialize");
    NVNT_nvnDeviceFinalize dev_fini = nvn_lookup("nvnDeviceFinalize");
    NVNT_nvnDeviceSetDepthMode set_depth = nvn_lookup("nvnDeviceSetDepthMode");
    NVNT_nvnDeviceGetDepthMode get_depth = nvn_lookup("nvnDeviceGetDepthMode");
    EXPECT("nvn device entry points resolved", 1, builder_defaults && dev_init && dev_fini && set_depth && get_depth);

    static NVNdeviceBuilder builder;
    static NVNdevice dev1, dev2, dev3, dev4;
    builder_defaults(&builder);

    // 1. Launcher device: near=0, loads vertex, layer, mesh, and pixel shaders cleanly.
    __block NvShader *vs_z1 = nil, *vs_lz1 = nil, *ms_z1 = nil, *fs1 = nil;
    NSString *log1 = capture_stderr(^{
        dev_init(&dev1, &builder);
        set_depth(&dev1, NVN_DEPTH_MODE_NEAR_IS_ZERO);
        vs_z1 = shader_get(@"depth-vs", 0);
        vs_lz1 = vs_z1 ? shader_get_layer(vs_z1) : nil;
        ms_z1 = shader_get(@"depth-ms", 4);
        fs1 = shader_get(@"depth-fs", 1);
    });
    EXPECT("device 1 depth mode is near=0", NVN_DEPTH_MODE_NEAR_IS_ZERO, get_depth(&dev1));
    EXPECT("device 1 vertex shader uses main0_z", 1, vs_z1 && [vs_z1->ventry isEqualToString:@"vs_z"] && vs_z1->depth_zero == 1);
    EXPECT("device 1 layer shader uses entry_layer_z", 1, vs_lz1 && [vs_lz1->ventry isEqualToString:@"vs_lz"]);
    EXPECT("device 1 mesh shader uses ms_z", 1, ms_z1 && [ms_z1->ventry isEqualToString:@"ms_z"]);
    EXPECT("device 1 pixel shader loaded", 1, fs1 != nil);
    EXPECT("device 1 emits no depth-mode [RED]", 1, [log1 rangeOfString:@"[RED]"].location == NSNotFound);
    uint32_t stale_gen = atomic_load(&g_depth_dev_gen);
    dev_fini(&dev1);

    // 2. Game device (same near=0 mode, like LEGO Harry Potter launcher -> game): re-initializing
    //    NVNdevice and setting near=0 must not emit [RED], and must reuse cached near=0 shaders.
    __block NvShader *vs_z2 = nil, *fs2 = nil;
    NSString *log2 = capture_stderr(^{
        dev_init(&dev2, &builder);
        // Simulate a late async prefetch from device 1 finishing after device 2 initialized:
        // its stale generation must not mark device 2 as having loaded vertex shaders.
        NvShader *late = shader_load_z(@"depth-vs", 0, 0, 1, stale_gen);
        EXPECT("late stale-gen load succeeded", 1, late != nil);
        set_depth(&dev2, NVN_DEPTH_MODE_NEAR_IS_ZERO);
        vs_z2 = shader_get(@"depth-vs", 0);
        fs2 = shader_get(@"depth-fs", 1);
    });
    EXPECT("device 2 re-init + near=0 emits no [RED]", 1, [log2 rangeOfString:@"[RED]"].location == NSNotFound);
    EXPECT("device 2 reuses cached near=0 vertex shader", 1, vs_z2 == vs_z1);
    EXPECT("device 2 reuses cached pixel shader", 1, fs2 == fs1);
    EXPECT("device 2 cache hit counted vertex shader for this device", 1, atomic_load(&g_vs_loaded) == 1);

    // 3. Mid-device depth switch AFTER a cached vertex shader was used on device 2 must still emit [RED].
    NSString *log2_mid = capture_stderr(^{
        set_depth(&dev2, NVN_DEPTH_MODE_NEAR_IS_MINUS_W);
    });
    EXPECT("mid-device depth change after cache hit emits [RED]", 1, [log2_mid rangeOfString:@"[RED] depth mode changed after 1 vertex shaders were loaded"].location != NSNotFound);
    dev_fini(&dev2);

    // 4. Third device keeps NVN default GL mode (near=-w): must NOT receive device 1's cached near=0 shaders.
    __block NvShader *vs_gl = nil, *vs_l = nil, *ms_gl = nil, *fs3 = nil;
    NSString *log3 = capture_stderr(^{
        dev_init(&dev3, &builder);
        vs_gl = shader_get(@"depth-vs", 0);
        vs_l = vs_gl ? shader_get_layer(vs_gl) : nil;
        ms_gl = shader_get(@"depth-ms", 4);
        fs3 = shader_get(@"depth-fs", 1);
    });
    EXPECT("device 3 default depth mode is GL", NVN_DEPTH_MODE_NEAR_IS_MINUS_W, get_depth(&dev3));
    EXPECT("device 3 emits no [RED]", 1, [log3 rangeOfString:@"[RED]"].location == NSNotFound);
    EXPECT("device 3 vertex shader uses GL entry, distinct from near=0", 1, vs_gl && vs_gl != vs_z1 && [vs_gl->ventry isEqualToString:@"vs_gl"] && vs_gl->depth_zero == 0);
    EXPECT("device 3 layer shader uses GL entry_layer, distinct from near=0", 1, vs_l && vs_l != vs_lz1 && [vs_l->ventry isEqualToString:@"vs_l"]);
    EXPECT("device 3 mesh shader uses GL ms_gl, distinct from near=0", 1, ms_gl && ms_gl != ms_z1 && [ms_gl->ventry isEqualToString:@"ms_gl"]);
    EXPECT("device 3 still shares depth-independent pixel shader", 1, fs3 == fs1);

    // 5. Mid-device depth switch on device 3 (freshly loaded GL vertex shader -> near=0) emits [RED].
    NSString *log3_mid = capture_stderr(^{
        set_depth(&dev3, NVN_DEPTH_MODE_NEAR_IS_ZERO);
    });
    EXPECT("mid-device GL->near=0 change emits [RED]", 1, [log3_mid rangeOfString:@"[RED] depth mode changed after 2 vertex shaders were loaded"].location != NSNotFound);
    dev_fini(&dev3);

    // 6. Fourth device starts clean again after device 3's mid-device warning.
    NSString *log4 = capture_stderr(^{
        dev_init(&dev4, &builder);
        set_depth(&dev4, NVN_DEPTH_MODE_NEAR_IS_ZERO);
    });
    EXPECT("device 4 re-init + near=0 after prior warning emits no [RED]", 1, [log4 rangeOfString:@"[RED]"].location == NSNotFound);
    dev_fini(&dev4);

    [NSFileManager.defaultManager removeItemAtPath:dir error:nil];
    printf("t_depth_mode: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
