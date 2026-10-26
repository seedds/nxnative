#import <Foundation/Foundation.h>

// Materialize a schema-4 virtual-layout RomFS patch as an APFS clone overlay:
// source prefix + inserted replacement packs + shifted physical root tail,
// with a whole-file virtual-to-physical read map. Legacy layouts fail closed.
// The source RomFS is opened
// read-only and is never modified.  The returned path is suitable for
// DM_ROMFS_OVERLAY.  Callers opt in by supplying a bundle; there is deliberately
// no automatic bundle discovery here.
BOOL tv_materialize_romfs_patch(NSString *romfs, NSString *writable,
    NSString *bundle, NSString *expectedGame, NSString **overlay,
    NSString **failure);
