// Save data as one file a player can keep: pack the committed saves and restore one. A front end (the iPhone
// launcher, the Mac settings window) calls these before the game starts; the host itself calls neither, because
// its mounts hold the working directories (dm_fs_save_in_use).
//
// The file carries what a Commit published ($DM_SAVE and, for a game that mounts both, <$DM_SAVE>_device), never
// the working directories beside them: an uncommitted save is discarded by the next mount, so it is not the
// player's save. Nothing here reads the game's files or needs the network.
#ifndef DM_SAVE_ARCHIVE_H
#define DM_SAVE_ARCHIVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DM_SAVE_OK = 0,
    DM_SAVE_ERR_USAGE = 1,    // no DM_SAVE, no output path, or the game holds a mounted save
    DM_SAVE_ERR_IO = 2,       // a host file could not be read or written
    DM_SAVE_ERR_FORMAT = 3,   // not an archive, or one outside the format (truncated, bad checksum, unsafe path)
    DM_SAVE_ERR_GAME = 4,     // save data of another game
    DM_SAVE_ERR_EMPTY = 5,    // there is nothing to export
};

// Writes the committed save data to path (replacing it atomically). title_id is recorded in the header (0 when the
// front end does not know it). Returns DM_SAVE_OK, or a DM_SAVE_ERR_* code; on failure err holds a line for the player.
int dm_save_export(const char *path, uint64_t title_id, char *err, size_t errlen);

// Replaces the committed save data with the archive at path, keeping the replaced trees beside them as
// <save>.pre-import. Refuses another game's archive when both title IDs are known. The same error contract.
int dm_save_import(const char *path, uint64_t title_id, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif // DM_SAVE_ARCHIVE_H
