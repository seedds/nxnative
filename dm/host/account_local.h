#pragma once
int dm_account_uid_exists(const void *uid);
// Copy the actual local UID; this grants no provider identity.
int dm_account_copy_uid(void *output);
