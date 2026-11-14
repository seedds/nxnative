// The shared controller-memory operations (switch2_ops_apple.m) against a fake controller: the 4 KB user-calibration sector at 0x1FC000, which answers reads with the echoed
// request, writes as flash does (a write only clears bits) and erases to 0xFF, with knobs for the ways a real link fails. No Bluetooth; replies come on the main queue.
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <string.h>
#include "../switch2_ops_apple.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

#define BASE S2_ADDR_USER_CAL_SECTOR
@interface FakeFlash : NSObject <S2Commander>
@property BOOL ignoreWrites;          // writes and erases are answered but change nothing
@property BOOL ignoreErase;           // erases are answered but erase nothing (writes still clear bits)
@property int erasesBeforeIgnore;     // with ignoreErase: the first N erases are done all the same
@property int deafAfterWrite;         // the first N reads sent after the first write get no reply (the read back that follows a write times out)
@property int failReadsBeforeAnswer;  // the first N reads of each address get no reply
@property BOOL staleReplies;          // the first read of each address gets no reply; that read's reply is handed to the next read sent
@property BOOL staleChain;            // every read's reply comes one read late: each read is answered with the reply of the read before it
@property BOOL stopAfterErase;        // an erase is done and answered, then nothing is answered (or done) any more: the link dropped
@property int dropWrites;             // the first N writes are answered but not done
@property int pairAnswers;            // the pairing commands answered before the controller goes quiet
@property NSUInteger chunk;
@property int reads, writes, erases, bad, refusable;   // bad: a write or erase outside the sector or below S2_WRITE_FLOOR; refusable: replies handed to a read of another address
@property int writesAfterIgnoredErase;                 // writes sent after an erase that was answered but not done
@property NSMutableArray<NSData *> *cmds;              // every command: cmd, sub, payload
- (uint8_t *)mem;      // the sector's bytes
- (NSData *)sector;   // a copy of them
@end
@implementation FakeFlash {
    uint8_t _mem[S2_SECTOR_SIZE];
    NSCountedSet<NSNumber *> *_seen;
    NSData *_stale;
    BOOL _silent, _ignoredErase, _wrote;
    int _deaf;
}
- (instancetype)init {
    if ((self = [super init])) { memset(_mem, 0xFF, sizeof _mem); _seen = [NSCountedSet new]; _cmds = [NSMutableArray new]; _chunk = 64; _pairAnswers = 1000; }
    return self;
}
- (uint8_t *)mem { return _mem; }
- (NSData *)sector { return [NSData dataWithBytes:_mem length:sizeof _mem]; }
- (NSUInteger)writeChunk { return _chunk; }
- (void)send:(uint8_t)cmd sub:(uint8_t)sub payload:(const uint8_t *)pl len:(size_t)n done:(void (^)(NSData *))done {
    uint8_t head[2] = {cmd, sub}; NSMutableData *rec = [NSMutableData dataWithBytes:head length:2]; [rec appendBytes:pl length:n]; [_cmds addObject:rec];
    NSData *reply = [self answer:cmd sub:sub payload:pl len:n];
    dispatch_async(dispatch_get_main_queue(), ^{ done(reply); });
}
- (NSMutableData *)frame:(uint8_t)cmd sub:(uint8_t)sub { uint8_t h[8] = {cmd, 0x01, 0x01, sub, 0, 0, 0, 0}; return [NSMutableData dataWithBytes:h length:8]; }
- (BOOL)inSector:(uint32_t)addr len:(size_t)len { return addr >= S2_WRITE_FLOOR && addr >= BASE && addr + len <= BASE + S2_SECTOR_SIZE; }
- (NSData *)answer:(uint8_t)cmd sub:(uint8_t)sub payload:(const uint8_t *)pl len:(size_t)n {
    if (_silent) return nil;
    if (cmd == S2_CMD_PAIR) return _pairAnswers-- > 0 ? [self frame:cmd sub:sub] : nil;
    if (cmd != S2_CMD_MEMORY || n < 8) return nil;
    uint32_t addr = s2_u32(pl + 4); uint8_t len = pl[0];
    if (sub == S2_SUB_MEMORY_READ) {
        _reads++;
        if (_deaf > 0) { _deaf--; return nil; }
        NSMutableData *r = [self frame:cmd sub:sub]; [r appendBytes:pl length:8];
        for (uint8_t i = 0; i < len; i++) { uint32_t a = addr + i; uint8_t v = a >= BASE && a < BASE + S2_SECTOR_SIZE ? _mem[a - BASE] : 0; [r appendBytes:&v length:1]; }
        [_seen addObject:@(addr)]; NSUInteger seen = [_seen countForObject:@(addr)];
        if (_failReadsBeforeAnswer && seen <= (NSUInteger)_failReadsBeforeAnswer) return nil;
        if (_staleReplies) {
            if (_stale) { NSData *s = _stale; _stale = nil; [self note:s for:pl]; return s; }
            if (seen == 1) { _stale = r; return nil; }
            return r;
        }
        if (_staleChain) { NSData *s = _stale; _stale = r; if (s) [self note:s for:pl]; return s; }
        return r;
    }
    if (sub == S2_SUB_MEMORY_WRITE) {
        _writes++;
        if (n != 8u + len || ![self inSector:addr len:len]) { _bad++; return nil; }
        if (_ignoredErase) _writesAfterIgnoredErase++;
        if (!_wrote) { _wrote = YES; _deaf = _deafAfterWrite; }
        if (_dropWrites > 0) _dropWrites--;
        else if (!_ignoreWrites) for (uint8_t i = 0; i < len; i++) _mem[addr - BASE + i] &= pl[8 + i];
        return [self frame:cmd sub:sub];
    }
    if (sub == S2_SUB_MEMORY_ERASE) {
        _erases++;
        if (![self inSector:addr len:1]) { _bad++; return nil; }
        BOOL ignored = _ignoreWrites || (_ignoreErase && _erases > _erasesBeforeIgnore);
        if (!ignored) memset(_mem, 0xFF, sizeof _mem);
        _ignoredErase = _ignoredErase || ignored;
        if (_stopAfterErase) _silent = YES;
        return [self frame:cmd sub:sub];
    }
    return nil;
}
- (void)note:(NSData *)reply for:(const uint8_t *)pl { if (memcmp((const uint8_t *)reply.bytes + 8, pl, 8)) _refusable++; }
@end

// Runs the main run loop until *flag is set; a hang fails after 30 s instead of blocking
static BOOL run_until(const BOOL *flag) {
    NSDate *limit = [NSDate dateWithTimeIntervalSinceNow:30];
    while (!*flag && limit.timeIntervalSinceNow > 0) [NSRunLoop.mainRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    return *flag;
}

static s2_cal g_cal;   // the calibration saved: a recentred factory one
static void cal_block(const s2_cal *c, uint8_t out[S2_USER_CAL_LEN]) { s2_user_cal_block(c, out); }

static NSData *with_block(NSData *before);
typedef struct { BOOL finished; S2CalOutcome outcome; int calls; } save_result;
static int g_scenario;
#define CHECK_IN(name, c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s: %s\n", __FILE__, __LINE__, name, #c); } } while (0)
// One save into f (into dir, or a fresh backup folder when dir is nil); the outcome, the backup path and the log lines. Every save is held to the same rules: done comes
// exactly once, nothing is written or erased outside the sector, and the sector ends as it was, as written, or blank (the factory calibration): never anything else
static save_result save_in(FakeFlash *f, const char *name, NSURL *dir, NSString **path, NSArray<NSString *> **lines, NSURL **dirOut) {
    if (!dir) {
        dir = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"t_switch2_ops-%d-%d", getpid(), ++g_scenario]] isDirectory:YES];
        [NSFileManager.defaultManager removeItemAtURL:dir error:nil];
    }
    NSData *before = f.sector;
    NSMutableArray<NSString *> *log = [NSMutableArray new];
    __block save_result res = {NO, S2CalNoBackup, 0}; __block NSString *p = nil;
    s2op_save_user_cal(f, "HCW1234567890X", g_cal, dir, ^(NSString *m) { [log addObject:m]; },
                       ^(S2CalOutcome o, NSString *backup) { res.outcome = o; p = backup; res.finished = YES; res.calls++; });
    BOOL done = run_until(&res.finished);
    if (!done) printf("FAIL %s: the save never finished\n", name);
    [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.2]];   // a second done would have come by now
    if (getenv("T_VERBOSE")) for (NSString *m in log) printf("  %s: %s\n", name, m.UTF8String);
    NSData *now = f.sector;
    CHECK_IN(name, res.calls == 1);
    CHECK_IN(name, f.bad == 0);
    CHECK_IN(name, [now isEqualToData:before] || [now isEqualToData:with_block(before)] || s2_all_ff(now.bytes, now.length));
    *path = p; *lines = log; *dirOut = dir;
    return res;
}
static save_result save(FakeFlash *f, const char *name, NSString **path, NSArray<NSString *> **lines, NSURL **dirOut) { return save_in(f, name, nil, path, lines, dirOut); }
static BOOL logged(NSArray<NSString *> *lines, NSString *part) { for (NSString *m in lines) if ([m containsString:part]) return YES; return NO; }
static BOOL backup_is(NSString *path, NSURL *dir, NSData *want) {
    NSString *d = dir.path; if (!path || !d) return NO;
    NSData *got = [NSData dataWithContentsOfFile:path];
    return got && [path hasPrefix:d] && [got isEqualToData:want];
}
// A sector with motion calibration at 0x00 and an older stick block at 0x40
static void older(FakeFlash *f) {
    for (int i = 0; i < 0x20; i++) f.mem[i] = (uint8_t)(i * 7 + 3);
    s2_cal o = g_cal; o.cx -= 30; o.cy += 12; o.maxx += 30; o.minx -= 30; o.maxy -= 12; o.miny += 12;
    uint8_t b[S2_USER_CAL_LEN]; cal_block(&o, b); memcpy(f.mem + S2_USER_STICK1_OFF, b, sizeof b);
}
static NSData *with_block(NSData *before) {
    NSMutableData *m = [before mutableCopy]; uint8_t b[S2_USER_CAL_LEN]; cal_block(&g_cal, b);
    [m replaceBytesInRange:NSMakeRange(S2_USER_STICK1_OFF, S2_USER_CAL_LEN) withBytes:b];
    return m;
}

int main(void) {
    @autoreleasepool {
        s2_cal factory = {2095, 1999, 1175, 1225, 1181, 1129, 1};
        CHECK(s2_cal_recentre(&factory, 2110, 1990, &g_cal));
        NSString *path; NSArray<NSString *> *lines; NSURL *dir; save_result r;

        {   // A blank sector: the block is written straight, no erase
            FakeFlash *f = [FakeFlash new]; NSData *before = f.sector;
            r = save(f, "blank", &path, &lines, &dir);
            CHECK(r.finished && r.outcome == S2CalSaved);
            CHECK([f.sector isEqualToData:with_block(before)] && f.erases == 0 && f.writes == 1 && f.bad == 0);
            CHECK(backup_is(path, dir, before) && [path.lastPathComponent hasPrefix:@"HCW1234567890X-1FC000-"]);
            CHECK(logged(lines, @"user calibration backed up to") && logged(lines, @"written and read back"));
            [NSFileManager.defaultManager removeItemAtURL:dir error:nil];
        }
        {   // An older block and motion bytes: the sector is erased and written back whole, the motion bytes kept
            FakeFlash *f = [FakeFlash new]; f.chunk = 48; older(f); NSData *before = f.sector;
            r = save(f, "older", &path, &lines, &dir);
            CHECK(r.finished && r.outcome == S2CalSaved);
            CHECK([f.sector isEqualToData:with_block(before)] && !memcmp(f.mem, before.bytes, 0x20) && f.erases == 1 && f.bad == 0);
            CHECK(backup_is(path, dir, before));
            [NSFileManager.defaultManager removeItemAtURL:dir error:nil];
        }
        {   // Writes and erases answered but not done: the memory reads as before
            FakeFlash *f = [FakeFlash new]; older(f); f.ignoreWrites = YES; NSData *before = f.sector;
            r = save(f, "ignoreWrites", &path, &lines, &dir);
            CHECK(r.finished && r.outcome == S2CalUnchanged && [f.sector isEqualToData:before] && logged(lines, @"reads as before: nothing changed"));
            CHECK(f.erases == 1 && f.writes == 0);   // the erase did not take, so nothing was written over the sector
            [NSFileManager.defaultManager removeItemAtURL:dir error:nil];
        }
        {   // The link drops right after the erase: the sector is left blank (the factory calibration is used), the backup is on disk and the log names it
            FakeFlash *f = [FakeFlash new]; older(f); f.stopAfterErase = YES; NSData *before = f.sector;
            r = save(f, "stopAfterErase", &path, &lines, &dir);
            CHECK(r.finished && r.outcome == S2CalRestoreFailedNotBlank && f.erases == 1 && f.writes == 0);   // never read blank again: not known to be blank
            CHECK(backup_is(path, dir, before) && s2_all_ff(f.mem, S2_SECTOR_SIZE));
            BOOL named = NO; for (NSString *m in lines) if ([m hasPrefix:@"interrupted: "] && [m containsString:[NSString stringWithFormat:@"backup at %@", path]]) named = YES;
            CHECK(named);
            [NSFileManager.defaultManager removeItemAtURL:dir error:nil];
        }
        {   // Reads that time out twice before answering: retried
            FakeFlash *f = [FakeFlash new]; older(f); f.failReadsBeforeAnswer = 2; NSData *before = f.sector;
            r = save(f, "failReads2", &path, &lines, &dir);
            CHECK(r.finished && r.outcome == S2CalSaved && [f.sector isEqualToData:with_block(before)] && backup_is(path, dir, before));
            [NSFileManager.defaultManager removeItemAtURL:dir error:nil];
        }
        {   // A read that never answers within S2_CAL_TRIES: no backup, nothing written
            FakeFlash *f = [FakeFlash new]; older(f); f.failReadsBeforeAnswer = S2_CAL_TRIES; NSData *before = f.sector;
            r = save(f, "failReads4", &path, &lines, &dir);
            CHECK(S2_CAL_TRIES == 4 && r.finished && r.outcome == S2CalNoBackup && !path && f.writes == 0 && f.erases == 0 && [f.sector isEqualToData:before]);
            CHECK(logged(lines, @"got no answer 4 times") && logged(lines, @"could not be read for a backup: nothing written"));
            NSString *dp = dir.path;
            CHECK(dp && (![NSFileManager.defaultManager fileExistsAtPath:dp] || ![NSFileManager.defaultManager contentsOfDirectoryAtPath:dp error:nil].count));
            [NSFileManager.defaultManager removeItemAtURL:dir error:nil];
        }
        {   // The first read of each address times out and its reply comes with the next read
            FakeFlash *f = [FakeFlash new]; older(f); f.staleReplies = YES; NSData *before = f.sector;
            r = save(f, "staleReplies", &path, &lines, &dir);
            CHECK(r.finished && r.outcome == S2CalSaved && [f.sector isEqualToData:with_block(before)] && backup_is(path, dir, before));
            [NSFileManager.defaultManager removeItemAtURL:dir error:nil];
        }
        {   // Every reply one read late, so most reads first get the reply for another address: refused by the echo check, retried
            FakeFlash *f = [FakeFlash new]; older(f); f.staleChain = YES; NSData *before = f.sector;
            r = save(f, "staleChain", &path, &lines, &dir);
            CHECK(r.finished && r.outcome == S2CalSaved && [f.sector isEqualToData:with_block(before)] && backup_is(path, dir, before) && f.refusable > 64);
            [NSFileManager.defaultManager removeItemAtURL:dir error:nil];
        }
        {   // A write lost on the way: the sector does not read as written, the backup is put back
            FakeFlash *f = [FakeFlash new]; older(f); f.dropWrites = 1; NSData *before = f.sector;
            r = save(f, "dropWrites", &path, &lines, &dir);
            CHECK(r.finished && r.outcome == S2CalRestored && [f.sector isEqualToData:before] && f.erases == 2 && logged(lines, @"the backup is back"));
            [NSFileManager.defaultManager removeItemAtURL:dir error:nil];
        }
        {   // An erase answered but not done (writes still clear bits): nothing is written over the old block, which would leave old & new, a calibration that is neither
            FakeFlash *f = [FakeFlash new]; older(f); f.ignoreErase = YES; NSData *before = f.sector;
            r = save(f, "ignoreErase", &path, &lines, &dir);
            CHECK(r.finished && r.outcome == S2CalUnchanged && [f.sector isEqualToData:before] && f.erases == 1 && f.writes == 0 && f.writesAfterIgnoredErase == 0);
            CHECK(logged(lines, @"does not read blank after the erase: nothing written") && logged(lines, @"reads as before: nothing changed") && backup_is(path, dir, before));
            [NSFileManager.defaultManager removeItemAtURL:dir error:nil];
        }
        {   // The new block is written, its read back times out, and the restore's erase is answered but not done: the backup is not written over it, and the outcome says
            // the sector is not blank (here it holds the new block)
            FakeFlash *f = [FakeFlash new]; older(f); f.ignoreErase = YES; f.erasesBeforeIgnore = 1; f.deafAfterWrite = S2_CAL_TRIES; NSData *before = f.sector;
            r = save(f, "ignoreRestoreErase", &path, &lines, &dir);
            CHECK(r.finished && r.outcome == S2CalRestoreFailedNotBlank && f.erases == 2 && f.writesAfterIgnoredErase == 0 && [f.sector isEqualToData:with_block(before)]);
            CHECK(logged(lines, @"the backup is not written over it") && logged(lines, @"may be neither the old nor the new one") && backup_is(path, dir, before));
            CHECK(!logged(lines, @"factory"));   // nothing claims the factory calibration is in use
            [NSFileManager.defaultManager removeItemAtURL:dir error:nil];
        }
        {   // Every write lost, the restore's too: the sector is left blank, and the outcome says so (the factory calibration is used)
            FakeFlash *f = [FakeFlash new]; older(f); f.dropWrites = 1000; NSData *before = f.sector;
            r = save(f, "dropAllWrites", &path, &lines, &dir);
            CHECK(r.finished && r.outcome == S2CalRestoreFailed && s2_all_ff(f.mem, S2_SECTOR_SIZE) && f.erases == 2 && backup_is(path, dir, before));
            CHECK(logged(lines, @"blank, so the controller uses its factory calibration"));
            [NSFileManager.defaultManager removeItemAtURL:dir error:nil];
        }
        {   // A backup folder that cannot be made: nothing written
            FakeFlash *f = [FakeFlash new]; older(f); NSData *before = f.sector;
            NSString *file = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"t_switch2_ops-%d-file", getpid()]];
            [@"x" writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:nil];
            r = save_in(f, "noFolder", [NSURL fileURLWithPath:[file stringByAppendingPathComponent:@"sub"]], &path, &lines, &dir);
            CHECK(r.finished && r.outcome == S2CalNoBackup && !path && f.writes == 0 && f.erases == 0 && [f.sector isEqualToData:before] && logged(lines, @"the backup could not be saved"));
            [NSFileManager.defaultManager removeItemAtPath:file error:nil];
        }
        {   // s2op_read: the bytes asked for, in reads of 0x40; nil when a read never answers
            FakeFlash *f = [FakeFlash new]; older(f);
            __block BOOL got = NO; __block NSData *d = nil;
            s2op_read(f, BASE, 0x90, S2_CAL_TRIES, nil, ^(NSData *x) { d = x; got = YES; });
            CHECK(run_until(&got) && d.length == 0x90 && !memcmp(d.bytes, f.mem, 0x90) && f.reads == 3);
            FakeFlash *g = [FakeFlash new]; g.failReadsBeforeAnswer = S2_CAL_TRIES; got = NO; d = [NSData data];
            s2op_read(g, BASE, 0x40, S2_CAL_TRIES, nil, ^(NSData *x) { d = x; got = YES; });
            CHECK(run_until(&got) && !d && g.reads == S2_CAL_TRIES);
            FakeFlash *h = [FakeFlash new]; h.failReadsBeforeAnswer = 1; got = NO; d = [NSData data];   // tries 1 (the handshake): one read, no retry
            s2op_read(h, BASE, 0x0B, 1, nil, ^(NSData *x) { d = x; got = YES; });
            CHECK(run_until(&got) && !d && h.reads == 1);
        }
        {   // Bonding: the host address, two keys, done, in that order
            FakeFlash *f = [FakeFlash new]; const uint8_t host[6] = {1, 2, 3, 4, 5, 6};
            NSMutableArray<NSString *> *log = [NSMutableArray new]; __block BOOL got = NO, ok = NO;
            s2op_bond(f, host, ^(NSString *m) { [log addObject:m]; }, ^(BOOL b) { ok = b; got = YES; });
            CHECK(run_until(&got) && ok && f.cmds.count == 4 && logged(log, @"paired with this Mac"));
            uint8_t hp[S2_PAIR_HOST_LEN]; s2_pair_host_payload(hp, host);
            if (f.cmds.count == 4) {
                const uint8_t *c0 = f.cmds[0].bytes;
                CHECK(c0[0] == S2_CMD_PAIR && c0[1] == S2_SUB_PAIR_HOST && f.cmds[0].length == 2 + S2_PAIR_HOST_LEN && !memcmp(c0 + 2, hp, sizeof hp));
                CHECK(((const uint8_t *)f.cmds[1].bytes)[1] == S2_SUB_PAIR_KEY1 && f.cmds[1].length == 2 + S2_PAIR_KEY_LEN && ((const uint8_t *)f.cmds[2].bytes)[1] == S2_SUB_PAIR_KEY2);
                CHECK(((const uint8_t *)f.cmds[3].bytes)[1] == S2_SUB_PAIR_DONE && f.cmds[3].length == 3 && ((const uint8_t *)f.cmds[3].bytes)[2] == 0);
                CHECK(memcmp((const uint8_t *)f.cmds[1].bytes + 3, (const uint8_t *)f.cmds[2].bytes + 3, 16));   // two different keys
            }
            FakeFlash *g = [FakeFlash new]; g.pairAnswers = 1; [log removeAllObjects]; got = NO; ok = YES;
            s2op_bond(g, host, ^(NSString *m) { [log addObject:m]; }, ^(BOOL b) { ok = b; got = YES; });
            CHECK(run_until(&got) && !ok && g.cmds.count == 2 && logged(log, @"pairing stopped at the first key (no answer)"));
        }
        // Where the backups go: Application Support/<bundle id, or nxnative for a tool without one>/Switch 2 calibration
        NSURL *bd = s2op_backup_dir();
        CHECK([bd.lastPathComponent isEqualToString:@"Switch 2 calibration"] && [bd.path containsString:@"Application Support/"]);
        CHECK([bd.URLByDeletingLastPathComponent.lastPathComponent isEqualToString:NSBundle.mainBundle.bundleIdentifier ?: @"nxnative"]);
    }
    printf("t_switch2_ops: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
