// The host's own record of the shaders its library lacks (DM_NVN_MISSING_KEYS, shader_missing_note and want_note): a --seen-shaders build reads this file, because a log is gone after two
// more starts of the app (GitHub issue #1519). Through the real shader_load and prog_get on an empty library folder: each key once, from both red lines, across runs and threads; the geometry
// routes' libraries are not keys of the map; a half-written last line costs no key; with the variable unset nothing is written. CPU only: no Metal device is needed.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static NSString *g_list;
static NSArray<NSString *> *lines(void) {
    NSString *s = [NSString stringWithContentsOfFile:g_list encoding:NSUTF8StringEncoding error:nil];
    if (!s) return @[];
    NSMutableArray *a = [[s componentsSeparatedByString:@"\n"] mutableCopy];
    if ([a.lastObject length] == 0) [a removeLastObject];
    return a;
}
static NSUInteger count_of(NSString *key) {
    NSUInteger n = 0; for (NSString *l in lines()) if ([l isEqualToString:key]) n++;
    return n;
}

typedef struct { want_list *w; int from, to; } fill_t;
static void *fill(void *arg) {
    fill_t *f = arg; char key[17];
    for (int round = 0; round < 2; round++) for (int i = f->from; i < f->to; i++) { snprintf(key, sizeof key, "%016x", i + 1); want_note(f->w, key); }
    return NULL;
}

int main(void) { @autoreleasepool {
    char tmpl[] = "/tmp/t_missing_keys.XXXXXX"; NSString *dir = @(mkdtemp(tmpl));
    g_list = [dir stringByAppendingPathComponent:@"shader_missing.txt"];
    g_msl_dir = [dir stringByAppendingPathComponent:@"empty-library"];
    [NSFileManager.defaultManager createDirectoryAtPath:g_msl_dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *pixel = @"aaaaaaaaaaaaaaa1", *geometry = @"aaaaaaaaaaaaaaa2", *route = @"aaaaaaaaaaaaaaa3";
    g_shmap = [NSMutableDictionary dictionaryWithDictionary:@{pixel: @"pixel", geometry: @"geometry", route: @"vertex"}];

    // Unset: what an iPhone and a full run without the launcher do
    unsetenv("DM_NVN_MISSING_KEYS");
    CHECK(!shader_load([pixel stringByAppendingString:@"_b"], 1, 0), "no library, no shader");
    CHECK(![NSFileManager.defaultManager fileExistsAtPath:g_list], "with the variable unset nothing is written");

    setenv("DM_NVN_MISSING_KEYS", g_list.fileSystemRepresentation, 1);
    // The red line of shader_load, once however many programs ask
    for (int i = 0; i < 3; i++) CHECK(!shader_load([pixel stringByAppendingString:@"_b"], 1, 0), "no library, no shader");
    CHECK(count_of(pixel) == 1, "a shader the library lacks is kept, once");
    // Other libraries of a shader, and a library with no key: the log's pattern takes neither, and a build could not build them from the shader map
    CHECK(!shader_load([route stringByAppendingString:@"_bgv"], 0, 0) && !shader_load([route stringByAppendingString:@"_bg1"], 2, 0) && !shader_load(@"rt_library_b", NVN_RT_RESOURCE_STAGE, 0), "no such libraries");
    CHECK(count_of(route) == 0 && lines().count == 1, "a geometry route's library is not a key of the map");
    // The red line of prog_get alone: a missing geometry translation returns from shader_load without one
    uint8_t md[8] = {0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xa2};
    prog_t p = {.magic = MAGIC('P', 'R', 'O', 'G'), .nstage = 1, .serial = nvn_new_serial()};
    prog_keys_t *pk = pk_get(p.serial); memcpy(pk->key[0], md, 8); pk->ok |= 1;
    NvProg *o = prog_get(&p);
    CHECK(o && !o->sh[2], "the program is made without its missing stage");
    CHECK(count_of(geometry) == 1, "a stage only prog_get reports missing is kept");

    // The next start: what the file holds is not added again, a new key is
    want_list next = {"DM_NVN_MISSING_KEYS", PTHREAD_MUTEX_INITIALIZER};
    NSUInteger before = lines().count;
    want_note(&next, pixel.UTF8String);
    CHECK(lines().count == before, "a key of an earlier run is not written again");
    want_note(&next, [route stringByAppendingString:@"_b"].UTF8String);
    CHECK(count_of(route) == 1 && lines().count == before + 1, "a new key is added, without its route suffix");

    // A last line a stopped host left half written costs neither it nor the next key their lines
    [@"aaaaaaaaaaaaaaa4\n1234" writeToFile:g_list atomically:NO encoding:NSUTF8StringEncoding error:nil];
    want_list torn = {"DM_NVN_MISSING_KEYS", PTHREAD_MUTEX_INITIALIZER};
    want_note(&torn, "aaaaaaaaaaaaaaa5"); want_note(&torn, "aaaaaaaaaaaaaaa4"); want_note(&torn, "aaaaaaaaaaaaaaa6");
    CHECK([lines() isEqualToArray:(@[@"aaaaaaaaaaaaaaa4", @"1234", @"aaaaaaaaaaaaaaa5", @"aaaaaaaaaaaaaaa6"])], "a half-written line stays apart from the next key");

    // Many keys from several threads (past the table's first sizes): each exactly once
    [NSFileManager.defaultManager removeItemAtPath:g_list error:nil];
    want_list many = {"DM_NVN_MISSING_KEYS", PTHREAD_MUTEX_INITIALIZER}; enum { THREADS = 4, EACH = 5000 };
    pthread_t th[THREADS]; fill_t f[THREADS];
    for (int i = 0; i < THREADS; i++) { f[i] = (fill_t){&many, 0, EACH * (i + 1)}; pthread_create(&th[i], NULL, fill, &f[i]); }   // overlapping ranges
    for (int i = 0; i < THREADS; i++) pthread_join(th[i], NULL);
    NSArray *all = lines();
    CHECK(all.count == EACH * THREADS && [NSSet setWithArray:all].count == EACH * THREADS, "each of 20,000 keys once, from four threads");

    [NSFileManager.defaultManager removeItemAtPath:dir error:nil];
    fprintf(stderr, "t_missing_keys: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
