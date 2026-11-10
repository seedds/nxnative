// The game window's Controller menu and panel (Mac): the Switch 2 stick tester, on the controller session the game already holds (pad_switch2_apple.h, the tester's side), so
// a stick that makes a character creep can be checked and calibrated mid-game and the new calibration applies at once. Drawn with AppKit.
// The menu, between Cheats and Window: Show Controller Window (⌘⇧P, or DM_CONTROLLER_WINDOW=1 at launch), then one disabled line per connected controller, or why there is none.
// The panel is a utility NSPanel like the Cheats one (floating, over a fullscreen game too, never key for a click, closed only to hide, frame autosave "DmControllerPanel"). It
// works on one controller and stick at a time (a picker with two or more, Left / Right on a Pro Controller 2): the whole range, the centre zoomed 12x with the dead zone drawn as
// the square s2_cal_axis applies (per axis), a marker where the stick came to rest after each release (orange inside the dead zone, red where the game would see it move), the
// readouts and tally, the dead zone slider (live, and saved as the launcher's choice) and the guided calibration: let go (the rest becomes the centre), edges (two turns at the
// edge give the travel, judged against the factory calibration), review and save (either stick of a Pro Controller 2), and a
// save's outcome stays on screen, naming the controller, whichever controller is shown or connected, until Done. While the panel is open the game gets nothing from Switch 2
// controllers (front_mac.m asks sw2_panel_open), so circles drawn here never move a character
#import "sw2_panel_mac.h"
#include "dm.h"
#include "pad_switch2_apple.h"

#define SW2_MAX 8          // controllers listed at most
#define SW2_MARKERS 20     // release markers kept: the oldest goes
#define SW2_ZOOM 12.f      // the centre view shows the nominal travel over this, each way

static NSWindow *g_game;
static void (*g_release)(void);

// ---- the stick views: what they draw is worked out by the panel ----

// Offsets in raw counts from the centre (y up), the travel each way, the dead zone in counts, the markers and whether the game would see each one move
typedef struct { float x, y, right, left, up, down, dead; int n; float mx[SW2_MARKERS], my[SW2_MARKERS]; unsigned char past[SW2_MARKERS]; } sw2_picture;
// The edges step: the stick against the travel let go measured from (-1..1 each way, y up), the turns so far, the furthest reach each way (right, left, up, down) and the
// S2_REACH_SHORT_ bits of the ways still short of the factory travel
typedef struct { float x, y, turns, reach[4]; int shrt; } sw2_edge;
enum { SW2_VIEW_RANGE, SW2_VIEW_CENTRE, SW2_VIEW_EDGE };

static NSColor *col_dead(void) { return NSColor.systemGreenColor; }
static NSColor *col_live(void) { return NSColor.controlAccentColor; }
static NSColor *col_inside(void) { return NSColor.systemOrangeColor; }
static NSColor *col_past(void) { return NSColor.systemRedColor; }
static NSColor *col_line(void) { return [NSColor.secondaryLabelColor colorWithAlphaComponent:0.25]; }
static NSColor *col_edge(void) { return [NSColor.secondaryLabelColor colorWithAlphaComponent:0.6]; }
static NSColor *col_fill(void) { return [NSColor.secondaryLabelColor colorWithAlphaComponent:0.06]; }

static void dot(NSPoint p, CGFloat r, NSColor *c) { [c setFill]; [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(p.x - r, p.y - r, 2 * r, 2 * r)] fill]; }
static void crosshair(NSPoint c, CGFloat r) {
    NSBezierPath *p = [NSBezierPath bezierPath];
    [p moveToPoint:NSMakePoint(c.x - r, c.y)]; [p lineToPoint:NSMakePoint(c.x + r, c.y)];
    [p moveToPoint:NSMakePoint(c.x, c.y - r)]; [p lineToPoint:NSMakePoint(c.x, c.y + r)];
    p.lineWidth = 1; [col_line() setStroke]; [p stroke];
}
static CGFloat clampf(CGFloat v, CGFloat lo, CGFloat hi) { return v < lo ? lo : v > hi ? hi : v; }

@interface DmCtlStickView : NSView
@property (nonatomic) int kind;
@property (nonatomic) sw2_picture pic;
@property (nonatomic) sw2_edge edge;
@end
@implementation DmCtlStickView
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSSize s = self.bounds.size; NSPoint c = NSMakePoint(s.width / 2, s.height / 2);   // y up: up on the stick is up on the screen
    sw2_picture p = _pic;
    if (_kind == SW2_VIEW_RANGE) {   // the travel as a circle (each side scaled to its own travel), the dead zone square, the stick as a dot
        CGFloat r = fmin(s.width, s.height) / 2 - 6;
        NSBezierPath *disc = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c.x - r, c.y - r, 2 * r, 2 * r)];
        [col_fill() setFill]; [disc fill]; [col_edge() setStroke]; [disc stroke];
        crosshair(c, r);
        NSRect dz = NSMakeRect(c.x - r * p.dead / p.left, c.y - r * p.dead / p.down, r * (p.dead / p.left + p.dead / p.right), r * (p.dead / p.down + p.dead / p.up));
        [[col_dead() colorWithAlphaComponent:0.25] setFill]; NSRectFillUsingOperation(dz, NSCompositingOperationSourceOver);
        NSBezierPath *b = [NSBezierPath bezierPathWithRect:dz]; b.lineWidth = 0.7; [col_dead() setStroke]; [b stroke];
        CGFloat sx = p.x / (p.x >= 0 ? p.right : p.left), sy = p.y / (p.y >= 0 ? p.up : p.down);
        dot(NSMakePoint(c.x + r * clampf(sx, -1.1, 1.1), c.y + r * clampf(sy, -1.1, 1.1)), 4.5, col_live());
    } else if (_kind == SW2_VIEW_CENTRE) {   // the nominal travel over 12 each way: the dead zone square, the rest markers, the stick; anything further out sits on the edge
        CGFloat r = fmin(s.width, s.height) / 2 - 6, k = r / (S2_TRAVEL_NOMINAL / SW2_ZOOM);   // points per count
        NSRect frame = NSMakeRect(c.x - r, c.y - r, 2 * r, 2 * r);
        [col_fill() setFill]; NSRectFillUsingOperation(frame, NSCompositingOperationSourceOver);
        NSBezierPath *f = [NSBezierPath bezierPathWithRect:frame]; f.lineWidth = 1; [col_edge() setStroke]; [f stroke];
        CGFloat h = fmin(p.dead * k, r); NSRect dz = NSMakeRect(c.x - h, c.y - h, 2 * h, 2 * h);
        [[col_dead() colorWithAlphaComponent:0.15] setFill]; NSRectFillUsingOperation(dz, NSCompositingOperationSourceOver);
        NSBezierPath *b = [NSBezierPath bezierPathWithRect:dz]; b.lineWidth = 1; [col_dead() setStroke]; [b stroke];
        crosshair(c, r);
        for (int i = 0; i < p.n; i++) dot(NSMakePoint(c.x + clampf(p.mx[i] * k, -r, r), c.y + clampf(p.my[i] * k, -r, r)), 3.2, p.past[i] ? col_past() : col_inside());
        dot(NSMakePoint(c.x + clampf(p.x * k, -r, r), c.y + clampf(p.y * k, -r, r)), 4.5, col_live());
    } else {   // the edges step: the travel as a dashed circle, the turns as an arc (two: the whole circle), the reach each way as ticks (orange while short), the stick
        sw2_edge e = _edge; CGFloat r = fmin(s.width, s.height) / 2 - 8;
        NSBezierPath *ring = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c.x - r, c.y - r, 2 * r, 2 * r)];
        CGFloat dash[2] = {3, 3}; [ring setLineDash:dash count:2 phase:0]; ring.lineWidth = 1; [col_edge() setStroke]; [ring stroke];
        if (e.turns > 0) {
            NSBezierPath *arc = [NSBezierPath bezierPath];
            [arc appendBezierPathWithArcWithCenter:c radius:r - 2 startAngle:0 endAngle:-360 * fmin(e.turns, 2) / 2 clockwise:YES];
            arc.lineWidth = 3; [col_live() setStroke]; [arc stroke];
        }
        const CGFloat dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1}; const int bit[4] = {S2_REACH_SHORT_RIGHT, S2_REACH_SHORT_LEFT, S2_REACH_SHORT_UP, S2_REACH_SHORT_DOWN};
        for (int i = 0; i < 4; i++) {
            CGFloat v = fmin(e.reach[i], 1.2); NSPoint q = NSMakePoint(c.x + dx[i] * r * v, c.y + dy[i] * r * v), n = NSMakePoint(dy[i] * 5, dx[i] * 5);
            NSBezierPath *t = [NSBezierPath bezierPath]; [t moveToPoint:NSMakePoint(q.x - n.x, q.y - n.y)]; [t lineToPoint:NSMakePoint(q.x + n.x, q.y + n.y)];
            t.lineWidth = 2; [((e.shrt & bit[i]) ? col_inside() : col_dead()) setStroke]; [t stroke];
        }
        dot(NSMakePoint(c.x + r * clampf(e.x, -1.1, 1.1), c.y + r * clampf(e.y, -1.1, 1.1)), 4.5, col_live());
    }
}
@end

// A rounded box behind each part; flipped, so the parts lay out from the top
@interface DmCtlBox : NSView @end
@implementation DmCtlBox
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)dirty { (void)dirty; [NSColor.controlBackgroundColor setFill]; [[NSBezierPath bezierPathWithRoundedRect:self.bounds xRadius:10 yRadius:10] fill]; }
@end
@interface DmCtlFlip : NSView @end
@implementation DmCtlFlip
- (BOOL)isFlipped { return YES; }
@end

// ---- the dead zone's saved choice: launcher.DM_SW2_DEADZONE in the app's preferences domain (its bundle ID), the string the settings row writes (LauncherSettings.swift) ----

static NSString *const kDeadEnv = @"DM_SW2_DEADZONE";
static BOOL env_list_has(const char *list, NSString *name) { return list && [[@(list) componentsSeparatedByString:@" "] containsObject:name]; }
// Why the slider's choice is not kept for the next start (nil: it is saved): launcher.c reads saved choices from the bundle ID's domain, only for settings its Info.plist offers
// (DMSettings), and not at all with DM_SAVED_CHOICES=0, which also keeps the settings window from writing them. A value an env file or the launch environment set is the
// player's fixed choice (DM_ENV_FILE_KEYS / DM_ENV_LAUNCH_KEYS from launcher.c): the settings window does not save over it (LauncherModel.choose), and neither does the slider
static NSString *dead_unsaved(void) {
    if (!NSBundle.mainBundle.bundleIdentifier) return @"For this run only: the game was not started from its app.";
    id rules = [NSBundle.mainBundle objectForInfoDictionaryKey:@"DMSettings"]; BOOL offered = NO;
    if ([rules isKindOfClass:NSArray.class]) for (id r in rules) if ([r isKindOfClass:NSDictionary.class] && [r[@"env"] isEqual:kDeadEnv]) offered = YES;
    if (!offered) return @"For this run only: this app was packed without the Stick dead zone setting.";
    const char *off = getenv("DM_SAVED_CHOICES"); if (off && !strcmp(off, "0")) return @"For this run only: saved choices are off (DM_SAVED_CHOICES=0).";
    if (env_list_has(getenv("DM_ENV_FILE_KEYS"), kDeadEnv)) return @"For this run only: your env file sets DM_SW2_DEADZONE, and its line applies at the next start.";
    if (env_list_has(getenv("DM_ENV_LAUNCH_KEYS"), kDeadEnv)) return @"For this run only: DM_SW2_DEADZONE is set at launch, and that value applies at the next start.";
    return nil;
}

// ---- the panel ----

// The SHORT bits of s2_reach_travel in words
static NSString *short_words(int f) {
    NSMutableArray *sides = [NSMutableArray new];
    if (f & S2_REACH_SHORT_RIGHT) [sides addObject:@"right"];
    if (f & S2_REACH_SHORT_LEFT) [sides addObject:@"left"];
    if (f & S2_REACH_SHORT_UP) [sides addObject:@"up"];
    if (f & S2_REACH_SHORT_DOWN) [sides addObject:@"down"];
    NSString *list = sides.count < 2 ? [sides componentsJoinedByString:@""]
                                     : [NSString stringWithFormat:@"%@ and %@", [[sides subarrayWithRange:NSMakeRange(0, sides.count - 1)] componentsJoinedByString:@", "], sides.lastObject];
    return [NSString stringWithFormat:@"Not all the way out yet: push the stick further %@, as far as it goes, and keep turning.", list];
}

// An outcome in plain words (the session reads the stick's calibration again after any of them, so the game uses what the controller holds now)
static NSString *outcome_words(S2CalOutcome o) {
    switch (o) {
    case S2CalSaved: return @"Saved in the controller: the game uses the new calibration now.";
    case S2CalUnchanged: return @"Nothing changed: the controller's calibration is as it was.";
    case S2CalRestored: return @"The controller did not keep the new calibration, so the old one was restored from the backup. Nothing changed.";
    case S2CalRestoreFailed: return @"The controller did not keep the new calibration, and the old one could not be restored. Its user calibration is blank, so it uses its factory calibration.";
    case S2CalRestoreFailedNotBlank: return @"The controller did not keep the new calibration, and the old one could not be restored: the controller's user calibration may be neither the old nor the new one.";
    case S2CalNoBackup: return @"Nothing changed: the controller's calibration could not be backed up (or the controller went away), so nothing was written.";
    }
    return @"The save ended in a way this window does not know.";
}

typedef enum { ST_TEST, ST_LETGO, ST_EDGES, ST_REVIEW, ST_SAVING, ST_RESULT } sw2_step;

@interface DmControllerPanel : NSObject <NSWindowDelegate>
@property (readonly) BOOL open;
- (void)show;
@end
static DmControllerPanel *g_panel;   // created on the first Show Controller Window, kept for the process's lifetime

@implementation DmControllerPanel {
    NSPanel *_panel;
    DmCtlFlip *_content;
    // the controllers (dm_switch2_controllers) and the one shown: its id, and its serial so the same controller back under a new id keeps its place
    dm_sw2_info _list[SW2_MAX]; int _n;
    BOOL _hasSel; uint32_t _sel; char _selSerial[15];
    int _stick;                  // 0: left (or a Joy-Con 2's only stick), 1: a Pro Controller 2's right stick
    uint16_t _rawX, _rawY;
    float _mx[SW2_MARKERS], _my[SW2_MARKERS]; int _nm;   // where the stick came to rest after each release, raw counts, oldest first
    sw2_step _step;
    s2_cal _old, _new;           // review: the calibration now and the one Save writes
    NSString *_who;              // saving and result: the controller saved to (kind and serial), captured at Save: it may be gone by the outcome
    S2CalOutcome _outcome; NSString *_backup;
    NSString *_hint;             // the calibration's advice, or why it stopped
    double _stillFor;            // let go: the share of the rest time the stick has kept still
    s2_release _release; s2_stick_rest _rest; s2_reach _reach; s2_turns _turns;
    s2_cal _centred;             // let go's result: the controller's calibration moved to the rest; the edges are measured from it
    s2_cal _limits;              // the factory calibration of the stick being calibrated: what its new travel is judged against (s2_reach_travel)
    double _deadShare, _deadKept;   // the slider's value and the one last saved (or read at start)
    // views
    NSPopUpButton *_picker; NSTextField *_kindLabel; NSSegmentedControl *_sticks;
    DmCtlBox *_wait; NSTextField *_waitTitle, *_waitBody;
    DmCtlBox *_live; NSTextField *_liveTitle, *_rangeCap, *_centreCap, *_readout, *_tally, *_deadName, *_deadValue, *_deadNote; DmCtlStickView *_range, *_centre; NSSlider *_dead;
    DmCtlBox *_cal; NSTextField *_calTitle, *_steps, *_whoLabel, *_big, *_text, *_spinText, *_grid, *_foot, *_small, *_hintLabel; NSProgressIndicator *_progress, *_spinner;
    DmCtlStickView *_edge; NSButton *_btnL, *_btnR;
    NSTextField *_note;
    NSString *_pickerSig;        // the controllers the picker's items were made from: rebuilt only when they change, not under an open popup at every report
    CGFloat _height;             // the content's height as update laid it out (fitHeight sizes the panel to it)
}

// ---- the controller and stick shown ----
- (const dm_sw2_info *)device { if (!_hasSel) return NULL; for (int i = 0; i < _n; i++) if (_list[i].id == _sel) return &_list[i]; return NULL; }
- (BOOL)twoSticks { const dm_sw2_info *d = [self device]; return d && d->pid == S2_PID_PRO; }
- (BOOL)rightStick { return [self twoSticks] && _stick == 1; }
// The stick's slot: its raw position, its calibration and its factory one are all at this index (a right Joy-Con 2's only stick is index 0 too: pad_switch2_apple.m fills it so)
- (int)slot { return [self rightStick] ? 1 : 0; }
// The calibration the game uses for this stick: the one read, or the defaults s2_cal_apply falls back to
- (s2_cal)calibration { const dm_sw2_info *d = [self device]; s2_cal c; if (d && d->cal[[self slot]].valid) c = d->cal[[self slot]]; else s2_cal_default(&c); return c; }
- (BOOL)otherSaving { const dm_sw2_info *d = [self device]; return d && d->saving; }   // a save on this controller runs that this panel did not start (DM_SW2_CALIBRATE's), and a second is refused
- (BOOL)canCalibrate { const dm_sw2_info *d = [self device]; return d && d->cal[[self slot]].valid && !d->saving; }
- (BOOL)canSave { return ![self otherSaving]; }
- (BOOL)busy { return _step == ST_SAVING; }
- (BOOL)holdsOutcome { return _step == ST_SAVING || _step == ST_RESULT; }   // stays on screen until Done, whichever controller is shown or connected
- (BOOL)calibrating { return _step == ST_LETGO || _step == ST_EDGES || _step == ST_REVIEW || _step == ST_SAVING; }
- (float)deadCounts { return dm_switch2_dead_zone(); }
// Whether the game would see a stick resting at (x, y) move: s2_cal_apply with today's dead zone gives anything but zero
- (BOOL)past:(float)x y:(float)y {
    s2_cal c = [self calibration]; float ox, oy;
    s2_cal_apply(&c, (uint16_t)clampf(roundf(x), 0, 4095), (uint16_t)clampf(roundf(y), 0, 4095), [self deadCounts], &ox, &oy);
    return ox != 0 || oy != 0;
}

// ---- the session's news ----
- (void)refresh {
    _n = dm_switch2_controllers(_list, SW2_MAX);
    if (![self device]) {
        BOOL lost = _hasSel && [self calibrating] && ![self busy];
        int pick = -1;
        for (int i = 0; i < _n && pick < 0; i++) if (_hasSel && !strcmp(_list[i].serial, _selSerial)) pick = i;   // the same controller, reconnected
        if (pick < 0 && _n) pick = 0;
        _hasSel = pick >= 0;
        if (_hasSel) { _sel = _list[pick].id; memcpy(_selSerial, _list[pick].serial, sizeof _selSerial); }
        [self restart:lost ? @"The controller went away, so the calibration stopped." : nil];
    }
    if (_stick == 1 && ![self twoSticks]) { _stick = 0; [self restart:nil]; }
    [self takeRaw];
}
// The stick's position as the session last had it: a controller or stick just picked shows where it is before its next report
- (void)takeRaw { const dm_sw2_info *d = [self device]; if (d) { _rawX = d->stick[[self slot]][0]; _rawY = d->stick[[self slot]][1]; } }

// Back to the live view with no markers (another controller or stick, or the calibration given up). A save running or its outcome stays: only Done clears it
- (void)restart:(NSString *)why {
    _nm = 0; memset(&_release, 0, sizeof _release);
    if ([self holdsOutcome]) return;
    _step = ST_TEST; _hint = why;
}

- (void)report:(uint32_t)cid {
    if (!_open || !_hasSel || cid != _sel) return;
    _n = dm_switch2_controllers(_list, SW2_MAX);
    const dm_sw2_info *d = [self device]; if (!d) return;
    int s = [self slot]; _rawX = d->stick[s][0]; _rawY = d->stick[s][1];
    s2_cal c = [self calibration]; float out[2]; BOOL full = NO;
    switch (_step) {
    case ST_TEST: case ST_RESULT:
        if (s2_release_update(&_release, &c, _rawX, _rawY, d->clock_ms, out)) {
            if (_nm == SW2_MARKERS) { memmove(_mx, _mx + 1, sizeof _mx - sizeof *_mx); memmove(_my, _my + 1, sizeof _my - sizeof *_my); _nm--; }
            _mx[_nm] = out[0]; _my[_nm] = out[1]; _nm++; full = YES;   // the tally's text may wrap differently
        }
        break;
    case ST_LETGO: full = [self letGo:d->clock_ms read:d->cal[s] factory:d->factory[s]]; break;
    case ST_EDGES: full = [self edges]; break;
    default: break;
    }
    if (full) [self update]; else [self updateLive];
}

// ---- the calibration ----
- (void)calibrate:(id)sender { (void)sender; if (![self canCalibrate] || [self calibrating]) return; memset(&_rest, 0, sizeof _rest); _stillFor = 0; _hint = nil; _step = ST_LETGO; [self update]; }
- (void)cancel:(id)sender { (void)sender; if ([self busy]) return; [self restart:nil]; [self update]; }
- (void)edgesAgain { memset(&_reach, 0, sizeof _reach); memset(&_turns, 0, sizeof _turns); _hint = nil; }
- (void)startOver:(id)sender { (void)sender; [self edgesAgain]; [self update]; }

// Let go: two seconds still, near the centre, and a centre that leaves travel each way; returns whether the panel's text changed
- (BOOL)letGo:(uint32_t)ms read:(s2_cal)read factory:(s2_cal)factory {
    float out[2];
    BOOL done = s2_stick_rest_update(&_rest, _rawX, _rawY, ms, out) != 0;
    _stillFor = _rest.n > 0 ? fmin(1, (double)(uint32_t)(ms - _rest.t0) / S2_STICK_REST_MS) : 0;
    if (!done) return NO;
    memset(&_rest, 0, sizeof _rest); _stillFor = 0;
    if (!s2_stick_rest_near(&read, out[0], out[1])) { _hint = @"The stick came to rest far from its centre, as if held: let go of it completely."; return YES; }
    s2_cal moved;
    if (!s2_cal_recentre(&read, out[0], out[1], &moved)) { _hint = @"That centre would leave the stick too little travel on one side: let go of it and wait again."; return YES; }
    _centred = moved; _limits = factory; [self edgesAgain]; _step = ST_EDGES;
    return YES;
}

- (sw2_edge)edgePicture {
    sw2_edge e; s2_cal c = _centred;
    e.x = s2_share((float)_rawX - c.cx, c.maxx, c.minx); e.y = s2_share((float)_rawY - c.cy, c.maxy, c.miny); e.turns = _turns.turns;
    e.reach[0] = _reach.up[0] / fmaxf(c.maxx, 1); e.reach[1] = _reach.down[0] / fmaxf(c.minx, 1); e.reach[2] = _reach.up[1] / fmaxf(c.maxy, 1); e.reach[3] = _reach.down[1] / fmaxf(c.miny, 1);
    e.shrt = s2_reach_short(&_reach, &_limits);
    return e;
}

// Edges: the reach each way and the turns; after two turns the travel, or what is wrong with it. Returns whether the panel's text changed
- (BOOL)edges {
    s2_reach_update(&_reach, &_centred, _rawX, _rawY);
    sw2_edge e = [self edgePicture]; float was = _turns.turns;
    s2_turns_update(&_turns, e.x, e.y);
    BOOL changed = (was < 1) != (_turns.turns < 1) || (was < S2_TURNS_WANTED) != (_turns.turns < S2_TURNS_WANTED);   // the turn label's text
    if (_turns.turns < S2_TURNS_WANTED) return changed;
    s2_cal nc; int f = s2_reach_travel(&_reach, &_centred, &_limits, &nc);
    const dm_sw2_info *d = [self device]; NSString *wasHint = _hint;
    if (f == 0 && d) { _hint = nil; _old = d->cal[[self slot]]; _new = nc; _step = ST_REVIEW; return YES; }
    if (f & S2_REACH_TOO_FAR) { [self edgesAgain]; _hint = @"The stick read further out than its edge can be, so the circles started over: rotate it slowly around its edge, twice."; return YES; }
    if (f) _hint = short_words(f);
    return changed || ![_hint isEqualToString:wasHint];
}

// Save: the controller is named now (kind and serial), since the outcome must say which one even after it went away (a disconnect mid-save fires the session's changed
// while the save still runs, with the controller already off the list)
- (void)save:(id)sender {
    (void)sender;
    const dm_sw2_info *d = [self device];
    if (_step != ST_REVIEW || ![self canSave] || !d) return;
    _who = [NSString stringWithFormat:@"%s %s", d->kind, d->serial]; _step = ST_SAVING; _hint = nil; _backup = nil;
    __weak DmControllerPanel *w = self;
    dm_switch2_save(d->id, [self slot], _new, ^(S2CalOutcome o, NSString *path) {
        DmControllerPanel *s = w; if (!s) return;
        s->_step = ST_RESULT; s->_outcome = o; s->_backup = path; memset(&s->_release, 0, sizeof s->_release);
        [s update];
    });
    [self update];
}
// Done after a save: its outcome goes, and the markers with it
- (void)dismiss:(id)sender { (void)sender; if (_step != ST_RESULT) return; _nm = 0; memset(&_release, 0, sizeof _release); _step = ST_TEST; _hint = nil; _who = nil; _backup = nil; [self update]; }
- (void)showBackup:(id)sender { (void)sender; if (_backup) [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:_backup]]]; }

// ---- the picker, the stick switch and the dead zone ----
- (void)picked:(NSPopUpButton *)p {
    int i = (int)p.selectedTag; if (i < 0 || i >= _n || (_hasSel && _list[i].id == _sel)) return;
    _hasSel = YES; _sel = _list[i].id; memcpy(_selSerial, _list[i].serial, sizeof _selSerial);
    if (_stick == 1 && _list[i].pid != S2_PID_PRO) _stick = 0;
    [self restart:nil]; [self takeRaw]; [self update];
}
- (void)stickPicked:(NSSegmentedControl *)s { int v = (int)s.selectedSegment; if (v == _stick || v < 0) return; _stick = v; [self restart:nil]; [self takeRaw]; [self update]; }
// The slider: the game's dead zone follows at once; the choice is saved (and logged) when the knob is let go, as the settings row's value (two decimals) under the launcher's key.
// Until the knob really moves the host's own value stays (40 counts unset is 0.0333, which a 0.01 step would turn into 0.03 at a mere click)
- (void)deadMoved:(NSSlider *)s {
    NSEventType t = NSApp.currentEvent.type;
    BOOL dragging = t == NSEventTypeLeftMouseDragged || t == NSEventTypeLeftMouseDown;
    if (s.doubleValue != _deadShare) {
        double v = round(s.doubleValue * 100) / 100; s.doubleValue = v;   // the settings row's step, 0.01
        if (v != _deadShare) { _deadShare = v; dm_switch2_set_dead_zone((float)v * S2_TRAVEL_NOMINAL); }
    }
    if (!dragging && _deadShare != _deadKept) {
        double v = _deadShare; _deadKept = v;
        NSString *val = [NSString stringWithFormat:@"%.2f", v], *why = dead_unsaved(), *bid = NSBundle.mainBundle.bundleIdentifier;
        if (!why) {   // CFPreferences on the bundle ID, as launcher.c reads it: dmhost runs inside the app (Contents/MacOS), so its main bundle is the app's
            CFPreferencesSetAppValue((__bridge CFStringRef)[@"launcher." stringByAppendingString:kDeadEnv], (__bridge CFStringRef)val, (__bridge CFStringRef)bid);
            CFPreferencesAppSynchronize((__bridge CFStringRef)bid);
        }
        LOG("front end: Switch 2 stick dead zone %s (%.0f counts), %s", val.UTF8String, v * S2_TRAVEL_NOMINAL, why ? "for this run only" : "saved as the launcher's choice");
    }
    if (dragging) {   // at event rate only what the dead zone changes: its label, the squares, the markers' colours and the tally; the layout once the knob is let go
        _deadValue.stringValue = [self deadLabel]; _tally.attributedStringValue = [self tallyText]; [self updateLive];
    } else [self update];
}

// ---- building ----
static NSTextField *label(CGFloat size, NSFontWeight w, NSColor *c) {
    NSTextField *l = [NSTextField wrappingLabelWithString:@""];
    l.font = [NSFont systemFontOfSize:size weight:w]; l.textColor = c; l.selectable = NO;   // a selectable label would make the panel key on a click and keep the keyboard from the game
    return l;
}
static NSButton *button(NSString *title, id target) { NSButton *b = [NSButton buttonWithTitle:title target:target action:nil]; return b; }

- (void)makePanel {
    _panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 690, 520) styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow
                                          backing:NSBackingStoreBuffered defer:NO];
    _panel.title = @"Controller";
    _panel.releasedWhenClosed = NO;         // the close button only hides (windowShouldClose)
    _panel.floatingPanel = YES;             // above the game window
    _panel.becomesKeyOnlyIfNeeded = YES;    // a click never takes the key window: the game keeps the keyboard
    _panel.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary;   // shows over the fullscreen game too
    _panel.delegate = self;
    _content = [[DmCtlFlip alloc] initWithFrame:NSMakeRect(0, 0, 690, 520)];
    _panel.contentView = _content;
    NSColor *pri = NSColor.labelColor, *sec = NSColor.secondaryLabelColor;

    _picker = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO]; _picker.target = self; _picker.action = @selector(picked:);
    _kindLabel = [NSTextField labelWithString:@""]; _kindLabel.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    _sticks = [NSSegmentedControl segmentedControlWithLabels:@[@"Left stick", @"Right stick"] trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(stickPicked:)];

    _wait = [DmCtlBox new]; _waitTitle = label(15, NSFontWeightRegular, pri); _waitBody = label(13, NSFontWeightRegular, sec);
    for (NSView *v in @[_waitTitle, _waitBody]) [_wait addSubview:v];

    _live = [DmCtlBox new];
    _liveTitle = label(13, NSFontWeightSemibold, pri); _liveTitle.stringValue = @"Live";
    _range = [[DmCtlStickView alloc] initWithFrame:NSZeroRect]; _range.kind = SW2_VIEW_RANGE;
    _centre = [[DmCtlStickView alloc] initWithFrame:NSZeroRect]; _centre.kind = SW2_VIEW_CENTRE;
    _range.toolTip = @"The stick's whole range"; _centre.toolTip = @"The stick's centre, zoomed, with where it came to rest after each release";
    _rangeCap = label(10, NSFontWeightRegular, pri); _rangeCap.stringValue = @"Whole range"; _rangeCap.alignment = NSTextAlignmentCenter;
    _centreCap = label(10, NSFontWeightRegular, pri); _centreCap.stringValue = @"Centre, zoomed 12×"; _centreCap.alignment = NSTextAlignmentCenter;
    _readout = label(13, NSFontWeightRegular, pri); _readout.font = [NSFont monospacedDigitSystemFontOfSize:13 weight:NSFontWeightRegular];
    _tally = label(13, NSFontWeightRegular, sec);
    _deadName = label(13, NSFontWeightRegular, pri); _deadName.stringValue = @"Dead zone";
    _deadValue = label(13, NSFontWeightRegular, sec); _deadValue.font = [NSFont monospacedDigitSystemFontOfSize:13 weight:NSFontWeightRegular]; _deadValue.alignment = NSTextAlignmentRight;
    _dead = [NSSlider sliderWithValue:0 minValue:0 maxValue:0.25 target:self action:@selector(deadMoved:)];   // the settings row's range (dm/games/settings.tsv), a share of S2_TRAVEL_NOMINAL
    _dead.continuous = YES; _dead.toolTip = @"Dead zone";
    _deadNote = label(11, NSFontWeightRegular, sec);
    for (NSView *v in @[_liveTitle, _range, _centre, _rangeCap, _centreCap, _readout, _tally, _deadName, _deadValue, _dead, _deadNote]) [_live addSubview:v];

    _cal = [DmCtlBox new];
    _calTitle = label(13, NSFontWeightSemibold, pri); _calTitle.stringValue = @"Calibrate";
    _steps = label(12, NSFontWeightRegular, pri);
    _whoLabel = label(12, NSFontWeightRegular, sec);
    _big = label(15, NSFontWeightRegular, pri);
    _text = label(13, NSFontWeightRegular, pri);
    _progress = [NSProgressIndicator new]; _progress.style = NSProgressIndicatorStyleBar; _progress.indeterminate = NO; _progress.minValue = 0; _progress.maxValue = 1;
    _spinner = [NSProgressIndicator new]; _spinner.style = NSProgressIndicatorStyleSpinning; _spinner.controlSize = NSControlSizeSmall;
    _spinText = label(13, NSFontWeightRegular, pri); _spinText.stringValue = @"Saving: backing up, writing, reading back…";
    _edge = [[DmCtlStickView alloc] initWithFrame:NSZeroRect]; _edge.kind = SW2_VIEW_EDGE; _edge.toolTip = @"Turns around the stick's edge";
    _grid = label(13, NSFontWeightRegular, pri);
    _foot = label(11, NSFontWeightRegular, sec);
    _small = label(13, NSFontWeightRegular, sec);
    _hintLabel = label(13, NSFontWeightRegular, NSColor.systemOrangeColor);
    _btnL = button(@"", self); _btnR = button(@"", self);
    for (NSView *v in @[_calTitle, _steps, _whoLabel, _big, _text, _progress, _spinner, _spinText, _edge, _grid, _foot, _small, _hintLabel, _btnL, _btnR]) [_cal addSubview:v];

    _note = label(11, NSFontWeightRegular, sec);
    _note.stringValue = @"While this window is open the game gets no input from Switch 2 controllers: close it to play.";
    for (NSView *v in @[_picker, _kindLabel, _sticks, _wait, _live, _cal, _note]) [_content addSubview:v];

    _deadShare = _deadKept = dm_switch2_dead_zone() / S2_TRAVEL_NOMINAL;   // what the game uses now (DM_SW2_DEADZONE as the host read it, 40 counts unset)
    __weak DmControllerPanel *w = self;
    dm_switch2_observe(^{ DmControllerPanel *s = w; if (!s) return; [s refresh]; if (s->_open) [s update]; },
                       ^(uint32_t cid) { [w report:cid]; });
    [self refresh];
    [self update];   // sizes the panel before the saved frame's place is applied
    _panel.frameAutosaveName = @"DmControllerPanel";
    if (![_panel setFrameUsingName:@"DmControllerPanel"] && g_game) {   // first use: beside the game window, below the Cheats panel's first place
        NSRect g = g_game.frame;
        [_panel setFrameOrigin:NSMakePoint(NSMaxX(g) + 8, NSMinY(g))];
    }
    [self fitHeight];
}

// ---- filling and laying out (flipped: y down from the top) ----

// v placed at x, y, w wide and as tall as its text wraps to (or h); returns the y below it with gap
static CGFloat place(NSView *v, CGFloat x, CGFloat y, CGFloat w, CGFloat h, CGFloat gap) {
    if ([v isKindOfClass:NSTextField.class] && h <= 0) h = ceil([((NSTextField *)v).cell cellSizeForBounds:NSMakeRect(0, 0, w, 10000)].height);
    v.frame = NSMakeRect(x, y, w, h); v.hidden = NO;
    return y + h + gap;
}
static void hide(NSArray<NSView *> *vs) { for (NSView *v in vs) v.hidden = YES; }

- (sw2_picture)picture {
    sw2_picture p; memset(&p, 0, sizeof p); s2_cal c = [self calibration];
    p.x = (float)_rawX - c.cx; p.y = (float)_rawY - c.cy; p.right = c.maxx; p.left = c.minx; p.up = c.maxy; p.down = c.miny; p.dead = [self deadCounts];
    p.n = _nm;
    for (int i = 0; i < _nm; i++) { p.mx[i] = _mx[i] - c.cx; p.my[i] = _my[i] - c.cy; p.past[i] = [self past:_mx[i] y:_my[i]]; }
    return p;
}
- (NSAttributedString *)tallyText {
    NSDictionary *sec = @{NSForegroundColorAttributeName: NSColor.secondaryLabelColor, NSFontAttributeName: [NSFont systemFontOfSize:13]};
    if (!_nm) return [[NSAttributedString alloc] initWithString:@"Push the stick and let it go: each release leaves a dot where it came to rest." attributes:sec];
    int past = 0; for (int i = 0; i < _nm; i++) past += [self past:_mx[i] y:_my[i]];
    NSMutableAttributedString *t = [[NSMutableAttributedString alloc] initWithString:[NSString stringWithFormat:@"Last %d release%s came to rest: %d inside, ", _nm, _nm == 1 ? "" : "s", _nm - past] attributes:sec];
    NSMutableDictionary *p = [sec mutableCopy]; if (past) p[NSForegroundColorAttributeName] = col_past();
    [t appendAttributedString:[[NSAttributedString alloc] initWithString:[NSString stringWithFormat:@"%d past the dead zone", past] attributes:p]];
    return t;
}
- (NSAttributedString *)stepsText {
    int place = _step == ST_LETGO ? 1 : _step == ST_EDGES ? 2 : _step == ST_REVIEW || _step == ST_SAVING ? 3 : 0;
    NSArray *names = @[@"1 Let go", @"2 Edges", @"3 Review and save"];
    NSMutableAttributedString *t = [NSMutableAttributedString new];
    for (int i = 1; i <= 3; i++) {
        if (i > 1) [t appendAttributedString:[[NSAttributedString alloc] initWithString:@" · " attributes:@{NSForegroundColorAttributeName: NSColor.secondaryLabelColor, NSFontAttributeName: [NSFont systemFontOfSize:12]}]];
        [t appendAttributedString:[[NSAttributedString alloc] initWithString:names[i - 1] attributes:@{NSForegroundColorAttributeName: i == place ? NSColor.labelColor : NSColor.secondaryLabelColor,
                                                                                                       NSFontAttributeName: [NSFont systemFontOfSize:12 weight:i == place ? NSFontWeightSemibold : NSFontWeightRegular]}]];
    }
    return t;
}
// Review: old against new, travel in counts from the centre, in columns
- (NSAttributedString *)gridText {
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.tabStops = @[[[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft location:64 options:@{}], [[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft location:160 options:@{}]];
    ps.paragraphSpacing = 3;
    NSFont *f = [NSFont monospacedDigitSystemFontOfSize:13 weight:NSFontWeightRegular];
    NSMutableAttributedString *t = [[NSMutableAttributedString alloc] initWithString:@"\tnow\tnew\n" attributes:@{NSFontAttributeName: f, NSParagraphStyleAttributeName: ps, NSForegroundColorAttributeName: NSColor.secondaryLabelColor}];
    s2_cal o = _old, n = _new;
    NSString *rows = [NSString stringWithFormat:@"Centre\t%.0f, %.0f\t%.0f, %.0f\nRight\t%.0f\t%.0f\nLeft\t%.0f\t%.0f\nUp\t%.0f\t%.0f\nDown\t%.0f\t%.0f",
                      roundf(o.cx), roundf(o.cy), roundf(n.cx), roundf(n.cy), roundf(o.maxx), roundf(n.maxx), roundf(o.minx), roundf(n.minx), roundf(o.maxy), roundf(n.maxy), roundf(o.miny), roundf(n.miny)];
    [t appendAttributedString:[[NSAttributedString alloc] initWithString:rows attributes:@{NSFontAttributeName: f, NSParagraphStyleAttributeName: ps, NSForegroundColorAttributeName: NSColor.labelColor}]];
    return t;
}
- (NSString *)deadLabel { return [NSString stringWithFormat:@"%d %% · %d counts", (int)lround(_deadShare * 100), (int)lroundf([self deadCounts])]; }   // as the settings row's label, with the counts

// The parts that change with every report: the dots, the readout, let go's progress, the edges view and its turns
- (void)updateLive {
    if (!_open) return;
    s2_cal c = [self calibration];
    _range.pic = _centre.pic = [self picture]; _range.needsDisplay = _centre.needsDisplay = YES;
    _readout.stringValue = [NSString stringWithFormat:@"raw %u, %u · off centre %d, %d", _rawX, _rawY, (int)lroundf((float)_rawX - c.cx), (int)lroundf((float)_rawY - c.cy)];
    if (_step == ST_LETGO) _progress.doubleValue = _stillFor;
    if (_step == ST_EDGES) {
        _edge.edge = [self edgePicture]; _edge.needsDisplay = YES;
        _small.stringValue = _turns.turns < 1 ? @"First turn" : _turns.turns < S2_TURNS_WANTED ? @"Second turn" : @"Both turns done";
    }
}

// Everything: texts, which parts show, their places, the panel's height
- (void)update {
    if (!_panel) return;
    const CGFloat W = 690, P = 14, LW = 354, CW = 294, IN = 12;
    const dm_sw2_info *d = [self device];
    BOOL calibrating = [self calibrating];
    // the top row: the picker with two or more controllers, else the one's kind; Left / Right on a Pro Controller 2
    hide(@[_picker, _kindLabel, _sticks]);
    CGFloat x = P;
    if (_n > 1) {
        NSMutableArray<NSString *> *titles = [NSMutableArray new];
        for (int i = 0; i < _n; i++) [titles addObject:[NSString stringWithFormat:@"%s · %s", _list[i].kind, _list[i].serial]];
        NSString *sig = [titles componentsJoinedByString:@"\n"];
        if (![sig isEqualToString:_pickerSig]) {
            _pickerSig = sig; [_picker removeAllItems];
            for (int i = 0; i < _n; i++) { [_picker addItemWithTitle:titles[i]]; _picker.lastItem.tag = i; }   // two Joy-Cons of one kind differ by serial, so no title repeats
        }
        for (int i = 0; i < _n; i++) if (d == &_list[i] && _picker.selectedTag != i) [_picker selectItemWithTag:i];
        [_picker sizeToFit]; place(_picker, x, P, _picker.frame.size.width, 25, 0); x += _picker.frame.size.width + 10;
    } else if (d) {
        _kindLabel.stringValue = @(d->kind); [_kindLabel sizeToFit]; place(_kindLabel, x, P + 4, _kindLabel.frame.size.width + 4, 0, 0); x += _kindLabel.frame.size.width + 10;
    }
    if ([self twoSticks]) { _sticks.selectedSegment = _stick; [_sticks sizeToFit]; place(_sticks, x, P + 1, _sticks.frame.size.width, 24, 0); }
    _picker.enabled = _sticks.enabled = !calibrating;
    CGFloat top = P + 26 + 10, bottom;

    if (!d && ![self holdsOutcome]) {   // nothing to show: why, and how to connect one
        hide(@[_live, _cal]);
        [self fillWait];
        CGFloat y = place(_waitTitle, IN, IN, W - 2 * P - 2 * IN, 0, 6); y = place(_waitBody, IN, y, W - 2 * P - 2 * IN, 0, IN);
        bottom = place(_wait, P, top, W - 2 * P, fmax(y, 200 + 2 * IN), 10);
    } else {
        CGFloat lh;
        if (d) { hide(@[_wait]); lh = [self layoutLive:LW inner:IN]; }
        else { hide(@[_live]); [self fillWait]; CGFloat y = place(_waitTitle, IN, IN, LW - 2 * IN, 0, 6); y = place(_waitBody, IN, y, LW - 2 * IN, 0, IN); lh = fmax(y, 200 + 2 * IN); }
        CGFloat ch = [self layoutCal:CW inner:IN];
        CGFloat h = fmax(lh, ch);
        place(d ? _live : _wait, P, top, LW, h, 0);
        place(_cal, P + LW + 14, top, CW, h, 0);
        [self placeButtons:CW inner:IN height:h];
        bottom = top + h + 10;
    }
    _height = place(_note, P, bottom, W - 2 * P, 0, P);
    [self updateLive];
    [self fitHeight];
}
- (void)fillWait {
    if (!dm_switch2_on()) {
        _waitTitle.stringValue = @"Switch 2 controllers are off";
        _waitBody.stringValue = @"Turn them on in the settings window (Controls) and start the game again.";
    } else {
        _waitTitle.stringValue = @"Press a button on your controller";
        _waitBody.stringValue = @"A controller paired with this Mac connects on a button press. To pair one, hold its small Sync button until the player lights sweep.";
    }
}
// The Live box's parts; returns its height
- (CGFloat)layoutLive:(CGFloat)bw inner:(CGFloat)in {
    CGFloat w = bw - 2 * in, y = place(_liveTitle, in, in, w, 0, 8);
    place(_range, in, y, 150, 150, 0); place(_centre, in + 150 + 12, y, 150, 150, 0); y += 154;
    place(_rangeCap, in, y, 150, 0, 0); y = place(_centreCap, in + 150 + 12, y, 150, 0, 8);
    y = place(_readout, in, y, w, 0, 6);
    _tally.attributedStringValue = [self tallyText]; y = place(_tally, in, y, w, 0, 10);
    _deadValue.stringValue = [self deadLabel];
    place(_deadName, in, y, w / 2, 0, 0); y = place(_deadValue, in + w / 2, y, w / 2, 0, 4);
    _dead.doubleValue = _deadShare; y = place(_dead, in, y, w, 21, 4);
    NSString *note = dead_unsaved();
    if (note) { _deadNote.stringValue = note; y = place(_deadNote, in, y, w, 0, 0); } else _deadNote.hidden = YES;
    return y + in;
}
// The Calibrate box's parts above its buttons, for the step; returns the height it needs, buttons included
- (CGFloat)layoutCal:(CGFloat)bw inner:(CGFloat)in {
    hide(@[_whoLabel, _big, _text, _progress, _spinner, _spinText, _edge, _grid, _foot, _small, _hintLabel, _btnL, _btnR]);
    CGFloat w = bw - 2 * in, y = place(_calTitle, in, in, w, 0, 6);
    _steps.attributedStringValue = [self stepsText]; y = place(_steps, in, y, w, 0, 10);
    NSMutableArray<NSString *> *small = [NSMutableArray new];
    switch (_step) {
    case ST_TEST:
        _text.stringValue = @"Sets the stick's centre and how far it reaches, as the console's calibration does, and saves them in the controller.";
        y = place(_text, in, y, w, 0, 8);
        if ([self otherSaving]) [small addObject:@"A save on this controller is still running."];
        else if ([self device] && ![self canCalibrate]) [small addObject:@"This stick's calibration could not be read, so it cannot be calibrated here."];
        break;
    case ST_LETGO:
        _big.stringValue = @"Let go of the stick and leave it still."; y = place(_big, in, y, w, 0, 8);
        _progress.doubleValue = _stillFor; y = place(_progress, in, y, w, 12, 8);
        [small addObject:@"Where it comes to rest becomes its centre."];
        break;
    case ST_EDGES:
        _big.stringValue = @"Rotate the stick slowly around its edge, twice"; y = place(_big, in, y, w, 0, 8);
        y = place(_edge, in + (w - 130) / 2, y, 130, 130, 8);
        [small addObject:_turns.turns < 1 ? @"First turn" : _turns.turns < S2_TURNS_WANTED ? @"Second turn" : @"Both turns done"];
        break;
    case ST_REVIEW:
        _grid.attributedStringValue = [self gridText]; y = place(_grid, in, y, w, 0, 6); _foot.toolTip = nil;
        _foot.stringValue = @"Travel in counts from the centre. Save backs up the controller's calibration memory first, then writes and reads it back; if it does not read back as written, the backup goes back in.";
        y = place(_foot, in, y, w, 0, 8);
        if ([self otherSaving]) [small addObject:@"A save on this controller is still running."];
        break;
    case ST_SAVING:
        _whoLabel.stringValue = _who ?: @""; y = place(_whoLabel, in, y, w, 0, 6);
        place(_spinner, in, y + 1, 16, 16, 0); [_spinner startAnimation:nil]; y = place(_spinText, in + 22, y, w - 22, 0, 8);
        break;
    case ST_RESULT:
        _whoLabel.stringValue = _who ?: @""; y = place(_whoLabel, in, y, w, 0, 6);
        _text.stringValue = outcome_words(_outcome); y = place(_text, in, y, w, 0, 6);
        if (_backup) {
            BOOL failed = _outcome == S2CalRestoreFailed || _outcome == S2CalRestoreFailedNotBlank;
            _foot.stringValue = [NSString stringWithFormat:failed ? @"The backup is at %@" : @"Backup: %@", _backup]; _foot.toolTip = _backup;
            y = place(_foot, in, y, w, 0, 8);
        }
        break;
    }
    if (_step != ST_SAVING) [_spinner stopAnimation:nil];
    if (small.count) { _small.stringValue = [small componentsJoinedByString:@"\n"]; y = place(_small, in, y, w, 0, 8); }
    if (_hint) { _hintLabel.stringValue = _hint; y = place(_hintLabel, in, y, w, 0, 8); }
    return fmax(y + 32 + in, 330 + 2 * in);   // the buttons' row below; at least 330 points, so the steps do not jump in height
}
// The step's buttons, at the bottom right of the Calibrate box
- (void)placeButtons:(CGFloat)bw inner:(CGFloat)in height:(CGFloat)h {
    NSButton *l = _btnL, *r = _btnR; SEL ls = NULL, rs = NULL; NSString *lt = nil, *rt = nil; BOOL ron = YES;
    switch (_step) {
    case ST_TEST: rt = @"Calibrate…"; rs = @selector(calibrate:); ron = [self canCalibrate]; break;
    case ST_LETGO: rt = @"Cancel"; rs = @selector(cancel:); break;
    case ST_EDGES: lt = @"Cancel"; ls = @selector(cancel:); rt = @"Start over"; rs = @selector(startOver:); break;
    case ST_REVIEW: lt = @"Cancel"; ls = @selector(cancel:); rt = @"Save"; rs = @selector(save:); ron = [self canSave]; break;
    case ST_SAVING: break;
    case ST_RESULT: if (_backup) { lt = @"Show Backup"; ls = @selector(showBackup:); } rt = @"Done"; rs = @selector(dismiss:); break;
    }
    CGFloat right = bw - in + 6;   // a push button's frame reaches about 6 points past its bezel each side: frames side by side look 12 apart
    if (rt) { r.title = rt; r.action = rs; r.enabled = ron; [r sizeToFit]; NSSize s = r.frame.size; place(r, right - s.width, h - in - s.height + 4, s.width, s.height, 0); right -= s.width; }
    if (lt) { l.title = lt; l.action = ls; l.enabled = YES; [l sizeToFit]; NSSize s = l.frame.size; place(l, right - s.width, h - in - s.height + 4, s.width, s.height, 0); }
}
// The panel as tall as its parts, its top edge kept
- (void)fitHeight {
    NSRect c = [_panel contentRectForFrameRect:_panel.frame]; const CGFloat w = 690;
    if (fabs(c.size.height - _height) < 0.5 && fabs(c.size.width - w) < 0.5) return;
    [_panel setFrame:[_panel frameRectForContentRect:NSMakeRect(c.origin.x, NSMaxY(c) - _height, w, _height)] display:YES];
}

// ---- shown and closed ----

// Shown: ordered front, not key, so the game keeps the keyboard. Opening it takes the Switch 2 controllers from the game: their state goes neutral once, then front_mac.m sends
// the game nothing from them while it is open (sw2_panel_open); the releases it marks start from the next push
- (void)show {
    if (!_panel) [self makePanel];
    if (!_open) {
        _open = YES; memset(&_release, 0, sizeof _release);
        if (g_release) g_release();
        LOG("front end: Controller window open: the game gets no Switch 2 controller input until it closes");
    }
    _deadShare = dm_switch2_dead_zone() / S2_TRAVEL_NOMINAL;
    [self refresh];
    [self update];
    [_panel orderFront:self];
}
// The close button: hide, not close. A calibration under way is given up (the stick goes back to the game); a save runs on, and its outcome waits for the next open
- (BOOL)windowShouldClose:(NSWindow *)w {
    if (w != _panel) return YES;
    if (_step == ST_LETGO || _step == ST_EDGES || _step == ST_REVIEW) [self restart:nil];
    _open = NO;
    if (g_game && _panel.isKeyWindow) [g_game makeKeyWindow];
    [_panel orderOut:nil];
    LOG("front end: Controller window closed: Switch 2 controllers reach the game again");
    return NO;
}
// Became key anyway (Full Keyboard Access on a button, say): the keyboard goes back to the game a main-queue turn later, once the click has settled
- (void)windowDidBecomeKey:(NSNotification *)n { (void)n; dispatch_async(dispatch_get_main_queue(), ^{ if (g_game && self->_panel.isKeyWindow) [g_game makeKeyWindow]; }); }
@end

// ---- the Controller menu: rebuilt on every open, so the list is the session's now ----

@interface DmControllerMenuDelegate : NSObject <NSMenuDelegate> @end
@implementation DmControllerMenuDelegate
- (void)menuNeedsUpdate:(NSMenu *)menu {
    [menu removeAllItems];
    NSMenuItem *sh = [menu addItemWithTitle:@"Show Controller Window" action:@selector(showPanel:) keyEquivalent:@"p"];
    sh.target = self;
    sh.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;   // ⌘⇧P: no other menu item has it (⌘Q, ⌘⇧C, ⌘R, ⌃⌘F), and the key map never sees ⌘
    [menu addItem:NSMenuItem.separatorItem];
    if (!dm_switch2_on()) { [menu addItemWithTitle:@"Switch 2 controllers are off: turn them on in the settings window (Controls)" action:nil keyEquivalent:@""].enabled = NO; return; }
    dm_sw2_info l[SW2_MAX]; int n = dm_switch2_controllers(l, SW2_MAX);
    if (!n) [menu addItemWithTitle:@"No Switch 2 controller connected: press a button on one" action:nil keyEquivalent:@""].enabled = NO;
    for (int i = 0; i < n; i++) {
        size_t len = strlen(l[i].serial); const char *tail = l[i].serial + (len > 4 ? len - 4 : 0);
        NSString *who = l[i].player >= 0 ? [NSString stringWithFormat:@"player %d", l[i].player + 1] : @"no player";
        [menu addItemWithTitle:[NSString stringWithFormat:@"%s · …%s · %@", l[i].kind, tail, who] action:nil keyEquivalent:@""].enabled = NO;
    }
}
- (void)showPanel:(NSMenuItem *)it { (void)it; sw2_panel_show(); }
@end
static DmControllerMenuDelegate *g_menu;   // NSMenu keeps its delegate unretained

void sw2_panel_setup(NSWindow *game, void (*release)(void)) { g_game = game; g_release = release; }
NSMenu *sw2_panel_menu(void) {
    g_menu = [DmControllerMenuDelegate new];
    NSMenu *m = [[NSMenu alloc] initWithTitle:@"Controller"];
    m.delegate = g_menu;
    m.autoenablesItems = NO;   // the delegate sets enabled on every row itself
    [g_menu menuNeedsUpdate:m];   // AppKit matches ⌘⇧P only against items that exist, and fills a delegate's menu only when it is opened
    return m;
}
void sw2_panel_show(void) { if (!g_panel) g_panel = [DmControllerPanel new]; [g_panel show]; }
int sw2_panel_open(void) { return g_panel && g_panel.open; }
