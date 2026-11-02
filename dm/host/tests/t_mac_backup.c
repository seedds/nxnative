// Manual backup round trips, wrong/damaged archives, and rollback on a failed second directory swap.
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <removefile.h>
static int swaps, fail_swap;
static int swap_for_test(const char *a, const char *b, unsigned flags) {
    if (++swaps == fail_swap) { errno = EIO; return -1; }
    return renamex_np(a, b, flags);
}
#define renamex_np swap_for_test
#include "../save_backup_mac.c"
#undef renamex_np
static void put(const char *p, const char *value) { FILE *f = fopen(p, "w"); assert(f); assert(fputs(value, f) >= 0); assert(!fclose(f)); }
static void contents(const char *p, const char *value) { char b[128] = {0}; FILE *f = fopen(p, "r"); assert(f); assert(fread(b, 1, sizeof b - 1, f) == strlen(value)); assert(!fclose(f)); assert(!strcmp(b, value)); }
int main(void) {
    char dir[] = "/tmp/nxnative-backup-XXXXXX"; assert(mkdtemp(dir)); assert(!chdir(dir));
    assert(!mkdir("save", 0700)); assert(!mkdir("save_device", 0700));
    put("save/a", "old normal"); put("save_device/b", "old device");
    assert(!setxattr("save/a", "org.d2rmobile.guest-name", "A", 1, 0, 0));
    assert(!mac_save_export("save/", "010093801237C000", "backup.dmsync"));
    ss_meta m; assert(!ss_read("backup.dmsync", NULL, &m)); assert(m.files == 2);
    put("save/a", "new normal"); put("save_device/b", "new device"); put("save/extra", "remove me");
    char kept[PATH_MAX];
    assert(mac_save_restore("save", "another-game", "backup.dmsync", kept, sizeof kept));
    contents("save/a", "new normal");
    put("broken.dmsync", "DMSYNC1\ntruncated");
    assert(mac_save_restore("save", "010093801237C000", "broken.dmsync", kept, sizeof kept));
    contents("save/a", "new normal");
    swaps = 0; fail_swap = 2;
    assert(mac_save_restore("save", "010093801237C000", "backup.dmsync", kept, sizeof kept));
    contents("save/a", "new normal"); contents("save_device/b", "new device"); contents("save/extra", "remove me");
    assert(*kept); char p[PATH_MAX]; snprintf(p, sizeof p, "%s/save/a", kept); contents(p, "new normal");
    fail_swap = 0;
    assert(!mac_save_restore("save/", "010093801237C000", "backup.dmsync", kept, sizeof kept));
    contents("save/a", "old normal"); contents("save_device/b", "old device"); assert(access("save/extra", F_OK));
    char guest[8]; assert(getxattr("save/a", "org.d2rmobile.guest-name", guest, sizeof guest, 0, 0) == 1 && *guest == 'A');
    snprintf(p, sizeof p, "%s/save/extra", kept); contents(p, "remove me");
    assert(!mkdir("empty", 0700)); assert(mac_save_export("empty", "010093801237C000", "backup.dmsync"));
    assert(!ss_read("backup.dmsync", NULL, &m) && m.files == 2);   // failed exports keep the previous archive
    assert(!removefile("save_device", NULL, REMOVEFILE_RECURSIVE));
    assert(!mac_save_export("save", "010093801237C000", "normal-only.dmsync"));
    assert(!mkdir("save_device", 0700)); put("save_device/extra", "remove device");
    assert(!mac_save_restore("save", "010093801237C000", "normal-only.dmsync", kept, sizeof kept));
    assert(access("save_device/extra", F_OK));
    assert(!chdir("/")); assert(!removefile(dir, NULL, REMOVEFILE_RECURSIVE));
    puts("Mac save backups: round trip, metadata, recovery, failed-swap rollback, invalid archives, empty exports and device removal pass");
    return 0;
}
