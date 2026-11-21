// Patch Ocarina of Time's Common.Product.100.Nin_Ounce_NVN2.bntx for PlayStation (DualSense) and Xbox Series controller layouts.
// Takes the game's extracted BNTX, texture table from oot_pad_overlay.py, and contributor-drawn controller diagrams; writes two patched BNTX files.
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dm/gfx/tex/nvn_deswizzle.h"

static uint8_t *load_png_straight(const char *path, uint32_t *outW, uint32_t *outH) {
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)url, NULL);
    if (!src) return NULL;
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, NULL);
    CFRelease(src);
    if (!img) return NULL;
    uint32_t w = (uint32_t)CGImageGetWidth(img), h = (uint32_t)CGImageGetHeight(img);
    *outW = w; *outH = h;
    uint8_t *buf = calloc((size_t)w * h, 4);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(buf, w, h, 8, w * 4, cs, kCGBitmapByteOrderDefault | kCGImageAlphaPremultipliedLast);
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
    CGContextRelease(ctx); CGColorSpaceRelease(cs); CGImageRelease(img);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint8_t a = buf[i * 4 + 3];
        if (a > 0 && a < 255) {
            for (int c = 0; c < 3; c++) {
                int v = (buf[i * 4 + c] * 255 + a / 2) / a;
                buf[i * 4 + c] = (uint8_t)(v > 255 ? 255 : v);
            }
        } else if (a == 0) {
            buf[i * 4] = buf[i * 4 + 1] = buf[i * 4 + 2] = 255;
        }
    }
    return buf;
}

static void unpremult_white_or_rgb(uint8_t *buf, size_t n, BOOL forceWhiteRGB) {
    for (size_t i = 0; i < n; i++) {
        uint8_t a = buf[i * 4 + 3];
        if (forceWhiteRGB || a == 0) {
            buf[i * 4] = buf[i * 4 + 1] = buf[i * 4 + 2] = 255;
        } else if (a < 255) {
            for (int c = 0; c < 3; c++) {
                int v = (buf[i * 4 + c] * 255 + a / 2) / a;
                buf[i * 4 + c] = (uint8_t)(v > 255 ? 255 : v);
            }
        }
    }
}

typedef struct { uint8_t *rgba; uint32_t w, h; } ImgBuf;

static ImgBuf img_alloc(uint32_t w, uint32_t h, uint8_t *rgba) {
    return (ImgBuf){ .rgba = rgba, .w = w, .h = h };
}

static void draw_ps_symbol(CGContextRef ctx, char kind, CGFloat cx, CGFloat cy, CGFloat sz, CGFloat stroke, BOOL rounded,
                           CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
    CGContextSaveGState(ctx);
    CGContextSetRGBStrokeColor(ctx, r, g, b, a);
    CGContextSetRGBFillColor(ctx, r, g, b, a);
    CGContextSetLineWidth(ctx, stroke);
    CGContextSetLineCap(ctx, rounded ? kCGLineCapRound : kCGLineCapButt);
    CGContextSetLineJoin(ctx, rounded ? kCGLineJoinRound : kCGLineJoinMiter);
    CGFloat half = sz * 0.5;
    if (kind == 'X') {
        CGFloat d = half * 0.86;
        CGContextMoveToPoint(ctx, cx - d, cy - d); CGContextAddLineToPoint(ctx, cx + d, cy + d);
        CGContextMoveToPoint(ctx, cx - d, cy + d); CGContextAddLineToPoint(ctx, cx + d, cy - d);
        CGContextStrokePath(ctx);
    } else if (kind == 'O') {
        CGFloat rad = half * 0.90;
        CGContextAddEllipseInRect(ctx, CGRectMake(cx - rad, cy - rad, rad * 2, rad * 2));
        CGContextStrokePath(ctx);
    } else if (kind == 'S') {
        CGFloat d = half * 0.85;
        if (rounded) {
            CGPathRef p = CGPathCreateWithRoundedRect(CGRectMake(cx - d, cy - d, d * 2, d * 2), stroke * 0.45, stroke * 0.45, NULL);
            CGContextAddPath(ctx, p); CGPathRelease(p);
        } else {
            CGContextAddRect(ctx, CGRectMake(cx - d, cy - d, d * 2, d * 2));
        }
        CGContextStrokePath(ctx);
    } else if (kind == 'T') {
        CGFloat h_tri = half * 0.95;
        CGFloat top_y = cy + h_tri * 0.95;
        CGFloat bot_y = cy - h_tri * 0.78;
        CGFloat dx = h_tri * 1.04;
        CGContextMoveToPoint(ctx, cx, top_y);
        CGContextAddLineToPoint(ctx, cx - dx, bot_y);
        CGContextAddLineToPoint(ctx, cx + dx, bot_y);
        CGContextClosePath(ctx);
        CGContextStrokePath(ctx);
    } else if (kind == 'P') {
        CGFloat w2 = half * 0.92, barH = stroke * 0.85;
        for (int i = -1; i <= 1; i++) {
            CGFloat yy = cy + i * (half * 0.58);
            CGPathRef p = CGPathCreateWithRoundedRect(CGRectMake(cx - w2, yy - barH * 0.5, w2 * 2, barH), barH * 0.45, barH * 0.45, NULL);
            CGContextAddPath(ctx, p); CGContextFillPath(ctx); CGPathRelease(p);
        }
    }
    CGContextRestoreGState(ctx);
}

static void draw_centered_text(CGContextRef ctx, NSString *text, CGFloat cx, CGFloat cy, CGFloat fontSize, NSString *fontName,
                               CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
    CGContextSaveGState(ctx);
    CTFontRef font = CTFontCreateWithName((__bridge CFStringRef)fontName, fontSize, NULL);
    NSDictionary *attrs = @{
        (__bridge NSString *)kCTFontAttributeName: (__bridge id)font,
        (__bridge NSString *)kCTForegroundColorFromContextAttributeName: @YES
    };
    CFAttributedStringRef astr = CFAttributedStringCreate(NULL, (__bridge CFStringRef)text, (__bridge CFDictionaryRef)attrs);
    CTLineRef line = CTLineCreateWithAttributedString(astr);
    CGRect bounds = CTLineGetBoundsWithOptions(line, kCTLineBoundsUseGlyphPathBounds);
    CGContextSetRGBFillColor(ctx, r, g, b, a);
    CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
    CGContextSetTextPosition(ctx, cx - CGRectGetMidX(bounds), cy - CGRectGetMidY(bounds));
    CTLineDraw(line, ctx);
    CFRelease(line); CFRelease(astr); CFRelease(font);
    CGContextRestoreGState(ctx);
}

static ImgBuf render_keytex_ps(char kind) {
    uint32_t w = 64, h = 64;
    uint8_t *buf = calloc((size_t)w * h, 4);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(buf, w, h, 8, w * 4, cs, kCGBitmapByteOrderDefault | kCGImageAlphaPremultipliedLast);
    CGFloat sz = (kind == 'P') ? 38.0 : 28.5, st = (kind == 'P') ? 5.6 : 5.4;
    draw_ps_symbol(ctx, kind, 32.0, 32.0, sz, st, YES, 1.0, 1.0, 1.0, 1.0);
    unpremult_white_or_rgb(buf, (size_t)w * h, YES);
    CGContextRelease(ctx); CGColorSpaceRelease(cs);
    return img_alloc(w, h, buf);
}

static ImgBuf render_ocarina_face_ps(const ImgBuf *origMask, char kind) {
    uint32_t w = origMask->w, h = origMask->h;
    uint8_t *buf = calloc((size_t)w * h, 4);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(buf, w, h, 8, w * 4, cs, kCGBitmapByteOrderDefault | kCGImageAlphaPremultipliedLast);
    CGContextSetRGBFillColor(ctx, 1, 1, 1, 1);
    CGContextFillRect(ctx, CGRectMake(0, 0, w, h));
    draw_ps_symbol(ctx, kind, w * 0.5, h * 0.5, 32.0, 5.8, YES, 0.0, 0.0, 0.0, 1.0);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint8_t a = origMask->rgba[i * 4 + 3];
        if (a == 0) {
            buf[i * 4] = buf[i * 4 + 1] = buf[i * 4 + 2] = 255;
            buf[i * 4 + 3] = 0;
        } else {
            uint8_t g = buf[i * 4];
            buf[i * 4] = buf[i * 4 + 1] = buf[i * 4 + 2] = g;
            buf[i * 4 + 3] = a;
        }
    }
    CGContextRelease(ctx); CGColorSpaceRelease(cs);
    return img_alloc(w, h, buf);
}

static ImgBuf render_ocarina_bumper(const ImgBuf *origMask, NSString *label, CGFloat cxOffset, CGFloat fontSize) {
    uint32_t w = origMask->w, h = origMask->h;
    uint8_t *buf = calloc((size_t)w * h, 4);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(buf, w, h, 8, w * 4, cs, kCGBitmapByteOrderDefault | kCGImageAlphaPremultipliedLast);
    CGContextSetRGBFillColor(ctx, 1, 1, 1, 1);
    CGContextFillRect(ctx, CGRectMake(0, 0, w, h));
    draw_centered_text(ctx, label, w * 0.5 + cxOffset, h * 0.5 + 1.0, fontSize, @"ArialRoundedMTBold", 0.0, 0.0, 0.0, 1.0);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint8_t a = origMask->rgba[i * 4 + 3];
        if (a == 0) {
            buf[i * 4] = buf[i * 4 + 1] = buf[i * 4 + 2] = 255;
            buf[i * 4 + 3] = 0;
        } else {
            uint8_t g = buf[i * 4];
            buf[i * 4] = buf[i * 4 + 1] = buf[i * 4 + 2] = g;
            buf[i * 4 + 3] = a;
        }
    }
    CGContextRelease(ctx); CGColorSpaceRelease(cs);
    return img_alloc(w, h, buf);
}

static ImgBuf place_controller_1160x680(const char *srcPath, double targetH, double targetCenterY) {
    uint32_t sw = 0, sh = 0;
    uint8_t *sb = load_png_straight(srcPath, &sw, &sh);
    if (!sb) { fprintf(stderr, "[RED] oot_pad_overlay: missing %s\n", srcPath); exit(1); }
    int minx = (int)sw, maxx = -1, miny = (int)sh, maxy = -1;
    for (int y = 0; y < (int)sh; y++) for (int x = 0; x < (int)sw; x++) {
        uint8_t *p = sb + ((size_t)y * sw + x) * 4;
        double lum = 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
        if (p[3] > 0 && p[3] < 190 && lum > 165.0) p[3] = 0;
        if (p[3] > 20) {
            if (x < minx) minx = x; if (x > maxx) maxx = x;
            if (y < miny) miny = y; if (y > maxy) maxy = y;
        }
    }
    double bw = maxx - minx + 1, bh = maxy - miny + 1;
    double scale = targetH / bh;
    double dw = bw * scale, dh = bh * scale;
    if (dw > 960.0) { scale *= 960.0 / dw; dw = bw * scale; dh = bh * scale; }

    uint8_t *pm = malloc((size_t)sw * sh * 4);
    for (size_t i = 0; i < (size_t)sw * sh; i++) {
        uint8_t a = sb[i * 4 + 3];
        pm[i * 4]     = (uint8_t)((sb[i * 4]     * a + 127) / 255);
        pm[i * 4 + 1] = (uint8_t)((sb[i * 4 + 1] * a + 127) / 255);
        pm[i * 4 + 2] = (uint8_t)((sb[i * 4 + 2] * a + 127) / 255);
        pm[i * 4 + 3] = a;
    }
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef sctx = CGBitmapContextCreate(pm, sw, sh, 8, sw * 4, cs, kCGBitmapByteOrderDefault | kCGImageAlphaPremultipliedLast);
    CGImageRef fullImg = CGBitmapContextCreateImage(sctx);
    CGImageRef cropImg = CGImageCreateWithImageInRect(fullImg, CGRectMake(minx, miny, bw, bh));

    uint32_t W = 1160, H = 680;
    uint8_t *db = calloc((size_t)W * H, 4);
    CGContextRef dctx = CGBitmapContextCreate(db, W, H, 8, W * 4, cs, kCGBitmapByteOrderDefault | kCGImageAlphaPremultipliedLast);
    CGContextSetInterpolationQuality(dctx, kCGInterpolationHigh);
    double dx = (W - dw) * 0.5;
    double topY = targetCenterY - dh * 0.5;
    double cgY = H - (topY + dh);
    CGContextDrawImage(dctx, CGRectMake(dx, cgY, dw, dh), cropImg);

    unpremult_white_or_rgb(db, (size_t)W * H, NO);
    CGImageRelease(cropImg); CGImageRelease(fullImg);
    CGContextRelease(sctx); CGContextRelease(dctx); CGColorSpaceRelease(cs);
    free(pm); free(sb);
    return img_alloc(W, H, db);
}

static ImgBuf make_warm_controller1(const ImgBuf *src, double bodyLo, double bodyHi) {
    size_t n = (size_t)src->w * src->h;
    uint8_t *b = malloc(n * 4);
    memcpy(b, src->rgba, n * 4);
    double xs[8] = {0.0, 14.0, bodyLo, bodyHi, 76.0, 102.0, 165.0, 255.0};
    double rs[8] = {25.0, 22.0, 15.0,   15.0,   52.0, 82.0,  119.0, 176.0};
    double gs[8] = {18.0, 16.0, 12.0,   12.0,   44.0, 71.0,  100.0, 148.0};
    double bs[8] = { 9.0, 10.0, 10.0,   10.0,   32.0, 52.0,   78.0, 118.0};
    for (size_t i = 0; i < n; i++) {
        if (b[i * 4 + 3] == 0) continue;
        double l = 0.299 * b[i * 4] + 0.587 * b[i * 4 + 1] + 0.114 * b[i * 4 + 2];
        int k = 0;
        while (k < 6 && l > xs[k + 1]) k++;
        double t = (l - xs[k]) / fmax(1e-6, xs[k + 1] - xs[k]);
        if (t < 0) t = 0; if (t > 1) t = 1;
        b[i * 4]     = (uint8_t)lrint(rs[k] + t * (rs[k + 1] - rs[k]));
        b[i * 4 + 1] = (uint8_t)lrint(gs[k] + t * (gs[k + 1] - gs[k]));
        b[i * 4 + 2] = (uint8_t)lrint(bs[k] + t * (bs[k + 1] - bs[k]));
    }
    return img_alloc(src->w, src->h, b);
}

// Left-half soft drop shadow matching the BFLY mirror seam at x = W - 1.
static ImgBuf make_shadow_half_440x580(const ImgBuf *pro) {
    uint32_t pw = pro->w, ph = pro->h;
    int W = 440, H = 580;
    double *a = calloc((size_t)W * H, sizeof(double));
    double *t = calloc((size_t)W * H, sizeof(double));
    for (int y = 0; y < H; y++) {
        double fsy = 340.0 + ((double)y - 290.0) / 0.773;
        int sy = (int)lrint(fsy);
        if (sy < 0 || sy >= (int)ph) continue;
        for (int x = 0; x < W; x++) {
            double fsx = 579.5 - ((double)(W - 1 - x)) / 0.745;
            int sx = (int)lrint(fsx);
            if (sx < 0 || sx >= (int)pw) continue;
            a[y * W + x] = (pro->rgba[((size_t)sy * pw + sx) * 4 + 3] > 32) ? 183.0 : 0.0;
        }
    }
    int r = 15;
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                double s = 0;
                for (int dx = -r; dx <= r; dx++) {
                    int xx = x + dx;
                    if (xx >= W) xx = W - 1 - (xx - W);
                    if (xx >= 0) s += a[y * W + xx];
                }
                t[y * W + x] = s / (2 * r + 1);
            }
        }
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                double s = 0;
                for (int dy = -r; dy <= r; dy++) {
                    int yy = y + dy;
                    if (yy >= 0 && yy < H) s += t[yy * W + x];
                }
                a[y * W + x] = s / (2 * r + 1);
            }
        }
    }
    uint8_t *out = malloc((size_t)W * H * 4);
    for (size_t i = 0; i < (size_t)W * H; i++) {
        uint8_t v = (uint8_t)fmin(255.0, fmax(0.0, lrint(a[i])));
        out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = 255;
        out[i * 4 + 3] = v;
    }
    free(t); free(a);
    return img_alloc((uint32_t)W, (uint32_t)H, out);
}

static inline CGFloat guide_y(CGFloat y_td) { return 1511.0 - y_td; }

static void draw_guide_callout(CGContextRef ctx, CGFloat barX, CGFloat barY0, CGFloat barY1, CGFloat startY,
                               CGFloat endX, CGFloat endY, BOOL hasDot) {
    CGContextSetRGBStrokeColor(ctx, 1, 1, 1, 1);
    CGContextSetRGBFillColor(ctx, 1, 1, 1, 1);
    CGContextSetLineCap(ctx, kCGLineCapRound);
    CGContextSetLineJoin(ctx, kCGLineJoinRound);

    CGContextSetLineWidth(ctx, 7.0);
    CGContextMoveToPoint(ctx, barX, guide_y(barY0));
    CGContextAddLineToPoint(ctx, barX, guide_y(barY1));
    CGContextStrokePath(ctx);

    CGContextSetLineWidth(ctx, 5.5);
    CGContextMoveToPoint(ctx, barX, guide_y(startY));
    CGContextAddLineToPoint(ctx, endX, guide_y(startY));
    if (fabs(endY - startY) > 1.0) CGContextAddLineToPoint(ctx, endX, guide_y(endY));
    CGContextStrokePath(ctx);

    if (hasDot) {
        CGFloat r = 10.5;
        CGContextBeginPath(ctx);
        CGContextMoveToPoint(ctx, endX, guide_y(endY - r));
        CGContextAddLineToPoint(ctx, endX + r, guide_y(endY));
        CGContextAddLineToPoint(ctx, endX, guide_y(endY + r));
        CGContextAddLineToPoint(ctx, endX - r, guide_y(endY));
        CGContextClosePath(ctx);
        CGContextFillPath(ctx);
    }
}

static void draw_guide_diamond(CGContextRef ctx, CGFloat barX, CGFloat barY0, CGFloat barY1, CGFloat startY,
                               CGFloat cx, CGFloat cy, CGFloat halfDiag, BOOL fromLeft) {
    CGContextSetRGBStrokeColor(ctx, 1, 1, 1, 1);
    CGContextSetLineCap(ctx, kCGLineCapRound);
    CGContextSetLineJoin(ctx, kCGLineJoinRound);

    CGContextSetLineWidth(ctx, 7.0);
    CGContextMoveToPoint(ctx, barX, guide_y(barY0));
    CGContextAddLineToPoint(ctx, barX, guide_y(barY1));
    CGContextStrokePath(ctx);

    CGFloat r = halfDiag;
    CGFloat edgeX = fromLeft ? (cx - r) : (cx + r);
    CGContextSetLineWidth(ctx, 5.5);
    CGContextMoveToPoint(ctx, barX, guide_y(startY));
    if (fabs(startY - cy) > 2.0) {
        CGFloat elbowX = fromLeft ? fmin(edgeX - 36.0, (barX + edgeX) * 0.5)
                                  : fmax(edgeX + 36.0, (barX + edgeX) * 0.5);
        CGContextAddLineToPoint(ctx, elbowX, guide_y(startY));
        CGContextAddLineToPoint(ctx, elbowX, guide_y(cy));
    }
    CGContextAddLineToPoint(ctx, edgeX, guide_y(cy));
    CGContextStrokePath(ctx);

    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, cx, guide_y(cy));
    CGContextRotateCTM(ctx, M_PI_4);
    CGFloat s = r * 1.4142 * 0.88;
    CGPathRef p = CGPathCreateWithRoundedRect(CGRectMake(-s/2, -s/2, s, s), s * 0.26, s * 0.26, NULL);
    CGContextAddPath(ctx, p);
    CGContextStrokePath(ctx);
    CGPathRelease(p);
    CGContextRestoreGState(ctx);
}

static CGContextRef begin_guide_1555(uint8_t **outBuf) {
    uint8_t *b = calloc(1555 * 1511, 4);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(b, 1555, 1511, 8, 1555 * 4, cs, kCGBitmapByteOrderDefault | kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    *outBuf = b;
    return ctx;
}

static ImgBuf end_guide_1555(CGContextRef ctx, uint8_t *b) {
    for (size_t i = 0; i < 1555 * 1511; i++) {
        uint8_t a = b[i * 4 + 3];
        b[i * 4] = b[i * 4 + 1] = b[i * 4 + 2] = 255;
        b[i * 4 + 3] = a;
    }
    CGContextRelease(ctx);
    return img_alloc(1555, 1511, b);
}

static void generate_guides(ImgBuf out[5], BOOL isPS) {
    uint8_t *b; CGContextRef c;
    if (isPS) {
        c = begin_guide_1555(&b);
        draw_guide_callout(c, 142, 290, 386, 336, 455, 452, YES);
        draw_guide_callout(c, 142, 623, 719, 714, 627, 785, YES);
        draw_guide_callout(c, 1413, 513, 609, 517, 997, 552, YES);
        draw_guide_diamond(c, 1413, 627, 1162, 665, 1076, 651, 153, NO);
        draw_guide_callout(c, 1413, 1182, 1382, 1186, 926, 787, YES);
        out[0] = end_guide_1555(c, b);

        c = begin_guide_1555(&b);
        draw_guide_callout(c, 142, 512, 608, 543, 556, 558, YES);
        out[1] = end_guide_1555(c, b);

        c = begin_guide_1555(&b);
        draw_guide_callout(c, 1413, 290, 386, 335, 1091, 451, YES);
        out[2] = end_guide_1555(c, b);

        c = begin_guide_1555(&b);
        draw_guide_diamond(c, 142, 736, 1185, 880, 484, 651, 127, YES);
        out[3] = end_guide_1555(c, b);

        c = begin_guide_1555(&b);
        draw_guide_callout(c, 142, 401, 497, 449.5, 485, 478, YES);
        draw_guide_callout(c, 1413, 402, 497, 449.0, 1063, 480, YES);
        out[4] = end_guide_1555(c, b);
    } else {
        c = begin_guide_1555(&b);
        draw_guide_callout(c, 142, 290, 386, 336, 535, 449, YES);
        draw_guide_callout(c, 142, 623, 719, 670, 528, 649, YES);
        draw_guide_callout(c, 1413, 513, 609, 525, 855, 622, YES);
        draw_guide_diamond(c, 1413, 627, 1162, 662, 1036, 655, 154, NO);
        draw_guide_callout(c, 1413, 1182, 1382, 1226, 916, 806, YES);
        out[0] = end_guide_1555(c, b);

        c = begin_guide_1555(&b);
        draw_guide_callout(c, 142, 512, 608, 562.5, 706, 625, YES);
        out[1] = end_guide_1555(c, b);

        c = begin_guide_1555(&b);
        draw_guide_callout(c, 1413, 290, 386, 335, 1015, 452, YES);
        out[2] = end_guide_1555(c, b);

        c = begin_guide_1555(&b);
        draw_guide_diamond(c, 142, 736, 1185, 780, 650, 808, 133, YES);
        out[3] = end_guide_1555(c, b);

        c = begin_guide_1555(&b);
        draw_guide_callout(c, 142, 401, 497, 449.5, 556, 480, YES);
        draw_guide_callout(c, 1413, 402, 497, 449.0, 995, 478, YES);
        out[4] = end_guide_1555(c, b);
    }
}

static void encode_bc4_block(const uint8_t px[16], uint8_t out[8]) {
    uint8_t mn = 255, mx = 0;
    for (int i = 0; i < 16; i++) { if (px[i] < mn) mn = px[i]; if (px[i] > mx) mx = px[i]; }
    out[0] = mx; out[1] = mn;
    if (mx == mn) { memset(out + 2, 0, 6); return; }
    uint8_t pal[8] = {
        mx, mn,
        (uint8_t)((6 * mx + 1 * mn) / 7), (uint8_t)((5 * mx + 2 * mn) / 7),
        (uint8_t)((4 * mx + 3 * mn) / 7), (uint8_t)((3 * mx + 4 * mn) / 7),
        (uint8_t)((2 * mx + 5 * mn) / 7), (uint8_t)((1 * mx + 6 * mn) / 7)
    };
    uint64_t bits = 0;
    for (int i = 0; i < 16; i++) {
        int best_c = 0, best_d = 1000;
        for (int c = 0; c < 8; c++) {
            int d = abs((int)pal[c] - (int)px[i]);
            if (d < best_d) { best_d = d; best_c = c; }
        }
        bits |= ((uint64_t)best_c) << (3 * i);
    }
    for (int j = 0; j < 6; j++) out[2 + j] = (uint8_t)((bits >> (8 * j)) & 0xff);
}

static void encode_img_into_bntx(const ImgBuf *imgBuf, const char *label, uint8_t *bntx, uint64_t mip0,
                                 uint32_t expectedW, uint32_t expectedH, int bfmt, int tlay, BOOL monoSwizzle) {
    uint32_t w = imgBuf->w, h = imgBuf->h;
    if (w != expectedW || h != expectedH) {
        fprintf(stderr, "[RED] oot_pad_overlay: %s size %ux%u != expected %ux%u\n", label, w, h, expectedW, expectedH);
        exit(1);
    }
    const uint8_t *src = imgBuf->rgba;
    int nvn_fmt = (bfmt == 0x1d01) ? 0x49 : 0x87;
    nvn_texdesc td = { .target = 1, .format = nvn_fmt, .width = w, .height = h, .depth = 1, .levels = 1, .pkg_layout = (tlay & 0xff) + 1 };
    nvn_texlayout L; nvn_tex_layout(&td, &L);
    uint32_t rb = 0, rows = 0, slices = 0, bpb = 0; uint64_t base = 0; unsigned th = 0, td2 = 0;
    if (nvn_deswizzle_geom(&td, 0, 0, &rb, &rows, &slices, &base, &th, &td2, &bpb) != 0) {
        fprintf(stderr, "[RED] oot_pad_overlay: geom fail %s\n", label); exit(1);
    }
    size_t lin_sz = (size_t)rb * rows;
    uint8_t *lin = calloc(1, lin_sz);

    if (bfmt == 0x1d01) {
        uint32_t bx_cnt = (w + 3) / 4, by_cnt = (h + 3) / 4;
        for (uint32_t by = 0; by < by_cnt; by++) {
            for (uint32_t bx = 0; bx < bx_cnt; bx++) {
                uint8_t p16[16];
                for (int ty = 0; ty < 4; ty++) for (int tx = 0; tx < 4; tx++) {
                    uint32_t x = bx * 4 + tx; if (x >= w) x = w - 1;
                    uint32_t y = by * 4 + ty; if (y >= h) y = h - 1;
                    p16[ty * 4 + tx] = src[(y * w + x) * 4 + 3];
                }
                encode_bc4_block(p16, lin + (size_t)by * rb + bx * 8);
            }
        }
    } else {
        uint8_t *flip = malloc((size_t)w * h * 4);
        for (uint32_t y = 0; y < h; y++) memcpy(flip + (size_t)y * w * 4, src + (size_t)(h - 1 - y) * w * 4, (size_t)w * 4);
        if (monoSwizzle) {
            for (size_t i = 0; i < (size_t)w * h; i++) {
                if (flip[i * 4 + 3] == 0) flip[i * 4] = flip[i * 4 + 1] = flip[i * 4 + 2] = 255;
            }
        }
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGDataProviderRef prov = CGDataProviderCreateWithData(NULL, flip, (size_t)w * h * 4, NULL);
        CGImageRef img = CGImageCreate(w, h, 8, 32, w * 4, cs, kCGBitmapByteOrderDefault | kCGImageAlphaLast, prov, NULL, false, kCGRenderingIntentDefault);
        NSMutableData *astc = [NSMutableData data];
        CGImageDestinationRef dst = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)astc, CFSTR("org.khronos.astc"), 1, NULL);
        NSDictionary *opts = @{ (__bridge NSString *)kCGImagePropertyASTCBlockSize : @4,
                               (__bridge NSString *)kCGImageDestinationLossyCompressionQuality : @1.0 };
        CGImageDestinationAddImage(dst, img, (__bridge CFDictionaryRef)opts);
        if (!CGImageDestinationFinalize(dst) || astc.length < 16 + lin_sz) {
            fprintf(stderr, "[RED] oot_pad_overlay: astc encode failed for %s (got %lu need %zu)\n", label, (unsigned long)astc.length, 16 + lin_sz);
            exit(1);
        }
        memcpy(lin, (const uint8_t *)astc.bytes + 16, lin_sz);
        CFRelease(dst); CGImageRelease(img); CGDataProviderRelease(prov); CGColorSpaceRelease(cs); free(flip);
    }

    if (nvn_swizzle(&td, 0, 0, lin, rb, bntx + mip0) != 0) {
        fprintf(stderr, "[RED] oot_pad_overlay: swizzle fail %s\n", label); exit(1);
    }
    free(lin);
}

static uint8_t swz_val(uint8_t c, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    switch (c) { case 0: return 0; case 1: return 255; case 2: return r; case 3: return g; case 4: return b; case 5: return a; default: return 255; }
}

static ImgBuf decode_bntx_tex(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLLibrary> lib,
                              const uint8_t *bntx, uint64_t mip0, uint32_t w, uint32_t h,
                              int bfmt, int tlay, uint8_t s0, uint8_t s1, uint8_t s2, uint8_t s3) {
    int nvn_fmt = (bfmt == 0x1d01) ? 0x49 : 0x87;
    MTLPixelFormat src_pf = (bfmt == 0x1d01) ? MTLPixelFormatBC4_RUnorm : MTLPixelFormatASTC_4x4_sRGB;
    MTLPixelFormat dst_pf = (bfmt == 0x1d01) ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatRGBA8Unorm_sRGB;
    nvn_texdesc td = { .target = 1, .format = nvn_fmt, .width = w, .height = h, .depth = 1, .levels = 1, .pkg_layout = (tlay & 0xff) + 1 };
    uint32_t rb = 0, rows = 0, slices = 0, bpb = 0; uint64_t base = 0; unsigned th = 0, td2 = 0;
    nvn_deswizzle_geom(&td, 0, 0, &rb, &rows, &slices, &base, &th, &td2, &bpb);
    uint8_t *lin = calloc(rows, rb);
    nvn_deswizzle(&td, 0, 0, bntx + mip0, lin, rb);

    MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src_pf width:w height:h mipmapped:NO];
    sd.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> src = [dev newTextureWithDescriptor:sd];
    [src replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:lin bytesPerRow:rb];
    free(lin);

    MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:dst_pf width:w height:h mipmapped:NO];
    dd.usage = MTLTextureUsageRenderTarget; dd.storageMode = MTLStorageModeShared;
    id<MTLTexture> dst = [dev newTextureWithDescriptor:dd];

    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
    pd.colorAttachments[0].pixelFormat = dst_pf;
    NSError *err = nil;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!pso) { fprintf(stderr, "[RED] oot_pad_overlay: Metal PSO error: %s\n", err.localizedDescription.UTF8String); exit(1); }

    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:pso]; [enc setFragmentTexture:src atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding]; [cb commit]; [cb waitUntilCompleted];

    uint8_t *rgba = malloc((size_t)w * h * 4);
    [dst getBytes:rgba bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint8_t r0 = rgba[i * 4], g0 = rgba[i * 4 + 1], b0 = rgba[i * 4 + 2], a0 = rgba[i * 4 + 3];
        rgba[i * 4]     = swz_val(s0, r0, g0, b0, a0);
        rgba[i * 4 + 1] = swz_val(s1, r0, g0, b0, a0);
        rgba[i * 4 + 2] = swz_val(s2, r0, g0, b0, a0);
        rgba[i * 4 + 3] = swz_val(s3, r0, g0, b0, a0);
    }
    return img_alloc(w, h, rgba);
}

typedef struct { char name[64]; uint32_t w, h, bfmt, layout, sz; uint64_t mip0; unsigned s[4]; } TexMeta;

static const TexMeta *find_meta(const TexMeta *metas, int nm, const char *name) {
    for (int i = 0; i < nm; i++) if (!strcmp(metas[i].name, name)) return &metas[i];
    fprintf(stderr, "[RED] oot_pad_overlay: texture %s missing from BNTX list\n", name);
    exit(1);
}

int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 7) {
        fprintf(stderr, "usage: oot_pad_overlay <common.bntx> <list.tsv> <dualsense.png> <xbox.png> <ps_out.bntx> <xb_out.bntx>\n");
        return 1;
    }
    nvn_gob_switch2 = 1;
    const char *bntxPath = argv[1];
    const char *listPath = argv[2];
    const char *psSrc = argv[3];
    const char *xbSrc = argv[4];
    NSString *psOutPath = [NSString stringWithUTF8String:argv[5]];
    NSString *xbOutPath = [NSString stringWithUTF8String:argv[6]];

    TexMeta metas[256]; int nm = 0;
    FILE *fp = fopen(listPath, "r");
    if (!fp) { fprintf(stderr, "[RED] oot_pad_overlay: cannot open %s\n", listPath); return 1; }
    while (nm < 256 && fscanf(fp, "%63s %u %u %x %u %u %llu %u %u %u %u",
                              metas[nm].name, &metas[nm].w, &metas[nm].h, &metas[nm].bfmt,
                              &metas[nm].layout, &metas[nm].sz, &metas[nm].mip0,
                              &metas[nm].s[0], &metas[nm].s[1], &metas[nm].s[2], &metas[nm].s[3]) == 11) nm++;
    fclose(fp);

    NSData *origBntx = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:bntxPath]];
    if (!origBntx) { fprintf(stderr, "[RED] oot_pad_overlay: cannot read %s\n", bntxPath); return 1; }
    const uint8_t *origBytes = origBntx.bytes;

    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) { fprintf(stderr, "[RED] oot_pad_overlay: no Metal device\n"); return 1; }
    id<MTLCommandQueue> q = [dev newCommandQueue];
    NSString *msl = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 p [[position]]; float2 uv; };\n"
        "vertex V vs(uint id [[vertex_id]]) { float2 p = float2((id&1)?3:-1,(id&2)?-3:1); V o; o.p=float4(p,0,1); o.uv=float2((p.x+1)*0.5,(1-p.y)*0.5); return o; }\n"
        "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]]) { constexpr sampler s(filter::nearest); return t.sample(s, in.uv); }\n";
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:msl options:nil error:&err];
    if (!lib) { fprintf(stderr, "[RED] oot_pad_overlay: Metal shader compile failed: %s\n", err.localizedDescription.UTF8String); return 1; }

    NSMutableData *psBntx = [origBntx mutableCopy];
    NSMutableData *xbBntx = [origBntx mutableCopy];
    uint8_t *psBytes = psBntx.mutableBytes, *xbBytes = xbBntx.mutableBytes;

    // 1. KeyTexFont face & +/Menu symbols
    const char *kfNames[4] = {"KeyTexFontA_00", "KeyTexFontB_00", "KeyTexFontX_00", "KeyTexFontY_00"};
    const char psSyms[4]   = {'X', 'O', 'S', 'T'};
    for (int i = 0; i < 4; i++) {
        ImgBuf img = render_keytex_ps(psSyms[i]);
        const TexMeta *m = find_meta(metas, nm, kfNames[i]);
        encode_img_into_bntx(&img, kfNames[i], psBytes, m->mip0, m->w, m->h, (int)m->bfmt, (int)m->layout, NO);
        free(img.rgba);
    }
    ImgBuf plusImg = render_keytex_ps('P');
    const TexMeta *plusM = find_meta(metas, nm, "ActionGuidePlus_00");
    encode_img_into_bntx(&plusImg, "ActionGuidePlus_00", psBytes, plusM->mip0, plusM->w, plusM->h, (int)plusM->bfmt, (int)plusM->layout, NO);
    encode_img_into_bntx(&plusImg, "ActionGuidePlus_00", xbBytes, plusM->mip0, plusM->w, plusM->h, (int)plusM->bfmt, (int)plusM->layout, NO);
    free(plusImg.rgba);

    // 2. Ocarina buttons (extract circular/pill silhouette alpha directly from the game's BNTX)
    const char *ocNames[4] = {"OcarinaButtonA_00", "OcarinaButtonB_00", "OcarinaButtonX_00", "OcarinaButtonY_00"};
    for (int i = 0; i < 4; i++) {
        const TexMeta *m = find_meta(metas, nm, ocNames[i]);
        ImgBuf mask = decode_bntx_tex(dev, q, lib, origBytes, m->mip0, m->w, m->h, (int)m->bfmt, (int)m->layout, m->s[0], m->s[1], m->s[2], m->s[3]);
        ImgBuf img = render_ocarina_face_ps(&mask, psSyms[i]);
        encode_img_into_bntx(&img, ocNames[i], psBytes, m->mip0, m->w, m->h, (int)m->bfmt, (int)m->layout, m->s[0] == 2 && m->s[1] == 2 && m->s[2] == 2);
        free(mask.rgba); free(img.rgba);
    }
    const char *bumpNames[2] = {"OcarinaButtonL_00", "OcarinaButtonR_00"};
    NSString *psBumps[2] = {@"L1", @"R1"}, *xbBumps[2] = {@"LB", @"RB"};
    CGFloat bumpShift[2] = {3.0, -3.0};
    for (int i = 0; i < 2; i++) {
        const TexMeta *m = find_meta(metas, nm, bumpNames[i]);
        BOOL mono = (m->s[0] == 2 && m->s[1] == 2 && m->s[2] == 2);
        ImgBuf mask = decode_bntx_tex(dev, q, lib, origBytes, m->mip0, m->w, m->h, (int)m->bfmt, (int)m->layout, m->s[0], m->s[1], m->s[2], m->s[3]);
        ImgBuf psB = render_ocarina_bumper(&mask, psBumps[i], bumpShift[i], 31.0);
        ImgBuf xbB = render_ocarina_bumper(&mask, xbBumps[i], bumpShift[i], 29.0);
        encode_img_into_bntx(&psB, bumpNames[i], psBytes, m->mip0, m->w, m->h, (int)m->bfmt, (int)m->layout, mono);
        encode_img_into_bntx(&xbB, bumpNames[i], xbBytes, m->mip0, m->w, m->h, (int)m->bfmt, (int)m->layout, mono);
        free(mask.rgba); free(psB.rgba); free(xbB.rgba);
    }

    // 3. Controller diagrams, warm variants, half shadows, and callouts across Pro, Grip, Console
    ImgBuf psCtrl  = place_controller_1160x680(psSrc, 595.0, 346.0);
    ImgBuf psCtrl1 = make_warm_controller1(&psCtrl, 34.0, 62.0);
    ImgBuf psSh    = make_shadow_half_440x580(&psCtrl);
    ImgBuf psGuides[5]; generate_guides(psGuides, YES);

    ImgBuf xbCtrl  = place_controller_1160x680(xbSrc, 595.0, 346.0);
    ImgBuf xbCtrl1 = make_warm_controller1(&xbCtrl, 28.0, 66.0);
    ImgBuf xbSh    = make_shadow_half_440x580(&xbCtrl);
    ImgBuf xbGuides[5]; generate_guides(xbGuides, NO);

    const char *families[3] = {"Pro", "Grip", "Console"};
    for (int f = 0; f < 3; f++) {
        char nmBuf[64];
        snprintf(nmBuf, sizeof nmBuf, "Controller_%s", families[f]);
        const TexMeta *mc = find_meta(metas, nm, nmBuf);
        encode_img_into_bntx(&psCtrl, nmBuf, psBytes, mc->mip0, mc->w, mc->h, (int)mc->bfmt, (int)mc->layout, NO);
        encode_img_into_bntx(&xbCtrl, nmBuf, xbBytes, mc->mip0, mc->w, mc->h, (int)mc->bfmt, (int)mc->layout, NO);

        snprintf(nmBuf, sizeof nmBuf, "Controller_%s1", families[f]);
        const TexMeta *mc1 = find_meta(metas, nm, nmBuf);
        encode_img_into_bntx(&psCtrl1, nmBuf, psBytes, mc1->mip0, mc1->w, mc1->h, (int)mc1->bfmt, (int)mc1->layout, NO);
        encode_img_into_bntx(&xbCtrl1, nmBuf, xbBytes, mc1->mip0, mc1->w, mc1->h, (int)mc1->bfmt, (int)mc1->layout, NO);

        snprintf(nmBuf, sizeof nmBuf, "ControllerSh_%s1", families[f]);
        const TexMeta *ms1 = find_meta(metas, nm, nmBuf);
        encode_img_into_bntx(&psSh, nmBuf, psBytes, ms1->mip0, ms1->w, ms1->h, (int)ms1->bfmt, (int)ms1->layout, NO);
        encode_img_into_bntx(&xbSh, nmBuf, xbBytes, ms1->mip0, ms1->w, ms1->h, (int)ms1->bfmt, (int)ms1->layout, NO);

        for (int g = 0; g < 5; g++) {
            snprintf(nmBuf, sizeof nmBuf, "ControllerGuide_%s_%02d", families[f], g);
            const TexMeta *mg = find_meta(metas, nm, nmBuf);
            BOOL mono = (mg->s[0] == 2 && mg->s[1] == 2 && mg->s[2] == 2);
            encode_img_into_bntx(&psGuides[g], nmBuf, psBytes, mg->mip0, mg->w, mg->h, (int)mg->bfmt, (int)mg->layout, mono);
            encode_img_into_bntx(&xbGuides[g], nmBuf, xbBytes, mg->mip0, mg->w, mg->h, (int)mg->bfmt, (int)mg->layout, mono);
        }
    }

    free(psCtrl.rgba); free(psCtrl1.rgba); free(psSh.rgba);
    free(xbCtrl.rgba); free(xbCtrl1.rgba); free(xbSh.rgba);
    for (int g = 0; g < 5; g++) { free(psGuides[g].rgba); free(xbGuides[g].rgba); }

    if (![psBntx writeToFile:psOutPath atomically:YES] || ![xbBntx writeToFile:xbOutPath atomically:YES]) {
        fprintf(stderr, "[RED] oot_pad_overlay: failed writing output BNTX\n");
        return 1;
    }
    return 0;
} }
