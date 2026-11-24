// A function's Metal 4 descriptor is noted once, as final (GitHub issue #3309). Metal hands out one MTLFunction per function content, device-wide, and the host keys a function's
// descriptor by that object (fn4_note, g_fn4), so two loads of one library share the entry: a program's shaders load on pp_bg_queue while its mesh base loads the same libraries on
// a queue of its own (mtl_prog_prefetch, mtk_prefetch). shader_load used to note its function as a plain library descriptor first and specialize it after: a pipeline built from that
// moment's descriptor named a function that declares constants without any, and Metal's validation aborted the app (`fragmentFunction main0 cannot be used to build a pipeline state`).
// Watches every note of the map while shader_load and pull_load load functions with and without constants: none may be a plain descriptor of a function that declares constants.
// Then two threads load one shader again and again while a third reads its descriptor. make check_t_fn4_once (the fixture is compiled here, with the SDK's compiler)
#include "../nvn/nvn_mtl.m"
#include <objc/runtime.h>
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static int sh(NSString *cmd) { int r = system(cmd.UTF8String); return WIFEXITED(r) ? WEXITSTATUS(r) : -1; }
static void put(NSString *path, NSString *text) { [text writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil]; }
static int lib(NSString *dir, NSString *name, NSString *src) {   // <dir>/<name>.metallib from MSL, with the SDK's compiler
    put([NSString stringWithFormat:@"%@/%@.metal", dir, name], src);
    return sh([NSString stringWithFormat:@"cd %@ && xcrun -sdk macosx metal -c %@.metal -o %@.air && xcrun -sdk macosx metallib %@.air -o %@.metallib && rm %@.metal %@.air", dir, name, name, name, name, name, name]);
}
static int specialized(id<MTLFunction> f) { return [fn4_of(f) isKindOfClass:MTL4SpecializedFunctionDescriptor.class]; }
// Every note of g_fn4 (the map's own setter, wrapped): plain counts the descriptors that are not specialized although their function declares constants
static IMP g_set; static _Atomic int g_notes, g_plain;
static void set_watch(id self, SEL sel, id d, id f) {
    if (self == g_fn4) { g_notes++; if (![d isKindOfClass:MTL4SpecializedFunctionDescriptor.class] && [(id<MTLFunction>)f functionConstantsDictionary].count) g_plain++; }
    ((void (*)(id, SEL, id, id))g_set)(self, sel, d, f);
}
static void watch(void) {
    Class c = object_getClass(g_fn4); Method m = class_getInstanceMethod(c, @selector(setObject:forKey:));
    g_set = method_getImplementation(m); class_replaceMethod(c, @selector(setObject:forKey:), (IMP)set_watch, method_getTypeEncoding(m));
}
int main(void) { @autoreleasepool {
    char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%s/t_fn4_once.XXXXXX", getenv("TMPDIR") ?: "/tmp"); if (!mkdtemp(tmp)) abort();
    NSString *dir = @(tmp);
    NSString *head = @"#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\n";
    // the constants a translated shader declares: one of SPIRV-Cross's (optional: is_function_constant_defined) and the host's blend fix-up switch, which shader_load sets false
    NSString *consts = [NSString stringWithFormat:@"constant bool k [[function_constant(0)]];\nconstant bool dm_blend_off [[function_constant(%d)]];\n", DMSH_BLEND_OFF_FC];
    NSString *plain = [head stringByAppendingString:@"fragment float4 main0() { return float4(1, 0, 0, 1); }\n"];
    NSString *fc = [NSString stringWithFormat:@"%@%@fragment float4 main0() { return is_function_constant_defined(k) || (is_function_constant_defined(dm_blend_off) && dm_blend_off) ? float4(1) : float4(0, 1, 0, 1); }\n", head, consts];
    NSString *vx = [NSString stringWithFormat:@"%@%@vertex V main0(uint id [[vertex_id]]) { V o; o.p = float4(float(id & 1), float(id >> 1), is_function_constant_defined(k) ? 1.0 : 0.0, 1); return o; }\n"
                    "vertex V main0_wl(uint id [[vertex_id]]) { V o; o.p = float4(float(id & 1), 0, is_function_constant_defined(k) ? 1.0 : 0.0, 1); return o; }\n", head, consts];
    if (sh(@"xcrun -sdk macosx -f metal >/dev/null 2>&1")) { printf("t_fn4_once: no Metal compiler (xcrun metal), not run\n"); sh([NSString stringWithFormat:@"rm -rf %@", dir]); return 0; }
    if (lib(dir, @"0000000000000001_b", plain) || lib(dir, @"0000000000000002_b", fc) || lib(dir, @"0000000000000003_b", vx) || lib(dir, @"0000000000000003_bp", vx)) {
        fprintf(stderr, "FAIL: the fixture's libraries did not compile\n"); sh([NSString stringWithFormat:@"rm -rf %@", dir]); return 1; }
    NSString *json = [NSString stringWithFormat:@"{\"chain\": \"%s\", \"route\": \"B\", \"bindings\": [], \"entry\": \"main0\"%%@}", DMSH_CHAIN];
    put([dir stringByAppendingPathComponent:@"0000000000000001_b.json"], [NSString stringWithFormat:json, @""]);
    put([dir stringByAppendingPathComponent:@"0000000000000002_b.json"], [NSString stringWithFormat:json, @""]);
    put([dir stringByAppendingPathComponent:@"0000000000000003_b.json"], [NSString stringWithFormat:json, @", \"pull\": {}"]);
    setenv("DM_MSL_DIR", tmp, 1);
    mtl_init(); g_msl_dir = dir;
    if (!pso4_compiler()) { printf("t_fn4_once: no Metal 4 compiler, not run\n"); sh([NSString stringWithFormat:@"rm -rf %@", dir]); return 0; }

    NvShader *n = shader_load(@"0000000000000001_b", 1, 0);
    CHECK(n && n->fn && [fn4_of(n->fn) isKindOfClass:MTL4LibraryFunctionDescriptor.class], "a function without constants keeps its library descriptor");
    watch();
    NvShader *a = shader_load(@"0000000000000002_b", 1, 0);
    CHECK(a && a->fn && specialized(a->fn), "a pixel shader that declares constants has a specialized descriptor");
    CHECK(g_notes > 0 && !g_plain, "shader_load never notes a function that declares constants as a plain descriptor");
    // the mesh base's own load of the program's pixel shader (mpf_shader), from a second library of the same file
    NvShader *b = shader_load(@"0000000000000002_b", 1, 0);
    printf("t_fn4_once: a second library of the same file gives %s MTLFunction\n", b && b->fn == a->fn ? "the same" : "another");
    CHECK(b && specialized(b->fn) && specialized(a->fn) && !g_plain, "a second load of the shader notes no plain descriptor either");
    NvShader *v = shader_load(@"0000000000000003_b", 0, 0);
    CHECK(v && v->has_pull && specialized(v->fn), "a vertex shader that declares constants has a specialized descriptor");
    if (v) pull_load(v);
    CHECK(v && v->pfn_state == 1 && specialized(v->pfn) && v->wlfn && specialized(v->wlfn) && !g_plain, "pull_load notes its entry points specialized, never plain");

    // the race itself: two loaders and a reader of the first load's descriptor, each on a queue of its own (a thread each, however few cores)
    __block _Atomic int stop = 0, seen = 0, bad = 0; id<MTLFunction> f = a ? a->fn : nil;
    dispatch_group_t g = dispatch_group_create(), loads = dispatch_group_create();
    dispatch_group_async(g, dispatch_queue_create("reader", DISPATCH_QUEUE_SERIAL), ^{
        while (!atomic_load(&stop)) { MTL4FunctionDescriptor *d = fn4_of(f); seen++; if (d && ![d isKindOfClass:MTL4SpecializedFunctionDescriptor.class]) bad++; } });
    for (int t = 0; t < 2; t++) dispatch_group_async(loads, dispatch_queue_create("loader", DISPATCH_QUEUE_SERIAL), ^{ for (int i = 0; i < 150; i++) @autoreleasepool { (void)shader_load(@"0000000000000002_b", 1, 0); } });
    dispatch_group_wait(loads, DISPATCH_TIME_FOREVER); atomic_store(&stop, 1); dispatch_group_wait(g, DISPATCH_TIME_FOREVER);
    printf("t_fn4_once: %d reads of the descriptor during 300 loads, %d plain; %d notes, %d plain\n", (int)seen, (int)bad, (int)g_notes, (int)g_plain);
    CHECK(!bad && !g_plain, "a concurrent reader never sees the shader's descriptor plain");

    sh([NSString stringWithFormat:@"rm -rf %@", dir]);
    printf("t_fn4_once: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
