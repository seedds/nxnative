// ExeFS patch formats (hle_exefs.c): IPS and IPS32 records (plain, a run, a record inside the NSO header dropped, the end
// marker, a file cut short), pchtxt as IPSwitch writes it (the build ID, offset_shift, enabled and disabled groups,
// little-endian values, strings with escapes, comments, @stop, a malformed line) and the build ID match of patch file
// names (prefix, padding, too short). Expectations are written by hand.
#define DM_EXEFS_PARSERS_ONLY
#include "../hle_exefs.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

typedef struct { int n; uint64_t off[16]; uint32_t len[16]; uint8_t data[16][32]; } got_t;
static int collect(void *ctx, const exefs_rec_t *r) {
    got_t *g = ctx; if (g->n == 16) return 1;
    g->off[g->n] = r->off; g->len[g->n] = r->len;
    for (uint32_t k = 0; k < r->len && k < 32; k++) g->data[g->n][k] = r->run ? r->fill : r->bytes[k];
    g->n++; return 0;
}

int main(void) {
    {   // IPS: a record at file offset 0x104 (image +4), a run of 3 at 0x200, one inside the header (dropped)
        const uint8_t ips[] = { 'P','A','T','C','H', 0x00,0x01,0x04, 0x00,0x02, 0xAA,0xBB, 0x00,0x02,0x00, 0x00,0x00, 0x00,0x03, 0x7F,
                                0x00,0x00,0x10, 0x00,0x01, 0x11, 'E','O','F' };
        got_t g = { 0 };
        CHECK(exefs_parse_ips(ips, sizeof ips, collect, &g) == 0, "IPS parses");
        CHECK(g.n == 2, "IPS: 2 records in the image, got %d", g.n);
        CHECK(g.off[0] == 4 && g.len[0] == 2 && g.data[0][0] == 0xAA && g.data[0][1] == 0xBB, "IPS record 1");
        CHECK(g.off[1] == 0x100 && g.len[1] == 3 && g.data[1][2] == 0x7F, "IPS run");
        got_t h = { 0 };
        CHECK(exefs_parse_ips(ips, sizeof ips - 2, collect, &h) == -1, "IPS cut short is refused");
    }
    {   // IPS32: a 4-byte offset past 16 MB
        const uint8_t ips[] = { 'I','P','S','3','2', 0x01,0x00,0x01,0x00, 0x00,0x04, 0x1F,0x20,0x03,0xD5, 'E','E','O','F' };
        got_t g = { 0 };
        CHECK(exefs_parse_ips(ips, sizeof ips, collect, &g) == 0 && g.n == 1, "IPS32 parses");
        CHECK(g.off[0] == 0x1000000 && g.len[0] == 4 && g.data[0][0] == 0x1F && g.data[0][3] == 0xD5, "IPS32 record");
        const uint8_t bad[] = { 'N','O','P','E','!' };
        CHECK(exefs_parse_ips(bad, sizeof bad, collect, &g) == -1, "not an IPS file");
    }
    {   // pchtxt
        const char *t =
            "@nsobid-E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348\n"
            "# a comment line\n"
            "@flag offset_shift 0x100\n"
            "0000 DEADBEEF\n"                       // before any group: not applied
            "// first group\n"
            "@enabled\n"
            "004AE798 DC861D14  // a trailing comment\r\n"
            "@disabled\n"
            "00000010 FFFFFFFF\n"
            "@enabled\n"
            "@little-endian\n"
            "00000020 11223344\n"
            "00000030 \"a\\n\\\"b\"\n"
            "@stop\n"
            "00000040 00\n";
        got_t g = { 0 }; char bid[80];
        CHECK(exefs_parse_pchtxt(t, bid, sizeof bid, collect, &g) == 0, "pchtxt parses");
        CHECK(!strcmp(bid, "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348"), "pchtxt build ID: %s", bid);
        CHECK(g.n == 3, "pchtxt: 3 records, got %d", g.n);
        CHECK(g.off[0] == 0x4AE798 && g.len[0] == 4 && g.data[0][0] == 0xDC && g.data[0][3] == 0x14, "pchtxt big-endian record at its shifted offset");
        CHECK(g.off[1] == 0x20 && g.data[1][0] == 0x44 && g.data[1][3] == 0x11, "pchtxt little-endian record");
        CHECK(g.off[2] == 0x30 && g.len[2] == 4 && !memcmp(g.data[2], "a\n\"b", 4), "pchtxt string");
        got_t h = { 0 };
        CHECK(exefs_parse_pchtxt("@enabled\n0010 ABC\n", bid, sizeof bid, collect, &h) == -1, "an odd number of digits is refused");
        CHECK(exefs_parse_pchtxt("@enabled\nzz 00\n", bid, sizeof bid, collect, &h) == -1, "a line without an offset is refused");
        got_t k = { 0 };   // no offset_shift: offsets count from the NSO header
        CHECK(exefs_parse_pchtxt("@enabled\n00000104 00\n000000F0 00\n", bid, sizeof bid, collect, &k) == 0 && k.n == 1 && k.off[0] == 4, "pchtxt without a shift");
    }
    {   // build IDs
        const char *mod = "e0f3ccfaf50065ae4ea01eb5aeb13b1a";
        CHECK(exefs_bid_match("E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348", 40, mod), "full 20-byte ID");
        CHECK(exefs_bid_match("E0F3CCFAF50065AE", 16, mod), "8-byte prefix");
        CHECK(!exefs_bid_match("E0F3CCFA", 8, mod), "too short a prefix");
        CHECK(!exefs_bid_match("E0F3CCFAF50065AF", 16, mod), "another build");
    }
    if (fails) { fprintf(stderr, "t_exefs: %d failed\n", fails); return 1; }
    printf("t_exefs: ok\n");
    return 0;
}
