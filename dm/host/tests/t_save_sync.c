// Cloud save sync unit test (takes seconds): dm/host/save_sync.c through hle_fs.c's own save mount and Commit, with a plain folder standing in for iCloud Drive.
// Each device's game session runs in a child process (the host's sync state is once per process, as it is once per start of the app); the parent checks what each left
// in its save, its backups and the cloud. All expected values are written by hand from the semantics in save_sync.h:
//   (1) packing: a save with folders, a guest name and a device save comes back the same (contents, guest name, dates); the hash ignores dates and .DS_Store and sees contents;
//       a file cut short, a flipped byte, a byte too many and a path out of the save are refused
//   (2) ss_decide's table: both changed, the newer one wins; with a front end that asks, it asks, whatever the times say
//   (3) Mac A has a save and the cloud none: A's session sends it; iPhone B has none: B's session brings it in; B saves: the cloud has B's
//   (4) A starts again: only the cloud's changed: it comes in, and A's old save is kept in A's backups ("local")
//   (5) both changed, the cloud's newer, with a front end that asks (DM_SAVE_SYNC_CHOICE): B played without sync, A sent a newer save; B starts with sync and no
//       choice yet ("ask"): neither replaces the other; with the player's choice of the cloud's: A's comes in, B's is kept
//   (6) both changed, this one newer, with a front end that asks: A played without sync later than B sent; A starts with no choice yet, then with a choice made on
//       another cloud save: nothing is replaced; with the player's choice of this one: A's goes out, B's is kept in A's backups ("cloud")
//   (7) the Mac launcher's send after the host exited: a save the host committed and did not send goes out
//   (8) the game's own folder (<folder>/<title ID>) chosen as the folder
//   (9) a new backup removes the oldest of its side beyond 10, and leaves the other side's and the player's own folders
//   (10) the launcher's DM_SAVE with a trailing slash: the same <save>.sync as the host's
//   (11) the cloud cannot be read at the start (a download that does not come in time), and can when the game saves: the cloud's changed save is not replaced,
//        neither by the sender nor by the Mac launcher after the host; the player's "offline" choice with the cloud unreadable all along sends nothing
//   (12) the first play on a new Mac, the cloud's save not read at the start: the new save does not replace the cloud's progress; the next start, with a front end
//        that asks, asks; the player's choice of the cloud's brings it in and keeps the new one
//   (13) a damaged file in the cloud: no save to lose, this device's replaces it and the file is kept in the backups ("cloud")
//   (14) no front end asks: both changed, the newer one wins at the start (the cloud's: it comes in, this one is kept); the cloud not read at the start and this
//        device's save newer: not sent that session, the next start sends it and keeps the cloud's in the backups ("cloud")
//   (17) the cloud shows no save though this device synced one there (removed, wrong folder, or iCloud has not listed it yet): with a front end that asks,
//        the report says conflict=1 cloud=0, the start sends nothing, nor does the launcher after the host; the player's local:none sends; no front end sends, as before
//   (18) conflict cloud backup rotates to newest 10 and keeps user folders
//   (19) a copy of the save at each start ("start"), sync on or off: one when the save changed since the newest copy, none when it did not, when DM_SAVE_BACKUPS=0,
//        or when there is no save; the oldest beyond 10 go while sync's backups and player folders stay; an unwritable backups folder does not stop the game;
//        with sync on, the copy is the save the game starts from (the cloud's when it came in); after a pull that did not finish, the copy is this device's save put back,
//        and a save the mount refuses (a journal that cannot be put back or read) is not copied
#include "../hle_fs.c"
#include <ctype.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/sysctl.h>

struct dm_flags g_flag; uint64_t g_t0_ns;
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }
void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); } else printf("  ✓ "); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
static void put(const char *p, const char *s) { FILE *f = fopen(p, "w"); fputs(s, f); fclose(f); }
static int same(const char *p, const char *s) {
    char b[64] = {0}; FILE *f = fopen(p, "r"); if (!f) return 0; size_t k = fread(b, 1, sizeof b - 1, f); fclose(f);
    return k == strlen(s) && !memcmp(b, s, k);
}
static void mkd(const char *p) { char c[1200]; snprintf(c, sizeof c, "%s", p); for (char *s = c + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(c, 0755); *s = '/'; } mkdir(c, 0755); }
static void set_mtime(const char *p, time_t t) { struct timeval tv[2] = {{t, 0}, {t, 0}}; utimes(p, tv); }
static time_t mtime_of(const char *p) { struct stat st; return stat(p, &st) ? 0 : st.st_mtimespec.tv_sec; }
static int exists(const char *p) { return !access(p, F_OK); }

static int is_side_backup(const char *name, const char *side) {
    if (strlen(name) < 20) return 0;
    const char *s = name + 19;
    if (*s != ' ') return 0;
    s++;
    size_t sl = strlen(side);
    if (strncmp(s, side, sl)) return 0;
    if (!s[sl]) return 1;
    if (s[sl] != ' ' || !s[sl + 1]) return 0;
    for (s += sl + 1; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}
static int cmp_backup_name(const char *a, const char *b) {
    int c = strncmp(a, b, 19);
    if (c) return c;
    const char *sa = strrchr(a + 19, ' '), *sb = strrchr(b + 19, ' ');
    unsigned long na = sa && isdigit((unsigned char)sa[1]) ? strtoul(sa + 1, NULL, 10) : 1;
    unsigned long nb = sb && isdigit((unsigned char)sb[1]) ? strtoul(sb + 1, NULL, 10) : 1;
    return na < nb ? -1 : na > nb ? 1 : strcmp(a, b);
}
// The newest backup of a save for `side` ("YYYY-MM-DD HH.MM.SS <side>" or with " <k>"), "" when there is none
static void backup_of(const char *save, const char *side, char *out, size_t n) {
    char dir[1200]; snprintf(dir, sizeof dir, "%s-backups", save); out[0] = 0;
    DIR *d = opendir(dir); struct dirent *e; char best[256] = "";
    while (d && (e = readdir(d))) if (is_side_backup(e->d_name, side) && (!*best || cmp_backup_name(e->d_name, best) > 0)) strlcpy(best, e->d_name, sizeof best);
    if (d) closedir(d);
    if (*best) snprintf(out, n, "%s/%s", dir, best);
}
// What the cloud holds of the game file, unpacked into a folder of its own
static int cloud_has(const char *cloud_file, const char *root, const char *file, const char *value) {
    static int k; char into[1200], p[1400]; ss_meta m;
    snprintf(into, sizeof into, "%s/peek%d", root, k++); mkd(into);
    if (ss_read(cloud_file, into, &m)) return 0;
    snprintf(p, sizeof p, "%s/save/%s", into, file); return same(p, value);
}

static Result write_save(const char *path, const char *s) {           // create -> open -> set size -> write -> close, as a game does
    uint64_t h; h_fs_CreateFile(path, 0); Result r = h_fs_OpenFile(&h, path, MODE_WRITE); if (r) return r;
    h_fs_SetFileSize(h, (int64_t)strlen(s));
    r = h_fs_WriteFile(h, 0, s, strlen(s), NULL); h_fs_CloseFile(h); return r;
}

enum { NOT_SENT = 64 };   // run(): the flush did not leave the save in step with the cloud
enum { READABLE, LATE, UNREADABLE };   // run(): the cloud's file all along, only after the save mount (a download that did not come in time), never
// One start of the game on a device: the save mount (the cloud's save comes in), then, with a file, the game writing it and committing, then a flush (the iPhone app
// going to the background). cloud NULL: sync off; choice: DM_SAVE_SYNC_CHOICE; cf: the cloud's file, made unreadable as `reach` says.
// Returns the child's failed steps, plus NOT_SENT when the flush did not leave the save in step with the cloud
static int run(const char *save, const char *cloud, const char *file, const char *value, const char *choice, const char *cf, int reach) {
    fflush(stdout); fflush(stderr);
    if (reach != READABLE) chmod(cf, 0);
    pid_t p = fork();
    if (!p) {
        setenv("DM_SAVE", save, 1);
        if (cloud) setenv("DM_SAVE_SYNC", cloud, 1); else unsetenv("DM_SAVE_SYNC");
        if (choice) setenv("DM_SAVE_SYNC_CHOICE", choice, 1); else unsetenv("DM_SAVE_SYNC_CHOICE");
        int bad = h_fs_MountSave("save", NULL) != RES_OK;
        if (reach == LATE) chmod(cf, 0644);
        if (file) { char gp[256]; snprintf(gp, sizeof gp, "save:/%s", file); bad += write_save(gp, value) != RES_OK; bad += h_fs_Commit("save") != RES_OK; }
        if (cloud && save_sync_flush(10000) != 1) bad += NOT_SENT;
        fflush(stdout); fflush(stderr); _exit(bad);
    }
    int st; waitpid(p, &st, 0);
    if (reach != READABLE) chmod(cf, 0644);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 99;
}
static int session(const char *save, const char *cloud, const char *file, const char *value) { return run(save, cloud, file, value, NULL, NULL, READABLE); }
// DM_SAVE_SYNC_CHOICE for the cloud's save as it is now: "<side>:<its hash>"
static const char *choice_of(const char *side, const char *cf) {
    static char c[64]; ss_meta m;
    if (ss_read(cf, NULL, &m)) snprintf(c, sizeof c, "%s:none", side); else snprintf(c, sizeof c, "%s:%s", side, m.hash);
    return c;
}
static int backups(const char *save, const char *side) {   // how many backups of that side
    char dir[1200]; snprintf(dir, sizeof dir, "%s-backups", save);
    int k = 0; DIR *d = opendir(dir); struct dirent *e;
    while (d && (e = readdir(d))) k += is_side_backup(e->d_name, side);
    if (d) closedir(d);
    return k;
}

int main(void) {
    char root[1024]; const char *sp = getenv("TMPDIR"); snprintf(root, sizeof root, "%s/t_save_sync.XXXXXX", sp ? sp : "/private/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    char p[1400], q[1400], u[1400], bundle[1400];
    ss_meta m, m2;

    printf("(1) packing\n");
    char P[1200]; snprintf(P, sizeof P, "%s/pack/save", root);
    snprintf(p, sizeof p, "%s/sub", P); mkd(p); snprintf(p, sizeof p, "%s/empty", P); mkd(p); snprintf(p, sizeof p, "%s_device", P); mkd(p);
    snprintf(p, sizeof p, "%s/a", P); put(p, "hello"); set_mtime(p, 1000000000);
    snprintf(p, sizeof p, "%s/sub/b", P); put(p, "xy"); setxattr(p, FS_GUEST_NAME, "B", 1, 0, XATTR_NOFOLLOW); set_mtime(p, 1000000100);
    snprintf(p, sizeof p, "%s/sub/.dm-case-1", P); put(p, "alias"); setxattr(p, FS_GUEST_NAME, "CaseFile", 8, 0, XATTR_NOFOLLOW); set_mtime(p, 1000000075);
    snprintf(p, sizeof p, "%s_device/d", P); put(p, "dev"); set_mtime(p, 1000000050);
    CHECK(!ss_scan(P, &m) && m.files == 4 && strlen(m.hash) == SS_HASH && m.saved == 1000000100, "scan: 4 files, a 32-digit hash, saved = the newest file's date (%d files, saved %lld)", m.files, (long long)m.saved);
    snprintf(bundle, sizeof bundle, "%s/pack/save.dmsync", root);
    CHECK(!ss_pack(P, "0100abc", bundle, &m2) && !strcmp(m2.hash, m.hash) && !strcmp(m2.title, "0100ABC"), "pack: the same hash as the scan, the title ID in capitals (%s)", m2.title);
    CHECK(!ss_read(bundle, NULL, &m2) && !strcmp(m2.hash, m.hash) && m2.files == 4 && m2.saved == 1000000100 && !strcmp(m2.title, "0100ABC") && m2.device[0], "read: hash, files, date, title and device come back (from %s)", m2.device);
    snprintf(u, sizeof u, "%s/pack/out", root); mkd(u);
    CHECK(!ss_read(bundle, u, &m2), "unpack");
    snprintf(p, sizeof p, "%s/save/a", u); snprintf(q, sizeof q, "%s/save/sub/b", u);
    char g[16] = {0}; ssize_t gl = getxattr(q, FS_GUEST_NAME, g, sizeof g - 1, 0, XATTR_NOFOLLOW);
    CHECK(same(p, "hello") && same(q, "xy") && gl == 1 && g[0] == 'B', "files and the guest name (hle_fs.c's FS_GUEST_NAME) come back");
    snprintf(q, sizeof q, "%s/save/sub/.dm-case-1", u);
    memset(g, 0, sizeof g); gl = getxattr(q, FS_GUEST_NAME, g, sizeof g - 1, 0, XATTR_NOFOLLOW);
    CHECK(same(q, "alias") && gl == 8 && !strcmp(g, "CaseFile"), "a .dm-case-1 alias round-trips its original guest name and contents");
    CHECK(mtime_of(p) == 1000000000, "a file's date comes back (%lld)", (long long)mtime_of(p));
    snprintf(p, sizeof p, "%s/save/empty", u); snprintf(q, sizeof q, "%s/save_device/d", u);
    struct stat st; CHECK(!stat(p, &st) && S_ISDIR(st.st_mode) && same(q, "dev"), "an empty folder and the device save come back");
    snprintf(p, sizeof p, "%s/save", u); CHECK(!ss_scan(p, &m2) && !strcmp(m2.hash, m.hash), "the unpacked save has the original's hash");
    snprintf(p, sizeof p, "%s/a", P); set_mtime(p, 1200000000);
    CHECK(!ss_scan(P, &m2) && !strcmp(m2.hash, m.hash) && m2.saved == 1200000000, "another date, the same hash (the date still says which is newer)");
    snprintf(p, sizeof p, "%s/.DS_Store", P); put(p, "finder");
    CHECK(!ss_scan(P, &m2) && !strcmp(m2.hash, m.hash), "Finder's .DS_Store is not part of the save");
    snprintf(p, sizeof p, "%s/a", P); put(p, "hellO");
    CHECK(!ss_scan(P, &m2) && strcmp(m2.hash, m.hash), "one changed byte, another hash");
    snprintf(p, sizeof p, "%s/pack/none", root); mkd(p);
    CHECK(!ss_scan(p, &m2) && !m2.files && !m2.hash[0], "an empty folder is no save: no files, no hash");
    {   FILE *f = fopen(bundle, "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f); char *b = malloc((size_t)n + 1); fread(b, 1, (size_t)n, f); fclose(f);
        snprintf(p, sizeof p, "%s/pack/bad.dmsync", root);
        f = fopen(p, "wb"); fwrite(b, 1, (size_t)n - 1, f); fclose(f); CHECK(ss_read(p, NULL, &m2) && !m2.files, "a file cut short by one byte is refused");
        f = fopen(p, "wb"); fwrite(b, 1, (size_t)n, f); fputc(0, f); fclose(f); CHECK(ss_read(p, NULL, &m2), "a byte too many is refused");
        b[n - 12] ^= 1; f = fopen(p, "wb"); fwrite(b, 1, (size_t)n, f); fclose(f); CHECK(ss_read(p, NULL, &m2), "a flipped byte in the contents is refused (the hash)");
        free(b);
    }
    {   // one entry, save/../escaped, in an otherwise well-formed file
        uint8_t hdr[144] = {0}, h[24] = {2}; uint32_t one = 1, n = 1; uint16_t pl = 15; uint64_t sz = 1;
        memcpy(hdr, "DMSYNC1\n", 8); memcpy(hdr + 8, &one, 4); memcpy(hdr + 12, &n, 4); memcpy(h + 2, &pl, 2); memcpy(h + 8, &sz, 8);
        snprintf(p, sizeof p, "%s/pack/escape.dmsync", root); FILE *f = fopen(p, "wb");
        fwrite(hdr, 1, sizeof hdr, f); fwrite(h, 1, sizeof h, f); fwrite("save/../escaped", 1, 15, f); fputc('z', f); fwrite("DMSYNCE\n", 1, 8, f); fclose(f);
        snprintf(u, sizeof u, "%s/pack/esc/in", root); mkd(u); snprintf(q, sizeof q, "%s/pack/esc/in/escaped", root);
        char q2[1400]; snprintf(q2, sizeof q2, "%s/pack/esc/escaped", root);
        CHECK(ss_read(p, u, &m2) && !exists(q) && !exists(q2), "a path that leaves the save is refused, and nothing is written");
    }

    printf("(2) which way\n");
    int both;
    CHECK(ss_decide("", "", "", 0, 0, 0, &both) == SS_KEEP && !both, "no save anywhere: keep");
    CHECK(ss_decide("L", "", "", 0, 0, 0, &both) == SS_PUSH && !both, "nothing in the cloud: send");
    CHECK(ss_decide("", "", "R", 0, 0, 0, &both) == SS_PULL && !both, "nothing here: bring in");
    CHECK(ss_decide("X", "B", "X", 0, 0, 0, &both) == SS_KEEP && !both, "the same here and in the cloud: keep");
    CHECK(ss_decide("B", "B", "R", 9, 1, 0, &both) == SS_PULL && !both, "only the cloud's changed: bring in, even with an older date");
    CHECK(ss_decide("L", "B", "B", 1, 9, 0, &both) == SS_PUSH && !both, "only this one changed: send, even with an older date");
    CHECK(ss_decide("L", "B", "R", 10, 20, 0, &both) == SS_PULL && both, "both changed, the cloud's newer: bring in");
    CHECK(ss_decide("L", "B", "R", 20, 10, 0, &both) == SS_PUSH && both, "both changed, this one newer: send");
    CHECK(ss_decide("L", "", "R", 20, 10, 0, &both) == SS_PUSH && both, "a first sync with a save of its own counts as both changed");
    CHECK(ss_decide("L", "B", "R", 10, 10, 0, &both) == SS_PUSH && both, "the same date: this device's is kept");
    CHECK(ss_decide("L", "B", "R", 10, 20, 1, &both) == SS_ASK && both, "a front end asks: both changed, the cloud's newer: ask");
    CHECK(ss_decide("L", "B", "R", 20, 10, 1, &both) == SS_ASK && both, "a front end asks: both changed, this one newer: ask");
    CHECK(ss_decide("L", "", "R", 20, 10, 1, &both) == SS_ASK && both, "a front end asks: a first sync with a save of its own: ask");
    CHECK(ss_decide("B", "B", "R", 9, 1, 1, &both) == SS_PULL && !both, "a front end asks: only the cloud's changed still comes in");
    CHECK(ss_decide("B", "B", "", 0, 0, 1, &both) == SS_ASK && both, "a front end asks: no save in the cloud though this device synced one: ask, not send");
    CHECK(ss_decide("L", "B", "", 0, 0, 1, &both) == SS_ASK && both, "a front end asks: the same with this device's save changed");
    CHECK(ss_decide("L", "", "", 0, 0, 1, &both) == SS_PUSH && !both, "a front end asks: nothing in the cloud and never synced: send");
    CHECK(ss_decide("B", "B", "", 0, 0, 0, &both) == SS_PUSH && both, "no front end: no save in the cloud though this device synced one: send, as before");

    char A[1200], B[1200], cloud[1200], cf[1400], id[24], back[1400];
    snprintf(A, sizeof A, "%s/mac/save", root); snprintf(B, sizeof B, "%s/iphone/save", root); snprintf(cloud, sizeof cloud, "%s/iCloud Drive/nxnative Saves", root);
    mkd(A); mkd(B); mkd(cloud);
    for (size_t i = 0; i <= strlen(DM_GAME_ID); i++) id[i] = (char)toupper((unsigned char)DM_GAME_ID[i]);
    snprintf(cf, sizeof cf, "%s/%s/save.dmsync", cloud, id);

    printf("(3) Mac -> cloud -> iPhone\n");
    snprintf(p, sizeof p, "%s/game.dat", A); put(p, "v1");
    CHECK(session(A, cloud, NULL, NULL) == 0 && cloud_has(cf, root, "game.dat", "v1"), "the Mac's first session with sync sends its save");
    snprintf(p, sizeof p, "%s.sync", A); CHECK(exists(p), "and remembers it as in step (%s)", p);
    snprintf(p, sizeof p, "%s.sync-tmp", A); CHECK(!exists(p), "nothing is left beside the save");
    snprintf(p, sizeof p, "%s/game.dat", B);
    CHECK(session(B, cloud, NULL, NULL) == 0 && same(p, "v1"), "the iPhone's first session brings it in");
    CHECK(!backups(B, "local"), "with no save of its own, the iPhone keeps no local backup");
    CHECK(session(B, cloud, "game.dat", "v2") == 0 && cloud_has(cf, root, "game.dat", "v2"), "the iPhone saves: the cloud has its save");

    printf("(4) the cloud's changed\n");
    snprintf(p, sizeof p, "%s/game.dat", A);
    CHECK(session(A, cloud, NULL, NULL) == 0 && same(p, "v2"), "the Mac starts again: the iPhone's save comes in");
    backup_of(A, "local", back, sizeof back); snprintf(q, sizeof q, "%s/save/game.dat", back);
    CHECK(*back && same(q, "v1"), "the Mac's old save is kept (%s)", back);

    printf("(5) both changed, the cloud's newer\n");
    snprintf(p, sizeof p, "%s/game.dat", B);
    CHECK(session(B, NULL, "game.dat", "v4") == 0 && same(p, "v4"), "the iPhone plays with sync off");
    set_mtime(p, time(NULL) - 100);
    CHECK(session(A, cloud, "game.dat", "v5") == 0 && cloud_has(cf, root, "game.dat", "v5"), "then the Mac saves and sends");
    CHECK(run(B, cloud, NULL, NULL, "ask", NULL, READABLE) == NOT_SENT && same(p, "v4") && cloud_has(cf, root, "game.dat", "v5") && !backups(B, "local") && !backups(B, "cloud"),
          "the iPhone starts with sync and no choice yet: it plays with its own, nothing is sent, the newer Mac save stays in the cloud");
    CHECK(run(B, cloud, NULL, NULL, choice_of("cloud", cf), NULL, READABLE) == 0 && same(p, "v5"), "the player chooses the cloud's: the Mac save comes in");
    backup_of(B, "local", back, sizeof back); snprintf(q, sizeof q, "%s/save/game.dat", back);
    CHECK(*back && same(q, "v4"), "the iPhone's own is kept (%s)", back);

    printf("(6) both changed, this one newer\n");
    snprintf(p, sizeof p, "%s/game.dat", A);
    CHECK(session(A, NULL, "game.dat", "v6") == 0, "the Mac plays with sync off");
    set_mtime(p, time(NULL) + 100);
    CHECK(session(B, cloud, "game.dat", "v7") == 0 && cloud_has(cf, root, "game.dat", "v7"), "the iPhone saves and sends");
    char old_choice[64]; snprintf(old_choice, sizeof old_choice, "%s", choice_of("local", cf));
    CHECK(run(A, cloud, NULL, NULL, "ask", NULL, READABLE) == NOT_SENT && same(p, "v6") && cloud_has(cf, root, "game.dat", "v7") && !backups(A, "cloud"),
          "the Mac starts with sync and no choice yet: its newer save stays here and is not sent, the iPhone's stays in the cloud");
    CHECK(session(B, cloud, "game.dat", "v7b") == 0 && cloud_has(cf, root, "game.dat", "v7b"), "the iPhone saves again");
    CHECK(run(A, cloud, NULL, NULL, old_choice, NULL, READABLE) == NOT_SENT && same(p, "v6") && cloud_has(cf, root, "game.dat", "v7b"),
          "a choice made on the cloud's save before that one replaces nothing");
    {   ss_meta here; char own[64] = "";   // the token names the cloud's save, not this device's (the launchers sent local:<this device's hash> until this was checked)
        if (!ss_scan(A, &here)) snprintf(own, sizeof own, "local:%s", here.hash);
        CHECK(*own && run(A, cloud, NULL, NULL, own, NULL, READABLE) == NOT_SENT && same(p, "v6") && cloud_has(cf, root, "game.dat", "v7b"),
              "local:<this device's hash> is not a choice for the cloud's save: nothing is replaced"); }
    CHECK(run(A, cloud, NULL, NULL, choice_of("local", cf), NULL, READABLE) == 0 && same(p, "v6") && cloud_has(cf, root, "game.dat", "v6"),
          "the player chooses this one: it goes out");
    backup_of(A, "cloud", back, sizeof back); snprintf(q, sizeof q, "%s/save/game.dat", back);
    CHECK(*back && same(q, "v7b"), "the iPhone's save it replaced in the cloud is kept on the Mac (%s)", back);

    printf("(7) the Mac launcher after the host\n");
    snprintf(p, sizeof p, "%s/game.dat", A); put(p, "v8");
    setenv("DM_SAVE_SYNC", cloud, 1);
    CHECK(save_sync_after_exit(A, DM_GAME_ID) == 1 && cloud_has(cf, root, "game.dat", "v8"), "a save the host left unsent goes out");
    time_t sent = mtime_of(cf);
    sleep(1);
    CHECK(save_sync_after_exit(A, DM_GAME_ID) == 1 && mtime_of(cf) == sent, "a second time there is nothing to send (the cloud's file is not rewritten)");
    unsetenv("DM_SAVE_SYNC");
    CHECK(save_sync_after_exit(A, DM_GAME_ID) == 1, "sync off: nothing to do");

    printf("(8) the game's own folder chosen\n");
    char C[1200], own[1300]; snprintf(C, sizeof C, "%s/mac2/save", root); mkd(C); snprintf(own, sizeof own, "%s/%s", cloud, id);
    snprintf(p, sizeof p, "%s/game.dat", C);
    CHECK(session(C, own, NULL, NULL) == 0 && same(p, "v8"), "<folder>/<title ID> itself as the folder: the save in it comes in");
    snprintf(q, sizeof q, "%s/%s", own, id); CHECK(!exists(q), "and no second <title ID> folder is made inside it");

    printf("(9) old backups go\n");
    snprintf(q, sizeof q, "%s-backups", C);
    for (int i = 0; i < 12; i++) { snprintf(p, sizeof p, "%s/2001-01-01 00.00.%02d local/save", q, i); mkd(p); }
    snprintf(p, sizeof p, "%s/2001-01-01 00.00.00 cloud", q); mkd(p); snprintf(p, sizeof p, "%s/my copy", q); mkd(p);
    snprintf(p, sizeof p, "%s/2001-01-01 00.00.00 local 2", q); mkd(p);
    CHECK(session(B, cloud, "game.dat", "v9") == 0 && cloud_has(cf, root, "game.dat", "v9"), "the iPhone saves again");
    snprintf(p, sizeof p, "%s/game.dat", C);
    CHECK(session(C, own, NULL, NULL) == 0 && same(p, "v9"), "the second Mac brings it in, keeping its own");
    int locals = 0; DIR *d = opendir(q); struct dirent *e;
    while (d && (e = readdir(d))) { size_t l = strlen(e->d_name); locals += l > 6 && (!strcmp(e->d_name + l - 6, " local") || strstr(e->d_name, " local ")); }
    if (d) closedir(d);
    backup_of(C, "local", back, sizeof back); snprintf(u, sizeof u, "%s/save/game.dat", back);
    CHECK(locals == 10 && same(u, "v8"), "10 local backups stay, the new one among them (%d)", locals);
    snprintf(p, sizeof p, "%s/2001-01-01 00.00.00 local 2", q); snprintf(u, sizeof u, "%s/2001-01-01 00.00.02 local", q);
    char u2[1400]; snprintf(u2, sizeof u2, "%s/2001-01-01 00.00.03 local", q);
    CHECK(!exists(p) && !exists(u) && exists(u2), "the four oldest went (00, 00 2, 01, 02)");
    snprintf(p, sizeof p, "%s/2001-01-01 00.00.00 cloud", q); snprintf(u, sizeof u, "%s/my copy", q);
    CHECK(exists(p) && exists(u), "the other side's backups and a folder of the player's stay");

    printf("(10) the Mac launcher's DM_SAVE with a slash after it\n");
    snprintf(p, sizeof p, "%s/game.dat", C); put(p, "v10");
    snprintf(u, sizeof u, "%s/", C); setenv("DM_SAVE_SYNC", own, 1);
    CHECK(save_sync_after_exit(u, DM_GAME_ID) == 1 && cloud_has(cf, root, "game.dat", "v10"), "it goes out");
    snprintf(p, sizeof p, "%s/.sync", C); CHECK(!exists(p), "and the sync state is beside the save, not inside it");
    unsetenv("DM_SAVE_SYNC");

    printf("(11) the cloud cannot be read at the start\n");
    char M[1200], N[1200], P2[1200], cloud2[1200], cf2[1400];
    snprintf(M, sizeof M, "%s/mac3/save", root); snprintf(N, sizeof N, "%s/mac4/save", root); snprintf(P2, sizeof P2, "%s/iphone2/save", root);
    snprintf(cloud2, sizeof cloud2, "%s/iCloud Drive/Other Saves", root); mkd(M); mkd(N); mkd(P2); mkd(cloud2);
    snprintf(cf2, sizeof cf2, "%s/%s/save.dmsync", cloud2, id);
    snprintf(p, sizeof p, "%s/game.dat", M); put(p, "m1");
    CHECK(session(M, cloud2, NULL, NULL) == 0 && session(P2, cloud2, "game.dat", "p1") == 0 && cloud_has(cf2, root, "game.dat", "p1"), "the Mac sends m1, the iPhone brings it in and sends p1");
    CHECK(run(M, cloud2, "game.dat", "m2", NULL, cf2, LATE) == NOT_SENT && same(p, "m2") && cloud_has(cf2, root, "game.dat", "p1") && !backups(M, "cloud"),
          "the Mac starts before the cloud's save can be read and saves m2: the iPhone's p1 stays in the cloud, nothing is moved into the Mac's backups");
    setenv("DM_SAVE_SYNC", cloud2, 1);
    CHECK(save_sync_after_exit(M, DM_GAME_ID) == 0 && cloud_has(cf2, root, "game.dat", "p1"), "nor does the Mac launcher's send after the host replace it");
    unsetenv("DM_SAVE_SYNC");
    CHECK(run(M, cloud2, "game.dat", "m3", "offline", cf2, UNREADABLE) == NOT_SENT && same(p, "m3") && cloud_has(cf2, root, "game.dat", "p1"),
          "the player plays offline with the cloud unreadable all along: m3 is played, nothing is sent");
    CHECK(run(M, cloud2, NULL, NULL, choice_of("cloud", cf2), NULL, READABLE) == 0 && same(p, "p1"), "with the cloud back, the player chooses the iPhone's: it comes in");
    backup_of(M, "local", back, sizeof back); snprintf(q, sizeof q, "%s/save/game.dat", back);
    CHECK(*back && same(q, "m3"), "and the Mac's m3 is kept (%s)", back);

    printf("(12) the first play on a new Mac\n");
    snprintf(p, sizeof p, "%s/game.dat", N);
    CHECK(run(N, cloud2, "game.dat", "n1", NULL, cf2, LATE) == NOT_SENT && same(p, "n1") && cloud_has(cf2, root, "game.dat", "p1"),
          "the new Mac starts before the cloud's save can be read and the game makes a new save: the cloud's progress stays");
    CHECK(run(N, cloud2, NULL, NULL, "ask", NULL, READABLE) == NOT_SENT && same(p, "n1") && cloud_has(cf2, root, "game.dat", "p1"), "its next start, with no choice yet, replaces nothing");
    CHECK(run(N, cloud2, NULL, NULL, choice_of("cloud", cf2), NULL, READABLE) == 0 && same(p, "p1"), "the player chooses the cloud's: it comes in");
    backup_of(N, "local", back, sizeof back); snprintf(q, sizeof q, "%s/save/game.dat", back);
    CHECK(*back && same(q, "n1"), "and the new save is kept (%s)", back);
    CHECK(session(N, cloud2, "game.dat", "n2") == 0 && cloud_has(cf2, root, "game.dat", "n2"), "from then on its saves go out");

    printf("(13) a damaged file in the cloud\n");
    {   char ln13[2048];
        put(cf2, "DMSYNC1\nnot a save");
        CHECK(!save_sync_report(N, DM_GAME_ID, cloud2, 0, ln13, sizeof ln13) && strstr(ln13, "cloud=0 ") && strstr(ln13, "conflict=0"),
              "a damaged file is not reported as a missing-save conflict (%s)", ln13);
        CHECK(run(N, cloud2, NULL, NULL, "ask", NULL, READABLE) == 0 && cloud_has(cf2, root, "game.dat", "n2"),
              "with a front end that asks, this device's unchanged save replaces the damaged file");
        backup_of(N, "cloud", back, sizeof back); snprintf(q, sizeof q, "%s/save.dmsync", back);
        CHECK(*back && same(q, "DMSYNC1\nnot a save"), "the damaged file is kept (%s)", back);
        unlink(q); rmdir(back);
        put(cf2, "DMSYNC1\nnot a save 2");
        CHECK(session(N, cloud2, "game.dat", "n3") == 0 && cloud_has(cf2, root, "game.dat", "n3"), "this device's save replaces it");
        backup_of(N, "cloud", back, sizeof back); snprintf(q, sizeof q, "%s/save.dmsync", back);
        CHECK(*back && same(q, "DMSYNC1\nnot a save 2"), "the damaged file is kept (%s)", back);
    }

    printf("(14) no front end asks: the newer one wins\n");
    char X[1200], Y[1200], cloud3[1200], cf3[1400];
    snprintf(X, sizeof X, "%s/mac5/save", root); snprintf(Y, sizeof Y, "%s/iphone3/save", root); snprintf(cloud3, sizeof cloud3, "%s/iCloud Drive/Third Saves", root);
    mkd(X); mkd(Y); mkd(cloud3); snprintf(cf3, sizeof cf3, "%s/%s/save.dmsync", cloud3, id);
    snprintf(p, sizeof p, "%s/game.dat", X);
    CHECK(session(Y, cloud3, "game.dat", "y1") == 0 && session(X, cloud3, NULL, NULL) == 0 && same(p, "y1"), "the iPhone sends y1, the Mac brings it in");
    CHECK(session(X, NULL, "game.dat", "x1") == 0, "the Mac plays with sync off");
    set_mtime(p, time(NULL) - 100);
    CHECK(session(Y, cloud3, "game.dat", "y2") == 0 && cloud_has(cf3, root, "game.dat", "y2"), "the iPhone saves and sends a newer save");
    CHECK(session(X, cloud3, NULL, NULL) == 0 && same(p, "y2"), "the Mac starts with sync: the iPhone's newer save comes in");
    backup_of(X, "local", back, sizeof back); snprintf(q, sizeof q, "%s/save/game.dat", back);
    CHECK(*back && same(q, "x1"), "and the Mac's own is kept (%s)", back);
    CHECK(session(Y, cloud3, "game.dat", "y3") == 0 && cloud_has(cf3, root, "game.dat", "y3"), "the iPhone saves again");
    CHECK(run(X, cloud3, "game.dat", "x2", NULL, cf3, LATE) == NOT_SENT && same(p, "x2") && cloud_has(cf3, root, "game.dat", "y3") && !backups(X, "cloud"),
          "the Mac starts before the cloud's save can be read and saves x2: not sent that session");
    CHECK(session(X, cloud3, NULL, NULL) == 0 && same(p, "x2") && cloud_has(cf3, root, "game.dat", "x2"), "its next start finds its save the newer one: it goes out");
    backup_of(X, "cloud", back, sizeof back); snprintf(q, sizeof q, "%s/save/game.dat", back);
    CHECK(*back && same(q, "y3"), "and the iPhone's save it replaced is kept on the Mac (%s)", back);

    printf("(15) the report a front end reads before it plays\n");
    char R1[1200], R2[1200], cloud4[1200], cf4[1400], own4[1400], line[2048];
    snprintf(R1, sizeof R1, "%s/mac6/save", root); snprintf(R2, sizeof R2, "%s/iphone4/save", root);
    snprintf(cloud4, sizeof cloud4, "%s/iCloud Drive/Fourth Saves", root); snprintf(own4, sizeof own4, "%s/%s", cloud4, id);
    mkd(R1); mkd(R2); mkd(cloud4); snprintf(cf4, sizeof cf4, "%s/%s/save.dmsync", cloud4, id);
#define REPORT(save, folder) (!save_sync_report(save, DM_GAME_ID, folder, 0, line, sizeof line) && !strchr(line, '\n'))
    CHECK(!strcmp(dm_save_title(), DM_GAME_ID), "the title the iPhone launcher names the cloud file with is the profile's");
    CHECK(REPORT(R1, cloud4) && strstr(line, "sync=1 cloud=0 ") && strstr(line, "local=- ") && strstr(line, "conflict=0"),
          "no save here, none in the cloud: no conflict, nothing to replace (%s)", line);
    snprintf(p, sizeof p, "%s/game.dat", R1); put(p, "r1-with-progress");
    CHECK(REPORT(R1, cloud4) && strstr(line, "files=1 size=16 ") && strstr(line, "chash=- ") && strstr(line, "conflict=0"),
          "a save here, the cloud's empty: no conflict, the mount sends it (%s)", line);
    CHECK(session(R1, cloud4, NULL, NULL) == 0 && cloud_has(cf4, root, "game.dat", "r1-with-progress"), "the session sends it");
    CHECK(REPORT(R1, cloud4) && strstr(line, "cloud=1 ") && strstr(line, "csize=16 ") && strstr(line, "sametitle=1 ") && strstr(line, "conflict=0") && !strstr(line, "cdevice=- "),
          "the same save here and in the cloud: no conflict, the cloud's device named (%s)", line);
    CHECK(REPORT(R1, own4) && strstr(line, "cloud=1 ") && strstr(line, "csize=16 "),
          "the game's own folder chosen as the folder: the same file as the mount's (%s)", line);
    snprintf(p, sizeof p, "%s/game.dat", R2);
    CHECK(REPORT(R2, cloud4) && strstr(line, "local=- ") && strstr(line, "cfiles=1 ") && strstr(line, "conflict=0"),
          "the cloud has one and this device none: it comes in, no conflict (%s)", line);
    CHECK(session(R2, cloud4, NULL, NULL) == 0 && same(p, "r1-with-progress"), "and the session brings it in");
    CHECK(session(R2, NULL, "game.dat", "r2") == 0 && same(p, "r2"), "the iPhone plays with sync off and saves r2");
    set_mtime(p, time(NULL) - 100);
    CHECK(session(R1, cloud4, "game.dat", "r3-more-progress") == 0 && cloud_has(cf4, root, "game.dat", "r3-more-progress"), "then the Mac saves more and sends it");
    snprintf(p, sizeof p, "%s/game.dat", R2); put(p, "new"); set_mtime(p, time(NULL) + 45);
    // the cloud's save was sent from this machine, so its device is this machine's model, as save_sync.c names it: a GitHub runner, a virtual machine, is "VirtualMac2,1"
    char model[64] = "", want[96]; size_t mlen = sizeof model;
    if (sysctlbyname("hw.model", model, &mlen, NULL, 0) || !model[0]) snprintf(model, sizeof model, "unknown");
    snprintf(want, sizeof want, "cdevice=%s ", model);
    CHECK(REPORT(R2, cloud4) && strstr(line, "conflict=1") && strstr(line, "cfiles=1 ") && strstr(line, want),
          "both changed: the report says conflict, with the cloud's device (%s)", line);
    CHECK(strstr(line, " pull=0") != NULL, "no pull pending (%s)", line);
    {   // a pull that did not finish: what is in place may be half the cloud's, so no comparison is offered; the start's mount puts this device's save back first
        char J[1300]; snprintf(J, sizeof J, "%s.pull", R2); put(J, "kept\t\nfolder\t0\t1\t0\t0\nfolder\t1\t0\t0\t0\n");
        CHECK(REPORT(R2, cloud4) && strstr(line, " pull=1") && strstr(line, "conflict=0"), "a pending pull: said so, and no conflict offered (%s)", line);
        unlink(J);
    }
    {   // the incident of the PR: the new game's save is the newer by date, so the player needs the sizes to tell them apart
        char *cs = strstr(line, "csaved="), *sv = strstr(line, " saved=");
        CHECK(cs && sv && atoll(sv + 7) > atoll(cs + 7) && strstr(line, " size=3 ") && strstr(line, "csize=16 "),
              "this device's date is the newer, its save the smaller (%s)", line);
    }
    chmod(cf4, 0);
    CHECK(REPORT(R2, cloud4) && strstr(line, "cloud=-1 ") && strstr(line, "chash=- ") && strstr(line, "conflict=0"),
          "the cloud cannot be read: said so, and no conflict to offer (%s)", line);
    chmod(cf4, 0644);
    CHECK(REPORT(R2, "0") && strstr(line, "sync=0 cloud=0 ") && !strstr(line, "local=- ") && strstr(line, "conflict=0"),
          "sync off: the local save alone is reported (%s)", line);
    setenv("DM_SAVE_SYNC", cloud4, 1);
    CHECK(REPORT(R2, NULL) && strstr(line, "conflict=1"), "no folder given: DM_SAVE_SYNC's, as the mount reads it (%s)", line);
    unsetenv("DM_SAVE_SYNC");
    CHECK(REPORT(R2, NULL) && strstr(line, "sync=0 "), "and with no DM_SAVE_SYNC, sync is off (%s)", line);
    snprintf(q, sizeof q, "%s.sync-tmp", R2);
    CHECK(access(q, F_OK), "the reports leave no files behind");
#undef REPORT

    {   // (15) a pull's journal the start cannot put back (this device's folder is neither in place nor in the backups it names): the game's save mount stops rather than
        // play on, commit and send a half-replaced save; the save and the journal are left as they were. With the journal gone, the mount goes on
        char S15[1200], J[1300], A[1300]; snprintf(S15, sizeof S15, "%s/s15/save", root); mkd(S15);
        snprintf(A, sizeof A, "%s/a", S15); put(A, "mine");
        snprintf(J, sizeof J, "%s.pull", S15); put(J, "kept\t/nonexistent/backup\nfolder\t0\t2\t1\t1\nfolder\t1\t0\t0\t0\n");
        CHECK(session(S15, NULL, NULL, NULL) != 0, "(15) the save mount stops");
        CHECK(same(A, "mine") && exists(J), "(15) the save and the journal are left as they were");
        unlink(J);
        CHECK(session(S15, NULL, NULL, NULL) == 0 && same(A, "mine"), "(15) with no journal the mount goes on");
    }

    printf("(16) a save folder that cannot be read is not an empty one\n");
    {   char U[1200], UD[1300], cloud5[1200], cf5[1400], into[1300];
        snprintf(U, sizeof U, "%s/mac7/save", root); snprintf(UD, sizeof UD, "%s_device", U);
        snprintf(cloud5, sizeof cloud5, "%s/iCloud Drive/Fifth Saves", root); snprintf(cf5, sizeof cf5, "%s/%s/save.dmsync", cloud5, id);
        mkd(U); mkd(UD); mkd(cloud5);
        snprintf(p, sizeof p, "%s/game.dat", U); put(p, "a1"); snprintf(q, sizeof q, "%s/dev.dat", UD); put(q, "d1");
        static int peek;   // the cloud's device save, unpacked into a folder of its own
#define CLOUD_DEVICE(value) (snprintf(into, sizeof into, "%s/peek16-%d", root, peek++), mkd(into), !ss_read(cf5, into, &m) && \
                             (snprintf(q, sizeof q, "%s/save_device/dev.dat", into), same(q, value)))
        CHECK(session(U, cloud5, NULL, NULL) == 0 && cloud_has(cf5, root, "game.dat", "a1") && CLOUD_DEVICE("d1"), "the Mac sends both saves, the game's and the device's");
        put(p, "a2"); chmod(UD, 0);
        setenv("DM_SAVE_SYNC", cloud5, 1);
        CHECK(save_sync_after_exit(U, DM_GAME_ID) == 0, "the game's save changed and the device save cannot be read: the launcher does not send");
        CHECK(cloud_has(cf5, root, "game.dat", "a1") && CLOUD_DEVICE("d1"), "the cloud keeps both, the device save included");
        CHECK(session(U, cloud5, NULL, NULL) == 0 && cloud_has(cf5, root, "game.dat", "a1") && CLOUD_DEVICE("d1"), "a start does not send either (sync is off for that session)");
        chmod(UD, 0755);
        CHECK(save_sync_after_exit(U, DM_GAME_ID) == 1 && cloud_has(cf5, root, "game.dat", "a2") && CLOUD_DEVICE("d1"), "readable again: both go out");
        unsetenv("DM_SAVE_SYNC");
#undef CLOUD_DEVICE
    }
    printf("(17) no save in the cloud, though this device synced one there\n");
    {   char W[1200], V[1200], cloud6[1200], cf6[1400], hid[1500], ln6[2048];
        snprintf(W, sizeof W, "%s/iphone5/save", root); snprintf(V, sizeof V, "%s/mac8/save", root);
        snprintf(cloud6, sizeof cloud6, "%s/iCloud Drive/Sixth Saves", root); snprintf(cf6, sizeof cf6, "%s/%s/save.dmsync", cloud6, id); snprintf(hid, sizeof hid, "%s.hidden", cf6);
        mkd(W); mkd(V); mkd(cloud6);
        snprintf(p, sizeof p, "%s/game.dat", W); put(p, "w1-old");
        CHECK(session(W, cloud6, NULL, NULL) == 0 && cloud_has(cf6, root, "game.dat", "w1-old"), "the iPhone sends w1-old");
        CHECK(session(V, cloud6, NULL, NULL) == 0 && session(V, cloud6, "game.dat", "v2-progress") == 0 && cloud_has(cf6, root, "game.dat", "v2-progress"),
              "the Mac brings it in, plays on and sends v2-progress");
        rename(cf6, hid);   // the iPhone's app reinstalled, the folder picked again: iCloud has not listed the Mac's file on the iPhone yet
        CHECK(!save_sync_report(W, DM_GAME_ID, cloud6, 0, ln6, sizeof ln6) && strstr(ln6, "cloud=0 ") && strstr(ln6, "conflict=1") && strstr(ln6, "chash=- "),
              "the report: no cloud save, and a conflict to ask about (%s)", ln6);
        CHECK(run(W, cloud6, NULL, NULL, "ask", NULL, READABLE) == 0 && same(p, "w1-old") && !exists(cf6),
              "the start with a front end that asks: plays this device's, sends nothing over the cloud it could not see");
        CHECK(run(W, cloud6, "game.dat", "w2", "offline", NULL, READABLE) == NOT_SENT && same(p, "w2") && !exists(cf6), "nor does a save during a game played without sending");
        setenv("DM_SAVE_SYNC", cloud6, 1);
        CHECK(save_sync_after_exit(W, DM_GAME_ID) == 0 && !exists(cf6), "nor the Mac-style send after the host");
        unsetenv("DM_SAVE_SYNC");
        rename(hid, cf6);   // iCloud lists it
        CHECK(cloud_has(cf6, root, "game.dat", "v2-progress"), "the Mac's progress is still the cloud's");
        CHECK(run(W, cloud6, NULL, NULL, "ask", NULL, READABLE) == NOT_SENT && same(p, "w2") && cloud_has(cf6, root, "game.dat", "v2-progress"),
              "the next start sees it: both changed, nothing replaced until the player chooses");
        CHECK(run(W, cloud6, NULL, NULL, choice_of("cloud", cf6), NULL, READABLE) == 0 && same(p, "v2-progress"), "the player chooses the cloud's: it comes in");
        rename(cf6, hid);
        put(p, "w3");
        CHECK(run(W, cloud6, NULL, NULL, "local:none", NULL, READABLE) == 0 && cloud_has(cf6, root, "game.dat", "w3"), "the player's local:none: this device's save is sent");
        unlink(hid); unlink(cf6); put(p, "w4");   // the cloud's file removed for good, and this device saves on
        CHECK(session(W, cloud6, NULL, NULL) == 0 && cloud_has(cf6, root, "game.dat", "w4"), "no front end asks: sent, as before");
    }
    printf("(18) conflict cloud backup rotates to newest 10 and keeps user folders\n");
    {   char K[1200], L[1200], cloud7[1200], cf7[1400], bdir[1300];
        snprintf(K, sizeof K, "%s/mac9/save", root); snprintf(L, sizeof L, "%s/iphone6/save", root);
        snprintf(cloud7, sizeof cloud7, "%s/iCloud Drive/Seventh Saves", root); mkd(K); mkd(L); mkd(cloud7);
        snprintf(cf7, sizeof cf7, "%s/%s/save.dmsync", cloud7, id);
        snprintf(p, sizeof p, "%s/game.dat", K); put(p, "k0");
        CHECK(session(K, cloud7, NULL, NULL) == 0 && session(L, cloud7, "game.dat", "l1") == 0, "Mac sends k0 and iPhone advances cloud to l1");
        CHECK(session(K, NULL, "game.dat", "k1") == 0, "Mac diverges offline with k1");
        snprintf(bdir, sizeof bdir, "%s-backups", K);
        for (int i = 1; i <= 10; i++) {
            char mark[32];
            snprintf(p, sizeof p, "%s/2001-01-01 00.00.%02d cloud/save", bdir, i); mkd(p);
            snprintf(p, sizeof p, "%s/2001-01-01 00.00.%02d cloud/save/mark.txt", bdir, i);
            snprintf(mark, sizeof mark, "old-%02d", i); put(p, mark);
        }
        snprintf(p, sizeof p, "%s/player-folder", bdir); mkd(p);
        snprintf(p, sizeof p, "%s/player-folder/keep.txt", bdir); put(p, "mine");
        CHECK(run(K, cloud7, NULL, NULL, choice_of("local", cf7), NULL, READABLE) == 0 && cloud_has(cf7, root, "game.dat", "k1"),
              "local choice backs up cloud save and sends k1");
        int clouds = 0; DIR *bd = opendir(bdir); struct dirent *be;
        while (bd && (be = readdir(bd))) {
            size_t l = strlen(be->d_name);
            clouds += l > 6 && (!strcmp(be->d_name + l - 6, " cloud") || strstr(be->d_name, " cloud "));
        }
        if (bd) closedir(bd);
        backup_of(K, "cloud", back, sizeof back); snprintf(q, sizeof q, "%s/save/game.dat", back);
        snprintf(p, sizeof p, "%s/2001-01-01 00.00.01 cloud", bdir);
        snprintf(u, sizeof u, "%s/2001-01-01 00.00.02 cloud/save/mark.txt", bdir);
        char userf[1400]; snprintf(userf, sizeof userf, "%s/player-folder/keep.txt", bdir);
        CHECK(clouds == 10 && *back && same(q, "l1") && !exists(p) && same(u, "old-02") && same(userf, "mine"),
              "exactly 10 completed cloud backups stay with l1 published, oldest pruned and user folder intact (%d)", clouds);
    }
    printf("(19) a copy of the save at each start\n");
    {
        char S[1200], E[1200], W[1200], O[1200], cloud8[1200];
        snprintf(S, sizeof S, "%s/start-a/save", root); snprintf(E, sizeof E, "%s/start-empty/save", root);
        snprintf(W, sizeof W, "%s/start-ro/save", root); snprintf(O, sizeof O, "%s/start-off/save", root);
        snprintf(cloud8, sizeof cloud8, "%s/iCloud Drive/Eighth Saves", root);
        mkd(S); mkd(E); mkd(W); mkd(O); mkd(cloud8);
        snprintf(p, sizeof p, "%s/game.dat", S); put(p, "s1");
        snprintf(p, sizeof p, "%s_device", S); mkd(p); snprintf(p, sizeof p, "%s_device/dev.dat", S); put(p, "d1");
        CHECK(session(S, NULL, NULL, NULL) == 0 && backups(S, "start") == 1, "a start with a save and sync off makes one copy (%d)", backups(S, "start"));
        backup_of(S, "start", back, sizeof back);
        snprintf(q, sizeof q, "%s/save/game.dat", back); snprintf(u, sizeof u, "%s/save_device/dev.dat", back);
        char hf[1500]; snprintf(hf, sizeof hf, "%s/.hash", back);
        CHECK(*back && same(q, "s1") && same(u, "d1") && exists(hf), "it holds the save, device save, and cached .hash (%s)", back);
        CHECK(session(S, NULL, "game.dat", "s2") == 0 && backups(S, "start") == 1, "the next start, save unchanged: no copy, though the game then saves (%d)", backups(S, "start"));
        CHECK(session(S, NULL, NULL, NULL) == 0 && backups(S, "start") == 2, "the start after it, save changed: one more (%d)", backups(S, "start"));
        backup_of(S, "start", back, sizeof back); snprintf(q, sizeof q, "%s/save/game.dat", back);
        CHECK(*back && same(q, "s2"), "the newest holds what the game saved (%s)", back);
        snprintf(p, sizeof p, "%s/game.dat", S); CHECK(same(p, "s2"), "the save itself is untouched");
        CHECK(session(E, NULL, NULL, NULL) == 0, "a first start with no save");
        snprintf(q, sizeof q, "%s-backups", E); CHECK(!exists(q), "makes no copy");
        snprintf(p, sizeof p, "%s/game.dat", O); put(p, "opt-out");
        setenv("DM_SAVE_BACKUPS", "0", 1);
        CHECK(session(O, NULL, NULL, NULL) == 0, "DM_SAVE_BACKUPS=0 mounts normally");
        unsetenv("DM_SAVE_BACKUPS");
        snprintf(q, sizeof q, "%s-backups", O); CHECK(!exists(q), "DM_SAVE_BACKUPS=0 creates no backups folder");
        snprintf(q, sizeof q, "%s-backups", S);
        for (int i = 0; i < 12; i++) { snprintf(p, sizeof p, "%s/2001-01-01 00.00.%02d start/save", q, i); mkd(p); }
        snprintf(p, sizeof p, "%s/2001-01-01 00.00.00 local", q); mkd(p);
        snprintf(p, sizeof p, "%s/2001-01-01 00.00.00 cloud", q); mkd(p);
        snprintf(p, sizeof p, "%s/my copy", q); mkd(p);
        snprintf(p, sizeof p, "%s/game.dat", S); put(p, "s3");
        CHECK(session(S, NULL, NULL, NULL) == 0 && backups(S, "start") == 10, "with 14 start copies already there, a new one leaves the newest 10 (%d)", backups(S, "start"));
        snprintf(p, sizeof p, "%s/2001-01-01 00.00.04 start", q); snprintf(u, sizeof u, "%s/2001-01-01 00.00.05 start", q);
        CHECK(!exists(p) && exists(u), "the five oldest went (00 to 04)");
        snprintf(p, sizeof p, "%s/2001-01-01 00.00.00 local", q); snprintf(u, sizeof u, "%s/2001-01-01 00.00.00 cloud", q);
        char u3[1400]; snprintf(u3, sizeof u3, "%s/my copy", q);
        CHECK(exists(p) && exists(u) && exists(u3), "sync's backups and a folder of the player's stay");
        snprintf(p, sizeof p, "%s/game.dat", W); put(p, "w1");
        snprintf(q, sizeof q, "%s-backups", W); mkd(q); chmod(q, 0555);
        CHECK(session(W, NULL, "game.dat", "w2") == 0 && backups(W, "start") == 0, "an unwritable backups folder makes no copy and still lets the game mount, save and commit");
        chmod(q, 0755);
        snprintf(p, sizeof p, "%s/game.dat", W); CHECK(same(p, "w2"), "what it saved is in the save");
        snprintf(p, sizeof p, "%s/game.dat", W); put(p, "w3");
        CHECK(session(W, cloud8, NULL, NULL) == 0 && session(E, cloud8, NULL, NULL) == 0, "with sync on, one device sends w3 and another brings it in");
        snprintf(p, sizeof p, "%s/game.dat", E);
        backup_of(E, "start", back, sizeof back); snprintf(q, sizeof q, "%s/save/game.dat", back);
        CHECK(same(p, "w3") && *back && same(q, "w3") && !backups(E, "local"), "the start copy is the save the game starts from, the cloud's (%s)", back);
        snprintf(p, sizeof p, "%s/game.dat", W); put(p, "w4"); snprintf(p, sizeof p, "%s/game.dat", E); put(p, "e1"); set_mtime(p, time(NULL) - 100);
        CHECK(session(W, cloud8, NULL, NULL) == 0 && session(E, cloud8, NULL, NULL) == 0, "both change; the newer, from the other device, comes in");
        backup_of(E, "local", back, sizeof back); snprintf(q, sizeof q, "%s/save/game.dat", back);
        snprintf(u, sizeof u, "%s/game.dat", E);
        CHECK(same(u, "w4") && *back && same(q, "e1"), "sync still keeps this device's own save as a local backup (%s)", back);
        backup_of(E, "start", back, sizeof back); snprintf(q, sizeof q, "%s/save/game.dat", back);
        CHECK(*back && same(q, "w4") && backups(E, "start") == 2, "and the start copy holds the save it brought in (%d)", backups(E, "start"));
        {   // a pull cut short after its swap: the next start puts this device's save back, then copies it
            char P19[1200], K19[1300], KS19[1400], J19[1300]; struct stat st19;
            snprintf(P19, sizeof P19, "%s/start-pull/save", root); mkd(P19); snprintf(p, sizeof p, "%s/game.dat", P19); put(p, "cloud");
            snprintf(K19, sizeof K19, "%s-backups/2001-01-01 00.00.00 local", P19); snprintf(KS19, sizeof KS19, "%s/save", K19); mkd(KS19);
            snprintf(p, sizeof p, "%s/game.dat", KS19); put(p, "mine"); stat(KS19, &st19);
            snprintf(J19, sizeof J19, "%s.pull", P19); FILE *jf = fopen(J19, "w");
            fprintf(jf, "kept\t%s\nfolder\t0\t2\t%llu\t%llu\nfolder\t1\t0\t0\t0\n", K19, (unsigned long long)st19.st_dev, (unsigned long long)st19.st_ino); fclose(jf);
            CHECK(session(P19, NULL, NULL, NULL) == 0 && !exists(J19), "a pull cut short: the next start puts this device's save back and mounts it");
            snprintf(p, sizeof p, "%s/game.dat", P19); backup_of(P19, "start", back, sizeof back); snprintf(q, sizeof q, "%s/save/game.dat", back);
            CHECK(same(p, "mine") && *back && same(q, "mine") && backups(P19, "start") == 1, "the start copy is this device's save put back, not the cloud's half (%s)", back);
        }
        {   // a save the mount refuses is not copied; once the journal is gone, the mount goes on and copies
            char P20[1200], J20[1300]; snprintf(P20, sizeof P20, "%s/start-refuse/save", root); mkd(P20); snprintf(p, sizeof p, "%s/game.dat", P20); put(p, "half");
            snprintf(J20, sizeof J20, "%s.pull", P20); put(J20, "kept\t/nonexistent/backup\nfolder\t0\t2\t1\t1\nfolder\t1\t0\t0\t0\n");
            CHECK(session(P20, NULL, NULL, NULL) != 0 && backups(P20, "start") == 0, "a journal that cannot be put back: the mount stops, and no start copy (%d)", backups(P20, "start"));
            put(J20, "not a journal\n");
            CHECK(session(P20, NULL, NULL, NULL) != 0 && backups(P20, "start") == 0, "a journal that cannot be read: the same (%d)", backups(P20, "start"));
            unlink(J20);
            CHECK(session(P20, NULL, NULL, NULL) == 0 && backups(P20, "start") == 1, "with no journal the mount goes on and copies the save (%d)", backups(P20, "start"));
        }
    }
    printf("(20) save capacity records round-trip, capacity-only change, backups, and older-bundle restore\n");
    {
        char croot[1200], nacp_path[1300], src[1300], dst1[1300], dst2[1300], dst3[1300], devonly[1300], bnd[1300];
        snprintf(croot, sizeof croot, "%s/cap", root); mkd(croot);
        snprintf(nacp_path, sizeof nacp_path, "%s/control.nacp", croot);
        static uint8_t cnacp[0x4000]; memset(cnacp, 0, sizeof cnacp);
        const int64_t acc_base[2] = {62 << 20, 1 << 20}, dev_base[2] = {32 << 20, 2 << 20};
        const int64_t acc_max[2] = {128 << 20, 8 << 20}, dev_max[2] = {64 << 20, 4 << 20};
        memcpy(cnacp + 0x3080, acc_base, sizeof acc_base); memcpy(cnacp + 0x3090, dev_base, sizeof dev_base);
        memcpy(cnacp + 0x3148, acc_max, sizeof acc_max);   memcpy(cnacp + 0x3158, dev_max, sizeof dev_max);
        FILE *nf = fopen(nacp_path, "wb"); fwrite(cnacp, 1, sizeof cnacp, nf); fclose(nf);
        setenv("DM_CONTROL_NACP", nacp_path, 1);
        const uint64_t uid1[2] = {DM_UID0, DM_UID1};
        int64_t sz = -1, jrn = -1;

        // Account extension (96/4) + progress file round-trips through ss_pack/ss_read without inventing a device record
        snprintf(src, sizeof src, "%s/src/save", croot); mkd(src);
        snprintf(p, sizeof p, "%s/progress.dat", src); put(p, "slot1"); set_mtime(p, 1000000100);
        setenv("DM_SAVE", src, 1);
        ss_meta m0, m1, mdev;
        CHECK(!ss_scan(src, &m0) && m0.files == 1, "cap: initial scan before extend");
        CHECK(h_fs_ExtendSave(uid1, 96 << 20, 4 << 20) == RES_OK, "cap: extend account save to 96/4 MiB");
        CHECK(!ss_scan(src, &m1) && m1.files == 1 && m1.bytes == m0.bytes && strcmp(m1.hash, m0.hash) != 0,
              "cap: account capacity-only change alters hash without changing file count or bytes");
        snprintf(bnd, sizeof bnd, "%s/acc.dmsync", croot);
        CHECK(!ss_pack(src, DM_GAME_ID, bnd, &m2) && !strcmp(m2.hash, m1.hash), "cap: pack account-extended save");
        snprintf(dst1, sizeof dst1, "%s/dst1", croot); mkd(dst1);
        CHECK(!ss_read(bnd, dst1, &m2) && !strcmp(m2.hash, m1.hash) && m2.files == 1, "cap: restore into fresh directory");
        snprintf(p, sizeof p, "%s/save", dst1); setenv("DM_SAVE", p, 1);
        snprintf(q, sizeof q, "%s/save/progress.dat", dst1);
        CHECK(same(q, "slot1") && h_fs_SaveSize(&sz, &jrn, uid1) == RES_OK && sz == (96 << 20) && jrn == (4 << 20),
              "cap: restored account save keeps progress file and 96/4 MiB capacity");
        CHECK(h_fs_DeviceSaveSize(&sz, &jrn) == RES_OK && sz == (32 << 20) && jrn == (2 << 20),
              "cap: device capacity stays at declared 32/2 MiB base when not extended");
        CHECK(!ss_scan(p, &m2) && !strcmp(m2.hash, m1.hash), "cap: restored directory scans to the same hash");

        // Independent device extension (48/4) even with no save_device/ directory (ACNH layout)
        setenv("DM_SAVE", src, 1);
        CHECK(h_fs_ExtendDeviceSave(48 << 20, 4 << 20) == RES_OK, "cap: extend device save to 48/4 MiB");
        CHECK(!ss_scan(src, &mdev) && mdev.files == 1 && strcmp(mdev.hash, m1.hash) != 0,
              "cap: device capacity-only change alters hash independently");
        snprintf(bnd, sizeof bnd, "%s/both.dmsync", croot);
        CHECK(!ss_pack(src, DM_GAME_ID, bnd, &m2) && !strcmp(m2.hash, mdev.hash), "cap: pack account+device extended save");
        snprintf(dst2, sizeof dst2, "%s/dst2", croot); mkd(dst2);
        CHECK(!ss_read(bnd, dst2, &m2) && !strcmp(m2.hash, mdev.hash), "cap: restore account+device capacities");
        snprintf(p, sizeof p, "%s/save", dst2); setenv("DM_SAVE", p, 1);
        CHECK(h_fs_SaveSize(&sz, &jrn, uid1) == RES_OK && sz == (96 << 20) && jrn == (4 << 20) &&
              h_fs_DeviceSaveSize(&sz, &jrn) == RES_OK && sz == (48 << 20) && jrn == (4 << 20),
              "cap: account (96/4) and device (48/4) capacities round-trip independently without save_device dir");

        // Device-only extension (no account .size) round-trips without affecting account size
        snprintf(devonly, sizeof devonly, "%s/devonly/save", croot); mkd(devonly);
        snprintf(p, sizeof p, "%s/island.dat", devonly); put(p, "island");
        setenv("DM_SAVE", devonly, 1);
        CHECK(h_fs_ExtendDeviceSave(48 << 20, 4 << 20) == RES_OK, "cap: device-only extend");
        snprintf(bnd, sizeof bnd, "%s/devonly.dmsync", croot);
        CHECK(!ss_pack(devonly, DM_GAME_ID, bnd, &m2), "cap: pack device-only extended save");
        snprintf(dst3, sizeof dst3, "%s/dst3", croot); mkd(dst3);
        CHECK(!ss_read(bnd, dst3, &m2), "cap: restore device-only extended save");
        snprintf(p, sizeof p, "%s/save", dst3); setenv("DM_SAVE", p, 1);
        CHECK(h_fs_SaveSize(&sz, &jrn, uid1) == RES_OK && sz == (62 << 20) && jrn == (1 << 20) &&
              h_fs_DeviceSaveSize(&sz, &jrn) == RES_OK && sz == (48 << 20) && jrn == (4 << 20),
              "cap: device-only extension keeps account at 62/1 base and restores device 48/4");

        // Malformed type-3 bundles are refused by ss_read (0-file bundle with size entry, bad path, invalid payload)
        {
            uint8_t hdr[144] = {0}, h[24] = {3}; uint32_t ver = 1, n = 1; uint16_t pl = 9; uint64_t esz = 18;
            memcpy(hdr, "DMSYNC1\n", 8); memcpy(hdr + 8, &ver, 4); memcpy(hdr + 12, &n, 4);
            memcpy(h + 2, &pl, 2); memcpy(h + 8, &esz, 8);
            snprintf(p, sizeof p, "%s/bad-size-only.dmsync", croot);
            FILE *bf = fopen(p, "wb");
            fwrite(hdr, 1, sizeof hdr, bf); fwrite(h, 1, sizeof h, bf); fwrite("save.size", 1, 9, bf); fwrite("100663296 4194304\n", 1, 18, bf); fwrite("DMSYNCE\n", 1, 8, bf); fclose(bf);
            CHECK(ss_read(p, NULL, &m2) != 0, "cap: type-3 size record with 0 save files is refused");
            snprintf(p, sizeof p, "%s/bad-size-path.dmsync", croot);
            pl = 10; memcpy(h + 2, &pl, 2);
            bf = fopen(p, "wb");
            fwrite(hdr, 1, sizeof hdr, bf); fwrite(h, 1, sizeof h, bf); fwrite("other.size", 1, 10, bf); fwrite("100663296 4194304\n", 1, 18, bf); fwrite("DMSYNCE\n", 1, 8, bf); fclose(bf);
            CHECK(ss_read(p, NULL, &m2) != 0, "cap: type-3 entry with disallowed path is refused");
        }

        // End-to-end cloud sync + live sender wake on capacity-only extend + start backup + restoring older bundle over existing save
        char CA[1200], CB[1200], ccloud[1200], ccf[1400], older_src[1200], older_bnd[1300];
        snprintf(CA, sizeof CA, "%s/devA/save", croot); snprintf(CB, sizeof CB, "%s/devB/save", croot);
        snprintf(ccloud, sizeof ccloud, "%s/cloud", croot);
        snprintf(ccf, sizeof ccf, "%s/%s/save.dmsync", ccloud, id);
        mkd(CA); mkd(CB); mkd(ccloud);
        snprintf(p, sizeof p, "%s/progress.dat", CA); put(p, "v1");
        CHECK(session(CA, ccloud, NULL, NULL) == 0 && session(CB, ccloud, NULL, NULL) == 0, "cap sync: initial sync of unextended save");
        // Capacity-only extension during live session wakes sender and syncs to cloud without a Commit
        fflush(stdout); fflush(stderr);
        pid_t cp = fork();
        if (!cp) {
            setenv("DM_SAVE", CA, 1); setenv("DM_SAVE_SYNC", ccloud, 1); unsetenv("DM_SAVE_SYNC_CHOICE");
            int bad = h_fs_MountSave("save", NULL) != RES_OK;
            bad += h_fs_ExtendSave(uid1, 96 << 20, 4 << 20) != RES_OK;
            if (save_sync_flush(10000) != 1) bad += NOT_SENT;
            fflush(stdout); fflush(stderr); _exit(bad);
        }
        int cst = 0; waitpid(cp, &cst, 0);
        CHECK(WIFEXITED(cst) && WEXITSTATUS(cst) == 0, "cap sync: capacity-only ExtendSaveData wakes sender and flushes to cloud");
        // Device B pulls the capacity-only update and preserves it in its start backup
        CHECK(session(CB, ccloud, NULL, NULL) == 0, "cap sync: Device B pulls capacity-only update");
        setenv("DM_SAVE", CB, 1);
        CHECK(h_fs_SaveSize(&sz, &jrn, uid1) == RES_OK && sz == (96 << 20) && jrn == (4 << 20),
              "cap sync: Device B reports restored 96/4 MiB account capacity");
        backup_of(CB, "start", back, sizeof back);
        snprintf(q, sizeof q, "%s/save.size", back);
        CHECK(*back && exists(q) && backups(CB, "start") == 2, "cap sync: start backup records the updated save.size (%d copies)", backups(CB, "start"));
        // Removing .hash still deduplicates via ss_scan (which reads backup's save.size)
        snprintf(q, sizeof q, "%s/.hash", back); unlink(q);
        CHECK(session(CB, NULL, NULL, NULL) == 0 && backups(CB, "start") == 2, "cap sync: start backup deduplicates via ss_scan with save.size");
        // Extending device capacity offline triggers a new start copy on next start (capacity-only change detected)
        CHECK(h_fs_ExtendDeviceSave(48 << 20, 4 << 20) == RES_OK, "cap sync: extend device save offline on Device B");
        CHECK(session(CB, NULL, NULL, NULL) == 0 && backups(CB, "start") == 3, "cap sync: device capacity-only change creates a new start backup");
        backup_of(CB, "start", back, sizeof back);
        snprintf(p, sizeof p, "%s/save.size", back); snprintf(q, sizeof q, "%s/save_device.size", back);
        CHECK(*back && exists(p) && exists(q), "cap sync: newest start backup holds both save.size and save_device.size");

        // Restore an older bundle (no .size entries) over Device B (which has both save.size and save_device.size)
        snprintf(older_src, sizeof older_src, "%s/older/save", croot); mkd(older_src);
        snprintf(p, sizeof p, "%s/progress.dat", older_src); put(p, "older-v0");
        snprintf(older_bnd, sizeof older_bnd, "%s/older.dmsync", croot);
        ss_meta m_older;
        CHECK(!ss_pack(older_src, DM_GAME_ID, ccf, &m_older), "cap sync: plant older bundle without size records in cloud");
        CHECK(run(CB, ccloud, NULL, NULL, choice_of("cloud", ccf), NULL, READABLE) == 0,
              "cap sync: pull older bundle over existing save with prior size records");
        setenv("DM_SAVE", CB, 1);
        snprintf(p, sizeof p, "%s.size", CB); snprintf(q, sizeof q, "%s_device.size", CB);
        CHECK(!exists(p) && !exists(q) &&
              h_fs_SaveSize(&sz, &jrn, uid1) == RES_OK && sz == (62 << 20) && jrn == (1 << 20) &&
              h_fs_DeviceSaveSize(&sz, &jrn) == RES_OK && sz == (32 << 20) && jrn == (2 << 20),
              "cap sync: restoring older bundle clears destination's prior size records back to declared base");
        backup_of(CB, "local", back, sizeof back);
        snprintf(p, sizeof p, "%s/save.size", back); snprintf(q, sizeof q, "%s/save_device.size", back);
        CHECK(*back && exists(p) && exists(q), "cap sync: destination's prior size records are preserved in local backup");
        CHECK(!ss_scan(CB, &m2) && !strcmp(m2.hash, m_older.hash), "cap sync: live save hash matches restored older bundle");
        backup_of(CB, "start", back, sizeof back);
        snprintf(p, sizeof p, "%s/save.size", back); snprintf(q, sizeof q, "%s/save_device.size", back);
        CHECK(*back && !exists(p) && !exists(q), "cap sync: start backup of restored older bundle holds no stale size records");

        unsetenv("DM_SAVE"); unsetenv("DM_CONTROL_NACP");
    }
    printf("== save sync: %d checks, %d failed ==\n", checks, fails);
    if (!fails) { char c[1200]; snprintf(c, sizeof c, "rm -rf '%s'", root); system(c); }
    return fails != 0;
}
