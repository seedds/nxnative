// Shared Swift launcher entry points. Configure after reading launch overrides and before showing the UI.
#pragma once
#include <stddef.h>
void dm_local_save_configure(const char *title, const char *default_save);
int dm_local_save_apply(char *error, size_t error_size);
