// Mods folder unit test (hle_mods.c): discovery and order.txt, the RomFS overlay list, the cheat folders, .pchtxt and IPS32 parsing with the
// NSO header shift, build ID matching, and the refusals that keep the rewriter's work intact. Expectations are written by hand.
#include "../hle_mods.c"
#include <unistd.h>

static char live_overlay[4096];
void dm_fs_overlay_set(const char *list) { strlcpy(live_overlay, list, sizeof live_overlay); }
static module_t mods[2]; module_t *g_mod = mods; int g_nmod = 2;
static int fails;
#define CHECK(c, what) do { if (!(c)) { fails++; printf("FAIL %s (line %d)\n", what, __LINE__); } } while (0)

static void put(const char *path, const void *data, size_t n) {
    char cmd[2200]; snprintf(cmd, sizeof cmd, "mkdir -p \"$(dirname '%s')\"", path);
    if (system(cmd)) exit(2);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, n, f) != n) exit(2);
    fclose(f);
}
static void puts_file(const char *path, const char *text) { put(path, text, strlen(text)); }
static uint32_t word_at(const module_t *m, uint64_t off) { uint32_t w; memcpy(&w, m->base + off, 4); return w; }

#define BUILD "0123456789abcdef0123456789abcdef"
#define NOP 0xD503201Fu

int main(void) {
    CHECK(!needs_rewriter(NOP), "nop");
    CHECK(!needs_rewriter(0xD65F03C0), "ret");
    CHECK(!needs_rewriter(0x52800240), "mov w0, #18");
    CHECK(!needs_rewriter(0x14000012), "b +0x48");
    CHECK(!needs_rewriter(0x91004820), "add x0, x1, #18");
    CHECK(!needs_rewriter(0xFD400012), "ldr d18, [x0]");
    CHECK(!needs_rewriter(0xD53B4200), "mrs x0, nzcv");
    CHECK(!needs_rewriter(0xF9800012), "prfm with prefetch operation 18");
    CHECK(needs_rewriter(0xAA0003F2) != NULL, "mov x18, x0");
    CHECK(needs_rewriter(0xF9400012) != NULL, "ldr x18, [x0]");
    CHECK(needs_rewriter(0xF9000240) != NULL, "str x0, [x18]");
    CHECK(needs_rewriter(0xD61F0240) != NULL, "br x18");
    CHECK(needs_rewriter(0x8B120020) != NULL, "add x0, x1, x18");
    CHECK(needs_rewriter(0xA9004812) != NULL, "stp x18, x18, [x0]");
    CHECK(needs_rewriter(0xD4000001) != NULL, "svc 0");
    CHECK(needs_rewriter(0xD53BD040) != NULL, "mrs x0, tpidr_el0");
    CHECK(needs_rewriter(0xD53BD060) != NULL, "mrs x0, tpidrro_el0");
    CHECK(needs_rewriter(0xD53BE020) != NULL, "mrs x0, cntpct_el0");
    CHECK(needs_rewriter(0x085F7C20) != NULL, "ldxrb w0, [x1]");
    CHECK(needs_rewriter(0xCB2063FF) != NULL, "sub sp, sp, x0");
    CHECK(needs_rewriter(0xD10263FF) != NULL, "sub sp, sp, #0x98");
    CHECK(needs_rewriter(0x910263FF) != NULL, "add sp, sp, #0x98");
    CHECK(!needs_rewriter(0xD10303FF), "sub sp, sp, #0xc0");
    CHECK(!needs_rewriter(0xD14007FF), "sub sp, sp, #1, lsl #12");
    CHECK(!needs_rewriter(0xD1026020), "sub x0, x1, #0x98");

    CHECK(bid_matches("0123456789ABCDEF0123456789ABCDEF", BUILD), "build ID, upper case");
    CHECK(bid_matches(BUILD "00000000", BUILD), "build ID with the zero tail of a 20-byte one");
    CHECK(bid_matches("0123456789abcdef", "0123456789abcdef0000000000000000"), "build ID with its zeros cut");
    CHECK(!bid_matches("0123456789abcdef", BUILD), "a cut build ID whose tail is not zero");
    CHECK(!bid_matches("1123456789abcdef0123456789abcdef", BUILD), "another build");

    const char *tmp = getenv("TMPDIR"); char base[1024], p[1536];
    snprintf(base, sizeof base, "%s/t_mods.%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    char tid[17]; snprintf(tid, sizeof tid, "%016llX", (unsigned long long)DM_GAME_PROGRAM_ID);

    snprintf(p, sizeof p, "%s/mods/Alpha/romfs/a.bin", base); puts_file(p, "a");
    snprintf(p, sizeof p, "%s/mods/Alpha/cheats/x.txt", base); puts_file(p, "");
    snprintf(p, sizeof p, "%s/mods/Beta/atmosphere/contents/%s/romfs/b.bin", base, tid); puts_file(p, "b");
    snprintf(p, sizeof p, "%s/mods/Gamma/romfs/c.bin", base); puts_file(p, "c");
    snprintf(p, sizeof p, "%s/mods/Off/romfs/d.bin", base); puts_file(p, "d");
    snprintf(p, sizeof p, "%s/mods/Pack/Blue/romfs/t.bin", base); puts_file(p, "1");
    snprintf(p, sizeof p, "%s/mods/Pack/Red/romfs/t.bin", base); puts_file(p, "2");
    snprintf(p, sizeof p, "%s/mods/Bundle/Hud/romfs/UI/hud.bin", base); puts_file(p, "5");
    snprintf(p, sizeof p, "%s/mods/Bundle/Music/romfs/Sound/a.bin", base); puts_file(p, "6");
    snprintf(p, sizeof p, "%s/mods/Bundle/Music2/romfs/Sound/a.bin", base); puts_file(p, "7");
    snprintf(p, sizeof p, "%s/mods/Tunics/Green/romfs/t.bin", base); puts_file(p, "3");
    snprintf(p, sizeof p, "%s/mods/Tunics/Pink/romfs/t.bin", base); puts_file(p, "4");
    snprintf(p, sizeof p, "%s/mods/order.txt", base); puts_file(p, "Gamma\n# Off\nMissing\nTunics/Pink\nTunics/Green\n");

    snprintf(p, sizeof p, "%s/mods/Alpha/exefs/patch.pchtxt", base);
    puts_file(p, "@nsobid-0123456789ABCDEF0123456789ABCDEF00000000\n@flag offset_shift 0x100\n// a comment\n@enabled\n"
                 "00000010 1F2003D5 // nop\n00004000 \"hi\\0\"\n@disabled\n00000020 1F2003D5\n@enabled\n00000030 C0035FD6\n@stop\n00000040 1F2003D5\n");
    snprintf(p, sizeof p, "%s/mods/Beta/atmosphere/exefs_patches/set/" BUILD ".ips", base);
    static const uint8_t ips[] = {'I','P','S','3','2', 0,0,0x01,0x10, 0,4, 0xC0,0x03,0x5F,0xD6, 0,0,0x41,0x08, 0,0, 0,3, 0xAB, 'E','E','O','F'};
    put(p, ips, sizeof ips);
    snprintf(p, sizeof p, "%s/mods/Gamma/exefs/x18.pchtxt", base);
    puts_file(p, "@nsobid-" BUILD "\n@flag offset_shift 0x100\n00000050 1F2003D5\n00000054 F20300AA\n");
    snprintf(p, sizeof p, "%s/mods/Gamma/exefs/moved.pchtxt", base);
    puts_file(p, "@nsobid-" BUILD "\n00000180 1F2003D5\n");
    snprintf(p, sizeof p, "%s/mods/Gamma/exefs/other.pchtxt", base);
    puts_file(p, "@nsobid-FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\n00000100 1F2003D5\n");
    snprintf(p, sizeof p, "%s/dm/main.changed.tsv", base);
    puts_file(p, "# BUILD_ID " BUILD "\n80\t8\n");

    char check[1536]; snprintf(check, sizeof check, "%s/check.tsv", base); setenv("DM_MODS_CHECK", check, 1);
    snprintf(p, sizeof p, "%s/mods", base); setenv("DM_MODS", p, 1);
    setenv("DM_ROMFS_OVERLAY", "/explicit", 1);
    dm_mods_init();

    CHECK(g_nmods == 7 && !strcmp(g_mods[0].name, "Gamma") && !strcmp(g_mods[1].name, "Tunics/Pink") && !strcmp(g_mods[2].name, "Alpha") && !strcmp(g_mods[3].name, "Beta")
          && !strcmp(g_mods[4].name, "Bundle/Hud") && !strcmp(g_mods[5].name, "Bundle/Music") && !strcmp(g_mods[6].name, "Pack/Blue"),
          "order: order.txt first, then by name, Off left out; a pack's mods that share no file are all on, of those that replace the same file one");
    CHECK(dm_mods_count() == 11, "every mod and variant is listed");
    for (int i = 0; i < dm_mods_count(); i++) {
        if (!strcmp(dm_mods_name(i), "Bundle/Hud")) CHECK(!dm_mods_exclusive(i), "a pack's mod with files of its own is no alternative");
        if (!strcmp(dm_mods_name(i), "Bundle/Music2")) CHECK(dm_mods_exclusive(i) && !dm_mods_on(i), "two of a pack with the same file are alternatives");
    }
    char want[4096]; snprintf(want, sizeof want, "/explicit:%s/mods/Gamma/romfs:%s/mods/Tunics/Pink/romfs:%s/mods/Alpha/romfs:%s/mods/Beta/atmosphere/contents/%s/romfs:%s/mods/Bundle/Hud/romfs:%s/mods/Bundle/Music/romfs:%s/mods/Pack/Blue/romfs", base, base, base, base, tid, base, base, base);
    CHECK(!strcmp(getenv("DM_ROMFS_OVERLAY"), want), "overlay list: the explicit folder, then the mods in order");
    snprintf(want, sizeof want, "%s/mods/Alpha/cheats", base);
    const char *owner = "";
    CHECK(dm_mods_cheat_dir(0, &owner) && !strcmp(dm_mods_cheat_dir(0, NULL), want) && !dm_mods_cheat_dir(1, NULL) && !strcmp(owner, "Alpha"), "cheat folders, each with its mod's name");
    CHECK(g_npfiles == 5, "five patch files found");

    module_t *m = &mods[0];
    strcpy(m->name, "main"); strcpy(m->build_id, BUILD);
    m->text_end = 0x4000; m->key_page_off = 0x8000;
    m->base = malloc(0x8000);
    for (uint64_t a = 0; a < 0x4000; a += 4) { uint32_t w = 0xAA0103E0; memcpy(m->base + a, &w, 4); }
    memset(m->base + 0x4000, 0x55, 0x4000);
    strcpy(mods[1].name, "sdk"); strcpy(mods[1].build_id, "99999999999999999999999999999999");
    snprintf(p, sizeof p, "%s/dm", base);
    dm_mods_patch_module(m, p);
    dm_mods_report_unmatched();

    CHECK(word_at(m, 0x30) == 0xD65F03C0, "pchtxt: a patch after @enabled");
    CHECK(word_at(m, 0x20) == 0xAA0103E0, "pchtxt: a patch after @disabled is left out");
    CHECK(word_at(m, 0x40) == 0xAA0103E0, "pchtxt: a patch after @stop is left out");
    CHECK(!memcmp(m->base + 0x4000, "hi\0\x55", 4), "pchtxt: a string with an escaped terminator");
    CHECK(word_at(m, 0x10) == NOP, "the higher mod's patch wins where two overlap (Alpha's nop over Beta's ret)");
    CHECK(m->base[0x4008] == 0xAB && m->base[0x400A] == 0xAB && m->base[0x400B] == 0x55, "IPS32: a run-length record");
    CHECK(word_at(m, 0x50) == 0xAA0103E0 && word_at(m, 0x54) == 0xAA0103E0, "a file with an x18 instruction is refused whole");
    CHECK(word_at(m, 0x80) == 0xAA0103E0, "a patch over a word the rewriter moved is refused");

    fflush(g_check);
    size_t cn; char *ct = (char *)read_file(check, &cn);
    CHECK(ct && strstr(ct, "\trefused\tGamma\t") && strstr(ct, "x18.pchtxt\t") && strstr(ct, "\nrec\t50\t1f2003d5\nrec\t54\tf20300aa\n"), "DM_MODS_CHECK: a refused file with its patches as image offsets");
    CHECK(ct && strstr(ct, "\tapplied\tAlpha\t") && strstr(ct, "\nrec\t4000\t686900\n"), "DM_MODS_CHECK: an applied file with its patches");
    CHECK(ct && strstr(ct, "file\tmain\t" BUILD "\tapplied\tBeta\t") < strstr(ct, "\tapplied\tAlpha\t"), "DM_MODS_CHECK: the files in the order they are applied, the lowest priority first");
    free(ct);

    char sha[65], q[1536], list[6000]; size_t fn; uint8_t *fb;
    snprintf(q, sizeof q, "%s/mods/Gamma/exefs/x18.pchtxt", base); fb = read_file(q, &fn); sha256_hex(fb, fn, sha); free(fb);
    char abc[65]; sha256_hex((const uint8_t *)"abc", 3, abc);
    CHECK(!strcmp(abc, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256 of \"abc\"");
    int o = snprintf(list, sizeof list, "baked\t%s\t%s\n", sha, q);
    snprintf(q, sizeof q, "%s/mods/Gamma/exefs/moved.pchtxt", base); fb = read_file(q, &fn); sha256_hex(fb, fn, sha); free(fb);
    o += snprintf(list + o, sizeof list - (size_t)o, "refused\t%s\t%s\tit patches a place where the rewriter has a cut\n", sha, q);
    snprintf(q, sizeof q, "%s/mods/Alpha/exefs/patch.pchtxt", base);
    o += snprintf(list + o, sizeof list - (size_t)o, "baked\t%064d\t%s\nbaked\t%064d\t%s/mods/Gone/exefs/gone.pchtxt\n", 0, q, 0, base);
    snprintf(p, sizeof p, "%s/dm2/main.mods.tsv", base); puts_file(p, list);
    snprintf(p, sizeof p, "%s/dm2/main.changed.tsv", base); puts_file(p, "# BUILD_ID " BUILD "\n");
    for (uint64_t a = 0; a < 0x4000; a += 4) { uint32_t w = 0xAA0103E0; memcpy(m->base + a, &w, 4); }
    snprintf(p, sizeof p, "%s/dm2", base);
    int nbaked = 0; baked_t *bl = baked_read(m, p, &nbaked);
    CHECK(bl && nbaked == 4 && !bl[0].refused && bl[1].refused && strstr(bl[1].why, "a cut") && strstr(bl[3].path, "gone.pchtxt"), "the rewritten module's list of patch files");
    free(bl);
    dm_mods_patch_module(m, p);
    CHECK(word_at(m, 0x50) == 0xAA0103E0 && word_at(m, 0x54) == 0xAA0103E0, "a file the module was rewritten with is not applied again");
    CHECK(word_at(m, 0x80) == 0xAA0103E0, "a file the rewriter refused is not applied");
    CHECK(word_at(m, 0x30) == 0xAA0103E0 && word_at(m, 0x10) == 0xD65F03C0, "a file that changed after the rewrite is not applied over it; one outside the list still is (Beta's ret)");
    fflush(g_check);
    ct = (char *)read_file(check, &cn);
    CHECK(ct && strstr(ct, "\tbaked\tGamma\t") && strstr(ct, "\trewriter-refused\tGamma\t"), "DM_MODS_CHECK on a rewritten module: what is in it and what the rewriter refused");
    free(ct);

    int green = -1, pink = -1;
    for (int i = 0; i < dm_mods_count(); i++) { if (!strcmp(dm_mods_name(i), "Tunics/Green")) green = i; if (!strcmp(dm_mods_name(i), "Tunics/Pink")) pink = i; }
    CHECK(green >= 0 && pink >= 0 && dm_mods_on(pink) && !dm_mods_on(green), "the pack's listed variants");
    dm_mods_set(green, 1);
    CHECK(dm_mods_on(green) && !dm_mods_on(pink) && !dm_mods_edited(), "turning a variant on turns its pack's other one off; a romfs-only mod needs no restart");
    snprintf(want, sizeof want, "/explicit:%s/mods/Gamma/romfs:%s/mods/Tunics/Green/romfs:%s/mods/Alpha/romfs:%s/mods/Beta/atmosphere/contents/%s/romfs:%s/mods/Bundle/Hud/romfs:%s/mods/Bundle/Music/romfs:%s/mods/Pack/Blue/romfs", base, base, base, base, tid, base, base, base);
    CHECK(!strcmp(live_overlay, want), "the running game's overlay list follows the choice, in order");
    for (int i = 0; i < dm_mods_count(); i++) if (!strcmp(dm_mods_name(i), "Alpha")) dm_mods_set(i, 0);
    CHECK(dm_mods_edited() && !strstr(live_overlay, "Alpha"), "a mod with code patches or cheats: its romfs goes at once, the rest waits for a restart");
    snprintf(p, sizeof p, "%s/mods/order.txt", base);
    size_t on; char *text = (char *)read_file(p, &on);
    CHECK(text && strstr(text, "\n# Tunics/Pink\nTunics/Green\n") && strstr(text, "\n# Pack/Red\n"), "order.txt is rewritten with the choice");
    free(text);

    char cmd[1200]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd)) fails++;
    printf(fails ? "t_mods: %d FAILED\n" : "t_mods: ok\n", fails);
    return fails != 0;
}
