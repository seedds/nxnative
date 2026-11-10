// Operations on a Switch 2 controller's memory and bond, one implementation for every client that talks to a controller (the host's CoreBluetooth session in
// pad_switch2_apple.m, and t_switch2_ops.m's fake flash): verified reads, the user stick calibration's backup -> write -> read back -> restore, and bonding. The client sends the commands
// (S2Commander); everything here is Foundation and switch2_proto.h only. Replies and completions come on the queue the commander answers on (the main queue in the host).
// Each operation keeps its commander until it completes: a commander whose link is gone must answer nil to everything, so that a save in progress ends (and says so).
#pragma once
#import <Foundation/Foundation.h>
#include "switch2_proto.h"

#define S2_CAL_TRIES 4   // each read of a verified read is tried this often (a read times out while the game's start keeps the main queue busy)

// A controller to talk to: one command in flight at a time, each answered once
@protocol S2Commander <NSObject>
// The command frame's reply (from its 8-byte header on); nil reply: no answer (timed out, or the link is gone)
- (void)send:(uint8_t)cmd sub:(uint8_t)sub payload:(const uint8_t *)pl len:(size_t)n done:(void (^)(NSData *reply))done;
- (NSUInteger)writeChunk;   // data bytes one memory write carries (16..64, a multiple of 16)
@end

typedef NS_ENUM(NSInteger, S2CalOutcome) {
    S2CalSaved,                   // written and read back as written: the controller uses it from now on
    S2CalUnchanged,               // the memory reads as before: nothing changed
    S2CalRestored,                // it did not read as written; the backup was put back and reads back
    S2CalRestoreFailed,           // it did not read as written and the backup could not be put back (the file is at the backup path); the sector reads blank, so the
                                  // controller uses its factory calibration
    S2CalNoBackup,                // the sector could not be read or the backup saved: nothing was written
    S2CalRestoreFailedNotBlank,   // as S2CalRestoreFailed, but the sector does not read blank (or could not be read): its user calibration may be neither the old nor the new one
};

NSURL *s2op_backup_dir(void);   // Application Support/<bundle id or "nxnative">/Switch 2 calibration
// n bytes from addr in reads of 0x40, each tried up to tries times (S2_CAL_TRIES for a save; 1 for the handshake's bounded wait) and taken only when the reply carries the read's length and address; nil when one never answers. log (may be nil) gets each refused reply and each read given up
void s2op_read(id<S2Commander> c, uint32_t addr, NSUInteger n, int tries, void (^log)(NSString *), void (^done)(NSData *));
// cal as the user calibration of the first stick slot (0x1FC040). The sector is backed up to a file in dir named after serial first; a blank block is written straight,
// otherwise the sector is erased and written back whole; it is then read back, and put back from the backup when it does not read as written. Nothing is written after an
// erase unless the sector then reads blank (an erase answered is not an erase done). log gets each step's line; done (once) the outcome and the backup's path (nil with
// S2CalNoBackup)
void s2op_save_user_cal(id<S2Commander> c, const char *serial, s2_cal cal, NSURL *dir, void (^log)(NSString *), void (^done)(S2CalOutcome, NSString *backupPath));
// Bonds the controller to host (its Bluetooth address, little endian) with fresh keys, as the console does (switch2_proto.h, Bonding); done(NO) when a step got no answer
void s2op_bond(id<S2Commander> c, const uint8_t host[6], void (^log)(NSString *), void (^done)(BOOL));
