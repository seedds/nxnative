// Virtual amiibo: a tag the host's NFC device finds when DM_AMIIBO names one (hle_sdk16.c's nfp take-overs call these). Plain C with stdio, so tests/t_amiibo.c checks the
// structures, the device states and the stored application area without a game, a Mac or an NFC reader.
//
// DM_AMIIBO is the amiibo ID, 16 hex digits as amiibo databases list them (Link, Super Smash Bros. series: 01000000 00040002), or the path of a file that holds one. The
// file is read again at every StartDetection, so writing another ID into it swaps the amiibo between scans, and emptying it takes the amiibo away. No amiibo dump or key
// is used: the game sees the amiibo's identity (ModelInfo), an owner and nickname (RegisterInfo) and the application area it creates, which the host keeps in
// <amiibo folder>/<ID>.area (DM_AMIIBO_DIR, else an amiibo folder beside DM_SAVE). Layouts and results as Horizon's nfp service answers them (libnx nfp.h for the
// structures; Ryujinx and yuzu for the result descriptions and the device states).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define DM_NFP_RESULT(desc) (115u | (uint32_t)(desc) << 9)   // module 115 (nfp)
#define DM_NFP_WRONG_AREA_SIZE   DM_NFP_RESULT(68)
#define DM_NFP_WRONG_STATE       DM_NFP_RESULT(73)
#define DM_NFP_TAG_NOT_FOUND     DM_NFP_RESULT(97)     // what the host answered for every tag operation before (hle_sdk16.c NFP_RES_TAG_NOT_FOUND)
#define DM_NFP_AREA_NOT_CREATED  DM_NFP_RESULT(128)    // no application area on this amiibo yet
#define DM_NFP_WRONG_AREA_ID     DM_NFP_RESULT(152)    // another game's application area
#define DM_NFP_AREA_EXISTS       DM_NFP_RESULT(168)

enum { DM_AMIIBO_AREA_SIZE = 0xd8 };   // the application area of an NTAG215 amiibo
// nfp DeviceState: 0 initialized, 1 searching, 2 tag found, 3 tag removed, 4 tag mounted
enum { DM_NFP_DEV_INIT = 0, DM_NFP_DEV_SEARCH = 1, DM_NFP_DEV_FOUND = 2, DM_NFP_DEV_REMOVED = 3, DM_NFP_DEV_MOUNTED = 4 };

typedef struct { uint8_t uid[10]; uint8_t uid_len; uint8_t r1[0x15]; uint32_t protocol, type; uint8_t r2[0x30]; } dm_nfp_tag_info;
typedef struct { uint8_t id[7]; uint8_t r[0x39]; } dm_nfp_model_info;   // the amiibo ID's first 7 bytes as the tag holds them (big-endian character and model numbers)
typedef struct { uint16_t year; uint8_t month, day; uint16_t write_counter, version; uint32_t area_size; uint8_t r[0x34]; } dm_nfp_common_info;
typedef struct { uint8_t mii[0x58]; uint16_t year; uint8_t month, day; char nickname[41]; uint8_t font_region; uint8_t r[0x7a]; } dm_nfp_register_info;
_Static_assert(sizeof(dm_nfp_tag_info) == 0x58, "nn::nfp::TagInfo");
_Static_assert(sizeof(dm_nfp_model_info) == 0x40, "nn::nfp::ModelInfo");
_Static_assert(sizeof(dm_nfp_common_info) == 0x40, "nn::nfp::CommonInfo");
_Static_assert(sizeof(dm_nfp_register_info) == 0x100, "nn::nfp::RegisterInfo");

// What the host knows of the amiibo on the device, and the device's state
typedef struct {
    int state;                          // DM_NFP_DEV_*
    int present;                        // an amiibo was found by the last StartDetection
    uint8_t id[8];
    int has_area, area_open, dirty;
    uint32_t access_id;
    uint16_t write_counter;
    uint8_t area[DM_AMIIBO_AREA_SIZE];
    char file[1024];                    // <amiibo folder>/<ID>.area, empty when there is no folder (the area then lasts until the game ends)
} dm_amiibo;

// 16 hex digits, optionally after 0x and with spaces or dashes between groups, at the start of s (leading whitespace skipped); anything may follow on the line
static inline int dm_amiibo_parse_id(const char *s, uint8_t id[8]) {
    if (!s) return 0;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    int n = 0; uint8_t b[8] = {0};
    for (; *s && n < 16; s++) {
        int v = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
        if (v < 0) { if ((*s == ' ' || *s == '-') && n && n % 2 == 0) continue; break; }
        b[n / 2] = (uint8_t)(b[n / 2] << 4 | v); n++;
    }
    if (n != 16) return 0;
    if ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F')) return 0;   // more than 16 digits: not an amiibo ID
    memcpy(id, b, 8); return 1;
}
// DM_AMIIBO: an ID, else a file whose first line holds one. Returns 1 with the ID, 0 when it names no amiibo (unset, empty, or an empty or unreadable file)
static inline int dm_amiibo_select(const char *env, uint8_t id[8]) {
    if (!env || !*env) return 0;
    if (dm_amiibo_parse_id(env, id)) return 1;
    FILE *f = fopen(env, "r");
    if (!f) return 0;
    char line[128] = ""; char *got = fgets(line, sizeof line, f); fclose(f);
    return got && dm_amiibo_parse_id(line, id);
}
static inline void dm_amiibo_id_text(const uint8_t id[8], char out[17]) { for (int i = 0; i < 8; i++) snprintf(out + 2 * i, 3, "%02x", id[i]); }

// The stored application area: "DMAB", version 1, the access ID, the write counter, the size, then the area
typedef struct { char magic[4]; uint16_t version, write_counter; uint32_t access_id, size; } dm_amiibo_file;
static inline void dm_amiibo_load(dm_amiibo *a) {
    a->has_area = 0; a->area_open = 0; a->dirty = 0; a->access_id = 0; a->write_counter = 0; memset(a->area, 0, sizeof a->area);
    FILE *f = *a->file ? fopen(a->file, "rb") : NULL;
    if (!f) return;
    dm_amiibo_file h;
    if (fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, "DMAB", 4) && h.version == 1 && h.size == DM_AMIIBO_AREA_SIZE && fread(a->area, DM_AMIIBO_AREA_SIZE, 1, f) == 1) {
        a->has_area = 1; a->access_id = h.access_id; a->write_counter = h.write_counter;
    } else memset(a->area, 0, sizeof a->area);   // a damaged file is an amiibo without an area, as a fresh one
    fclose(f);
}
// Writes the area through a temporary file renamed over the old one, so a crash mid-write keeps the previous area. Returns 0 on success (also when there is no folder)
static inline int dm_amiibo_store(dm_amiibo *a) {
    if (!*a->file) { a->dirty = 0; return 0; }
    char tmp[1040]; snprintf(tmp, sizeof tmp, "%s.tmp", a->file);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    dm_amiibo_file h = {{'D', 'M', 'A', 'B'}, 1, a->write_counter, a->access_id, DM_AMIIBO_AREA_SIZE};
    int ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(a->area, DM_AMIIBO_AREA_SIZE, 1, f) == 1;
    if (fclose(f)) ok = 0;
    if (!ok || rename(tmp, a->file)) { remove(tmp); return -1; }
    a->dirty = 0; return 0;
}

// StartDetection: the amiibo DM_AMIIBO names now (id, as dm_amiibo_select gives it; NULL: none), its area from the folder (dir; NULL or empty: none). Returns 1 when a tag is
// found, 0 when the device searches on. The caller selects the amiibo first, outside its lock: dm_amiibo_select may read a file, and nothing blocks under the host's amiibo mutex
static inline int dm_amiibo_detect(dm_amiibo *a, const uint8_t *id, const char *dir) {
    a->area_open = 0;
    a->present = id != NULL;
    if (!a->present) { a->state = DM_NFP_DEV_SEARCH; return 0; }
    memcpy(a->id, id, 8);
    char t[17]; dm_amiibo_id_text(a->id, t);
    // A folder path too long for the file name keeps the area in memory, as with no folder, rather than writing to a cut-short name
    if (!dir || !*dir || (size_t)snprintf(a->file, sizeof a->file, "%s/%s.area", dir, t) >= sizeof a->file) a->file[0] = 0;
    dm_amiibo_load(a);
    a->state = DM_NFP_DEV_FOUND;
    return 1;
}
static inline void dm_amiibo_stop(dm_amiibo *a) { a->state = DM_NFP_DEV_INIT; a->area_open = 0; }

// What a call that needs a tag gets when there is none: the answer the host always gave (tag not found), so a game without DM_AMIIBO sees no change
static inline uint32_t dm_amiibo_need_tag(const dm_amiibo *a) { return a->state == DM_NFP_DEV_FOUND || a->state == DM_NFP_DEV_MOUNTED ? 0 : DM_NFP_TAG_NOT_FOUND; }
static inline uint32_t dm_amiibo_need_mount(const dm_amiibo *a) { uint32_t r = dm_amiibo_need_tag(a); return r ? r : a->state == DM_NFP_DEV_MOUNTED ? 0 : DM_NFP_WRONG_STATE; }

static inline uint32_t dm_amiibo_tag_info(const dm_amiibo *a, dm_nfp_tag_info *o) {
    uint32_t r = dm_amiibo_need_tag(a); if (r) return r;
    memset(o, 0, sizeof *o);
    // A 7-byte NTAG215 UID (NXP's 0x04 first), made from the ID so the same amiibo always reads the same tag: games that count scans per tag see one tag per amiibo
    uint64_t h = 0xcbf29ce484222325ull; for (int i = 0; i < 8; i++) { h ^= a->id[i]; h *= 0x100000001b3ull; }
    o->uid[0] = 0x04; for (int i = 1; i < 7; i++) o->uid[i] = (uint8_t)(h >> (8 * i));
    o->uid_len = 7; o->protocol = 1; o->type = 2;   // NFC-A, Type 2 tag
    return 0;
}
static inline uint32_t dm_amiibo_mount(dm_amiibo *a) {
    uint32_t r = dm_amiibo_need_tag(a); if (r) return r;
    a->state = DM_NFP_DEV_MOUNTED; return 0;
}
// Unmount: an area the game set but did not flush is dropped, as on Horizon (only Flush writes the tag)
static inline uint32_t dm_amiibo_unmount(dm_amiibo *a) {
    uint32_t r = dm_amiibo_need_mount(a); if (r) return r;
    if (a->dirty) dm_amiibo_load(a);
    a->state = DM_NFP_DEV_FOUND; a->area_open = 0; return 0;
}
static inline void dm_amiibo_today(uint16_t *y, uint8_t *m, uint8_t *d) {
    time_t t = time(NULL); struct tm tm; gmtime_r(&t, &tm);
    *y = (uint16_t)(tm.tm_year + 1900); *m = (uint8_t)(tm.tm_mon + 1); *d = (uint8_t)tm.tm_mday;
}
static inline uint32_t dm_amiibo_model_info(const dm_amiibo *a, dm_nfp_model_info *o) {
    uint32_t r = dm_amiibo_need_mount(a); if (r) return r;
    memset(o, 0, sizeof *o); memcpy(o->id, a->id, 7); return 0;
}
static inline uint32_t dm_amiibo_common_info(const dm_amiibo *a, dm_nfp_common_info *o) {
    uint32_t r = dm_amiibo_need_mount(a); if (r) return r;
    memset(o, 0, sizeof *o); dm_amiibo_today(&o->year, &o->month, &o->day);
    o->write_counter = a->write_counter; o->area_size = DM_AMIIBO_AREA_SIZE; return 0;
}
// The owner (mii: a valid CharInfo the caller supplies, the host's default Mii) and the nickname (UTF-8, cut to 10 characters / 40 bytes at a character boundary)
static inline uint32_t dm_amiibo_register_info(const dm_amiibo *a, dm_nfp_register_info *o, const void *mii, const char *nickname) {
    uint32_t r = dm_amiibo_need_mount(a); if (r) return r;
    memset(o, 0, sizeof *o);
    if (mii) memcpy(o->mii, mii, sizeof o->mii);
    o->year = 2026; o->month = 1; o->day = 1;   // registered once, on a fixed day: the same for every scan
    size_t n = 0, chars = 0; const char *s = nickname && *nickname ? nickname : "amiibo";
    while (s[n] && chars < 10) {
        size_t l = ((unsigned char)s[n] & 0x80) == 0 ? 1 : ((unsigned char)s[n] & 0xe0) == 0xc0 ? 2 : ((unsigned char)s[n] & 0xf0) == 0xe0 ? 3 : 4;
        if (n + l > 40) break;
        size_t k = 1; while (k < l && s[n + k]) k++;
        if (k < l) break;   // a character cut short by the string's end
        n += l; chars++;
    }
    memcpy(o->nickname, s, n);
    return 0;
}
static inline uint32_t dm_amiibo_open_area(dm_amiibo *a, uint32_t access_id) {
    uint32_t r = dm_amiibo_need_mount(a); if (r) return r;
    if (!a->has_area) return DM_NFP_AREA_NOT_CREATED;
    if (a->access_id != access_id) return DM_NFP_WRONG_AREA_ID;
    a->area_open = 1; return 0;
}
static inline uint32_t dm_amiibo_get_area(const dm_amiibo *a, void *buf, uint64_t *n, uint64_t cap) {
    if (n) *n = 0;
    uint32_t r = dm_amiibo_need_mount(a); if (r) return r;
    if (!a->area_open) return DM_NFP_AREA_NOT_CREATED;
    uint64_t k = cap < DM_AMIIBO_AREA_SIZE ? cap : DM_AMIIBO_AREA_SIZE;
    if (buf) memcpy(buf, a->area, (size_t)k); else k = 0;
    if (n) *n = k;
    return 0;
}
static inline uint32_t dm_amiibo_set_area(dm_amiibo *a, const void *buf, uint64_t size) {
    uint32_t r = dm_amiibo_need_mount(a); if (r) return r;
    if (!a->area_open) return DM_NFP_AREA_NOT_CREATED;
    if (size > DM_AMIIBO_AREA_SIZE || (size && !buf)) return DM_NFP_WRONG_AREA_SIZE;
    memset(a->area, 0, sizeof a->area); if (size) memcpy(a->area, buf, (size_t)size);
    a->dirty = 1; return 0;
}
// Flush: the area is written, the write counter goes up. Returns -1 (as a Result: not 0) only when the folder cannot be written
static inline uint32_t dm_amiibo_flush(dm_amiibo *a) {
    uint32_t r = dm_amiibo_need_mount(a); if (r) return r;
    if (!a->dirty) return 0;
    a->write_counter++;
    return dm_amiibo_store(a) ? DM_NFP_RESULT(88) : 0;   // 88: writing the amiibo failed (yuzu's ResultWriteAmiiboFailed)
}
// CreateApplicationArea (recreate = 0, fails on an amiibo that has one) and RecreateApplicationArea (1): the area is made and written at once, as on Horizon
static inline uint32_t dm_amiibo_create_area(dm_amiibo *a, uint32_t access_id, const void *data, uint64_t size, int recreate) {
    uint32_t r = dm_amiibo_need_mount(a); if (r) return r;
    if (!recreate && a->has_area) return DM_NFP_AREA_EXISTS;
    if (size > DM_AMIIBO_AREA_SIZE || (size && !data)) return DM_NFP_WRONG_AREA_SIZE;
    memset(a->area, 0, sizeof a->area); if (size) memcpy(a->area, data, (size_t)size);
    a->access_id = access_id; a->has_area = 1; a->area_open = 0; a->write_counter++;
    return dm_amiibo_store(a) ? DM_NFP_RESULT(88) : 0;
}
