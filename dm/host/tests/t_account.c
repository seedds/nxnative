// account::GetUserId copies the Uid of any handle, as the SDK's accessor does: the host's own handles and handles a game built itself, whatever their third word.
// The user selector adds a local user when the game leaves every existing one out (a second local player), and the lists, count, existence and nicknames follow.
#include "../hle_account.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

void hle_signal_system_event(void *g) { (void)g; }
void dm_applet_enter(const char *name) { (void)name; }
void dm_applet_leave(void) {}
static int g_test_players = 1;
int dm_players_reported(void) { return g_test_players; }

int main(void) {
    UserHandle h; Uid out;
    assert(h_acc_TryOpenPre(&h));
    assert(h_acc_GetUserId(&out, &h) == RES_OK);
    assert(!memcmp(&out, &k_uid, sizeof out));
    // A handle the game built: EA SPORTS FC 27's second controller passes third word 0x2, the FC 26 patch reports 8
    const uint64_t third[] = {2, 8, 0, UINT64_MAX};
    for (unsigned i = 0; i < sizeof third / sizeof third[0]; i++) {
        h = (UserHandle){{0x7012222b20, 0x400000001, third[i]}};
        assert(h_acc_GetUserId(&out, &h) == RES_OK);
        assert(out.v[0] == 0x7012222b20 && out.v[1] == 0x400000001);
        assert(h.v[2] == third[i]);
        h.v[0] = h.v[1] = 0;
        assert(h_acc_GetUserId(&out, &h) == RES_OK);
        assert(out.v[0] == 0 && out.v[1] == 0);
    }
    // One user until the game asks for more: the selector with nothing left out picks user 1 and adds none
    uint8_t set[0x88] = {0}; Uid u, list[DM_USERS + 1]; int n; uint8_t ex; char nick[33];
    assert(h_acc_ShowSel(&u, set) == RES_OK && !memcmp(&u, &k_uid, sizeof u) && dm_uid_user(&u) == 0);
    assert(h_acc_UserCount(&n) == RES_OK && n == 1);
    assert(h_acc_ListAll(&n, list, 8) == RES_OK && n == 1);
    // Player 1's Uid left out (Minecraft Dungeons II's second player): user 2 is added, exists, is listed and has a nickname of its own
    memcpy(set, &k_uid, 16);
    assert(h_acc_ShowSel(&u, set) == RES_OK && dm_uid_user(&u) == 1);
    assert(h_acc_UserCount(&n) == RES_OK && n == 2);
    assert(h_acc_ListOpen(&n, list, 8) == RES_OK && n == 2 && !memcmp(&list[1], &u, sizeof u));
    assert(h_acc_ListOpen(&n, list, 1) == RES_OK && n == 1);
    assert(h_acc_Exists(&ex, &u) == RES_OK && ex);
    assert(h_acc_GetNick(nick, &u) == RES_OK && !strcmp(nick, "Player 2"));
    // The same request again picks user 2 (no new user); user 1 still has DM_NICK's name
    assert(h_acc_ShowSel(&u, set) == RES_OK && dm_uid_user(&u) == 1);
    assert(h_acc_UserCount(&n) == RES_OK && n == 2);
    setenv("DM_NICK", "Steve", 1); assert(h_acc_GetNick(nick, &k_uid) == RES_OK && !strcmp(nick, "Steve"));
    // Users 1 and 2 left out: user 3; a Uid that is no user does not exist
    Uid u2 = u; memcpy(set + 16, &u2, 16);
    assert(h_acc_ShowSel(&u, set) == RES_OK && dm_uid_user(&u) == 2);
    Uid none = {{0x1234, 0x5678}}; assert(h_acc_Exists(&ex, &none) == RES_OK && !ex && dm_uid_user(&none) == -1);
    // Every user left out with all DM_USERS made: canceled
    for (int i = 0; i < 8; i++) { Uid w = {{DM_UID0 + (uint64_t)i, DM_UID1}}; memcpy(set + 16 * i, &w, 16); }
    for (int i = 0; i < DM_USERS; i++) h_acc_ShowSel(&u, set);
    assert(h_acc_UserCount(&n) == RES_OK && n == DM_USERS);
    assert(h_acc_ShowSel(&u, set) == RES_ACC_CANCELED);
    // A new player with nothing left out (Minecraft Dungeons II's player 2): two players, user 1 open, so a user nobody has open; one player: user 1, as before
    g_users = 1; memset(set, 0, sizeof set);
    assert(h_acc_ShowSel(&u, set) == RES_OK && dm_uid_user(&u) == 0);
    g_test_players = 2;
    assert(h_acc_ShowSel(&u, set) == RES_OK && dm_uid_user(&u) == 1);
    assert(h_acc_UserCount(&n) == RES_OK && n == 2);
    assert(h_acc_ShowSel(&u, set) == RES_OK && dm_uid_user(&u) == 1);    // asked again before it opened user 2: the same user, none added
    assert(h_acc_UserCount(&n) == RES_OK && n == 2);
    UserHandle h2; assert(h_acc_OpenUser(&h2, &u) == RES_OK);
    assert(h_acc_ShowSel(&u, set) == RES_OK && dm_uid_user(&u) == 0);    // both players have a user open: the players choose, user 1 first
    g_test_players = 3;
    assert(h_acc_ShowSel(&u, set) == RES_OK && dm_uid_user(&u) == 2);    // a third player: a third user
    h_acc_CloseUser(&h2); g_test_players = 2;
    assert(h_acc_ShowSel(&u, set) == RES_OK && dm_uid_user(&u) == 1);    // player 2 left and came back: user 2 is free again
    puts("t_account: OK");
    return 0;
}
