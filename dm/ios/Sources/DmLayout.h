// Shared picture and input geometry; division includes UIKit's interactive margins.
#pragma once
#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

typedef struct { CGRect game, controls; BOOL separated, topSafeOnly; } DmLayout;

static inline UIEdgeInsets dm_game_safe_insets(CGRect bounds, UIEdgeInsets insets, BOOL duo) {
    return duo && bounds.size.height > bounds.size.width ? UIEdgeInsetsMake(insets.top, 0, 0, 0) : insets;
}

static inline CGRect dm_aspect_fit(CGRect b) {
    CGFloat w = b.size.width, h = b.size.height;
    if (w <= 0 || h <= 0) return CGRectZero;
    if (w / h > 16.0 / 9.0) w = h * 16.0 / 9.0; else h = w * 9.0 / 16.0;
    return CGRectMake(floor(CGRectGetMidX(b) - w / 2), floor(CGRectGetMidY(b) - h / 2), w, h);
}

static inline CGRect dm_horizontal_division(CGRect b, CGRect division) {
    if (CGRectIsNull(division) || division.size.width <= division.size.height) return CGRectNull;
    // Preserve zero-height inactive folds and clip regions during display handoff.
    CGFloat x = MAX(CGRectGetMinX(b), CGRectGetMinX(division));
    CGFloat y = MAX(CGRectGetMinY(b), CGRectGetMinY(division));
    CGFloat right = MIN(CGRectGetMaxX(b), CGRectGetMaxX(division));
    CGFloat bottom = MIN(CGRectGetMaxY(b), CGRectGetMaxY(division));
    if (right - x < b.size.width * 0.9 || bottom < y ||
        y <= CGRectGetMinY(b) || bottom >= CGRectGetMaxY(b)) return CGRectNull;
    return CGRectMake(x, y, right - x, bottom - y);
}

static inline DmLayout dm_layout(CGRect b, UIEdgeInsets insets, BOOL partiallyOpen, CGRect division) {
    DmLayout l = { dm_aspect_fit(b), b, NO, NO };
    if (CGRectIsEmpty(b)) return l;
    division = dm_horizontal_division(b, division);
    BOOL portrait = b.size.height > b.size.width;
    if (!portrait && (!partiallyOpen || CGRectIsNull(division))) return l;
    if (CGRectIsNull(division)) division = CGRectMake(b.origin.x, CGRectGetMidY(b), b.size.width, 0);
    CGRect safe = UIEdgeInsetsInsetRect(b, insets);
    CGRect upper = CGRectMake(CGRectGetMinX(safe), CGRectGetMinY(safe), safe.size.width,
                             CGRectGetMinY(division) - CGRectGetMinY(safe));
    CGRect lower = CGRectMake(CGRectGetMinX(b), CGRectGetMaxY(division), b.size.width,
                             CGRectGetMaxY(b) - CGRectGetMaxY(division));
    if (upper.size.height <= 0 || CGRectGetMaxY(safe) <= CGRectGetMinY(lower)) return l;
    l.game = dm_aspect_fit(upper); l.controls = lower; l.separated = YES;
    return l;
}

UIView *dm_gamepad_view(CGRect frame);
void dm_gamepad_arrange(UIView *view, DmLayout layout);
void dm_gamepad_reset(void);

NS_ASSUME_NONNULL_END
