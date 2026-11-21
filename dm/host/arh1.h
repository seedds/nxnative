// Monolith's ARH1 archive index (Xenoblade Chronicles 2, Definitive Edition and 3, and their Switch 2 Editions: bf2.arh / bf3.arh beside bf2.ard / bf3.ard, aoc<n>.arh
// in add-on content), read so that files of mod folders can replace archive members (hle_fs.c, "Archive members of mod folders"; docs/reference/services.md).
// Layout (https://github.com/roccodev/ard-tools/blob/master/docs/arh1.md, checked against bf3.arh 2.2.1 and bf2.arh 3.0.1):
//   header 48 bytes: "arh1", u32 ?, u32 dictionary entries, u32 strings offset, u32 strings size, u32 dictionary offset, u32 dictionary size, u32 file table offset,
//          u32 file count, u32 key
//   strings and dictionary: each whole u32 word XORed with key ^ 0xF3F35353 (a last partial word is plain). The dictionary is a trie of (i32 next, i32 prev): from node i
//          with next >= 0 the character c leads to node next ^ c, whose prev must be i; a node with next < 0 ends the path, -next being the offset of its remaining characters
//          (possibly none) in the strings, NUL-terminated and followed by the file's u32 ID
//   file table (plain): per file u64 ARD offset, u32 stored size, u32 expanded size (0: stored as is), u32 flags (bit 0 hidden, bit 1 XBC1 wrapper), u32 ID
// A member stored as is has flags 0 and expanded size 0 (36,603 of bf3.arh's 76,445 files), so a replacement is pointed at its bytes with those values.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ARH1_HEADER 48
#define ARH1_ENTRY 24
typedef struct {
    uint8_t *strings; uint32_t nstrings;   // decrypted copies
    int32_t *dict; uint32_t ndict;         // pairs: dict[2i] next, dict[2i+1] prev
    uint32_t table, nfiles;                // file table offset and count in the index
} arh1_t;

static uint32_t arh1_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void arh1_free(arh1_t *a) { free(a->strings); free(a->dict); memset(a, 0, sizeof *a); }

// 0 with *a filled (free with arh1_free), -1 for data that is not a valid ARH1 index
static int arh1_open(const uint8_t *d, size_t n, arh1_t *a) {
    memset(a, 0, sizeof *a);
    if (n < ARH1_HEADER || memcmp(d, "arh1", 4)) return -1;
    uint32_t so = arh1_u32(d + 12), sn = arh1_u32(d + 16), no = arh1_u32(d + 20), nn = arh1_u32(d + 24), fo = arh1_u32(d + 28), fn = arh1_u32(d + 32);
    uint32_t key = arh1_u32(d + 36) ^ 0xF3F35353u;
    if (so > n || sn > n - so || no > n || nn > n - no || nn % 8 || fo > n || fn > (n - fo) / ARH1_ENTRY || !nn) return -1;
    a->strings = calloc((size_t)sn + 1, 1); a->dict = calloc(nn, 1);   // zeroed: every word is then defined, whatever the loops below cover
    if (!a->strings || !a->dict) { arh1_free(a); return -1; }
    memcpy(a->strings, d + so, sn); a->strings[sn] = 0;   // a terminator past the end bounds every string scan
    for (uint32_t i = 0; i + 4 <= sn; i += 4) { uint32_t w = arh1_u32(a->strings + i) ^ key; memcpy(a->strings + i, &w, 4); }
    for (uint32_t i = 0; i < nn / 4; i++) { uint32_t w = arh1_u32(d + no + i * 4) ^ key; memcpy((uint8_t *)a->dict + i * 4, &w, 4); }
    a->nstrings = sn; a->ndict = nn / 8; a->table = fo; a->nfiles = fn;
    return 0;
}

// The ID of the member at path ("/chr/pc/pc010101.wimdo"), or -1
static int64_t arh1_find(const arh1_t *a, const char *path) {
    size_t len = strlen(path); uint32_t node = 0;
    while (a->dict[2 * node] >= 0) {
        if (!len) return -1;   // the path ends inside a directory
        uint8_t c = (uint8_t)*path; if (c > 0x7f) return -1;
        uint32_t next = (uint32_t)a->dict[2 * node] ^ c;
        if (next >= a->ndict || (uint32_t)a->dict[2 * next + 1] != node) return -1;
        node = next; path++; len--;
    }
    int64_t s = -(int64_t)a->dict[2 * node];
    if (s >= a->nstrings) return -1;
    while (s < a->nstrings && a->strings[s]) {   // the node's remaining characters; none when the trie spelled the whole path (7,168 of bf3.arh's members)
        if (!len || (uint8_t)*path != a->strings[s]) return -1;
        s++; path++; len--;
    }
    if (len || s + 5 > a->nstrings) return -1;
    return arh1_u32(a->strings + s + 1);
}

// The file table entry of member id in the index d (n bytes), or NULL. IDs are the entries' indexes in the games seen; a table where they are not is searched
static uint8_t *arh1_entry(const arh1_t *a, uint8_t *d, uint32_t id) {
    if (id < a->nfiles && arh1_u32(d + a->table + (size_t)id * ARH1_ENTRY + 20) == id) return d + a->table + (size_t)id * ARH1_ENTRY;
    for (uint32_t i = 0; i < a->nfiles; i++) if (arh1_u32(d + a->table + (size_t)i * ARH1_ENTRY + 20) == id) return d + a->table + (size_t)i * ARH1_ENTRY;
    return NULL;
}

// Points member id at size bytes stored as is at off in the ARD; 0, or -1 when there is no such member
static int arh1_redirect(const arh1_t *a, uint8_t *d, uint32_t id, uint64_t off, uint32_t size) {
    uint8_t *e = arh1_entry(a, d, id);
    if (!e) return -1;
    uint32_t zero = 0, flags = arh1_u32(e + 16) & ~2u;   // no XBC1 wrapper; the hidden bit stays as the game had it
    memcpy(e, &off, 8); memcpy(e + 8, &size, 4); memcpy(e + 12, &zero, 4); memcpy(e + 16, &flags, 4);
    return 0;
}
