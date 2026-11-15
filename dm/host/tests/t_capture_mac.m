#import <Foundation/Foundation.h>
#define DM_GAME_ID "test-title"
#include "../capture_mac.h"
#include <assert.h>
int main(void) { @autoreleasepool {
    NSString *root = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    assert([NSFileManager.defaultManager createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:nil]);
    NSString *path = [root stringByAppendingPathComponent:@"engine.log"];
    NSData *data = [@"0123456789" dataUsingEncoding:NSUTF8StringEncoding];
    assert([data writeToFile:path atomically:YES]);
    assert([capture_log_slice(path, NO, 4) isEqualToData:[@"0123" dataUsingEncoding:NSUTF8StringEncoding]]);
    assert([capture_log_slice(path, YES, 4) isEqualToData:[@"6789" dataUsingEncoding:NSUTF8StringEncoding]]);
    assert([capture_log_slice(path, YES, 100) isEqualToData:data]);
    assert(capture_log_slice(path, YES, 0).length == 0);
    assert(!capture_log_slice(root, YES, 8));
    assert(!capture_log_slice(@"/dev/null", YES, 8));
    assert(!capture_log_slice([root stringByAppendingPathComponent:@"missing"], YES, 8));
    NSMutableData *large = [NSMutableData dataWithLength:9 * 1024 * 1024];
    memset(large.mutableBytes, 'a', large.length); ((char *)large.mutableBytes)[large.length - 1] = 'z';
    assert([large writeToFile:path atomically:YES]);
    NSData *tail = capture_log_slice(path, YES, 8 * 1024 * 1024);
    assert(tail.length == 8 * 1024 * 1024 && ((const char *)tail.bytes)[tail.length - 1] == 'z');
    int saved = dup(STDERR_FILENO), output = open(path.fileSystemRepresentation, O_WRONLY);
    assert(saved >= 0 && output >= 0 && dup2(output, STDERR_FILENO) >= 0); close(output);
    NSString *captured = capture_stderr_path();
    assert(dup2(saved, STDERR_FILENO) >= 0); close(saved);
    struct stat a, b; assert(captured && !stat(captured.fileSystemRepresentation, &a) && !stat(path.fileSystemRepresentation, &b));
    assert(a.st_dev == b.st_dev && a.st_ino == b.st_ino);
    setenv("DM_FPS_TARGET", "60", 1); setenv("DM_SAVE", "/private/save", 1);
    NSDictionary *meta = capture_metadata();
    assert([meta[@"settings"][@"DM_FPS_TARGET"] isEqualToString:@"60"]);
    assert(!meta[@"settings"][@"DM_SAVE"]);
    assert([meta[@"requested_pass_frames"] intValue] == 2);
    assert([NSJSONSerialization dataWithJSONObject:meta options:0 error:nil]);
    assert([NSFileManager.defaultManager removeItemAtPath:root error:nil]);
    puts("PASS: bounded log slices, invalid sources, large tail and settings whitelist");
} return 0; }
