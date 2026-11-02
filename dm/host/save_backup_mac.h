// Manual Mac backups use the cloud save's checked format, including device saves and guest-name metadata.
#pragma once
#include <stddef.h>
int mac_save_export(const char *save, const char *title, const char *archive);
// No host may be using this save. Keeps a recovery copy in `kept`; a failed replacement rolls back.
int mac_save_restore(const char *save, const char *title, const char *archive, char *kept, size_t size);
