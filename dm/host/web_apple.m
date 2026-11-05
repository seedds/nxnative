// Offline HTML pages in a web view (front.h front_web_*; hle_web.c drives them): a WKWebView over the game picture, on the Mac (front_mac.m) and the iPhone (front_ios.m) alike.
// The page is served from its HtmlDocument folder through a scheme of its own (dmweb://doc/<document path>), so its scripts may read its other files (the pause menu's sounds
// are XMLHttpRequests). A script at document start gives it the Switch browser's JavaScript extension and controller (SHIM below): window.nx (addEventListener("message"),
// sendMessage, endApplet, the footer's setAssign / unsetAssign, anything else a logged no-op), navigator.getGamepads as the browser maps the controller (the standard mapping:
// 0 B, 1 A, 2 Y, 3 X, 4 L, 5 R, 6 ZL, 7 ZR, 8 −, 9 +, 10 / 11 stick presses, 12-15 up down left right), and key events: the D-pad and the left stick are the arrow keys with key
// repeat, A is keyCode 32 (as the pause menu reads it) and clicks the focused link or button when the page does not take it. B goes back, or ends the page from its first one
// (exit reason 1), unless the page took B over with nx.footer. The page ends when it loads a URL under its callback URL (exit reason 3); other URLs outside the document are refused.
// Input: hle_hid.c's state while the page is shown (dm_input_peek: the keyboard, every controller, DM_CTL), sent to the page 60 times a second; buttons held when it appears count
// only once released. A click or touch on the page focuses what it lands on and presses A there. The page is transparent where it draws nothing, over the game's picture; its
// 1280x720 is fitted to the game's 16:9 picture. Its sounds follow the host's audio (DM_AUDIO; silent on the Mac by default)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
typedef UIView DmView;
#else
#import <Cocoa/Cocoa.h>
typedef NSView DmView;
#endif
#import <WebKit/WebKit.h>
#include "dm.h"
#include "dm_input.h"
#include "front.h"

void front_web_attach(DmView *box);   // front_mac.m / front_ios.m: puts the page's view over the game picture (main thread)
void front_web_detach(DmView *box);   // and takes it away again, handing the keyboard focus back to the game

static NSString *const SHIM = @R"JS(
(function () {
  if (window.__dm) return;
  var cfg = window.__dm_cfg || {};
  var post = function (m) { try { window.webkit.messageHandlers.dm.postMessage(m); } catch (e) {} };
  ['log', 'warn', 'error'].forEach(function (k) {
    var f = console[k];
    console[k] = function () { post({k: 'log', d: k + ': ' + Array.prototype.map.call(arguments, String).join(' ')}); return f.apply(console, arguments); };
  });
  window.addEventListener('error', function (e) { post({k: 'log', d: 'error: ' + e.message + ' (' + e.filename + ':' + e.lineno + ')'}); });
  var seen = {};
  var noop = function (name) { return function () { if (!seen[name]) { seen[name] = 1; post({k: 'log', d: 'nx.' + name + ' is not implemented by the host; ignored'}); } }; };
  var stub = function (o, prefix) {
    return new Proxy(o, {get: function (t, p) { return p in t || typeof p !== 'string' || p === 'then' || p === 'toJSON' ? t[p] : noop(prefix + p); }});
  };
  // The footer's buttons: true = the browser's own action (B: back), false = none (unsetAssign), a function = the page's
  var assign = {B: true};
  var listeners = {};
  var nx = {
    addEventListener: function (t, f) { (listeners[t] = listeners[t] || []).push(f); },
    removeEventListener: function (t, f) { listeners[t] = (listeners[t] || []).filter(function (x) { return x !== f; }); },
    sendMessage: function (s) { post({k: 'msg', d: String(s)}); return true; },
    endApplet: function () { post({k: 'end', r: 0}); },
    setCursorScrollSpeed: function () {},
    footer: stub({
      setAssign: function (b, label, fn) { assign[b] = typeof fn === 'function' ? fn : true; },
      unsetAssign: function (b) { assign[b] = false; },
      setFixed: function () {}
    }, 'footer.')
  };
  window.nx = stub(nx, '');

  // Sounds follow the host's audio: muted, every connection to the speakers goes through a gain of 0 (the sounds still play and end, for pages that wait for them)
  if (cfg.muted && window.AudioNode) {
    var connect = AudioNode.prototype.connect, quiet = new WeakMap();
    AudioNode.prototype.connect = function (dst) {
      if (dst instanceof AudioDestinationNode) {
        var g = quiet.get(dst.context);
        if (!g) { g = dst.context.createGain(); g.gain.value = 0; connect.call(g, dst); quiet.set(dst.context, g); }
        arguments[0] = g;
      }
      return connect.apply(this, arguments);
    };
    document.addEventListener('play', function (e) { e.target.muted = true; }, true);
  }
  // The page's look as on the Switch: no focus ring when it asks for none, a sans-serif default with the button icons of the Nintendo extension font (the host's stand-in,
  // at their private-use code points), and its :focus rules on the focused element whether or not the window is focused (WebKit matches :focus only in the key window,
  // and on the iPhone the web view never takes the keyboard): each such rule is copied for [data-dm-focus], which follows the focus
  var style = function () {
    var m = document.querySelector('meta[name="focus-ring-visibility"]'), s = document.createElement('style');
    s.textContent = '@font-face{font-family:"dm-nx-ext";src:url("/__dm_font/nintendo_ext.ttf");unicode-range:U+E000-E1FF}' +
      'html{font-family:"dm-nx-ext",-apple-system,"Helvetica Neue",Helvetica,Arial,sans-serif;-webkit-tap-highlight-color:transparent;-webkit-user-select:none;-webkit-touch-callout:none}' +
      (m && m.content === 'hidden' ? ':focus,[data-dm-focus]{outline:none!important}' : '');
    (document.head || document.documentElement).insertBefore(s, (document.head || document.documentElement).firstChild);
  };
  var mark = function (el) {
    var o = document.querySelectorAll('[data-dm-focus]');
    for (var i = 0; i < o.length; i++) if (o[i] !== el) o[i].removeAttribute('data-dm-focus');
    if (el && el !== document.body && el !== document.documentElement && el.setAttribute) el.setAttribute('data-dm-focus', '');
  };
  var mirror = function () {
    var out = [];
    var walk = function (rules) {
      for (var i = 0; i < rules.length; i++) {
        var r = rules[i];
        if (r.selectorText && r.selectorText.indexOf(':focus') >= 0) out.push(r.cssText.replace(/:focus(?![-\w])/g, '[data-dm-focus]'));
        else if (r.cssRules && r.media) { var k = out.length; walk(r.cssRules); if (out.length > k) out.splice(k, 0, '@media ' + r.media.mediaText + '{'), out.push('}'); }
      }
    };
    for (var i = 0; i < document.styleSheets.length; i++) { try { walk(document.styleSheets[i].cssRules); } catch (e) {} }
    var s = document.createElement('style'); s.textContent = out.join('\n'); document.head.appendChild(s);
    mark(document.activeElement);
  };
  document.addEventListener('focus', function (e) { mark(e.target); }, true);
  document.addEventListener('blur', function () { setTimeout(function () { mark(document.activeElement); }, 0); }, true);
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', style); else style();
  window.addEventListener('load', mirror);

  // The controller: host bits (nn::hid::NpadButton: A0 B1 X2 Y3 LS4 RS5 L6 R7 ZL8 ZR9 +10 −11 left12 up13 right14 down15) in the standard mapping's order
  var STD = [1, 0, 3, 2, 6, 7, 8, 9, 11, 10, 4, 5, 13, 15, 12, 14];
  var cur = 0, unread = 0, axes = [0, 0, 0, 0];
  var pad = function () {
    var b = cur | unread; unread = 0;
    return {id: 'Nintendo Switch Controller', index: 0, connected: true, mapping: 'standard', timestamp: performance.now(),
            buttons: STD.map(function (k) { var p = (b >> k & 1) === 1; return {pressed: p, touched: p, value: p ? 1 : 0}; }), axes: axes.slice()};
  };
  try { Object.defineProperty(Navigator.prototype, 'getGamepads', {configurable: true, value: function () { return [pad(), null, null, null]; }}); } catch (e) {}

  var KEYS = {37: ['ArrowLeft', 'ArrowLeft'], 38: ['ArrowUp', 'ArrowUp'], 39: ['ArrowRight', 'ArrowRight'], 40: ['ArrowDown', 'ArrowDown'], 32: [' ', 'Space']};
  var target = function () { var a = document.activeElement; return a && a !== document.documentElement ? a : document.body || document.documentElement; };
  var key = function (type, code, repeat, at) {
    var e = new KeyboardEvent(type, {key: KEYS[code][0], code: KEYS[code][1], bubbles: true, cancelable: true, repeat: !!repeat});
    Object.defineProperty(e, 'keyCode', {get: function () { return code; }});
    Object.defineProperty(e, 'which', {get: function () { return code; }});
    var t = at || target(), r = t.dispatchEvent(e);
    if (cfg.debug && type === 'keydown') { var a = document.activeElement; post({k: 'log', d: 'key ' + code + ' at ' + (t.id || t.tagName) + ' -> focus ' + (a && (a.id || a.tagName)) + (a && a.matches(':focus') ? ' (:focus)' : ' (not :focus)') + ' hasFocus ' + document.hasFocus()}); }
    return r;
  };
  var clickable = function (el) { return el && el.closest && el.closest('a[href],button,input,select,textarea,label,summary,[onclick]'); };
  var pressA = function (at, fromClick) {
    var el = at || target(), go = key('keydown', 32, false, el);
    key('keyup', 32, false, el);
    if (go && !fromClick && clickable(el)) el.click();
  };
  var footer = function (b) {
    var a = assign[b];
    if (typeof a === 'function') { a(); return; }
    if (a !== true || b !== 'B') return;
    if (history.length > 1) history.back(); else post({k: 'end', r: 1});
  };
  // Arrow keys from the D-pad and the left stick (half way), repeated after `delay` frames every `pulse` frames while held
  var DIRS = [[37, 12, 0, -1], [38, 13, 1, -1], [39, 14, 0, 1], [40, 15, 1, 1]];   // key, host bit, axis, its sign (the page's axes: right and down positive)
  var delay = cfg.repeat_delay || 30, pulse = cfg.repeat_pulse || 6, held = {};
  var dirs = function (b) {
    var d = 0;
    DIRS.forEach(function (x, i) { if ((b >> x[1] & 1) || axes[x[2]] * x[3] > 0.5) d |= 1 << i; });
    return d;
  };
  var lastDirs = 0;
  window.__dm = {
    pad: function (b, lx, ly, rx, ry) {
      var was = cur, down = b & ~was;
      cur = b; unread |= down;
      axes = [lx / 32767, -ly / 32767, rx / 32767, -ry / 32767];
      var d = dirs(b), now = performance.now();
      DIRS.forEach(function (x, i) {
        var on = d >> i & 1, before = lastDirs >> i & 1;
        if (on && !before) { held[x[0]] = now + delay * 1000 / 60; key('keydown', x[0]); }
        if (!on && before) { delete held[x[0]]; key('keyup', x[0]); }
      });
      lastDirs = d;
      if (down & 1) pressA();
      if (down & 2) footer('B');
      if (down & 4) footer('X');
      if (down & 8) footer('Y');
      if (down & 0x400) footer('Plus');
      if (down & 0x800) footer('Minus');
    },
    recv: function (s) { (listeners.message || []).forEach(function (f) { try { f({type: 'message', data: s}); } catch (e) { console.error(e && e.message); } }); },
    show: function () { lastDirs = 0; held = {}; cur = unread = 0; }
  };
  var tick = function () {
    var now = performance.now();
    Object.keys(held).forEach(function (k) { if (now >= held[k]) { held[k] = now + pulse * 1000 / 60; key('keydown', +k, true); } });
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
  // A click or a touch: what it lands on takes the focus and A is pressed there (pages made for the controller act on A alone)
  document.addEventListener('click', function (e) {
    var el = clickable(e.target) || (e.target && e.target.closest && e.target.closest('[tabindex]'));
    if (!e.isTrusted || !el) return;
    if (el.focus) el.focus();
    pressA(el, true);
  }, true);
})();
)JS";

@interface DmWebBox : DmView <WKNavigationDelegate>
@property (nonatomic, strong) WKWebView *web;
@property (nonatomic, copy) NSString *dir, *callback, *top;   // top: the document path's first folder
@property (nonatomic) BOOL shown, loaded, closed;
@property (nonatomic, strong) NSMutableArray<NSString *> *pending;   // content messages sent before the page loaded
@property (nonatomic, strong) NSTimer *timer;
@property (nonatomic) uint64_t mask, last_b; @property (nonatomic) int32_t last_ax0, last_ax1, last_ax2, last_ax3;
@end
// The configuration keeps its scheme and message handlers: they reach the page's view weakly, or every page shown would stay in memory with its view
@interface DmWebRelay : NSObject <WKScriptMessageHandler, WKURLSchemeHandler>
@property (nonatomic, weak) DmWebBox *box;
@end
@interface DmWebBox ()
- (void)serve:(id<WKURLSchemeTask>)t;
- (void)script:(WKScriptMessage *)m;
@end
@implementation DmWebRelay
- (void)webView:(WKWebView *)wv startURLSchemeTask:(id<WKURLSchemeTask>)t {
    (void)wv; DmWebBox *b = _box;
    if (b) [b serve:t];
    else { [t didFailWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCancelled userInfo:nil]]; }
}
- (void)webView:(WKWebView *)wv stopURLSchemeTask:(id<WKURLSchemeTask>)t { (void)wv; (void)t; }
- (void)userContentController:(WKUserContentController *)uc didReceiveScriptMessage:(WKScriptMessage *)m { (void)uc; [_box script:m]; }
@end
static DmWebBox *g_box;          // the page shown (main thread)
static _Atomic int g_touch;      // a page is shown: touches on no control of the iPhone's gamepad go to it (front_web_touch)
int front_web_touch(void) { return g_touch; }

static NSString *json(NSString *s) {   // a JavaScript string literal
    NSData *d = [NSJSONSerialization dataWithJSONObject:@[s ?: @""] options:0 error:nil];
    NSString *a = d ? [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] : nil;
    return a ? [a substringWithRange:NSMakeRange(1, a.length - 2)] : @"\"\"";   // a string the serializer refuses reads as empty
}
static NSString *mime(NSString *ext) {
    NSDictionary *m = @{@"html": @"text/html", @"htm": @"text/html", @"css": @"text/css", @"js": @"text/javascript", @"json": @"application/json", @"png": @"image/png",
                        @"jpg": @"image/jpeg", @"jpeg": @"image/jpeg", @"gif": @"image/gif", @"webp": @"image/webp", @"svg": @"image/svg+xml", @"wav": @"audio/wav",
                        @"mp3": @"audio/mpeg", @"ogg": @"audio/ogg", @"m4a": @"audio/mp4", @"mp4": @"video/mp4", @"webm": @"video/webm", @"woff": @"font/woff",
                        @"woff2": @"font/woff2", @"ttf": @"font/ttf", @"otf": @"font/otf", @"txt": @"text/plain", @"xml": @"application/xml"};
    return m[ext.lowercaseString] ?: @"application/octet-stream";
}

@implementation DmWebBox
- (instancetype)initWithDir:(NSString *)dir callback:(NSString *)cb opts:(dm_web_opts)o {
    if (!(self = [super initWithFrame:CGRectZero])) return nil;
    _dir = dir; _callback = cb; _pending = [NSMutableArray array];
    WKWebViewConfiguration *c = [WKWebViewConfiguration new];
    DmWebRelay *relay = [DmWebRelay new]; relay.box = self;
    [c setURLSchemeHandler:relay forURLScheme:@"dmweb"];
    c.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;
#if TARGET_OS_IPHONE
    c.allowsInlineMediaPlayback = YES;
#endif
    const char *ae = getenv("DM_AUDIO"); int sound = ae ? atoi(ae) != 0 : TARGET_OS_IPHONE;   // as hle_audio.c real_audio
    const char *dbg = getenv("DM_WEB_LOG");   // DM_WEB_LOG=1: each key the page is given and where the focus is after it
    NSString *cfg = [NSString stringWithFormat:@"window.__dm_cfg={muted:%s,repeat_delay:%u,repeat_pulse:%u,debug:%s};", sound ? "false" : "true", o.repeat_delay, o.repeat_pulse, dbg && atoi(dbg) ? "true" : "false"];
    [c.userContentController addUserScript:[[WKUserScript alloc] initWithSource:[cfg stringByAppendingString:SHIM] injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:NO]];
    [c.userContentController addScriptMessageHandler:relay name:@"dm"];
    _web = [[WKWebView alloc] initWithFrame:CGRectMake(0, 0, 1280, 720) configuration:c];
    _web.navigationDelegate = self;
#if TARGET_OS_IPHONE
    _web.opaque = NO; _web.backgroundColor = UIColor.clearColor; _web.scrollView.backgroundColor = UIColor.clearColor;
    _web.scrollView.scrollEnabled = NO; _web.scrollView.bounces = NO; _web.scrollView.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;
    self.backgroundColor = UIColor.clearColor; self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
#else
    [_web setValue:@NO forKey:@"drawsBackground"];   // transparent where the page draws nothing
    self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
#endif
    [self addSubview:_web];
    [self setHiddenState:YES];
    return self;
}
- (void)setHiddenState:(BOOL)h {   // hidden: not drawn and takes no input, but still in the window, so its scripts and timers run at full speed while it loads
#if TARGET_OS_IPHONE
    self.alpha = h ? 0 : 1;
#else
    self.alphaValue = h ? 0 : 1;
#endif
}
// The page's 1280x720 fitted into the 16:9 picture the present centres in the view
- (void)place {
    CGSize b = self.bounds.size; if (b.width < 1 || b.height < 1) return;
    double w = b.width, h = b.height; if (w / h > 16.0 / 9.0) w = h * 16.0 / 9.0; else h = w * 9.0 / 16.0;
    CGRect r = CGRectMake(floor((b.width - w) / 2), floor((b.height - h) / 2), w, h);
#if TARGET_OS_IPHONE
    _web.transform = CGAffineTransformIdentity; _web.bounds = CGRectMake(0, 0, 1280, 720);   // laid out at the Switch's size and scaled, so the page's viewport is 1280 wide
    _web.center = CGPointMake(CGRectGetMidX(r), CGRectGetMidY(r)); _web.transform = CGAffineTransformMakeScale(w / 1280, h / 720);
#else
    _web.frame = r; _web.pageZoom = w / 1280;
#endif
}
#if TARGET_OS_IPHONE
- (void)layoutSubviews { [super layoutSubviews]; [self place]; }
- (UIView *)hitTest:(CGPoint)p withEvent:(UIEvent *)e { return _shown ? [super hitTest:p withEvent:e] : nil; }
#else
- (void)resizeSubviewsWithOldSize:(NSSize)s { [super resizeSubviewsWithOldSize:s]; [self place]; }
- (NSView *)hitTest:(NSPoint)p { return _shown ? [super hitTest:p] : nil; }
#endif

// The document's files; anything else is not served
- (void)serve:(id<WKURLSchemeTask>)t {
    NSURL *u = t.request.URL; NSString *rel = [u.path stringByStandardizingPath];
    while ([rel hasPrefix:@"/"]) rel = [rel substringFromIndex:1];
    NSString *f = [_dir stringByAppendingPathComponent:rel]; NSData *d = nil;
    if ([rel isEqualToString:@"__dm_font/nintendo_ext.ttf"]) {   // the Nintendo extension font (button icons): the host's stand-in, as the shared font service gives it (hle_sdk16.c)
        const char *e = getenv("DM_SHARED_FONT_EXT"), *fd = getenv("DM_FONT_DIR");
        f = (e ? @(e) : fd ? [@(fd) stringByAppendingPathComponent:@"nintendo_ext.ttf"] : nil) ?: @"";   // @"": no path, or one that is not UTF-8, is not found
        d = [NSData dataWithContentsOfFile:f];
    } else if (rel.length && ![rel containsString:@".."] && [f hasPrefix:_dir]) {
        d = [NSData dataWithContentsOfFile:f];
        // A relative URL that climbs out of the document's folder (the pause menu's stylesheet, css/index.css, names ../../img/ for the images in the folder's img/): the Switch
        // finds it inside the document's folder, and so does the host
        if (!d && _top.length && ![rel hasPrefix:[_top stringByAppendingString:@"/"]]) {
            NSString *g = [[_dir stringByAppendingPathComponent:_top] stringByAppendingPathComponent:rel];
            if ((d = [NSData dataWithContentsOfFile:g])) { static int n; if (n++ < 4) LOG("web: %s found in the document's folder as %s/%s", u.absoluteString.UTF8String, _top.UTF8String, rel.UTF8String); }
        }
    }
    NSHTTPURLResponse *r = [[NSHTTPURLResponse alloc] initWithURL:u statusCode:d ? 200 : 404 HTTPVersion:@"HTTP/1.1"
                                                     headerFields:@{@"Content-Type": mime(f.pathExtension), @"Content-Length": [NSString stringWithFormat:@"%lu", (unsigned long)d.length]}];
    if (!d) LOG("[RED] web: the page asked for %s, which its document does not have", u.absoluteString.UTF8String);
    [t didReceiveResponse:r]; [t didReceiveData:d ?: [NSData data]]; [t didFinish];   // NOLINT(clang-analyzer-nullability.NullablePassedToNonnull): the request's own URL and a constant status line always make a response
}

- (void)webView:(WKWebView *)wv decidePolicyForNavigationAction:(WKNavigationAction *)a decisionHandler:(void (^)(WKNavigationActionPolicy))done {
    (void)wv; NSString *u = a.request.URL.absoluteString;
    if (_callback.length && [u hasPrefix:_callback]) {   // the page's end: the URL is its last URL
        done(WKNavigationActionPolicyCancel);
        if (!_closed) { _closed = YES; [self stopInput]; [self setHiddenState:YES]; _shown = NO; g_touch = 0; dm_web_ended(3, u.UTF8String); }
        return;
    }
    NSString *s = a.request.URL.scheme.lowercaseString;
    if ([s isEqualToString:@"dmweb"] || [s isEqualToString:@"about"] || [s isEqualToString:@"data"] || [s isEqualToString:@"blob"]) { done(WKNavigationActionPolicyAllow); return; }
    LOG("web: the page went to %s: refused (offline pages stay in their document)", u.UTF8String);
    done(WKNavigationActionPolicyCancel);
}
- (void)webView:(WKWebView *)wv didFinishNavigation:(WKNavigation *)n {
    (void)wv; (void)n; if (_loaded) return;
    _loaded = YES;
    for (NSString *m in _pending) [self deliver:m];
    [_pending removeAllObjects];
}
- (void)failed:(NSError *)e { if (!_loaded && !_closed) dm_web_failed(e.localizedDescription.UTF8String); }
- (void)webView:(WKWebView *)wv didFailProvisionalNavigation:(WKNavigation *)n withError:(NSError *)e { (void)wv; (void)n; [self failed:e]; }
- (void)webView:(WKWebView *)wv didFailNavigation:(WKNavigation *)n withError:(NSError *)e { (void)wv; (void)n; [self failed:e]; }
- (void)webViewWebContentProcessDidTerminate:(WKWebView *)wv { (void)wv; LOG("[RED] web: the page's process ended"); if (!_closed) { _closed = YES; [self stopInput]; dm_web_ended(4, NULL); } }

- (void)script:(WKScriptMessage *)m {
    if (_closed || ![m.body isKindOfClass:NSDictionary.class]) return;
    NSDictionary *b = m.body; NSString *k = b[@"k"];
    if ([k isEqualToString:@"msg"]) { const char *s = [b[@"d"] description].UTF8String ?: ""; dm_web_message(s, strlen(s)); }
    else if ([k isEqualToString:@"end"]) { _closed = YES; [self stopInput]; [self setHiddenState:YES]; _shown = NO; g_touch = 0; dm_web_ended((uint32_t)[b[@"r"] intValue], NULL); }
    else if ([k isEqualToString:@"log"]) { static int n; if (n++ < 200) LOG("web: page %s", [b[@"d"] description].UTF8String); }
}
- (void)deliver:(NSString *)m { [_web evaluateJavaScript:[NSString stringWithFormat:@"window.__dm&&__dm.recv(%@)", json(m)] completionHandler:nil]; }
- (void)send:(NSString *)m { if (_loaded) [self deliver:m]; else [_pending addObject:m]; }

// Input while shown: hle_hid.c's state (the keyboard, every controller, DM_CTL) at 60 Hz, sent when it changes; buttons held as the page appears are left out until released
- (void)poll {
    uint64_t b; int32_t a[4]; dm_input_peek(&b, a);
    _mask &= b; b &= ~_mask;
    for (int i = 0; i < 4; i++) if (abs(a[i]) < DM_STICK_MAX / 8) a[i] = 0;   // a resting stick
    if (b == _last_b && a[0] == _last_ax0 && a[1] == _last_ax1 && a[2] == _last_ax2 && a[3] == _last_ax3) return;
    _last_b = b; _last_ax0 = a[0]; _last_ax1 = a[1]; _last_ax2 = a[2]; _last_ax3 = a[3];
    [_web evaluateJavaScript:[NSString stringWithFormat:@"window.__dm&&__dm.pad(%llu,%d,%d,%d,%d)", (unsigned long long)b, a[0], a[1], a[2], a[3]] completionHandler:nil];
}
- (void)startInput {
    uint64_t b; int32_t a[4]; dm_input_peek(&b, a); _mask = b; _last_b = 0; _last_ax0 = _last_ax1 = _last_ax2 = _last_ax3 = 0;
    [_web evaluateJavaScript:@"window.__dm&&__dm.show()" completionHandler:nil];
    __weak DmWebBox *me = self;
    _timer = [NSTimer timerWithTimeInterval:1.0 / 60 repeats:YES block:^(NSTimer *t) { (void)t; [me poll]; }];
    [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
}
- (void)stopInput { [_timer invalidate]; _timer = nil; }
- (void)close {
    _closed = YES; [self stopInput]; _shown = NO; g_touch = 0;
    [_web.configuration.userContentController removeScriptMessageHandlerForName:@"dm"];
    [_web stopLoading]; _web.navigationDelegate = nil;
    front_web_detach(self); [self removeFromSuperview];
}
@end

int front_web_open(const char *dir, const char *path, const char *callback, int hidden, const dm_web_opts *opts) {
    if (!front_metal_layer()) return -1;   // no window: the stand-in answers
    NSString *d = @(dir), *p = @(path), *cb = @(callback); dm_web_opts o = opts ? *opts : (dm_web_opts){0, 0};
    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_box) { [g_box close]; g_box = nil; }
        DmWebBox *box = g_box = [[DmWebBox alloc] initWithDir:d callback:cb opts:o];
        NSArray *parts = [p componentsSeparatedByString:@"/"]; if (parts.count > 1) box.top = parts[0];
        front_web_attach(box);
        [box place];
        NSString *q = [p stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLFragmentAllowedCharacterSet];
        [box.web loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:[@"dmweb://doc/" stringByAppendingString:q ?: @""]]]];   // NOLINT(clang-analyzer-nullability.NullablePassedToNonnull): percent-encoded, it always parses; a path that does not encode reaches the [RED] line of a page not found
        if (!hidden) { box.shown = YES; g_touch = 1; [box setHiddenState:NO]; [box startInput]; }
    });
    return 0;
}
void front_web_show(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        DmWebBox *b = g_box; if (!b || b.closed || b.shown) return;
        b.shown = YES; g_touch = 1; [b setHiddenState:NO];
#if !TARGET_OS_IPHONE
        [b.window makeFirstResponder:b.web];   // the page's focused element shows as focused only in the window's focused view (the game's key monitor still takes the keys)
#endif
        [b startInput];
    });
}
void front_web_send(const char *msg, size_t n) {
    NSString *m = [[NSString alloc] initWithBytes:msg length:n encoding:NSUTF8StringEncoding] ?: @"";
    dispatch_async(dispatch_get_main_queue(), ^{ if (g_box && !g_box.closed) [g_box send:m]; });
}
// DM_CTL "webshot <path>": the page as a PNG (Mac; a screenshot of the game has only the game's picture)
void front_web_shot(const char *path) {
#if !TARGET_OS_IPHONE
    NSString *p = @(path);
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!g_box) { LOG("webshot: no page"); return; }
        [g_box.web takeSnapshotWithConfiguration:nil completionHandler:^(NSImage *im, NSError *e) {
            NSData *tiff = im.TIFFRepresentation;
            NSBitmapImageRep *r = tiff ? [[NSBitmapImageRep alloc] initWithData:tiff] : nil;
            NSData *d = [r representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            if (![d writeToFile:p atomically:YES]) LOG("[RED] webshot: nothing written to %s (%s)", p.UTF8String, e.localizedDescription.UTF8String ?: "-");
        }];
    });
#else
    (void)path;
#endif
}
void front_web_close(void) { dispatch_async(dispatch_get_main_queue(), ^{ if (g_box) { [g_box close]; g_box = nil; } }); }
