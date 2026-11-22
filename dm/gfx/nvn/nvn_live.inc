// A Mac development run may invoke the packer's translator for an unmapped NVN1 stage.
// Keep its library and reflection in a separate cache and give the submit thread only
// completed entries. The game still owns the control and code while SetShaders runs.
#if TARGET_OS_OSX
#include <spawn.h>
#include <sys/wait.h>

static NSMutableDictionary<NSString *, id> *g_live_shaders;
static os_unfair_lock g_live_index = OS_UNFAIR_LOCK_INIT;
static pthread_mutex_t g_live_build = PTHREAD_MUTEX_INITIALIZER;
static NSString *g_live_python, *g_live_compiler, *g_live_cache;

static void live_configure(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *python = getenv("DM_NVN_RUNTIME_PYTHON");
        const char *compiler = getenv("DM_NVN_RUNTIME_COMPILER");
        const char *temp = getenv("DM_TEMP");
        if (python && *python && compiler && *compiler && temp && *temp) {
            g_live_python = @(python);
            g_live_compiler = @(compiler);
            g_live_cache = [@(temp) stringByAppendingPathComponent:@"nvn-shaders"];
            g_live_shaders = [NSMutableDictionary dictionary];
        }
    });
}

static NSDictionary<NSString *, NSString *> *live_lookup(NSString *key) {
    live_configure();
    if (!g_live_shaders) return nil;
    os_unfair_lock_lock(&g_live_index);
    id entry = g_live_shaders[key];
    os_unfair_lock_unlock(&g_live_index);
    return entry == NSNull.null ? nil : entry;
}

static NSString *live_artifact(NSString *name) {
    if (name.length <= 17 || [name characterAtIndex:16] != '_') return nil;
    NSString *directory = live_lookup([name substringToIndex:16])[@"directory"];
    return [directory stringByAppendingPathComponent:name];
}

static NSDictionary *live_build(NSString *key, const NVNshaderData *sd, uint32_t nctl, uint32_t ncode) {
    NSString *pattern = [NSTemporaryDirectory() stringByAppendingPathComponent:@"nxnative-nvn.XXXXXX"];
    char directory[PATH_MAX];
    if (![pattern getFileSystemRepresentation:directory maxLength:sizeof directory] || !mkdtemp(directory)) return nil;
    NSString *request = @(directory);
    NSDictionary *entry = nil;
    NSData *control = [NSData dataWithBytesNoCopy:(void *)sd->control length:nctl freeWhenDone:NO];
    NSData *code = [NSData dataWithBytesNoCopy:(void *)(uintptr_t)sd->data length:ncode freeWhenDone:NO];
    if ([control writeToFile:[request stringByAppendingPathComponent:@"control"] atomically:NO] &&
        [code writeToFile:[request stringByAppendingPathComponent:@"code"] atomically:NO]) {
        NSString *profile = [g_shmap_path stringByDeletingLastPathComponent];
        const char *argv[] = {g_live_python.fileSystemRepresentation, "-B", g_live_compiler.fileSystemRepresentation,
                             profile.fileSystemRepresentation, g_live_cache.fileSystemRepresentation,
                             DMSH_CHAIN, directory, key.UTF8String, NULL};
        extern char **environ;
        pid_t child;
        int err = posix_spawn(&child, argv[0], NULL, NULL, (char *const *)argv, environ);
        if (err) MLOG("[RED] NVN runtime translator: %s", strerror(err));
        else {
            int status; pid_t waited;
            do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
            if (waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                NSData *result = [NSData dataWithContentsOfFile:[request stringByAppendingPathComponent:@"result.json"]];
                id json = result ? [NSJSONSerialization JSONObjectWithData:result options:0 error:nil] : nil;
                if ([json isKindOfClass:NSDictionary.class] &&
                    ([json[@"stage"] isEqual:@"vertex"] || [json[@"stage"] isEqual:@"pixel"]) &&
                    [json[@"directory"] isKindOfClass:NSString.class]) entry = json;
            }
        }
    }
    [NSFileManager.defaultManager removeItemAtPath:request error:nil];
    return entry;
}

static void live_compile(NSString *key, const NVNshaderData *sd) {
    if (!g_live_shaders || !sd->control || !sd->data) return;
    uint32_t nctl, ncode;
    nvn_ctl_lens(sd->control, &nctl, &ncode);
    if (nctl < 0x760 || ncode < 0x80 || ncode >= (1u << 24) || !mtl_addr_known(sd->data, ncode)) return;
    uint32_t magic, sph;
    const uint8_t *code = (const uint8_t *)(uintptr_t)sd->data;
    memcpy(&magic, code, sizeof magic);
    memcpy(&sph, code + 0x30, sizeof sph);
    unsigned type = (sph >> 10) & 15;
    if (magic != 0x12345678 || (type != 0 && type != 1 && type != 5)) return;

    dm_host_mutex_lock(&g_live_build);
    os_unfair_lock_lock(&g_live_index);
    id saved = g_live_shaders[key];
    os_unfair_lock_unlock(&g_live_index);
    if (!saved) {
        // Translation runs in a host process. The guest's other threads must
        // be able to use this guest core while SetShaders waits for it.
        dm_host_work++;
        NSDictionary *entry = live_build(key, sd, nctl, ncode);
        dm_host_work--;
        os_unfair_lock_lock(&g_live_index);
        g_live_shaders[key] = entry ?: NSNull.null;
        os_unfair_lock_unlock(&g_live_index);
        if (entry) MLOG("runtime shader %s (%s) ready", key.UTF8String, [entry[@"stage"] UTF8String]);
    }
    pthread_mutex_unlock(&g_live_build);
}
#else
static NSDictionary *live_lookup(__unused NSString *key) { return nil; }
static NSString *live_artifact(__unused NSString *name) { return nil; }
#endif
