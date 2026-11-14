// Operations on a Switch 2 controller's memory and bond (switch2_ops_apple.h). Every block holds its commander until the operation completes; a commander answers each
// command once, nil when there is no answer.
#import "switch2_ops_apple.h"

static void s2op_log(void (^log)(NSString *), NSString *fmt, ...) NS_FORMAT_FUNCTION(2, 3);
static void s2op_log(void (^log)(NSString *), NSString *fmt, ...) {
    if (!log) return;
    va_list ap; va_start(ap, fmt); NSString *m = [[NSString alloc] initWithFormat:fmt arguments:ap]; va_end(ap);
    log(m);
}

// ---- reads ----
// len bytes at addr, taken only when the reply carries this read's length and address (a late reply to a read that timed out is not another read's data). A reply refused
// logs its bytes 8-15, so a controller that lays them out otherwise shows in the log rather than as a calibration that "does not read"
static void s2op_read_exact(id<S2Commander> c, uint8_t len, uint32_t addr, void (^log)(NSString *), void (^done)(NSData *)) {
    uint8_t p[8]; s2_memory_payload(p, len, addr);
    [c send:S2_CMD_MEMORY sub:S2_SUB_MEMORY_READ payload:p len:8 done:^(NSData *r) {
        if (s2_memory_reply_for(r.bytes, r.length, len, addr)) { done([r subdataWithRange:NSMakeRange(S2_REPLY_DATA, len)]); return; }
        if (r.length >= 16) { const uint8_t *b = r.bytes;
            s2op_log(log, @"the read of 0x%x bytes at 0x%06x got a reply for another (%zu bytes, 8-15: %02x %02x %02x %02x %02x %02x %02x %02x)", len, addr, (size_t)r.length, b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]); }
        done(nil);
    }];
}
static void s2op_read_from(id<S2Commander> c, uint32_t addr, NSUInteger n, NSMutableData *out, int attempt, int tries, void (^log)(NSString *), void (^done)(NSData *)) {
    if (out.length >= n) { done([out copy]); return; }
    uint8_t len = (uint8_t)MIN((NSUInteger)0x40, n - out.length); uint32_t at = addr + (uint32_t)out.length;
    s2op_read_exact(c, len, at, log, ^(NSData *d) {
        if (d) { [out appendData:d]; s2op_read_from(c, addr, n, out, 1, tries, log, done); return; }
        if (attempt >= tries) { s2op_log(log, @"the read of 0x%x bytes at 0x%06x got no answer %d times", len, at, attempt); done(nil); return; }
        s2op_read_from(c, addr, n, out, attempt + 1, tries, log, done);
    });
}
void s2op_read(id<S2Commander> c, uint32_t addr, NSUInteger n, int tries, void (^log)(NSString *), void (^done)(NSData *)) { s2op_read_from(c, addr, n, [NSMutableData data], 1, tries, log, done); }

// ---- write and erase ----
// data written at addr from off; skip (after an erase): chunks that are all 0xFF are already so
static void s2op_write(id<S2Commander> c, NSData *data, uint32_t addr, NSUInteger off, BOOL skip, void (^done)(BOOL)) {
    NSUInteger chunk = [c writeChunk];
    if (!chunk) { done(NO); return; }
    while (skip && off < data.length && s2_all_ff((const uint8_t *)data.bytes + off, MIN(chunk, data.length - off))) off += chunk;
    if (off >= data.length) { done(YES); return; }
    NSUInteger n = MIN(chunk, data.length - off);
    uint8_t pl[8 + 0x80]; size_t pn = s2_memory_write_payload(pl, sizeof pl, addr + (uint32_t)off, (const uint8_t *)data.bytes + off, n);
    if (!pn) { done(NO); return; }
    [c send:S2_CMD_MEMORY sub:S2_SUB_MEMORY_WRITE payload:pl len:pn done:^(NSData *r) {
        if (!r) { done(NO); return; }
        s2op_write(c, data, addr, off + n, skip, done);
    }];
}
// The user calibration sector erased, then read back: done gets what it reads now (nil when it could not be read). An answer is no proof of the erase, and a write over
// bytes that were not erased leaves old & new (flash clears bits), a calibration that is neither: the caller writes only into a sector that reads blank
static void s2op_erase_sector(id<S2Commander> c, void (^log)(NSString *), void (^done)(NSData *)) {
    uint8_t pl[8]; s2_memory_erase_payload(pl, S2_ADDR_USER_CAL_SECTOR);
    [c send:S2_CMD_MEMORY sub:S2_SUB_MEMORY_ERASE payload:pl len:8 done:^(NSData *r) {
        if (!r) s2op_log(log, @"the erase got no answer (it may have happened all the same)");
        s2op_read_from(c, S2_ADDR_USER_CAL_SECTOR, S2_SECTOR_SIZE, [NSMutableData data], 1, S2_CAL_TRIES, log, done);
    }];
}
static BOOL s2op_blank(NSData *d) { return d && s2_all_ff(d.bytes, d.length); }

// ---- the user stick calibration (switch2_proto.h, Writing a user stick calibration) ----
NSURL *s2op_backup_dir(void) {
    NSURL *base = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
    return [base URLByAppendingPathComponent:[NSString stringWithFormat:@"%@/Switch 2 calibration", NSBundle.mainBundle.bundleIdentifier ?: @"nxnative"] isDirectory:YES];
}
// A file per save: the oldest is the controller as it came
static NSString *s2op_back_up(NSData *sector, NSURL *dir, NSString *serial) {
    NSFileManager *fm = NSFileManager.defaultManager;
    if (!dir || ![fm createDirectoryAtURL:dir withIntermediateDirectories:YES attributes:nil error:nil]) return nil;
    NSURL *f = [dir URLByAppendingPathComponent:[NSString stringWithFormat:@"%@-1FC000-%.0f.bin", serial, NSDate.date.timeIntervalSince1970]];
    return f && [sector writeToURL:f atomically:YES] ? f.path : nil;
}
// How a restore that did not bring the backup back left the sector: blank (the factory calibration is used: S2CalRestoreFailed), or anything else, including a sector that
// could not be read (S2CalRestoreFailedNotBlank)
static void s2op_restore_failed(NSData *now, NSString *path, void (^log)(NSString *), void (^done)(S2CalOutcome, NSString *)) {
    if (s2op_blank(now)) {
        s2op_log(log, @"the backup could not be put back; it is at %@. The user calibration is blank, so the controller uses its factory calibration", path);
        done(S2CalRestoreFailed, path); return;
    }
    s2op_log(log, @"the backup could not be put back (%@): the controller's user calibration may be neither the old nor the new one; the backup is at %@",
             now ? @"the sector reads otherwise" : @"the sector could not be read", path);
    // No answer to the last read: the link went (or the controller stopped answering) after the memory may have been erased or written
    if (!now) s2op_log(log, @"interrupted: the controller stopped answering during the save, its user calibration may be blank or half written, backup at %@", path);
    done(S2CalRestoreFailedNotBlank, path);
}
// The sector read back: as written, the calibration is used from now on; as before, nothing changed; anything else, the backup is put back (erase, read blank, write, read
// again). A sector that does not read blank after the erase is not written over
static void s2op_settle(id<S2Commander> c, NSData *want, NSData *old, NSString *path, s2_cal cal, void (^log)(NSString *), void (^done)(S2CalOutcome, NSString *)) {
    s2op_read_from(c, S2_ADDR_USER_CAL_SECTOR, S2_SECTOR_SIZE, [NSMutableData data], 1, S2_CAL_TRIES, log, ^(NSData *now) {
        if (now && [now isEqualToData:want]) {
            s2op_log(log, @"user calibration written and read back: centre %.0f, %.0f, travel %.0f, %.0f up and %.0f, %.0f down. It stays in the controller until it is calibrated again",
                     cal.cx, cal.cy, cal.maxx, cal.maxy, cal.minx, cal.miny);
            done(S2CalSaved, path); return;
        }
        if (now && [now isEqualToData:old]) { s2op_log(log, @"the controller's memory reads as before: nothing changed"); done(S2CalUnchanged, path); return; }
        s2op_log(log, @"the user calibration does not read as written%@: putting the backup back", now ? @"" : @" (it could not be read)");
        s2op_erase_sector(c, log, ^(NSData *erased) {
            if (!s2op_blank(erased)) {
                s2op_log(log, @"the sector %@ after the erase: the backup is not written over it", erased ? @"does not read blank" : @"could not be read");
                s2op_restore_failed(erased, path, log, done); return;
            }
            s2op_write(c, old, S2_ADDR_USER_CAL_SECTOR, 0, YES, ^(BOOL ok) {
                if (!ok) s2op_log(log, @"a write of the backup got no answer");
                s2op_read_from(c, S2_ADDR_USER_CAL_SECTOR, S2_SECTOR_SIZE, [NSMutableData data], 1, S2_CAL_TRIES, log, ^(NSData *again) {
                    if (again && [again isEqualToData:old]) { s2op_log(log, @"the backup is back: the controller is as it was"); done(S2CalRestored, path); return; }
                    s2op_restore_failed(again, path, log, done);
                });
            });
        });
    });
}
void s2op_save_user_cal(id<S2Commander> c, const char *serial, s2_cal cal, NSURL *dir, void (^log)(NSString *), void (^done)(S2CalOutcome, NSString *)) {
    void (^finish)(S2CalOutcome, NSString *) = ^(S2CalOutcome o, NSString *p) { if (done) done(o, p); };
    NSString *ser = serial && *serial ? @(serial) : @"unknown";   // copied: the caller's buffer need not outlive the call
    uint8_t b[S2_USER_CAL_LEN]; s2_user_cal_block(&cal, b);
    NSData *blk = [NSData dataWithBytes:b length:sizeof b];
    s2op_read_from(c, S2_ADDR_USER_CAL_SECTOR, S2_SECTOR_SIZE, [NSMutableData data], 1, S2_CAL_TRIES, log, ^(NSData *old) {
        if (!old) { s2op_log(log, @"the user calibration could not be read for a backup: nothing written"); finish(S2CalNoBackup, nil); return; }
        NSString *path = s2op_back_up(old, dir, ser);
        if (!path) { s2op_log(log, @"the backup could not be saved: nothing written"); finish(S2CalNoBackup, nil); return; }
        const uint8_t *o = old.bytes;
        s2op_log(log, @"user calibration backed up to %@; marker B2 A1 at 0x1FC040 %s, 0x1FC060 %s, 0x1FC080 %s", path,
                 s2_user_cal_marked(o + 0x40) ? "yes" : "no", s2_user_cal_marked(o + 0x60) ? "yes" : "no", s2_user_cal_marked(o + 0x80) ? "yes" : "no");
        NSMutableData *want = [old mutableCopy]; [want replaceBytesInRange:NSMakeRange(S2_USER_STICK1_OFF, S2_USER_CAL_LEN) withBytes:blk.bytes];
        void (^check)(BOOL) = ^(BOOL ok) { if (!ok) s2op_log(log, @"a write got no answer"); s2op_settle(c, want, old, path, cal, log, finish); };
        if (s2_all_ff(o + S2_USER_STICK1_OFF, S2_USER_CAL_LEN)) s2op_write(c, blk, S2_ADDR_USER_CAL_SECTOR + S2_USER_STICK1_OFF, 0, NO, check);
        else s2op_erase_sector(c, log, ^(NSData *erased) {
            if (!s2op_blank(erased)) {   // the final check says whether anything changed: nothing, when the sector still reads as the backup
                s2op_log(log, @"the sector %@ after the erase: nothing written", erased ? @"does not read blank" : @"could not be read");
                s2op_settle(c, want, old, path, cal, log, finish); return;
            }
            s2op_write(c, want, S2_ADDR_USER_CAL_SECTOR, 0, YES, check);
        });
    });
}

// ---- bonding: the host's address, two fresh keys, done ----
void s2op_bond(id<S2Commander> c, const uint8_t host[6], void (^log)(NSString *), void (^done)(BOOL)) {
    uint8_t hp[S2_PAIR_HOST_LEN], k1[16], k2[16], key1[S2_PAIR_KEY_LEN], key2[S2_PAIR_KEY_LEN], last = 0;
    arc4random_buf(k1, sizeof k1); arc4random_buf(k2, sizeof k2);
    s2_pair_host_payload(hp, host); s2_pair_key_payload(key1, k1); s2_pair_key_payload(key2, k2);
    NSData *z = [NSData dataWithBytes:&last length:1], *h = [NSData dataWithBytes:hp length:sizeof hp], *a = [NSData dataWithBytes:key1 length:sizeof key1], *b = [NSData dataWithBytes:key2 length:sizeof key2];
    void (^finish)(BOOL) = ^(BOOL ok) { if (done) done(ok); };
    void (^step)(NSData *, NSString *, void (^)(void)) = ^(NSData *r, NSString *what, void (^next)(void)) {
        if (!r) { s2op_log(log, @"pairing stopped at %@ (no answer): it plays now, but needs Sync again next time", what); finish(NO); return; }
        next();
    };
    [c send:S2_CMD_PAIR sub:S2_SUB_PAIR_HOST payload:h.bytes len:h.length done:^(NSData *r1) { step(r1, @"the host address", ^{
        [c send:S2_CMD_PAIR sub:S2_SUB_PAIR_KEY1 payload:a.bytes len:a.length done:^(NSData *r2) { step(r2, @"the first key", ^{
            [c send:S2_CMD_PAIR sub:S2_SUB_PAIR_KEY2 payload:b.bytes len:b.length done:^(NSData *r3) { step(r3, @"the second key", ^{
                [c send:S2_CMD_PAIR sub:S2_SUB_PAIR_DONE payload:z.bytes len:z.length done:^(NSData *r4) { step(r4, @"the last step", ^{
                    s2op_log(log, @"paired with this Mac: from now on a press of any of its buttons connects it, no Sync needed");
                    finish(YES);
                }); }];
            }); }];
        }); }];
    }); }];
}
