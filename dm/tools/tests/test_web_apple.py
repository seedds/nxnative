"""Exercise the real offline web helpers without a window, WebKit process or network."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
HOST = REPO / 'dm/host'

RUNNER = r'''
#import "web_apple.m"
#import <objc/runtime.h>
#include <assert.h>

static int failures;
void *front_metal_layer(void) { return (void *)1; }
void front_web_attach(DmView *box) { (void)box; abort(); }
void front_web_detach(DmView *box) { (void)box; }
void dm_web_failed(const char *why) { assert(why && *why); failures++; }
void dm_web_ended(uint32_t reason, const char *url) { (void)reason; (void)url; }
void dm_web_message(const char *msg, size_t n) { (void)msg; (void)n; }
void dm_input_peek(uint64_t *buttons, int32_t axes[4]) { *buttons = 0; memset(axes, 0, 4 * sizeof(*axes)); }

// Simulate the nullable Foundation results, restoring each method immediately.
static id no_result(id self, SEL cmd, ...) { (void)self; (void)cmd; return nil; }
static void with_nil(Class cls, SEL sel, void (^body)(void)) {
    Method m = class_getInstanceMethod(cls, sel);
    IMP old = method_setImplementation(m, (IMP)no_result);
    @try { body(); } @finally { method_setImplementation(m, old); }
}
static void drain(void) {
    __block BOOL done = NO;
    dispatch_async(dispatch_get_main_queue(), ^{ done = YES; });
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:2];
    while (!done && deadline.timeIntervalSinceNow > 0)
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    assert(done);
}
@interface Request : NSURLRequest
@property (nonatomic, strong) NSURL *testURL;
@end
@implementation Request
- (NSURL *)URL { return _testURL; }
@end
@interface Task : NSObject <WKURLSchemeTask>
@property (nonatomic, copy) NSURLRequest *request;
@property (nonatomic, strong) NSError *error;
@property (nonatomic, strong) NSHTTPURLResponse *response;
@property (nonatomic, strong) NSData *data;
@property (nonatomic) int finished, events;
@end
@implementation Task
- (void)didFailWithError:(NSError *)e { assert(!_events++); _error = e; }
- (void)didReceiveResponse:(NSURLResponse *)r { assert(!_events++ && r); _response = (id)r; }
- (void)didReceiveData:(NSData *)d { assert(_events++ == 1 && d); _data = d; }
- (void)didFinish { assert(_events++ == 2); _finished++; }
@end
@interface Page : NSObject
@property (nonatomic, copy) NSString *script;
@property (nonatomic, strong) NSImage *image;
@end
@implementation Page
- (void)evaluateJavaScript:(NSString *)s completionHandler:(id)handler { (void)handler; _script = s; }
- (void)takeSnapshotWithConfiguration:(id)c completionHandler:(void (^)(NSImage *, NSError *))done {
    (void)c; done(_image, nil);
}
@end
@interface ImageWithoutTIFF : NSImage
@end
@implementation ImageWithoutTIFF
- (NSData *)TIFFRepresentation { return nil; }
@end
static Task *task(NSURL *url) {
    Request *r = [Request new]; r.testURL = url;
    Task *t = [Task new]; t.request = r; return t;
}
int main(int argc, char **argv) {
    @autoreleasepool {
        assert(argc == 2);
        NSString *root = @(argv[1]);
        DmWebBox *box = [[DmWebBox alloc] initWithFrame:NSZeroRect];
        box.dir = root;
        Page *page = [Page new]; box.web = (id)page;
        assert([json(@"a\"\nb") isEqualToString:@"\"a\\\"\\nb\""]);
        [box deliver:@"hello"]; assert([page.script containsString:@"recv(\"hello\")"]);
        page.script = nil;
        with_nil(object_getClass(NSJSONSerialization.class), @selector(dataWithJSONObject:options:error:), ^{
            assert(!json(@"hello"));
            [box deliver:@"hello"]; assert(!page.script);
        });

        Task *missing = task(nil); [box serve:missing];
        assert(missing.error.code == NSURLErrorBadURL && missing.events == 1);
        Task *noPath = task([NSURL URLWithString:@"mailto:test@example.invalid"]); [box serve:noPath];
        assert(noPath.error.code == NSURLErrorBadURL && noPath.events == 1);
        NSData *body = [@"hello" dataUsingEncoding:NSUTF8StringEncoding];
        assert([body writeToFile:[root stringByAppendingPathComponent:@"index.html"] atomically:YES]);
        NSURL *url = [NSURL URLWithString:@"dmweb://doc/index.html"];
        Task *ok = task(url); [box serve:ok];
        assert(ok.response.statusCode == 200 && [ok.data isEqualToData:body] && ok.finished == 1);
        Task *notFound = task([NSURL URLWithString:@"dmweb://doc/missing.html"]); [box serve:notFound];
        assert(notFound.response.statusCode == 404 && notFound.data.length == 0 && notFound.finished == 1);
        // Invalid UTF-8 in either font override must fail before reading a path.
        const char *ext = getenv("DM_SHARED_FONT_EXT"), *dir = getenv("DM_FONT_DIR");
        char *savedExt = ext ? strdup(ext) : NULL, *savedDir = dir ? strdup(dir) : NULL;
        NSURL *fontURL = [NSURL URLWithString:@"dmweb://doc/__dm_font/nintendo_ext.ttf"];
        setenv("DM_SHARED_FONT_EXT", "\xff", 1);
        setenv("DM_FONT_DIR", root.UTF8String, 1);
        Task *badFont = task(fontURL); [box serve:badFont];
        assert(badFont.error.code == NSURLErrorBadURL && badFont.events == 1);
        unsetenv("DM_SHARED_FONT_EXT");
        setenv("DM_FONT_DIR", "\xff", 1);
        Task *badFontDir = task(fontURL); [box serve:badFontDir];
        assert(badFontDir.error.code == NSURLErrorBadURL && badFontDir.events == 1);
        // An explicit font path still takes precedence over the directory override.
        NSString *fontPath = [root stringByAppendingPathComponent:@"nintendo_ext.ttf"];
        assert([body writeToFile:fontPath atomically:YES]);
        setenv("DM_SHARED_FONT_EXT", fontPath.UTF8String, 1);
        Task *font = task(fontURL); [box serve:font];
        assert(font.response.statusCode == 200 && [font.data isEqualToData:body] && font.finished == 1);
        unsetenv("DM_SHARED_FONT_EXT");
        setenv("DM_FONT_DIR", root.UTF8String, 1);
        Task *fontDir = task(fontURL); [box serve:fontDir];
        assert(fontDir.response.statusCode == 200 && [fontDir.data isEqualToData:body] && fontDir.finished == 1);
        if (savedExt) { setenv("DM_SHARED_FONT_EXT", savedExt, 1); free(savedExt); }
        else unsetenv("DM_SHARED_FONT_EXT");
        if (savedDir) { setenv("DM_FONT_DIR", savedDir, 1); free(savedDir); }
        else unsetenv("DM_FONT_DIR");
        with_nil(NSHTTPURLResponse.class, @selector(initWithURL:statusCode:HTTPVersion:headerFields:), ^{
            Task *badResponse = task(url); [box serve:badResponse];
            assert(badResponse.error.code == NSURLErrorCannotParseResponse && badResponse.events == 1);
        });
        // No view is attached or WKWebView constructed when encoding the initial URL fails.
        with_nil(NSString.class, @selector(stringByAddingPercentEncodingWithAllowedCharacters:), ^{
            assert(front_web_open(argv[1], "index.html", "", 0, NULL) == 0);
            drain(); assert(failures == 1 && !g_box && !front_web_touch());
        });
        with_nil(object_getClass(NSURL.class), @selector(URLWithString:), ^{
            assert(front_web_open(argv[1], "index.html", "", 0, NULL) == 0);
            drain(); assert(failures == 2 && !g_box && !front_web_touch());
        });
        g_box = box;
        page.image = [ImageWithoutTIFF new];
        NSString *shot = [root stringByAppendingPathComponent:@"shot.png"];
        front_web_shot(shot.UTF8String); drain();
        assert(![NSFileManager.defaultManager fileExistsAtPath:shot]);
        g_box = nil;
    }
    return 0;
}
'''


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'requires Apple SDK and clang')
class WebAppleTests(unittest.TestCase):
    def test_nullable_results(self):
        with tempfile.TemporaryDirectory(prefix='web-apple-') as td:
            root = Path(td)
            # Use a real game profile, but no game assets or generated repository files.
            result = subprocess.run([sys.executable, str(REPO / 'dm/games/game.py'), 'header',
                                     '0100152000022000', str(root / 'game_profile.h')],
                                    cwd=REPO, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            (root / 'runner.m').write_text(RUNNER)
            result = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-UNDEBUG',
                                     '-Wno-deprecated-declarations', '-I' + str(root), '-I' + str(HOST),
                                     str(root / 'runner.m'), '-framework', 'Cocoa', '-framework', 'WebKit',
                                     '-o', str(root / 'runner')], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(root / 'runner'), td], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
