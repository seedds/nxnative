// Unit test of the Atmosphère-format cheat engine (hle_cheat.c) against a fake guest: one module ("main") and one heap
// block stand in for the loader and hle_svc.c, and the engine's extern hooks (heap/alias ranges, mapped check, held
// buttons, pause, guest-write prepare, now_ns) are stubbed here. The opcode words below are hand-encoded in dmnt's
// documented layouts, so the test doubles as a decoder reference: every line says which instruction it spells.
// Default semantics under test: every cheat starts OFF at load; toggles.txt (and the menu, whose flips persist back to
// toggles.txt) is what turns rows on -- only the master cheat runs unconditionally, as in dmnt.
#include "../dm.h"
#include "../hle_cheat.h"
#include <assert.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// ---- fake guest memory ----
module_t g_mod[MAX_MOD];       // normally loader.c's; the test stands in for the loader
int g_nmod;
static uint8_t *M;             // the fake main module: [0, 0x10000) code, [0x10000, 0x24000) data
static uint8_t *H;             // the fake heap
static int g_heap_open;
static uint64_t g_held;        // buttons "held" for keypress conditions
static int g_pause_on[2];      // pause-all call counts: [0] = on-calls, [1] = off-calls

void svc_guest_range(uint8_t **base, size_t *size) { *base = H - 0x1000; *size = 0x100000000ull; }   // covers the heap region for the silent-reject test
void svc_heap_range(uint8_t **base, size_t *size) { *base = H; *size = g_heap_open ? 0x10000 : 0; }
void svc_alias_range(uint8_t **base, size_t *size) { *base = NULL; *size = 0; }
int svc_guest_mapped(uint64_t a, size_t n) { (void)a; (void)n; return 0; }
uint64_t dm_input_held(void) { return g_held; }
void svc_thread_pause_all(int on) { g_pause_on[on ? 0 : 1]++; }
void dm_guest_write_prepare(void *addr, size_t len) { (void)addr; (void)len; }
uint64_t now_ns(void) { static uint64_t t; return t += 1000000; }

// ---- helpers ----
static char g_dir[4096];
static int g_fail;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail = 1; } } while (0)

static void write_file(const char *name, const char *text) {
    char p[4200]; snprintf(p, sizeof p, "%s/%s", g_dir, name);
    FILE *f = fopen(p, "w"); assert(f);
    fputs(text, f); fclose(f);
}
static void set_on(const char *names) {              // toggles.txt with every listed name on (the reload applies it)
    char b[2048] = "";
    for (const char *n = names; *n; ) {
        const char *e = strchr(n, ',') ? strchr(n, ',') : n + strlen(n);
        char line[300]; snprintf(line, sizeof line, "[%.*s] on\n", (int)(e - n), n);
        strlcat(b, line, sizeof b);
        n = *e ? e + 1 : e;
    }
    write_file("toggles.txt", b);
}
static void unlink_file(const char *name) {
    char p[4200]; snprintf(p, sizeof p, "%s/%s", g_dir, name); unlink(p);
}
static uint32_t rd32(size_t off) { uint32_t v; memcpy(&v, M + off, 4); return v; }
static uint64_t rd64(size_t off) { uint64_t v; memcpy(&v, M + off, 8); return v; }
static int toggles_has(const char *needle) {
    char p[4200]; snprintf(p, sizeof p, "%s/toggles.txt", g_dir);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    char b[8192]; size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f);
    return strstr(b, needle) != NULL;
}

int main(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s", (tmp && *tmp) ? tmp : "/tmp");
    size_t dl = strlen(g_dir); while (dl && g_dir[dl - 1] == '/') g_dir[--dl] = 0;
    strcat(g_dir, "/t_cheat.d"); mkdir(g_dir, 0755);

    M = mmap(NULL, 0x24000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    H = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(M != MAP_FAILED && H != MAP_FAILED);
    g_mod[0] = (module_t){ .name = "main", .base = M, .size = 0x24000, .text_end = 0x10000,
                           .key_page_off = 0x20000, .key_page_size = 0x4000, .patch_off = 0x24000, .patch_len = 0 };
    strcpy(g_mod[0].build_id, "0123456789abcdef0123456789abcdef");
    g_nmod = 1;
    g_heap_open = 1;

    // 1. Discovery by the dmnt 16-hex name; a plain store stays OFF by default, toggles.txt turns it on:
    //    04000000 00011000 12345678 = "store u32 0x12345678 at main+0x11000"
    write_file("0123456789abcdef.txt", "[Store]\n04000000 00011000 12345678\n");
    CHECK(cheat_reload(g_dir) == 1);
    CHECK(cheat_enabled_count() == 0);                   // everything off at load
    *(uint32_t *)(M + 0x11000) = 0;
    cheat_execute();
    CHECK(rd32(0x11000) == 0);                           // not enabled: nothing runs
    set_on("Store");
    CHECK(cheat_reload(g_dir) == 1);
    CHECK(cheat_enabled_count() == 1);
    cheat_execute();
    CHECK(rd32(0x11000) == 0x12345678);

    // 2. The 32-hex name variant works too; master + register chain: load R15 from main+0x10000, add 0x100, store u32
    //    0xABCD through it (the pattern every distributed cheat table starts with). The master runs unconditionally.
    unlink_file("0123456789abcdef.txt");
    unlink_file("toggles.txt");
    write_file("0123456789abcdef0123456789abcdef.txt",
               "{Master}\n580F0000 00010000\n780F0000 00000100\n640F0000 00000000 0000ABCD\n");
    CHECK(cheat_reload(g_dir) == 1);
    uint64_t point = (uint64_t)(M + 0x10500); memcpy(M + 0x10000, &point, 8);
    *(uint32_t *)(M + 0x10600) = 0;
    cheat_execute();
    CHECK(rd32(0x10600) == 0xABCD);
    CHECK(cheat_enabled_count() == 1);                   // the master alone

    // 3. Conditional block (EQ on the u32 at main+0x12000): runs at 1, skipped at 2
    unlink_file("0123456789abcdef0123456789abcdef.txt");
    write_file("0123456789abcdef.txt",
               "[Cond]\n14050000 00012000 00000001\n04000000 00013000 00000007\n20000000\n");
    set_on("Cond");
    CHECK(cheat_reload(g_dir) == 1);
    *(uint32_t *)(M + 0x12000) = 1; *(uint32_t *)(M + 0x13000) = 9;
    cheat_execute();
    CHECK(rd32(0x13000) == 7);
    *(uint32_t *)(M + 0x12000) = 2; *(uint32_t *)(M + 0x13000) = 9;
    cheat_execute();
    CHECK(rd32(0x13000) == 9);

    // 4. Keypress conditional on A (mask bit 0): only with the button held; and loop + arithmetic + store-through-register:
    //    R2 = 1, five times R2 += 3 (= 16), stored as u64 through R3 = main+0x14000
    write_file("0123456789abcdef.txt",
               "[Key]\n80000001\n08000000 00015000 11223344 55667788\n20000000\n"
               "[Loop]\n40020000 00000000 00000001\n40030000 00000000 00014000\n30010000 00000005\n78020000 00000003\n31010000\nA8230300\n");
    set_on("Key,Loop");
    CHECK(cheat_reload(g_dir) == 1);
    g_held = 0; *(uint64_t *)(M + 0x15000) = 0;
    cheat_execute();
    CHECK(rd64(0x15000) == 0);                       // not held: the keypress block is skipped
    CHECK(rd64(0x14000) == 16);                      // the loop cheat does not depend on buttons
    g_held = 1; cheat_execute();
    CHECK(rd64(0x15000) == 0x1122334455667788ull);   // 64-bit store with both value words (high word first)

    // 5. toggles.txt enables one of two (everything is off without a line)
    write_file("0123456789abcdef.txt", "[Key]\n04000000 00016000 0000000F\n[Other]\n04000000 00017000 0000000E\n");
    set_on("Other");
    CHECK(cheat_reload(g_dir) == 1);
    CHECK(cheat_enabled_count() == 1);
    cheat_execute();
    CHECK(rd32(0x16000) == 0 && rd32(0x17000) == 0xE);

    // 6. A junk header line (a tool's title, like this game's bare table header) is skipped whole;
    //    garbage inside a code line still rejects the file: 'Z' is not a hex digit
    write_file("0123456789abcdef.txt", "\xE6\x97\xB7\xE9\x87\x8E\xE4\xB9\x8B\xE6\x81\xAF test table v1.9.0\n[Store]\n04000000 00011000 12345678\n");
    set_on("Store");
    CHECK(cheat_reload(g_dir) == 1);
    *(uint32_t *)(M + 0x11000) = 0;
    cheat_execute();
    CHECK(rd32(0x11000) == 0x12345678);
    write_file("0123456789abcdef.txt", "[Bad]\n04000000 ZZ\n");
    unlink_file("toggles.txt");
    CHECK(cheat_reload(g_dir) == 0);
    CHECK(cheat_enabled_count() == 0);

    // 7. The guard: a store beyond the module is skipped (and counted) while a heap store goes through; a heap store
    //    before the heap is opened is skipped silently (dmnt attaches to a process whose heap already exists)
    write_file("0123456789abcdef.txt",
               "[Out]\n04000000 00500000 00000001\n"
               "[Heap]\n04100000 00000010 00000002\n");   // 0TMR00AA with M=1 (bits 23-20): the heap region
    set_on("Out,Heap");
    CHECK(cheat_reload(g_dir) == 1);
    *(uint32_t *)(H + 0x10) = 0;
    int before = cheat_reject_count();
    cheat_execute();
    CHECK(cheat_reject_count() == before + 1);           // the out-of-range store was rejected once, the heap store went through
    CHECK(*(uint32_t *)(H + 0x10) == 2);
    write_file("0123456789abcdef.txt", "[Heap]\n04100000 00000010 00000003\n");
    set_on("Heap");
    CHECK(cheat_reload(g_dir) == 1);
    g_heap_open = 0;
    *(uint32_t *)(H + 0x10) = 0;
    before = cheat_reject_count();
    cheat_execute();                                     // heap not opened: the store must be silently skipped
    CHECK(cheat_reject_count() == before);
    CHECK(*(uint32_t *)(H + 0x10) == 0);
    g_heap_open = 1;

    // 8. Pause / resume opcodes reach the thread layer once per pass, not once per tick of a held state
    write_file("0123456789abcdef.txt", "[Pause]\nFF000000\nFF100000\n");
    set_on("Pause");
    CHECK(cheat_reload(g_dir) == 1);
    int p0 = g_pause_on[0], r0 = g_pause_on[1];
    cheat_execute();
    CHECK(g_pause_on[0] == p0 + 1 && g_pause_on[1] == r0 + 1);

    // 9. A write into the module's code area (Mac path: unseal, write, flush, reseal)
    write_file("0123456789abcdef.txt", "[Code]\n04000000 00000010 0000BEEF\n");
    set_on("Code");
    CHECK(cheat_reload(g_dir) == 1);
    cheat_execute();
    CHECK(rd32(0x10) == 0xBEEF);

    // 10. Baked patches (patch_nso DM_CHEAT_FILE): a fake DMCF flags page on the module. Off by default; toggles.txt
    //     turns rows on -- including a baked patch with no file row, which only toggles.txt can reach; keypress gate applies
    uint8_t *fp = M + 0x20400;                  // inside the module: the page must not reach past the mapping
    g_mod[0].flags_page_off = 0x20400;
    memset(fp, 0, 0x2000);
    memcpy(fp, "DMCF", 4);
    *(uint32_t *)(fp + 4) = 2;
    *(uint32_t *)(fp + 0x20) = 0;            strcpy((char *)fp + 0x20 + 8, "Baked one");
    *(uint32_t *)(fp + 0x20 + 0x48) = 0x10;  strcpy((char *)fp + 0x20 + 0x48 + 8, "Baked key");   // key L3, no file row
    g_held = 0;
    write_file("0123456789abcdef.txt", "[Baked one]\n04000000 00018000 00000001\n");
    unlink_file("toggles.txt");
    CHECK(cheat_reload(g_dir) == 1);
    cheat_baked_pump();
    CHECK(fp[0x1400] == 0 && fp[0x1404] == 0);       // both off at boot
    set_on("Baked one,Baked key");
    CHECK(cheat_reload(g_dir) == 1);
    cheat_baked_pump();
    CHECK(fp[0x1400] == 1);                          // file row turned on through toggles.txt
    CHECK(fp[0x1404] == 0);                          // key mask 0x10 (L3) not held: gated off
    g_held = 0x10;
    cheat_baked_pump();
    CHECK(fp[0x1404] == 1);                          // held: on
    write_file("toggles.txt", "[Baked one] off\n[Baked key] on\n");
    CHECK(cheat_reload(g_dir) == 1);
    g_held = 0;
    cheat_baked_pump();
    CHECK(fp[0x1400] == 0);                          // toggled off through toggles.txt
    *(uint32_t *)(M + 0x18000) = 0;
    cheat_execute();
    CHECK(rd32(0x18000) == 0);                       // the baked name is skipped in the VM: only a real stub (the rewrite) would write

    // 11. The menu view: one row per baked patch plus one per unbaked file cheat; rows start off, toggling flips the
    //     shared state, and every flip is written back to toggles.txt so the next launch starts from it
    write_file("0123456789abcdef.txt", "[Baked one]\n04000000 0001A000 00000001\n[Vm one]\n04000000 0001B000 00000002\n");
    write_file("toggles.txt", "[Baked key] off\n");   // a baked-only bit remembers earlier toggles until a file says otherwise
    CHECK(cheat_reload(g_dir) == 1);
    int rows = cheat_menu_count();
    CHECK(rows == 3);                                // 2 baked (Baked one, Baked key) + 1 file-only (Vm one)
    char mn[64]; int on = 0;
    cheat_menu_get(0, mn, sizeof mn, &on);
    CHECK(!strcmp(mn, "Baked one") && on == 0);      // off until toggled
    cheat_menu_get(2, mn, sizeof mn, &on);
    CHECK(!strcmp(mn, "Vm one") && on == 0);
    unlink_file("toggles.txt");                      // no file yet: the first menu flip creates it
    cheat_menu_toggle(0);                            // baked row backed by a file entry: both flip, and persist
    cheat_baked_pump();
    CHECK(fp[0x1400] == 1);
    CHECK(toggles_has("[Baked one] on"));
    cheat_menu_toggle(2);                            // file-only row
    cheat_execute();
    CHECK(rd32(0x1B000) == 2);                       // Vm one now runs
    CHECK(toggles_has("[Vm one] on") && toggles_has("[Baked key] off"));
    *(uint32_t *)(M + 0x1B000) = 7;                  // a switched-off cheat stops writing; what it wrote stays (dmnt semantics)
    cheat_menu_toggle(2);
    cheat_execute();
    CHECK(rd32(0x1B000) == 7);
    CHECK(toggles_has("[Vm one] off"));              // the flip-back persisted too
    cheat_menu_toggle(2);                            // on again; the file now says on
    // and the next launch starts from exactly that file
    write_file("0123456789abcdef.txt", "[Baked one]\n04000000 0001A000 00000001\n[Vm one]\n04000000 0001B000 00000002\n");
    CHECK(cheat_reload(g_dir) == 1);
    *(uint32_t *)(M + 0x1B000) = 0;
    cheat_execute();
    CHECK(rd32(0x1A000) == 0 && rd32(0x1B000) == 2); // Baked one still skipped in the VM, Vm one carried over as on

    unlink_file("0123456789abcdef.txt");
    unlink_file("toggles.txt");
    rmdir(g_dir);
    if (g_fail) { fprintf(stderr, "t_cheat: FAILED\n"); return 1; }
    printf("t_cheat: OK\n");
    return 0;
}
