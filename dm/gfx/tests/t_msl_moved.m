// Keep loading shader libraries and reflection JSON after the cache or app bundle directory is moved while running, and close any prior tracking fd on re-read.
#include "../nvn/nvn_mtl.m"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod;
int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int g_checks, g_failed;
#define EXPECT_EQ(name, want, got) do { \
    long long w_ = (long long)(want), g_ = (long long)(got); g_checks++; \
    printf("%s expected %lld -> got %lld %s\n", name, w_, g_, w_ == g_ ? "OK" : "FAIL"); \
    g_failed += w_ != g_; \
} while (0)

static void put(NSString *path, NSString *text) {
    [NSFileManager.defaultManager createDirectoryAtPath:path.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
    if (![text writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil]) {
        fprintf(stderr, "FAIL: cannot write %s\n", path.UTF8String);
        exit(2);
    }
}

static int run_xcrun(NSString *dir, NSArray<NSString *> *args) {
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/xcrun"];
    task.currentDirectoryURL = [NSURL fileURLWithPath:dir];
    task.arguments = args;
    if (![task launchAndReturnError:nil]) return -1;
    [task waitUntilExit];
    return task.terminationStatus;
}

static int compile_metallib(NSString *dir, NSString *name) {
    NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\nvertex float4 main0(uint id [[vertex_id]]) { return float4(0, 0, 0, 1); }\n";
    NSString *metal = [NSString stringWithFormat:@"%@.metal", name];
    NSString *air = [NSString stringWithFormat:@"%@.air", name];
    NSString *lib = [NSString stringWithFormat:@"%@.metallib", name];
    put([dir stringByAppendingPathComponent:metal], src);
    int rc = run_xcrun(dir, @[@"-sdk", @"macosx", @"metal", @"-c", metal, @"-o", air]);
    if (rc == 0) rc = run_xcrun(dir, @[@"-sdk", @"macosx", @"metallib", air, @"-o", lib]);
    [NSFileManager.defaultManager removeItemAtPath:[dir stringByAppendingPathComponent:metal] error:nil];
    [NSFileManager.defaultManager removeItemAtPath:[dir stringByAppendingPathComponent:air] error:nil];
    return rc;
}

int main(void) { @autoreleasepool {
    char tmpl[] = "/tmp/t_msl_moved.XXXXXX";
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 2; }
    NSString *sandbox = @(tmpl);
    NSString *src_root = [sandbox stringByAppendingPathComponent:@"AppA"];
    NSString *dst_root = [sandbox stringByAppendingPathComponent:@"AppB"];
    NSString *shmap = [sandbox stringByAppendingPathComponent:@"shader_map.tsv"];
    put(shmap, @"# no shaders\n");

    NSString *b_rel = [NSString stringWithFormat:@"b-%s", DMSH_CHAIN];
    NSString *src_b = [src_root stringByAppendingPathComponent:b_rel];
    NSString *dst_b = [dst_root stringByAppendingPathComponent:b_rel];
    put([src_b stringByAppendingPathComponent:@"inputs.tsv"],
        [NSString stringWithFormat:@"game\t%s\ninputs\tinit\n", DM_GAME_ID]);
    put([src_b stringByAppendingPathComponent:@"before_b.json"], @"{\"phase\":\"before\"}");

    setenv("DM_NVN_SHMAP", shmap.fileSystemRepresentation, 1);
    setenv("DM_MSL_DIR", src_root.fileSystemRepresentation, 1);
    unsetenv("DM_ROMFS");

    mtl_init();
    shmap_read();

    int tracked_fd = g_msl_fd;
    EXPECT_EQ("shmap_read opened directory fd", 1, tracked_fd >= 0);
    EXPECT_EQ("shmap_read recorded canonical F_GETPATH", 1, g_msl_fd_path != nil);

    NSData *before = msl_data([g_msl_dir stringByAppendingPathComponent:@"before_b.json"]);
    NSString *before_str = before ? [[NSString alloc] initWithData:before encoding:NSUTF8StringEncoding] : @"";
    EXPECT_EQ("initial msl_data reads before move", 1, [before_str isEqualToString:@"{\"phase\":\"before\"}"]);
    EXPECT_EQ("missing file before move returns 0", 0, msl_has([g_msl_dir stringByAppendingPathComponent:@"missing_b.json"]));
    EXPECT_EQ("symlinked /tmp does not false-trigger move warning before rename", 0, atomic_load(&g_msl_moved_told));

    if (rename(src_root.fileSystemRepresentation, dst_root.fileSystemRepresentation) != 0) {
        perror("rename");
        return 2;
    }
    EXPECT_EQ("old path no longer exists on disk", 0, access([g_msl_dir stringByAppendingPathComponent:@"before_b.json"].fileSystemRepresentation, F_OK) == 0);

    // Write additional cache artifacts into the moved location.
    put([dst_b stringByAppendingPathComponent:@"after_b.json"], @"{\"phase\":\"after\"}");
    EXPECT_EQ("compile metallib in moved cache dir", 0, compile_metallib(dst_b, @"after_b"));

    // Queries still pass the stale g_msl_dir path captured before the move.
    EXPECT_EQ("msl_has resolves moved json via F_GETPATH", 1, msl_has([g_msl_dir stringByAppendingPathComponent:@"after_b.json"]));
    EXPECT_EQ("msl_has still returns 0 for missing file", 0, msl_has([g_msl_dir stringByAppendingPathComponent:@"missing_b.json"]));
    EXPECT_EQ("msl_moved rejects sibling prefix without slash", 1, msl_moved([g_msl_dir stringByAppendingString:@"_sibling/after_b.json"]) == nil);

    NSData *after = msl_data([g_msl_dir stringByAppendingPathComponent:@"after_b.json"]);
    NSString *after_str = after ? [[NSString alloc] initWithData:after encoding:NSUTF8StringEncoding] : @"";
    EXPECT_EQ("msl_data reads newly written file in moved dir", 1, [after_str isEqualToString:@"{\"phase\":\"after\"}"]);
    EXPECT_EQ("move logged once", 1, atomic_load(&g_msl_moved_told));

    NSError *err = nil;
    id<MTLLibrary> lib = lib_from_file([g_msl_dir stringByAppendingPathComponent:@"after_b.metallib"], &err);
    EXPECT_EQ("lib_from_file loads newly written metallib from moved dir", 1, lib != nil && [lib newFunctionWithName:@"main0"] != nil);

    // Re-reading shmap_read() into an absent directory must close the previous tracked fd and reset state.
    g_shmap = nil;
    setenv("DM_MSL_DIR", [sandbox stringByAppendingPathComponent:@"MissingApp"].fileSystemRepresentation, 1);
    shmap_read();
    EXPECT_EQ("re-read with missing dir resets g_msl_fd to -1", -1, g_msl_fd);
    EXPECT_EQ("re-read clears g_msl_fd_path", 1, g_msl_fd_path == nil);
    EXPECT_EQ("re-read resets g_msl_moved_told", 0, atomic_load(&g_msl_moved_told));
    errno = 0;
    int fd_alive = fcntl(tracked_fd, F_GETFD);
    EXPECT_EQ("re-read shmap_read closed prior tracked directory fd", 1, fd_alive == -1 && errno == EBADF);

    // Re-reading into the moved destination re-establishes tracking without logging a move.
    g_shmap = nil;
    setenv("DM_MSL_DIR", dst_root.fileSystemRepresentation, 1);
    shmap_read();
    EXPECT_EQ("re-read with moved dir opens new fd", 1, g_msl_fd >= 0);
    NSData *after2 = msl_data([g_msl_dir stringByAppendingPathComponent:@"after_b.json"]);
    NSString *after2_str = after2 ? [[NSString alloc] initWithData:after2 encoding:NSUTF8StringEncoding] : @"";
    EXPECT_EQ("re-read reads file directly from new dir", 1, [after2_str isEqualToString:@"{\"phase\":\"after\"}"]);
    EXPECT_EQ("direct read after re-read does not log move", 0, atomic_load(&g_msl_moved_told));

    if (g_msl_fd >= 0) { close(g_msl_fd); g_msl_fd = -1; }
    g_msl_fd_path = nil;
    [NSFileManager.defaultManager removeItemAtPath:sandbox error:nil];
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed != 0;
} }
