// Discord activity on the Mac (issue #1845): while the game runs, the player's Discord profile shows "Playing <application>", the game's name, its version and the time
// played. Off unless the player turns it on (DM_DISCORD=1, the Mac settings window's Discord activity). Mac only: the iPhone has no Discord client socket to reach, and the
// _mac name keeps this file out of the iOS app (dm/ios/tools/gen_build.py).
//
// Discord shows the name of a registered application (discord.com/developers), found by its ID, above the activity: DM_DISCORD_APP_ID, else the ID built in below. Until
// the project registers one (named nxnative), the built-in ID is empty and the activity needs DM_DISCORD_APP_ID. The protocol is in discord_rpc.h.
//
// All of it runs on one background thread: Discord not running, slow or closing never holds the game. Without the client it tries again every 15 s, so starting Discord
// after the game is enough. Closing the socket (the game quitting) clears the activity on Discord's side.
#include "dm.h"
#include "discord_rpc.h"
#include <pthread.h>

#ifndef DM_DISCORD_APP_ID_DEFAULT
#define DM_DISCORD_APP_ID_DEFAULT ""   // the project's registered Discord application, once it has one
#endif

static char g_app_id[24], g_details[512], g_state[64];   // the name whole: drpc_json_escape cuts it to Discord's limit at a UTF-8 character boundary
static int64_t g_start;

// "Version 1.4.3": the display version of main's build from the profile's build rows, as GetDisplayVersion reports it (hle_sdk16.c); empty for a build the profile lacks
static void display_version(char *out, size_t cap) {
    out[0] = 0;
#ifdef DM_GAME_DISPLAY_VERSIONS
    const struct { const char *build, *ver; } k[] = {DM_GAME_DISPLAY_VERSIONS};
    const char *b = dm_main_build();
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) if (k[i].build && k[i].ver && !strcmp(b, k[i].build)) { snprintf(out, cap, "Version %s", k[i].ver); return; }
#else
    (void)cap;
#endif
}

static void *discord_thread(void *unused) {
    (void)unused;
    char tmp[1024] = ""; const char *dirs[8];
    if (!confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof tmp) || strlen(tmp) >= sizeof tmp - 1) tmp[0] = 0;
    int nd = drpc_dirs(dirs, 8, tmp), said_waiting = 0;
    unsigned nonce = 1;
    for (;;) {
        int err; char why[256];
        int fd = drpc_connect(dirs, nd, g_app_id, 3000, &err, why, sizeof why);
        if (fd < 0) {
            if (err == 1) {   // Discord answered and turned the ID down: trying again will not change that
                LOG("Discord activity: Discord refused application ID %s (%s); check DM_DISCORD_APP_ID", g_app_id, why);
                return NULL;
            }
            if (!said_waiting) { LOG("Discord activity: Discord is not running (or did not answer); trying again every 15 s"); said_waiting = 1; }
            sleep(15); continue;
        }
        int r = drpc_set_activity(fd, getpid(), g_details, g_state, g_start, nonce++, 3000, why, sizeof why);
        if (r == 1) {   // a field Discord does not take: the same request would fail again, so stop rather than repeat it
            LOG("Discord activity: Discord refused the activity (%s)", why);
            close(fd); return NULL;
        }
        if (r == 0) {
            LOG("Discord activity: showing %s%s%s", g_details, *g_state ? ", " : "", g_state); said_waiting = 0;
            while (!drpc_pump(fd, 60000)) {}   // keep the socket (the activity lasts while it is open) until Discord closes it
        }
        close(fd);
        sleep(5);   // Discord quit or restarted: look for it again
    }
}

// Called once by the Mac front end when the game window opens (front_mac.m front_run)
void dm_discord_start(void) {
    const char *on = getenv("DM_DISCORD");
    if (!on || atoi(on) <= 0) return;
    const char *id = getenv("DM_DISCORD_APP_ID");
    if (!id || !*id) id = DM_DISCORD_APP_ID_DEFAULT;
    if (!*id) { LOG("Discord activity: no Discord application ID (DM_DISCORD_APP_ID): not shown"); return; }
    if (!drpc_valid_app_id(id)) { LOG("Discord activity: DM_DISCORD_APP_ID is not an application ID (15-20 digits): %s", id); return; }
    snprintf(g_app_id, sizeof g_app_id, "%s", id);
    snprintf(g_details, sizeof g_details, "%s", DM_GAME_NAME);
    if (strlen(g_details) < 2) snprintf(g_details, sizeof g_details, "%s", DM_GAME_ID);   // Discord takes at least 2 characters; the title ID always has 16
    display_version(g_state, sizeof g_state);
    g_start = (int64_t)time(NULL);
    pthread_t th; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &a, discord_thread, NULL)) LOG("Discord activity: cannot start its thread");
    pthread_attr_destroy(&a);
}
