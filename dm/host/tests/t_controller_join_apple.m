#import "../controller_join_apple.h"
#include <assert.h>

int main(void) { @autoreleasepool {
    __block int joined=1,waiting=1,a=0,b=0,shown=0,hidden=0,resumed=0,cleared=0;
    __block BOOL available=YES;
    NSMutableArray *answers=[NSMutableArray new];
    DmControllerJoin *join=[DmControllerJoin new];
    join.available=^BOOL { return available; };
    join.sample=^(int *j,int *r,int *w,int *aa,int *bb) { *j=joined; *r=joined; *w=waiting; *aa=a; *bb=b; };
    join.show=^(const ctrl_prompt *s) { assert(s->phase==CP_OPEN); shown++; };
    join.hide=^{ hidden++; }; join.quiesce=^{ cleared++; }; join.resume=^{ resumed++; };
    void (^answer)(int)=^(int r) { [answers addObject:@(r)]; };

    [join openMinimum:2 maximum:4 completion:answer];
    assert(join->state.phase==CP_OPEN && [join blocksInput] && shown && cleared==1);
    [join beginEditing];
    joined=2; waiting=0; a=1;
    assert([join pollConfirm:YES cancel:YES] && !answers.count && hidden==1);
    // Apply returns to the original card. Its no-waiting rule may finish, but
    // the button held in the editor still stays out of the game until release.
    [join endEditing];
    assert(answers.count==1 && [answers[0] intValue]==1 && join->state.phase==CP_SETTLE && !resumed);
    [join openMinimum:3 maximum:4 completion:answer];
    assert(join.queue.count==1 && join->state.phase==CP_SETTLE);
    a=0; [join poll];
    assert(join.current && join.queue.count==0 && join->state.phase==CP_OPEN && resumed==1);
    [join beginEditing];
    joined=1; waiting=1; b=1;
    [join endEditing]; // editor Cancel must not cancel the SDK request
    assert(join->state.phase==CP_OPEN && answers.count==1);
    b=0; [join poll]; b=1; [join poll];
    assert(join->state.phase==CP_SETTLE && answers.count==2 && ![answers[1] intValue]);
    b=0; [join poll]; assert(![join blocksInput]);

    // Manual editor first: queued joining starts only once that editor closes.
    [join beginEditing];
    [join openMinimum:2 maximum:2 completion:answer];
    assert(!join.current && join.queue.count==1);
    [join endEditing]; assert(join.current && join->state.phase==CP_OPEN);
    [join openMinimum:3 maximum:4 completion:answer];
    [join cancelAll];
    assert(answers.count==4 && ![answers[2] intValue] && ![answers[3] intValue] && !join.current && !join.queue.count);

    available=NO; [join openMinimum:2 maximum:4 completion:answer];
    assert([answers.lastObject intValue]==-1 && !join.current && ![join blocksInput]);
    available=YES; joined=2; waiting=1;
    [join openMinimum:2 maximum:4 completion:answer];
    [join pollConfirm:YES cancel:YES];
    assert(![answers.lastObject intValue]); // cancel still wins
    assert(![join blocksInput]);

    // Apply can detach the controller's gameplay handler. Polling only while
    // the UI owns input must still notice its eventual release.
    [join beginEditing]; a=1; [join endEditing];
    assert([join blocksInput] && join.releaseTimer);
    a=0;
    NSDate *releaseDeadline=[NSDate dateWithTimeIntervalSinceNow:2];
    while ([join blocksInput] && releaseDeadline.timeIntervalSinceNow>0)
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
    assert(![join blocksInput] && !join.releaseTimer);

    // Exercise the real guest-to-main wait bridge, not just its UI half.
    joined=2; waiting=0; __block BOOL finished=NO; __block int result=-2;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
        int value=[join requestMinimum:2 maximum:2];
        dispatch_async(dispatch_get_main_queue(), ^{ result=value; finished=YES; });
    });
    NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:5];
    while (!finished && deadline.timeIntervalSinceNow>0) [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
    assert(finished && result==1 && ![join blocksInput]);
    __block BOOL active=YES;
    join.inputActive=^BOOL { return active; };
    joined=1; waiting=1; a=b=0;
    NSUInteger previous=answers.count;
    [join openMinimum:2 maximum:4 completion:answer];
    active=NO; b=1; [join pollConfirm:YES cancel:YES];
    assert(join->state.phase==CP_OPEN && answers.count==previous);
    b=0; [join poll]; joined=2; a=1; [join poll];
    assert(join->state.phase==CP_OPEN && join->state.ready==2);
    active=YES; [join poll];
    assert(join->state.phase==CP_OPEN); // the held joining A is still not a new press
    a=0; [join poll]; b=1; [join poll];
    assert(join->state.phase==CP_SETTLE && answers.count==previous+1 && ![answers.lastObject intValue]);
    active=NO; [join poll]; assert([join blocksInput]);
    b=0; [join poll]; assert(![join blocksInput]);

    joined=1; waiting=1;
    [join openMinimum:2 maximum:2 completion:answer];
    joined=2; waiting=0; a=1; [join poll];
    assert(join->state.phase==CP_SETTLE && [answers.lastObject intValue]);
    [join poll]; assert([join blocksInput]);
    a=0; [join poll]; assert(![join blocksInput]);
    puts("joining focus: background actions ignored, counts live and physical releases preserved OK");
    puts("controller joining coordinator OK");
} return 0; }
