// The host owns device identities before and after the launch window. The wrapper
// receives session UUIDs over an inherited socket, never names or enumeration indices.
// Only setup messages cross the socket. Input, motion and haptics stay in this process.
#pragma once
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#include <sys/socket.h>
#include <errno.h>
#include <unistd.h>
#include "controller_setup_apple.h"

static NSString *cs_accept(NSDictionary *request) {
    NSDictionary *environment = request[@"environment"];
    if (![environment isKindOfClass:NSDictionary.class] || ![request[@"explicit"] isKindOfClass:NSNumber.class]) return @"Invalid controller setup.";
    NSDictionary *next = nil;
    NSString *error = cs_validate_choices(request[@"choices"], &next);
    if (error) return error;
    for (id key in environment) {
        id value = environment[key];
        if (![key isKindOfClass:NSString.class] || ![value isKindOfClass:NSString.class] || ![key length] || [key containsString:@"="] ||
            strlen([key UTF8String]) != [key lengthOfBytesUsingEncoding:NSUTF8StringEncoding] ||
            strlen([value UTF8String]) != [value lengthOfBytesUsingEncoding:NSUTF8StringEncoding]) return @"Invalid launch setting.";
    }
    for (NSString *key in environment) if (setenv(key.UTF8String, [environment[key] UTF8String], 1)) return @"A launch setting could not be applied.";
    cs_choices = next;
    cs_explicit = 0;
    for (NSDictionary *choice in next.allValues)
        if ([choice[@"player"] intValue] != -1 || ![choice[@"style"] isEqual:@"auto"]) cs_explicit = 1;
    return nil;
}

static int cs_send(int fd, NSDictionary *message) {
    NSData *json = [NSJSONSerialization dataWithJSONObject:message options:0 error:NULL];
    NSMutableData *data = [json mutableCopy]; [data appendBytes:"\n" length:1];
    const char *bytes = data.bytes; size_t left = data.length;
    while (left) {
        ssize_t n = send(fd, bytes, left, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        bytes += n; left -= (size_t)n;
    }
    return 1;
}

static int cs_run(int fd, cs_identify_fn signal) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
        [NSApp finishLaunching];   // initialize the Mac application services used by GameController, without a game window
        int no_sigpipe = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof no_sigpipe);
        __block int done = 0, start = 0;
        void (^snapshot)(void) = ^{
            if (!cs_send(fd, @{@"controllers": cs_update(GCController.controllers)})) done = 1;
        };
        NSMutableArray *observers = [NSMutableArray new];
        for (NSString *name in @[GCControllerDidConnectNotification, GCControllerDidDisconnectNotification])
            [observers addObject:[NSNotificationCenter.defaultCenter addObserverForName:name object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { (void)n; snapshot(); }]];
        NSMutableData *buffer = [NSMutableData new];
        dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(source, ^{
            char bytes[4096]; ssize_t count = recv(fd, bytes, sizeof bytes, MSG_DONTWAIT);
            if (count < 0 && (errno == EAGAIN || errno == EINTR)) return;
            if (count <= 0 || buffer.length + (NSUInteger)count > 262144) { done = 1; return; }
            [buffer appendBytes:bytes length:(NSUInteger)count];
            NSData *newline = [NSData dataWithBytes:"\n" length:1];
            while (!done) {
                NSRange line = [buffer rangeOfData:newline options:0 range:NSMakeRange(0, buffer.length)];
                if (line.location == NSNotFound) return;
                id message = [NSJSONSerialization JSONObjectWithData:[buffer subdataWithRange:NSMakeRange(0, line.location)] options:0 error:NULL];
                [buffer replaceBytesInRange:NSMakeRange(0, NSMaxRange(line)) withBytes:NULL length:0];
                cs_update(GCController.controllers);
                if ([message isKindOfClass:NSDictionary.class] && [message[@"identify"] isKindOfClass:NSString.class]) {
                    NSString *key = message[@"identify"];
                    BOOL ok = cs_identify(key, signal);
                    if (!cs_send(fd, @{@"identified": key, @"ok": @(ok)})) done = 1;
                    continue;
                }
                NSString *error = [message isKindOfClass:NSDictionary.class] ? cs_accept(message) : @"Invalid launch request.";
                start = error == nil;
                cs_send(fd, error ? @{@"error": error} : @{@"started": @YES});
                done = 1;
            }
        });
        dispatch_resume(source);
        snapshot();
        // Keep GameController discovery and the main dispatch queue running without loading
        // a game or creating its window until Play arrives from the wrapper's launch UI.
        while (!done) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
        for (id observer in observers) [NSNotificationCenter.defaultCenter removeObserver:observer];
        dispatch_source_cancel(source);
        close(fd);
        return start;
    }
}
