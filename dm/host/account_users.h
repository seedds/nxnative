// Local account identities and their durable registration, shared by account and fs.
// User 1 keeps the original host UID and save path; additional slots keep PR #2846's UIDs.
#pragma once
#include <stdint.h>
#include <string.h>
#define DM_USERS 8
#define DM_UID0 UINT64_C(0x78626c0000000001)
#define DM_UID1 UINT64_C(0x53574e4154495645)
static inline int dm_uid_user(const void *uid) {
    if (!uid) return -1;
    uint64_t v[2]; memcpy(v, uid, sizeof v);
    return v[1] == DM_UID1 && v[0] >= DM_UID0 && v[0] < DM_UID0 + DM_USERS ? (int)(v[0] - DM_UID0) : -1;
}
// A bit per registered user, or 0 on an unreadable/corrupt registry. Never includes session state.
unsigned dm_account_users(void);
// Register an unused slot durably before exposing it to a game. 0 on success, -1 on failure.
int dm_account_create_user(int user);
// Registered UID -> slot, -1 for an unknown UID. NULL is not a guest account.
int dm_account_user(const void *uid);
