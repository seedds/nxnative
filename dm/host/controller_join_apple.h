// The existing joining prompt owns the applet request. Its editor is a child,
// never a second applet answer. Shared by AppKit/UIKit; all state is main-thread.
#pragma once
#import <Foundation/Foundation.h>
#include "ctrl_prompt.h"

@interface DmJoinRequest : NSObject
@property int minimum, maximum;
@property (copy) void (^completion)(int);
@end
@implementation DmJoinRequest @end

@interface DmControllerJoin : NSObject {
@public ctrl_prompt state;
}
@property (copy) BOOL (^available)(void);
@property (copy) BOOL (^inputActive)(void);
@property (copy) void (^sample)(int *joined, int *ready, int *waiting, int *a, int *b);
@property (copy) void (^show)(const ctrl_prompt *);
@property (copy) void (^hide)(void);
@property (copy) void (^quiesce)(void);
@property (copy) void (^resume)(void);
@property (strong) NSMutableArray<DmJoinRequest *> *queue;
@property (strong) DmJoinRequest *current;
@property BOOL editing;
@property (strong) NSTimer *releaseTimer;
- (int)requestMinimum:(int)minimum maximum:(int)maximum;
- (void)openMinimum:(int)minimum maximum:(int)maximum completion:(void (^)(int))completion;
- (BOOL)blocksInput;
- (BOOL)pollConfirm:(BOOL)confirm cancel:(BOOL)cancel;
- (BOOL)poll;
- (void)beginEditing;
- (void)endEditing;
- (void)cancelAll;
@end

@implementation DmControllerJoin
- (void)dealloc { [_releaseTimer invalidate]; }
- (BOOL)blocksInput { return self.editing || cp_blocks_input(state.phase); }
- (void)watchInput {
    if (self.releaseTimer) return;
    // An explicitly Unassigned source, or a device detached by Apply, may have
    // no gameplay callback left. It must still be able to release the closing
    // A/B. Observe only while UI owns input, without replacing backend handlers.
    __weak DmControllerJoin *weak=self;
    self.releaseTimer=[NSTimer timerWithTimeInterval:.05 repeats:YES block:^(NSTimer *timer) {
        (void)timer; [weak poll];
    }];
    [NSRunLoop.mainRunLoop addTimer:self.releaseTimer forMode:NSRunLoopCommonModes];
}
- (void)pump {
    if (self.editing || state.phase != CP_IDLE || !self.queue.count) return;
    DmJoinRequest *request = self.queue.firstObject;
    [self.queue removeObjectAtIndex:0];
    if (!self.available()) { request.completion(-1); [self pump]; return; }
    self.current = request;
    [self watchInput];
    self.quiesce();
    int joined, ready, waiting, a, b; self.sample(&joined, &ready, &waiting, &a, &b);
    cp_open_ready(&state, request.minimum, request.maximum, joined, ready, a, b);
    self.show(&state);
    [self poll];
}
- (void)openMinimum:(int)minimum maximum:(int)maximum completion:(void (^)(int))completion {
    NSAssert(NSThread.isMainThread, @"Join UI belongs to the main thread");
    DmJoinRequest *request = [DmJoinRequest new];
    request.minimum = minimum; request.maximum = maximum; request.completion = completion;
    if (!self.queue) self.queue = [NSMutableArray new];
    [self.queue addObject:request];
    [self pump];
}
- (int)requestMinimum:(int)minimum maximum:(int)maximum {
    if (NSThread.isMainThread) return -1;
    dispatch_semaphore_t done = dispatch_semaphore_create(0); __block int answer = -1;
    dispatch_async(dispatch_get_main_queue(), ^{
        [self openMinimum:minimum maximum:maximum completion:^(int result) {
            answer = result; dispatch_semaphore_signal(done);
        }];
    });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    return answer;
}
- (BOOL)poll { return [self pollConfirm:NO cancel:NO]; }
- (BOOL)pollConfirm:(BOOL)confirm cancel:(BOOL)cancel {
    if (self.editing) return YES;
    if (state.phase == CP_IDLE) return NO;
    int joined, ready, waiting, a, b; self.sample(&joined, &ready, &waiting, &a, &b);
    cp_phase was = state.phase;
    cp_step_ready(&state, joined, ready, waiting, a, b, confirm, cancel, !self.inputActive || self.inputActive());
    if (was == CP_OPEN && state.phase == CP_OPEN) self.show(&state);
    if (was == CP_OPEN && state.phase == CP_SETTLE) {
        self.hide();
        DmJoinRequest *request = self.current; self.current = nil;
        request.completion(state.ok);
        cp_step_ready(&state, joined, ready, 0, a, b, 0, 0, 1);
    }
    if (state.phase == CP_IDLE) {
        [self.releaseTimer invalidate]; self.releaseTimer=nil;
        self.resume(); [self pump];
    }
    return [self blocksInput];
}
- (void)beginEditing {
    self.editing = YES;
    [self watchInput];
    if (state.phase == CP_OPEN) self.hide();
}
- (void)endEditing {
    if (!self.editing) return;
    self.editing = NO;
    if (state.phase == CP_OPEN) {
        // Re-arm from the controls as they are after Apply/Cancel. A press made
        // in the editor must not become a fresh confirm/cancel on the card.
        int joined, ready, waiting, a, b; self.sample(&joined, &ready, &waiting, &a, &b);
        cp_open_ready(&state, state.min, state.max, joined, ready, a, b);
        self.show(&state);
    } else if (state.phase == CP_IDLE) {
        // Direct editor requests need the same release protection as Done on
        // the joining card. There is no second guest completion to deliver.
        state.phase=CP_SETTLE;
        self.quiesce();
    }
    if (state.phase != CP_IDLE) [self poll];
    else [self pump];
}
- (void)cancelAll {
    self.hide();
    DmJoinRequest *current = self.current; self.current = nil;
    NSArray *queued = [self.queue copy]; [self.queue removeAllObjects];
    state = (ctrl_prompt){0};
    self.editing=NO;
    [self.releaseTimer invalidate]; self.releaseTimer=nil;
    if (current) current.completion(0);
    for (DmJoinRequest *request in queued) request.completion(0);
}
@end
