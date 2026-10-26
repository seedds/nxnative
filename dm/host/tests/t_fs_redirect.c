// Virtual sequential RomFS archive test.  Logical records remain contiguous
// and sorted while their physical bytes live in disjoint source/replacement
// ranges of one materialized file.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include "../hle_fs.c"

struct dm_flags g_flag;
uint64_t g_t0_ns;
uint64_t now_ns(void) { return 0; }
void dm_guest_write_prepare(void *p, size_t n) { (void)p; (void)n; }

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); } else printf("  ✓ "); printf(__VA_ARGS__); printf("\n"); } while (0)

static void write_bytes(const char *path, const void *bytes, size_t count) {
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(bytes, 1, count, f) != count || fclose(f)) { perror(path); exit(2); }
}

static void setup(char *root, size_t root_size, int malformed) {
    const char *tmp = getenv("TMPDIR");
    snprintf(root, root_size, "%s/t_fs_redirect.XXXXXX", tmp ? tmp : "/private/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); exit(2); }
    char rom[1200], arc[1200], archive[1300], overlay[1200], oarc[1200], oarchive[1300], sidecar[1300];
    snprintf(rom, sizeof rom, "%s/rom", root);
    snprintf(arc, sizeof arc, "%s/arc", rom);
    snprintf(overlay, sizeof overlay, "%s/overlay", root);
    snprintf(oarc, sizeof oarc, "%s/arc", overlay);
    mkdir(rom, 0755); mkdir(arc, 0755); mkdir(overlay, 0755); mkdir(oarc, 0755);
    snprintf(archive, sizeof archive, "%s/data.trpfs", arc);
    uint8_t data[256]; memset(data, 0, sizeof data);
    uint64_t virtual_root = 64; memcpy(data + 8, &virtual_root, sizeof virtual_root);
    memcpy(data + 32, "PACK-ONE-1234567", 16);
    memcpy(data + 160, "PACK-TWO-REPLACEMENT-12345678901", 32);
    memcpy(data + 224, "ROOT-TABLE-123456", 16);
    write_bytes(archive, data, sizeof data);
    snprintf(oarchive, sizeof oarchive, "%s/data.trpfs", oarc);
    write_bytes(oarchive, data, sizeof data);
    snprintf(sidecar, sizeof sidecar, "%s/redirects.tsv", root);
    FILE *f = fopen(sidecar, "w");
    if (!f) { perror(sidecar); exit(2); }
    fputs("DM_ROMFS_REDIRECTS_V1\tarc/data.trpfs\t4\t80\t256\n"
          "0\t16\t0\t16\n"
          "16\t16\t32\t16\n"
          "32\t32\t160\t32\n"
          "64\t16\t224\t16\n", f);
    if (malformed) fputs("trailing\n", f);
    fclose(f);
    setenv("DM_ROMFS", rom, 1);
    setenv("DM_ROMFS_OVERLAY", overlay, 1);
    setenv("DM_ROMFS_REDIRECTS", sidecar, 1);
}

static int malformed_child(void) {
    char root[1024]; setup(root, sizeof root, 1);
    if (h_fs_MountRom("data", NULL, 0) != RES_OK) return 3;
    uint64_t h = 0;
    // The malformed configured sidecar must terminate before a handle is
    // returned; surviving this call is a test failure.
    return h_fs_OpenFile(&h, "data:/arc/data.trpfs", MODE_READ) == RES_OK ? 0 : 4;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "malformed")) return malformed_child();
    char root[1024]; setup(root, sizeof root, 0);
    CHECK(h_fs_MountRom("data", NULL, 0) == RES_OK, "mount redirect fixture");
    uint64_t h = 0; size_t got = 0; char out[64] = {0}; int64_t file_size = 0;
    CHECK(h_fs_OpenFile(&h, "data:/arc/data.trpfs", MODE_READ) == RES_OK,
          "open configured archive");
    CHECK(h_fs_GetFileSize(&file_size, h) == RES_OK && file_size == 80,
          "GetFileSize reports the 80-byte virtual archive");
    uint64_t read_root = 0;
    CHECK(h_fs_ReadFile(&got, h, 0, out, 16) == RES_OK && got == 16 &&
          (memcpy(&read_root, out + 8, sizeof read_root), read_root == 64),
          "header read stays physical and exposes the virtual root pointer");
    memset(out, 0, sizeof out); got = 0;
    CHECK(h_fs_ReadFile(&got, h, 16, out, 16) == RES_OK && got == 16 &&
          !memcmp(out, "PACK-ONE-1234567", 16), "first virtual pack reads its source range");
    memset(out, 0, sizeof out); got = 0;
    CHECK(h_fs_ReadFile(&got, h, 32, out, 32) == RES_OK && got == 32 &&
          !memcmp(out, "PACK-TWO-REPLACEMENT-12345678901", 32),
          "second virtual pack reads its disjoint replacement range");
    memset(out, 0, sizeof out); got = 0;
    CHECK(h_fs_ReadFile(&got, h, 64, out, 16) == RES_OK && got == 16 &&
          !memcmp(out, "ROOT-TABLE-123456", 16), "virtual EOF root reads its physical root range");
    uint8_t cross[48] = {0}, expected[48] = {0};
    memcpy(expected, &read_root, 8); memcpy(expected + 8, "PACK-ONE-1234567", 16);
    memcpy(expected + 24, "PACK-TWO-REPLACEMENT-12345678901", 24); got = 0;
    CHECK(h_fs_ReadFile(&got, h, 8, cross, sizeof cross) == RES_OK && got == sizeof cross &&
          !memcmp(cross, expected, sizeof cross), "one partial read crosses header and two pack rows");
    got = 99;
    CHECK(h_fs_ReadFile(&got, h, 79, out, 2) == RES_FAIL && got == 0,
          "read past virtual EOF fails closed");
    h_fs_CloseFile(h);

    DirectoryEntry entries[8]; int64_t nentry = 0; uint64_t directory = 0;
    CHECK(h_fs_OpenDirectory(&directory, "data:/arc", DIRMODE_FILE) == RES_OK &&
          h_fs_ReadDirectory(&nentry, entries, directory, 8) == RES_OK && nentry == 1 &&
          !strcmp(entries[0].name, "data.trpfs") && entries[0].size == 80,
          "overlay directory snapshot reports virtual file size");
    h_fs_CloseDirectory(directory);
    unsetenv("DM_ROMFS_OVERLAY");
    CHECK(h_fs_MountRom("plain", NULL, 0) == RES_OK &&
          h_fs_OpenDirectory(&directory, "plain:/arc", DIRMODE_FILE) == RES_OK &&
          h_fs_ReadDirectory(&nentry, entries, directory, 8) == RES_OK && nentry == 1 &&
          !strcmp(entries[0].name, "data.trpfs") && entries[0].size == 80,
          "ordinary directory enumeration reports virtual file size");
    h_fs_CloseDirectory(directory);

    pid_t child = fork();
    if (!child) { execl(argv[0], argv[0], "malformed", NULL); _exit(127); }
    int status = 0; waitpid(child, &status, 0);
    CHECK(!(WIFEXITED(status) && WEXITSTATUS(status) == 0),
          "malformed redirect sidecar terminates before opening the archive");

    char command[1200]; snprintf(command, sizeof command, "rm -rf '%s'", root); (void)system(command);
    printf("== %d checks, %d failed ==\n", checks, fails);
    return fails != 0;
}
