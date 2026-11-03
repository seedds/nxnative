// The iOS quit prompt (quit_prompt.h) and its touch steps, checked on the Mac: built and run by test_quit_prompt.py.
// Every line that starts with FAIL is a broken rule; the exit status is the number of them (capped).
// With QP_CAPTURE_DIR set it also draws the prompt over a phone screen to PNGs (quit-<screen>-<language>.png), for visual QA.
#import <AppKit/AppKit.h>
#import "quit_prompt.h"
#include <stdio.h>

static int g_fail;
#define FAIL(...) do { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } while (0)

typedef struct { const char *name; CGSize size; } Screen;
// The prompt's words in the launchers' six languages (frontend/l10n homeQuit.*, ios.host.quitNote): title, note, quit, cancel
typedef struct { const char *lang, *title, *note, *quit, *cancel; } Words;
static const Words k_words[] = {
    {"en", "Quit Super Mario Odyssey?", "Unsaved progress will be lost. The app closes and opens on your games next time.", "Quit", "Cancel"},
    {"de", "Super Mario Odyssey beenden?", "Nicht gesicherter Fortschritt geht verloren. Die App wird geschlossen und zeigt beim nächsten Öffnen deine Spiele.", "Beenden", "Abbrechen"},
    {"fr", "Quitter Super Mario Odyssey ?", "La progression non sauvegardée sera perdue. L’app se ferme et s’ouvre sur vos jeux la prochaine fois.", "Quitter", "Annuler"},
    {"it", "Uscire da Super Mario Odyssey?", "I progressi non salvati andranno persi. L’app si chiude e la prossima volta si apre sui tuoi giochi.", "Esci", "Annulla"},
    {"sv", "Avsluta Super Mario Odyssey?", "Osparade framsteg går förlorade. Appen stängs och öppnas med dina spel nästa gång.", "Avsluta", "Avbryt"},
    {"zh", "退出Super Mario Odyssey？", "未保存的进度将会丢失。App 会关闭，下次打开时显示你的游戏。", "退出", "取消"},
};

static void check_states(void) {
    home_quit q = {0};
    if (hq_touch_open(&q, 0, 0, 0) != HQ_PROMPT || !hq_blocks_input(q.phase)) FAIL("Quit in the menu opens the prompt and takes the controllers");
    if (hq_step(&q, 1, 0, 0, 0) != HQ_PROMPT) FAIL("the prompt stays open with nothing pressed");
    if (hq_touch_cancel(&q) != HQ_SETTLE) FAIL("Cancel closes it");
    if (hq_step(&q, 2, 0, 0, 0) != HQ_IDLE) FAIL("with every button up the game has its input again");
    hq_touch_open(&q, 0, 0, 0);
    if (hq_touch_quit(&q) != HQ_QUIT || hq_touch_cancel(&q) != HQ_QUIT || hq_step(&q, 3, 0, 0, 1) != HQ_QUIT) FAIL("Quit is final");

    q = (home_quit){0};
    hq_touch_open(&q, 0, 1, 0);
    if (hq_step(&q, 1, 0, 1, 0) != HQ_PROMPT) FAIL("an A held when the menu opened the prompt does not quit");
    hq_step(&q, 1.1, 0, 0, 0);
    if (hq_step(&q, 1.2, 0, 1, 0) != HQ_QUIT) FAIL("a new A quits, as in the Home prompt");

    q = (home_quit){0};
    hq_touch_open(&q, 0, 0, 1);
    if (hq_step(&q, 1, 0, 0, 1) != HQ_PROMPT) FAIL("a B held when the menu opened the prompt does not close it");
    hq_touch_cancel(&q);
    if (hq_step(&q, 1.1, 0, 0, 1) != HQ_SETTLE || hq_step(&q, 1.2, 0, 0, 0) != HQ_IDLE) FAIL("after Cancel the game gets the controllers back once B is up");

    q = (home_quit){0};
    hq_step(&q, 0, 1, 0, 0);
    if (hq_touch_open(&q, 1, 0, 0) != HQ_PROMPT) FAIL("the menu's Quit during a Home hold opens the prompt at once");
    if (hq_step(&q, 0.5, 1, 0, 0) != HQ_PROMPT) FAIL("the Home still held does not close it");

    q = (home_quit){0};
    hq_step(&q, 0, 1, 0, 0); hq_step(&q, 2, 1, 0, 0); hq_touch_cancel(&q);
    if (hq_touch_open(&q, 0, 0, 0) != HQ_SETTLE) FAIL("the menu's Quit while the prompt settles does not open it again");
    if (hq_touch_cancel(&q) != HQ_SETTLE || hq_touch_quit(&q) != HQ_SETTLE) FAIL("Cancel and Quit do nothing with no prompt open");
    q = (home_quit){0};
    hq_touch_open(&q, 0, 0, 0); hq_cancel(&q);
    if (q.phase != HQ_IDLE) FAIL("leaving the app closes the prompt the menu opened");
}

static void check_layout(const Screen *s, const Words *w) {
    for (int glyphs = 0; glyphs < 2; glyphs++) {
        NSString *title = @(w->title), *note = @(w->note), *quit = @(w->quit), *cancel = @(w->cancel);
        qp_layout l = qp_layout_make(s->size, title, note, quit, cancel, glyphs);
        CGRect b = {CGPointZero, s->size};
        const char *n = s->name, *lang = w->lang;
        if (!CGRectContainsRect(CGRectInset(b, 8, 8), l.card)) FAIL("%s %s: the card leaves the screen (%.0f x %.0f)", n, lang, l.card.size.width, l.card.size.height);
        if (!CGRectContainsRect(l.card, l.quit) || !CGRectContainsRect(l.card, l.cancel) || !CGRectContainsRect(l.card, l.title) || !CGRectContainsRect(l.card, l.note)) FAIL("%s %s: a part leaves the card", n, lang);
        if (l.quit.size.width != QP_ACTION_W * l.k || l.cancel.size.width != l.quit.size.width || l.cancel.size.height != l.quit.size.height) FAIL("%s %s: the pills are not one fixed width", n, lang);
        if (CGRectIntersectsRect(l.quit, l.cancel) || CGRectGetMinY(l.quit) < CGRectGetMaxY(l.note)) FAIL("%s %s: the pills overlap", n, lang);
        // every language's labels fit their pill at full size (the width is chosen for the longest, "Abbrechen")
        CGFloat room = qp_label_room(l.k, glyphs);
        if (l.labelSize[0] != 22 * l.k || l.labelSize[1] != 22 * l.k) FAIL("%s %s: a label had to shrink (%.1f %.1f, room %.0f)", n, lang, l.labelSize[0], l.labelSize[1], room);
        if (qp_hit(&l, CGPointMake(CGRectGetMidX(l.quit), CGRectGetMidY(l.quit))) != QP_HIT_QUIT) FAIL("%s %s: a tap on Quit", n, lang);
        if (qp_hit(&l, CGPointMake(CGRectGetMidX(l.cancel), CGRectGetMidY(l.cancel))) != QP_HIT_CANCEL) FAIL("%s %s: a tap on Cancel", n, lang);
        if (qp_hit(&l, CGPointMake(CGRectGetMidX(l.title), CGRectGetMidY(l.title))) != QP_HIT_NONE) FAIL("%s %s: a tap on the title does something", n, lang);
        if (qp_hit(&l, CGPointMake(4, 4)) != QP_HIT_OUTSIDE || qp_hit(&l, CGPointMake(s->size.width - 4, s->size.height - 4)) != QP_HIT_OUTSIDE) FAIL("%s %s: a tap outside the card does not cancel", n, lang);
        if (l.k < 0.45) FAIL("%s %s: the prompt is too small to read (k %.2f)", n, lang, l.k);
    }
}

// ---- Capture ----
static void capture(const char *dir, const Screen *s, const Words *w, int prompt) {
    CGFloat scale = 2; size_t W = (size_t)(s->size.width * scale), H = (size_t)(s->size.height * scale);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef c = CGBitmapContextCreate(NULL, W, H, 8, 0, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGContextTranslateCTM(c, 0, H); CGContextScaleCTM(c, scale, -scale);   // y down, as UIKit
    CGFloat comps[] = {0.20, 0.24, 0.30, 1, 0.11, 0.13, 0.12, 1}, locs[] = {0, 1};
    CGGradientRef gr = CGGradientCreateWithColorComponents(cs, comps, locs, 2);
    CGContextDrawLinearGradient(c, gr, CGPointZero, CGPointMake(0, s->size.height), 0); CGGradientRelease(gr);
    CGRect b = {CGPointZero, s->size};
    if (prompt) {
        NSString *title = @(w->title), *note = @(w->note), *quit = @(w->quit), *cancel = @(w->cancel);
        qp_layout l = qp_layout_make(s->size, title, note, quit, cancel, 0);
        CALayer *root = [CALayer layer]; root.frame = b; root.geometryFlipped = YES;
        [root addSublayer:qp_prompt_layer(b, &l, title, note, quit, cancel, NULL, NULL, scale, NULL)];
        [root renderInContext:c];   // the context is y down: a flipped root draws its text upright in it
    }
    CGImageRef img = CGBitmapContextCreateImage(c);
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:img];
    NSString *path = [NSString stringWithFormat:@"%s/quit-%s-%s.png", dir, s->name, w->lang];
    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
    CGImageRelease(img); CGContextRelease(c); CGColorSpaceRelease(cs);
}

int main(void) {
    @autoreleasepool {
        const Screen screens[] = {
            {"iphone-se", {667, 375}},
            {"iphone", {852, 393}},
            {"iphone-pro-max", {932, 430}},
            {"ipad-mini", {1133, 744}},
            {"ipad-pro-13", {1376, 1032}},
            {"portrait-390", {390, 844}},
            {"portrait-320", {320, 568}},
            {"duo-inner", {951, 669}},
        };
        int ns = (int)(sizeof screens / sizeof *screens), nw = (int)(sizeof k_words / sizeof *k_words);
        check_states();
        for (int i = 0; i < ns; i++) for (int j = 0; j < nw; j++) check_layout(&screens[i], &k_words[j]);
        // a label longer than any language's is made smaller, never wider than its pill
        qp_layout l = qp_layout_make(CGSizeMake(852, 393), @"Quit?", @"", @"Quit this game right now", @"Cancel", 1);
        if (!(l.labelSize[0] < 22 * l.k) || l.labelSize[0] < 22 * l.k * 0.7 - 0.01 || l.quit.size.width != QP_ACTION_W * l.k) FAIL("a long label: %.1f", l.labelSize[0]);
        const char *dir = getenv("QP_CAPTURE_DIR");
        if (dir && *dir)
            for (int j = 0; j < nw; j++) {
                capture(dir, &screens[1], &k_words[j], 1);
                if (j == 0) { capture(dir, &screens[0], &k_words[j], 1); capture(dir, &screens[3], &k_words[j], 1); }
            }
        if (!g_fail) printf("ok: %d screens, %d languages\n", ns, nw);
    }
    return g_fail > 100 ? 100 : g_fail;
}
