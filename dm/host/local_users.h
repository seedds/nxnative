// Local console accounts shared by the guest account service, filesystem and launcher.
// The registry is loaded once, after launch settings are installed. See docs/frontends.md.
#pragma once
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DM_LEGACY_USER_ID "53574e415449564578626c0000000001"
#define DM_MAX_LOCAL_USERS 8
typedef struct { uint64_t v[2]; } dm_user_uid; // guest order: low, high
typedef struct {
    dm_user_uid uid;
    char id[33];       // canonical hex, high word first
    char nickname[33]; // up to 32 UTF-8 bytes
} dm_local_user;
typedef struct {
    int count, configured;
    dm_local_user users[DM_MAX_LOCAL_USERS];
} dm_user_roster;

// Launcher initialization (before the first roster lookup). Paths supplied by the platform are absolute.
int dm_users_prepare_file(const char *base_directory, const char *default_file);
const dm_user_roster *dm_users(void);
const dm_local_user *dm_user_find(const dm_user_uid *uid);
const dm_local_user *dm_selected_user(void);

// The launcher edits and reads on one UI thread before guest start. Look up IDs
// again after editing: a concurrent launcher's changes may reorder the roster.
// Once the session starts, the roster and its pointers are immutable. Invalid configuration has
// zero users and a diagnostic. Selection is locked by an explicit DM_USER_ID or at game start.
int dm_users_count(void);
const char *dm_users_id_at(int index);
const char *dm_users_nickname_at(int index);
int dm_users_selected_index(void);
int dm_users_selection_locked(void);
int dm_users_select_index(int index); // before game start; exports the ID to a child host
const char *dm_users_error(void);     // NULL on success
int dm_users_can_edit(void);
// Launcher only. NULL rename_id creates and selects a new user. Renaming keeps the target ID.
// Returns NULL on success or a diagnostic. A failed edit leaves the loaded roster untouched.
const char *dm_users_edit(const char *rename_id, const char *nickname);
int dm_save_prepare_root(const char *root, const char *title_id, const char *source_save,
                         char *out, size_t out_size, char *error, size_t error_size);
int dm_users_begin_session(void);    // freezes selection, returns 0 on error
int dm_users_write_json(FILE *out);  // discovery without loading game files; 1 on success

#ifdef __cplusplus
}
#endif
