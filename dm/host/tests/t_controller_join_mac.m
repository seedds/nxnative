// Real upstream card + real configuration sheet, with synthetic controller state.
#import "../controller_panel_mac.h"
#import "../controller_join_apple.h"
#import "../controller_join_mac.h"
#include <assert.h>

static void settle(BOOL (^ready)(void)) {
    NSDate *end=[NSDate dateWithTimeIntervalSinceNow:5];
    while (!ready() && end.timeIntervalSinceNow>0) [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
    assert(ready());
}
static NSButton *configureButton(NSView *view) {
    if ([view isKindOfClass:NSButton.class] && [view.identifier isEqual:@"controller.join.configure"]) return (id)view;
    for (NSView *child in view.subviews) { NSButton *found=configureButton(child); if (found) return found; }
    return nil;
}
int main(void) { @autoreleasepool {
    [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited]; [NSApp finishLaunching];
    NSWindow *parent=[[NSWindow alloc] initWithContentRect:NSMakeRect(-10000,-10000,900,800) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    parent.releasedWhenClosed=NO; [parent orderBack:nil];
    DmControllerJoin *join=[DmControllerJoin new];
    DmControllerUI *ui=[DmControllerUI new]; ui.parent=parent;
    NSArray *devices=@[@{@"id":@"a",@"name":@"DualSense"},@{@"id":@"b",@"name":@"DualSense"}];
    NSArray *choices=@[@{@"id":@"a",@"player":@1,@"style":@"pro"},@{@"id":@"b",@"player":@2,@"style":@"pro"}];
    __block int joined=1,waiting=1,a=0,b=0,answers=0,result=-1;
    __block NSView *card=nil;
    ui.snapshot=^{ return @{@"controllers":devices,@"choices":choices}; };
    ui.requirements=^{ return (dm_controller_requirements){0x1f,3,DM_FALLBACK_NONE}; };
    ui.requestLimits=^(int *lo,int *hi) { *lo=2; *hi=4; };
    ui.applyChoices=^NSString *(NSArray *next,int minimum,int maximum) {
        (void)next; assert(minimum==2 && maximum==4); joined=2; waiting=0; return nil;
    };
    ui.didOpen=^{ [join beginEditing]; }; ui.didClose=^{ [join endEditing]; };
    join.available=^BOOL { return YES; };
    join.sample=^(int *j,int *r,int *w,int *aa,int *bb) { *j=joined; *r=joined; *w=waiting; *aa=a; *bb=b; };
    join.show=^(const ctrl_prompt *s) {
        assert(!ui.panel && !parent.attachedSheet); // no overlapping popup and editor
        [card removeFromSuperview];
        card=controller_join_card(parent.contentView.bounds,s,nil,nil,ui,@selector(showForRequest:));
        [parent.contentView addSubview:card];
    };
    join.hide=^{ [card removeFromSuperview]; card=nil; };
    join.quiesce=^{}; join.resume=^{};
    [join openMinimum:2 maximum:4 completion:^(int r) { answers++; result=r; }];
    NSButton *configure=configureButton(card); assert(configure);
    [configure performClick:nil];
    assert(ui.panel && parent.attachedSheet==ui.panel.window && !card && join.editing);
    assert(ui.panel.minimum==2 && ui.panel.maximum==4 && ui.panel.recommend && !answers);
    joined=2; [join pollConfirm:YES cancel:YES]; [ui refresh];
    assert(ui.panel && !card && !answers); // hotplug/confirm cannot revive the card while editing
    joined=1;
    [ui.panel cancel:nil];
    settle(^BOOL { return ui.panel==nil; });
    assert(card && !join.editing && join->state.phase==CP_OPEN && !answers);
    [configureButton(card) performClick:nil];
    a=1; [ui.panel apply:nil];
    settle(^BOOL { return ui.panel==nil; });
    assert(!card && answers==1 && result==1 && [join blocksInput]);
    a=0; [join poll]; assert(![join blocksInput]);
    // Both manual-menu and popup access use the current pending limits.
    [ui showFromMenu:nil]; assert(ui.panel.minimum==2 && ui.panel.maximum==4);
    [join openMinimum:3 maximum:4 completion:^(int r) { answers++; result=r; }];
    assert(join.queue.count==1 && !card && ui.panel);
    [ui.panel cancel:nil]; settle(^BOOL { return ui.panel==nil; });
    assert(card && join.current && !ui.panel);
    [join pollConfirm:NO cancel:YES]; assert(!card && answers==2 && !result);
    [join cancelAll];
    __block int liveMinimum=0;
    ui.requestLimits=^(int *minimum,int *maximum) { *minimum=liveMinimum; *maximum=8; };
    [ui showFromMenu:nil];
    assert(ui.panel.minimum==0 && ui.panel.applyButton.enabled && !card);
    liveMinimum=3; [ui.panel refreshRequirements];
    assert(ui.panel.minimum==3 && !ui.panel.applyButton.enabled && !card);
    [ui.panel cancel:nil]; settle(^BOOL { return ui.panel==nil; });
    ui.requestLimits=nil;
    __block int evaluated=0;
    ui.evaluateChoices=^dm_controller_evaluation(NSArray *draft) {
        evaluated++;
        return controller_choices_evaluate((dm_controller_requirements){0x1f,3,DM_FALLBACK_NONE},draft);
    };
    [ui openMinimum:1 maximum:1 completion:nil];
    assert(evaluated && ui.panel.evaluateChoices && ui.panel.applyButton.enabled);
    // Two unchanged sources remain assigned while the UI accepts the SDK cap.
    assert(ui.panel.choices.count==2 && !card);
    [ui.panel cancel:nil]; settle(^BOOL { return ui.panel==nil; });
    [join cancelAll]; [parent close];
    puts("native joining card and controller editor OK");
} return 0; }
