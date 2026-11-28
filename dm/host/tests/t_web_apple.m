// Smoke test for web_apple.m's injected JS shim and DmWebBox script-message handler, without spawning a WKWebView WebContent process.
#import <JavaScriptCore/JavaScriptCore.h>
#import "../web_apple.m"
#include <assert.h>
#include <stdio.h>

static int ended, url_set;
static uint32_t last_reason;

void *front_metal_layer(void) { return NULL; }
void front_web_attach(DmView *box) { (void)box; }
void front_web_detach(DmView *box) { (void)box; }
void dm_input_peek(uint64_t *b, int32_t a[4]) { *b = 0; a[0] = a[1] = a[2] = a[3] = 0; }
void dm_web_message(const char *msg, size_t n) { (void)msg; (void)n; }
void dm_web_ended(uint32_t reason, const char *url) { ended++; last_reason = reason; url_set = url != NULL; }
void dm_web_failed(const char *why) { (void)why; }

@interface FakeScriptMessage : NSObject
@property (nonatomic, strong) id body;
@end
@implementation FakeScriptMessage
@end

static void run_js_close(NSString *js, uint32_t want_reason) {
    DmWebBox *box = [DmWebBox new];
    box.shown = YES; g_touch = 1;
    ended = 0; last_reason = 999; url_set = 1;

    JSContext *ctx = [JSContext new];
    __block NSString *exc = nil;
    ctx.exceptionHandler = ^(JSContext *c, JSValue *e) { (void)c; exc = e.toString; };
    ctx[@"__post"] = ^(id body) {
        FakeScriptMessage *m = [FakeScriptMessage new];
        m.body = body;
        [box script:(WKScriptMessage *)m];
    };
    [ctx evaluateScript:
        @"var window = this;"
         "window.addEventListener = function () {};"
         "var document = {readyState: 'loading', addEventListener: function () {}, querySelector: function () { return null; },"
         "                createElement: function () { return {}; }, documentElement: {insertBefore: function () {}, firstChild: null}};"
         "var Navigator = {prototype: {}};"
         "var requestAnimationFrame = function () {};"
         "window.webkit = {messageHandlers: {dm: {postMessage: __post}}};"];
    [ctx evaluateScript:SHIM];
    assert(!exc);
    [ctx evaluateScript:js];
    assert(!exc);
    assert(ended == 1 && last_reason == want_reason && !url_set);
    assert(box.closed && !box.shown && !front_web_touch());
}

int main(void) {
    @autoreleasepool {
        run_js_close(@"close();", 4);
        run_js_close(@"window.close();", 4);
        run_js_close(@"close(); close();", 4);
        run_js_close(@"nx.endApplet();", 0);
        puts("t_web_apple: close(), window.close(), duplicate close and nx.endApplet() passed");
    }
    return 0;
}
