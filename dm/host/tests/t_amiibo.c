// amiibo.h: the virtual amiibo the host's NFC device finds with DM_AMIIBO, without a game: IDs, the nfp structures, the device states and results, the stored area.
#include "amiibo.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static const uint8_t LINK[8] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x02};   // Link, Super Smash Bros. series
static const uint8_t ZELDA[8] = {0x01, 0x01, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x02};  // Zelda, Super Smash Bros. series

// What StartDetection gives dm_amiibo_detect: the amiibo DM_AMIIBO names (selected before the host's lock), or NULL
static const uint8_t *named(const char *env) { static uint8_t id[8]; return dm_amiibo_select(env, id) ? id : NULL; }
// A file with this text (the DM_AMIIBO file, a damaged area file)
static void write_file(const char *path, const char *text) { FILE *f = fopen(path, "w"); assert(f); assert(fputs(text, f) >= 0); assert(!fclose(f)); }

int main(void) {
    uint8_t id[8];
    // IDs as amiibo lists print them
    assert(dm_amiibo_parse_id("0100000000040002", id) && !memcmp(id, LINK, 8));
    assert(dm_amiibo_parse_id("  0x0100000000040002\n", id) && !memcmp(id, LINK, 8));
    assert(dm_amiibo_parse_id("01000000 00040002", id) && !memcmp(id, LINK, 8));
    assert(dm_amiibo_parse_id("01000000-00040002 Link (SSB)", id) && !memcmp(id, LINK, 8));
    assert(dm_amiibo_parse_id("01000000000400020", id) == 0);   // 17 digits
    assert(!dm_amiibo_parse_id("010000000004000", id) && !dm_amiibo_parse_id("0 100000000040002", id) && !dm_amiibo_parse_id("", id) && !dm_amiibo_parse_id(NULL, id));
    char t[17]; dm_amiibo_id_text(LINK, t); assert(!strcmp(t, "0100000000040002"));

    char dir[] = "/tmp/t_amiibo.XXXXXX"; assert(mkdtemp(dir));
    char sel[300]; snprintf(sel, sizeof sel, "%s/amiibo.txt", dir);
    // DM_AMIIBO as a file: read again at each detection; empty means no amiibo
    write_file(sel, "0100000000040002 Link\n");
    assert(dm_amiibo_select(sel, id) && !memcmp(id, LINK, 8));
    write_file(sel, "");
    assert(!dm_amiibo_select(sel, id) && !dm_amiibo_select(NULL, id) && !dm_amiibo_select("", id) && !dm_amiibo_select("/nonexistent/amiibo.txt", id));

    dm_amiibo a; memset(&a, 0, sizeof a);
    dm_nfp_tag_info ti; dm_nfp_model_info mi; dm_nfp_common_info ci; dm_nfp_register_info ri; uint8_t buf[0x100]; uint64_t n;
    // No amiibo: the device searches and every tag call answers tag not found, as the host always did
    assert(dm_amiibo_detect(&a, NULL, dir) == 0 && a.state == DM_NFP_DEV_SEARCH);
    memset(&ti, 0x5a, sizeof ti);
    assert(dm_amiibo_tag_info(&a, &ti) == DM_NFP_TAG_NOT_FOUND && ((uint8_t *)&ti)[0] == 0x5a);
    assert(dm_amiibo_mount(&a) == DM_NFP_TAG_NOT_FOUND && dm_amiibo_model_info(&a, &mi) == DM_NFP_TAG_NOT_FOUND && dm_amiibo_open_area(&a, 1) == DM_NFP_TAG_NOT_FOUND);
    assert(dm_amiibo_unmount(&a) == DM_NFP_TAG_NOT_FOUND && dm_amiibo_flush(&a) == DM_NFP_TAG_NOT_FOUND);

    // Link on the reader
    assert(dm_amiibo_detect(&a, named("0100000000040002"), dir) == 1 && a.state == DM_NFP_DEV_FOUND);
    assert(dm_amiibo_tag_info(&a, &ti) == 0 && ti.uid_len == 7 && ti.uid[0] == 0x04 && ti.protocol == 1 && ti.type == 2);
    { dm_nfp_tag_info again; dm_amiibo_tag_info(&a, &again); assert(!memcmp(&ti, &again, sizeof ti)); }   // the same tag every scan
    assert(dm_amiibo_model_info(&a, &mi) == DM_NFP_WRONG_STATE);   // found, not mounted
    assert(dm_amiibo_mount(&a) == 0 && a.state == DM_NFP_DEV_MOUNTED);
    assert(dm_amiibo_model_info(&a, &mi) == 0 && !memcmp(mi.id, LINK, 7)); for (size_t i = 0; i < sizeof mi.r; i++) assert(!mi.r[i]);
    assert(dm_amiibo_common_info(&a, &ci) == 0 && ci.area_size == 0xd8 && ci.year >= 2026 && ci.month >= 1 && ci.month <= 12 && ci.write_counter == 0);
    uint8_t mii[0x58]; memset(mii, 0x11, sizeof mii);
    assert(dm_amiibo_register_info(&a, &ri, mii, "Hero of Hyrule") == 0 && !memcmp(ri.mii, mii, sizeof mii) && !strcmp(ri.nickname, "Hero of Hy"));   // 10 characters
    assert(dm_amiibo_register_info(&a, &ri, NULL, "\xE3\x83\xAA\xE3\x83\xB3\xE3\x82\xAF\xE3\x83\xAA\xE3\x83\xB3\xE3\x82\xAF\xE3\x83\xAA\xE3\x83\xB3\xE3\x82\xAF\xE3\x83\xAA\xE3\x83\xB3\xE3\x82\xAF") == 0
           && strlen(ri.nickname) == 30);   // twelve 3-byte characters: the first 10 (30 bytes)
    assert(dm_amiibo_register_info(&a, &ri, NULL, NULL) == 0 && !strcmp(ri.nickname, "amiibo"));
    // No area yet: open fails, create makes it and writes it at once
    assert(dm_amiibo_open_area(&a, 0x1019c800) == DM_NFP_AREA_NOT_CREATED);
    uint8_t init[0x20]; for (int i = 0; i < 0x20; i++) init[i] = (uint8_t)i;
    assert(dm_amiibo_create_area(&a, 0x1019c800, init, 0x100, 0) == DM_NFP_WRONG_AREA_SIZE);
    assert(dm_amiibo_create_area(&a, 0x1019c800, init, sizeof init, 0) == 0);
    char file[300]; snprintf(file, sizeof file, "%s/0100000000040002.area", dir); struct stat st; assert(!stat(file, &st) && st.st_size == 16 + 0xd8);
    assert(dm_amiibo_create_area(&a, 0x1019c800, init, sizeof init, 0) == DM_NFP_AREA_EXISTS);
    assert(dm_amiibo_open_area(&a, 0x12345678) == DM_NFP_WRONG_AREA_ID);   // another game's
    assert(dm_amiibo_get_area(&a, buf, &n, sizeof buf) == DM_NFP_AREA_NOT_CREATED && n == 0);   // not opened
    assert(dm_amiibo_open_area(&a, 0x1019c800) == 0);
    memset(buf, 0xee, sizeof buf);
    assert(dm_amiibo_get_area(&a, buf, &n, sizeof buf) == 0 && n == 0xd8 && !memcmp(buf, init, sizeof init) && buf[0x20] == 0 && buf[0xd7] == 0 && buf[0xd8] == 0xee);
    assert(dm_amiibo_get_area(&a, buf, &n, 4) == 0 && n == 4);
    // Set, then unmount without Flush: dropped, as only Flush writes the tag
    uint8_t v2[0xd8]; memset(v2, 0x42, sizeof v2);
    assert(dm_amiibo_set_area(&a, v2, sizeof v2 + 1) == DM_NFP_WRONG_AREA_SIZE);
    assert(dm_amiibo_set_area(&a, v2, sizeof v2) == 0);
    assert(dm_amiibo_unmount(&a) == 0 && a.state == DM_NFP_DEV_FOUND && !a.area_open);
    assert(dm_amiibo_mount(&a) == 0 && dm_amiibo_open_area(&a, 0x1019c800) == 0 && dm_amiibo_get_area(&a, buf, &n, sizeof buf) == 0 && !memcmp(buf, init, sizeof init));
    // Set and Flush: kept, the write counter goes up, and it is there at the next scan
    assert(dm_amiibo_set_area(&a, v2, sizeof v2) == 0 && dm_amiibo_flush(&a) == 0);
    assert(dm_amiibo_common_info(&a, &ci) == 0 && ci.write_counter == 2);   // create, then this flush
    dm_amiibo_stop(&a); assert(a.state == DM_NFP_DEV_INIT && !a.area_open);
    dm_amiibo b; memset(&b, 0, sizeof b);
    assert(dm_amiibo_detect(&b, named("0x0100000000040002"), dir) == 1 && dm_amiibo_mount(&b) == 0 && dm_amiibo_open_area(&b, 0x1019c800) == 0);
    assert(dm_amiibo_get_area(&b, buf, &n, sizeof buf) == 0 && !memcmp(buf, v2, sizeof v2) && b.write_counter == 2);
    // Recreate replaces another game's area
    assert(dm_amiibo_create_area(&b, 0x0005, NULL, 0, 1) == 0 && dm_amiibo_open_area(&b, 0x0005) == 0 && dm_amiibo_open_area(&b, 0x1019c800) == DM_NFP_WRONG_AREA_ID);
    // Another amiibo has its own file and no area
    assert(dm_amiibo_detect(&b, ZELDA, dir) == 1 && dm_amiibo_mount(&b) == 0 && dm_amiibo_open_area(&b, 0x0005) == DM_NFP_AREA_NOT_CREATED);
    // A damaged file is an amiibo without an area
    write_file(file, "garbage");
    assert(dm_amiibo_detect(&b, LINK, dir) == 1 && dm_amiibo_mount(&b) == 0 && dm_amiibo_open_area(&b, 0x0005) == DM_NFP_AREA_NOT_CREATED);
    // No folder: the area lasts until the game ends
    assert(dm_amiibo_detect(&b, LINK, NULL) == 1 && dm_amiibo_mount(&b) == 0 && dm_amiibo_create_area(&b, 7, init, 4, 0) == 0 && dm_amiibo_flush(&b) == 0);
    // Taking the amiibo away (an emptied DM_AMIIBO file) searches again
    assert(dm_amiibo_detect(&b, named(sel), dir) == 0 && b.state == DM_NFP_DEV_SEARCH && dm_amiibo_mount(&b) == DM_NFP_TAG_NOT_FOUND);

    // The folder holds only the selection file and Link's area (the other amiibo made none, and a failed write leaves no .tmp): rmdir checks that
    char tmp[300]; snprintf(tmp, sizeof tmp, "%s.tmp", file);
    assert(!unlink(sel) && !unlink(file) && (!unlink(tmp) || errno == ENOENT) && !rmdir(dir));
    puts("t_amiibo: ok");
    return 0;
}
