// The pipeline archive an app carries as <gpu>.mtl4x (build_ipa.sh; LiveContainer's signer failed on the real Mach-O) comes back as Documents/pso_bundle/<gpu>.mtl4a: a clone
// with the Mach-O magic restored, reused while the bundled file is the same, made again when it changes; a file without the DMPA mark is refused (nvn_mtl.m pso_bundle_unwrap)
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
static NSData *bytes(NSString *p) { return [NSData dataWithContentsOfFile:p]; }
int main(void) { @autoreleasepool {
    NSString *root = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"t_pso_unwrap.%d", getpid()]];
    NSString *app = [root stringByAppendingPathComponent:@"app/pso"], *docs = [root stringByAppendingPathComponent:@"docs"];
    [NSFileManager.defaultManager createDirectoryAtPath:app withIntermediateDirectories:YES attributes:nil error:nil];
    [NSFileManager.defaultManager createDirectoryAtPath:docs withIntermediateDirectories:YES attributes:nil error:nil];
    setenv("DM_PSO_USER", [docs stringByAppendingPathComponent:@"pso_user.tsv"].fileSystemRepresentation, 1);
    NSMutableData *body = [NSMutableData dataWithLength:3 << 20]; arc4random_buf(body.mutableBytes, body.length);
    NSMutableData *marked = [NSMutableData dataWithBytes:"DMPA" length:4]; [marked appendData:body];
    NSString *src = [app stringByAppendingPathComponent:@"applegpu_test.mtl4x"]; [marked writeToFile:src atomically:NO];
    CHECK(!pso_bundle_unwrap(app, @"applegpu_none"), "no bundled file for this GPU: nil");
    NSString *dst = pso_bundle_unwrap(app, @"applegpu_test");
    CHECK(dst && [dst hasSuffix:@"pso_bundle/applegpu_test.mtl4a"], "the archive comes back in Documents/pso_bundle");
    NSData *d = bytes(dst); const uint8_t magic[4] = {0xcf, 0xfa, 0xed, 0xfe};
    CHECK(d.length == marked.length && !memcmp(d.bytes, magic, 4), "its Mach-O magic is restored");
    CHECK(d.length == marked.length && !memcmp((const uint8_t *)d.bytes + 4, body.bytes, body.length), "the rest is the bundled file's bytes");
    CHECK([bytes(src) isEqualToData:marked], "the bundled file is left as it was");
    struct stat a, b; stat(dst.fileSystemRepresentation, &a); usleep(20000);
    NSString *again = pso_bundle_unwrap(app, @"applegpu_test"); stat(again.fileSystemRepresentation, &b);
    CHECK([again isEqualToString:dst] && a.st_ino == b.st_ino, "a second start reuses the clone");
    [marked replaceBytesInRange:NSMakeRange(8, 4) withBytes:"NEW!"]; usleep(20000); [marked writeToFile:src atomically:YES];
    NSString *third = pso_bundle_unwrap(app, @"applegpu_test"); NSData *t = bytes(third);
    CHECK(t.length == marked.length && !memcmp((const uint8_t *)t.bytes + 8, "NEW!", 4) && !memcmp(t.bytes, magic, 4), "a changed bundled file is cloned again");
    NSString *plain = [app stringByAppendingPathComponent:@"applegpu_plain.mtl4x"]; [body writeToFile:plain atomically:NO];
    CHECK(!pso_bundle_unwrap(app, @"applegpu_plain"), "a file without the DMPA mark is refused");
    CHECK(![NSFileManager.defaultManager fileExistsAtPath:[docs stringByAppendingPathComponent:@"pso_bundle/applegpu_plain.mtl4a"]], "and leaves no archive behind");
    [NSFileManager.defaultManager removeItemAtPath:root error:nil];
    printf("t_pso_unwrap: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
