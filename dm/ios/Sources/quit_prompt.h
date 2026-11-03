// Quit to the launcher on iOS (LNPANDY/NX2Apple#421): the quit prompt front_ios.m draws over the game, and the touch steps of home_quit.h's state machine.
// Two ways open the prompt: Quit in the in-game menu (the gear at the top centre, GameMenu.swift), and holding a controller's Home button for 2 seconds
// (home_quit.h, as on the Mac). The prompt is the Mac's (home_quit_mac.h): a dim scrim, a card with "Quit <game>?", a note and two pills of one fixed width
// whatever the language, the primary one cream. A tap on Quit or the controller's A quits, a tap on Cancel or outside the card, B or a new Home press closes it.
// Core Animation and Core Text only, no UIKit, so tests/quit_prompt/quit_prompt_check.m lays it out, hit-tests it and draws it on the Mac.
#pragma once
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreText/CoreText.h>
#include "home_quit.h"

NS_ASSUME_NONNULL_BEGIN

// ---- The touch steps of the state machine (home_quit.h has the controller's) ----
// Quit was picked in the in-game menu: the prompt opens from IDLE or from a Home hold under way. home, a, b: the controllers' buttons now, so one already held does not count.
static inline hq_phase hq_touch_open(home_quit *q, int home, int a, int b) {
    if (q->phase == HQ_IDLE || q->phase == HQ_HOLDING) { q->phase = HQ_PROMPT; q->home = home; q->a_up = !a; q->b_up = !b; }
    return q->phase;
}
// Cancel or a tap outside the card: the prompt closes, and the controllers go back to the game once Home, A and B are up (hq_step's SETTLE).
static inline hq_phase hq_touch_cancel(home_quit *q) { if (q->phase == HQ_PROMPT) q->phase = HQ_SETTLE; return q->phase; }
// Quit: final, as A in the prompt.
static inline hq_phase hq_touch_quit(home_quit *q) { if (q->phase == HQ_PROMPT) q->phase = HQ_QUIT; return q->phase; }

// ---- The prompt's layout ----
// Sizes in points of a 720-high view, as home_quit_mac.h, times k: k follows the height between 1 and 2 and shrinks so the card fits a phone held upright.
#define QP_CARD_W 600.0
#define QP_ACTION_W 220.0   // every action pill is this wide in every language: the longest label of the six ("Abbrechen", "Annuler") fits at full size
#define QP_ACTION_H 56.0
#define QP_ACTION_GAP 16.0
#define QP_GLYPH 24.0
enum { QP_HIT_NONE = 0, QP_HIT_QUIT, QP_HIT_CANCEL, QP_HIT_OUTSIDE };

typedef struct {
    CGFloat k;
    CGRect card, title, note, quit, cancel;    // in the view's points, y down
    CGFloat titleSize, noteSize, labelSize[2];  // the label sizes of Quit and Cancel, each fitted to its pill
} qp_layout;

static inline CGFloat qp_scale(CGSize s) {
    CGFloat k = s.height / 720; k = k < 1 ? 1 : k > 2 ? 2 : k;
    CGFloat w = (s.width - 32) / QP_CARD_W, h = (s.height - 24) / 300;
    if (k > w) k = w;
    if (k > h) k = h;
    return k < 0.4 ? 0.4 : k;
}

static inline CTFontRef qp_font(CGFloat size, CGFloat weight) {   // the system font at a weight (-1 to 1, as UIFontWeight): +1 reference, the caller releases it
    CTFontRef base = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size, NULL);
    CTFontDescriptorRef d = CTFontCopyFontDescriptor(base);
    NSDictionary *attrs = @{(__bridge id)kCTFontTraitsAttribute: @{(__bridge id)kCTFontWeightTrait: @(weight)}};
    CTFontDescriptorRef w = CTFontDescriptorCreateCopyWithAttributes(d, (__bridge CFDictionaryRef)attrs);
    CTFontRef f = CTFontCreateWithFontDescriptor(w, size, NULL);
    CFRelease(w); CFRelease(d); CFRelease(base);
    return f;
}
static inline NSAttributedString *qp_text(NSString *s, CTFontRef f) { return [[NSAttributedString alloc] initWithString:s attributes:@{(__bridge id)kCTFontAttributeName: (__bridge id)f}]; }
static inline CGFloat qp_line_width(NSString *s, CGFloat size, CGFloat weight) {
    CTFontRef f = qp_font(size, weight);
    CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)qp_text(s, f));
    CGFloat w = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
    CFRelease(line); CFRelease(f);
    return ceil(w);
}
static inline CGFloat qp_wrapped_height(NSString *s, CGFloat size, CGFloat weight, CGFloat width) {
    if (!s.length) return 0;
    CTFontRef f = qp_font(size, weight);
    CTFramesetterRef fs = CTFramesetterCreateWithAttributedString((__bridge CFAttributedStringRef)qp_text(s, f));
    CGSize z = CTFramesetterSuggestFrameSizeWithConstraints(fs, CFRangeMake(0, 0), NULL, CGSizeMake(width, CGFLOAT_MAX), NULL);
    CFRelease(fs); CFRelease(f);
    return ceil(z.height) + 2;
}
// The font size that fits text into room on one line: size, or smaller down to 70 % of it (then the label is cut), as home_quit_mac.h's hq_fit
static inline CGFloat qp_fit(NSString *s, CGFloat size, CGFloat room) {
    CGFloat w = qp_line_width(s, size, 0.3);
    if (w <= room) return size;
    CGFloat f = floor(size * room / w * 10) / 10;
    return f < size * 0.7 ? size * 0.7 : f;
}
static inline CGFloat qp_label_room(CGFloat k, int glyphs) { return QP_ACTION_W * k - 2 * 20 * k - (glyphs ? (QP_GLYPH + 10) * k : 0); }

static inline qp_layout qp_layout_at(CGSize bounds, CGFloat k, NSString *title, NSString *note, NSString *quit, NSString *cancel, int glyphs) {
    qp_layout l; memset(&l, 0, sizeof l);
    l.k = k; l.titleSize = 34 * k; l.noteSize = 20 * k;
    CGFloat cw = QP_CARD_W * k, inner = cw - 96 * k;
    CGFloat th = qp_wrapped_height(title, l.titleSize, 0.4, inner), nh = qp_wrapped_height(note, l.noteSize, 0, inner);
    CGFloat ch = 40 * k + th + (nh > 0 ? 14 * k + nh : 0) + 32 * k + QP_ACTION_H * k + 36 * k;
    l.card = CGRectMake(floor((bounds.width - cw) / 2), floor((bounds.height - ch) / 2), cw, ch);
    l.title = CGRectMake(l.card.origin.x + 48 * k, l.card.origin.y + 40 * k, inner, th);
    l.note = CGRectMake(l.title.origin.x, CGRectGetMaxY(l.title) + (nh > 0 ? 14 * k : 0), inner, nh);
    CGFloat aw = QP_ACTION_W * k, x0 = CGRectGetMidX(l.card) - aw - QP_ACTION_GAP * k / 2, y = CGRectGetMaxY(l.note) + 32 * k;
    l.quit = CGRectMake(x0, y, aw, QP_ACTION_H * k);
    l.cancel = CGRectMake(x0 + aw + QP_ACTION_GAP * k, y, aw, QP_ACTION_H * k);
    CGFloat room = qp_label_room(k, glyphs);
    l.labelSize[0] = qp_fit(quit, 22 * k, room); l.labelSize[1] = qp_fit(cancel, 22 * k, room);
    return l;
}
// The layout for a view of this size: a card taller than the view (a long note on a small screen) takes a smaller k
static inline qp_layout qp_layout_make(CGSize bounds, NSString *title, NSString *note, NSString *quit, NSString *cancel, int glyphs) {
    CGFloat k = qp_scale(bounds);
    qp_layout l = qp_layout_at(bounds, k, title, note, quit, cancel, glyphs);
    for (int i = 0; i < 8 && l.card.size.height > bounds.height - 16 && k > 0.4; i++) {
        k = fmax(0.4, k * (bounds.height - 16) / l.card.size.height);
        l = qp_layout_at(bounds, k, title, note, quit, cancel, glyphs);
    }
    return l;
}
// What a touch at p is on: a pill (with a margin of 6 points at k 1), the card (nothing), or outside it (cancels)
static inline int qp_hit(const qp_layout *l, CGPoint p) {
    CGFloat e = 6 * l->k;
    if (CGRectContainsPoint(CGRectInset(l->quit, -e, -e), p)) return QP_HIT_QUIT;
    if (CGRectContainsPoint(CGRectInset(l->cancel, -e, -e), p)) return QP_HIT_CANCEL;
    if (CGRectContainsPoint(l->card, p)) return QP_HIT_NONE;
    return QP_HIT_OUTSIDE;
}

// ---- The layers ----
// The look is the launcher's console mode, as home_quit_mac.h: its background, surface and cream (LauncherPalette)
static inline CGColorRef qp_color(CGFloat r, CGFloat g, CGFloat b, CGFloat a) {   // autoreleased: valid while the caller's pool lives
    CGColorRef c = CGColorCreateSRGB(r, g, b, a);
    return (__bridge CGColorRef)CFBridgingRelease(c);
}
#define QP_BACKGROUND qp_color(0.055, 0.055, 0.06, 1)
#define QP_SURFACE qp_color(0.10, 0.10, 0.11, 1)
#define QP_CREAM qp_color(1, 0.94, 0.84, 1)

// Text broken into the lines Core Text lays it out in at this width, joined by line breaks: CATextLayer's own wrapping breaks at spaces only, so Chinese
// stayed one line and was cut (qp_wrapped_height measured two)
static inline NSString *qp_lines(NSString *s, CGFloat size, CGFloat weight, CGFloat width) {
    CTFontRef f = qp_font(size, weight);
    CTFramesetterRef fs = CTFramesetterCreateWithAttributedString((__bridge CFAttributedStringRef)qp_text(s, f));
    CGPathRef path = CGPathCreateWithRect(CGRectMake(0, 0, width, 100000), NULL);
    CTFrameRef frame = CTFramesetterCreateFrame(fs, CFRangeMake(0, 0), path, NULL);
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    for (id line in (__bridge NSArray *)CTFrameGetLines(frame)) {
        CFRange r = CTLineGetStringRange((__bridge CTLineRef)line);
        [out addObject:[[s substringWithRange:NSMakeRange((NSUInteger)r.location, (NSUInteger)r.length)] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]];
    }
    CFRelease(frame); CGPathRelease(path); CFRelease(fs); CFRelease(f);
    return [out componentsJoinedByString:@"\n"];
}
static inline CATextLayer *qp_text_layer(NSString *s, CGFloat size, CGFloat weight, CGColorRef color, CGRect frame, int wrap, CGFloat scale) {
    CATextLayer *t = [CATextLayer layer];
    CTFontRef f = qp_font(size, weight);
    t.font = f; t.fontSize = size; CFRelease(f);
    t.string = wrap ? qp_lines(s, size, weight, frame.size.width) : s; t.foregroundColor = color; t.alignmentMode = kCAAlignmentCenter; t.contentsScale = scale;
    t.wrapped = NO; t.truncationMode = wrap ? kCATruncationNone : kCATruncationEnd;
    t.frame = frame;
    return t;
}
// One pill: the glyph (a template image tinted fg, nil for none) and the label, centred
static inline CALayer *qp_action_layer(CGRect frame, NSString *label, CGFloat labelSize, CGImageRef _Nullable glyph, int primary, CGFloat k, CGFloat scale) {
    CALayer *pill = [CALayer layer];
    pill.frame = frame; pill.cornerRadius = 28 * k;
    if (primary) pill.backgroundColor = QP_CREAM;
    else { pill.borderWidth = 1; pill.borderColor = qp_color(1, 1, 1, 0.16); }
    CGColorRef fg = primary ? QP_BACKGROUND : qp_color(1, 1, 1, 0.8);
    CGFloat g = glyph ? QP_GLYPH * k : 0, gap = glyph ? 10 * k : 0, room = frame.size.width - 40 * k - g - gap;
    CGFloat tw = fmin(qp_line_width(label, labelSize, 0.3) + 2, room), th = ceil(labelSize * 1.3);
    CGFloat x = (frame.size.width - g - gap - tw) / 2, mid = frame.size.height / 2;
    if (glyph) {
        CALayer *mask = [CALayer layer]; mask.frame = CGRectMake(0, 0, g, g); mask.contents = (__bridge id)glyph; mask.contentsGravity = kCAGravityResizeAspect;
        CALayer *tint = [CALayer layer]; tint.frame = CGRectMake(x, mid - g / 2, g, g); tint.backgroundColor = fg; tint.mask = mask;
        [pill addSublayer:tint];
    }
    [pill addSublayer:qp_text_layer(label, labelSize, 0.3, fg, CGRectMake(x + g + gap, mid - th / 2, tw, th), 0, scale)];
    return pill;
}
// The prompt over the whole view: the scrim and the card. glyphs: the controller's A and B images (nil without a controller: the pills show the words only)
static inline CALayer *qp_prompt_layer(CGRect bounds, const qp_layout *l, NSString *title, NSString *note, NSString *quit, NSString *cancel,
                                       CGImageRef _Nullable aGlyph, CGImageRef _Nullable bGlyph, CGFloat scale, CALayer *_Nullable *_Nullable actions) {
    CGFloat k = l->k;
    CALayer *scrim = [CALayer layer]; scrim.frame = bounds; scrim.backgroundColor = qp_color(0, 0, 0, 0.6);
    CALayer *card = [CALayer layer]; card.frame = l->card; card.backgroundColor = QP_SURFACE; card.cornerRadius = 28 * k;
    card.borderWidth = 1; card.borderColor = qp_color(1, 1, 1, 0.10);
    [scrim addSublayer:card];
    [scrim addSublayer:qp_text_layer(title, l->titleSize, 0.4, qp_color(1, 1, 1, 1), l->title, 1, scale)];
    if (l->note.size.height > 0) [scrim addSublayer:qp_text_layer(note, l->noteSize, 0, qp_color(1, 1, 1, 0.6), l->note, 1, scale)];
    CALayer *row = [CALayer layer]; row.frame = bounds;
    [row addSublayer:qp_action_layer(l->quit, quit, l->labelSize[0], aGlyph, 1, k, scale)];
    [row addSublayer:qp_action_layer(l->cancel, cancel, l->labelSize[1], bGlyph, 0, k, scale)];
    [scrim addSublayer:row];
    if (actions) *actions = row;
    return scrim;
}

// The hold indicator at the top of the view while Home is held: a pill with a ring that fills, and its words. qp_hold_set moves the ring
static inline CALayer *qp_hold_layer(CGRect bounds, NSString *label, CGFloat scale, CAShapeLayer *_Nullable *_Nonnull ringOut) {
    CGFloat k = qp_scale(bounds.size), d = 26 * k, line = 3.5 * k, fs = 20 * k;
    CGFloat tw = qp_line_width(label, fs, 0.3) + 2, w = 18 * k + d + 12 * k + tw + 24 * k, h = 56 * k;
    CALayer *pill = [CALayer layer];
    pill.frame = CGRectMake(floor((bounds.size.width - w) / 2), 24 * k, w, h);
    pill.backgroundColor = qp_color(0.055, 0.055, 0.06, 0.92); pill.cornerRadius = h / 2;
    pill.borderWidth = 1; pill.borderColor = qp_color(1, 1, 1, 0.12);
    CGPathRef circle = CGPathCreateWithEllipseInRect(CGRectInset(CGRectMake(0, 0, d, d), line / 2, line / 2), NULL);
    CAShapeLayer *track = [CAShapeLayer layer], *ring = [CAShapeLayer layer];
    for (CAShapeLayer *s in @[track, ring]) { s.frame = CGRectMake(18 * k, (h - d) / 2, d, d); s.path = circle; s.fillColor = NULL; s.lineWidth = line; [pill addSublayer:s]; }
    CGPathRelease(circle);
    track.strokeColor = qp_color(1, 1, 1, 0.18);
    ring.strokeColor = QP_CREAM; ring.lineCap = kCALineCapRound; ring.strokeEnd = 0;
    ring.transform = CATransform3DMakeRotation(-M_PI_2, 0, 0, 1);   // from the top, clockwise (y down)
    CGFloat th = ceil(fs * 1.3);
    [pill addSublayer:qp_text_layer(label, fs, 0.3, qp_color(1, 1, 1, 0.9), CGRectMake(18 * k + d + 12 * k, (h - th) / 2, tw, th), 0, scale)];
    *ringOut = ring;
    return pill;
}
static inline void qp_hold_set(CAShapeLayer *ring, double fill) {
    [CATransaction begin]; [CATransaction setDisableActions:YES]; ring.strokeEnd = fill; [CATransaction commit];
}

NS_ASSUME_NONNULL_END
