// Private Mac launcher/host bridge. Only pack_app's fixed, ABI/profile-checked bundle library is loaded.
#pragma once
#define DM_MAC_HOST_ABI "1"
#ifdef __cplusplus
extern "C" {
#endif
const char *dm_mac_host_identity(void);
int dm_mac_host_entry(int argc, char **argv);
void *dm_mac_players_view(void); // unretained NSView; the host owns it and its controller objects
void dm_mac_players_finish(int play);
#ifdef __cplusplus
}
#endif
