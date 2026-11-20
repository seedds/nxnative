// Exercise the actual verifier with ephemeral DER fixtures, never account data.
#include "../nextendo_x509.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static unsigned char *read_file(const char *path, size_t *size) {
    *size = 0; FILE *file = fopen(path, "rb"); if (!file) return NULL;
    if (fseek(file, 0, SEEK_END)) { int error=errno; fclose(file); errno=error; return NULL; }
    long length = ftell(file); int length_error=errno;
    if (length < 1 || length > 65537) { fclose(file); errno=length_error; return NULL; }
    if (fseek(file, 0, SEEK_SET)) { int error=errno; fclose(file); errno=error; return NULL; }
    unsigned char *bytes = malloc((size_t)length);
    if (!bytes) { int error=errno; fclose(file); errno=error; return NULL; }
    size_t got=fread(bytes,1,(size_t)length,file); int read_error=errno;
    int closed=fclose(file);
    if (got!=(size_t)length || closed) { free(bytes); if (got!=(size_t)length) errno=read_error; return NULL; }
    *size=(size_t)length; return bytes;
}
int main(int argc, char **argv) {
    if (argc == 5 && !strcmp(argv[1], "--pin")) {
        size_t pin_size, leaf_size;
        unsigned char *pin = read_file(argv[3], &pin_size), *leaf = read_file(argv[4], &leaf_size);
        bool valid = dm_nextendo_x509_verify_pin(argv[2], pin, pin_size, leaf, leaf_size);
        free(pin); free(leaf); return valid ? 0 : 1;
    }
    if (argc < 4 || argc > 19 || !dm_nextendo_x509_available()) return 2;
    size_t anchor_size = 0, sizes[16] = {0}, count = (size_t)argc - 3;
    unsigned char *anchor = read_file(argv[2], &anchor_size); const unsigned char *chain[16] = {0};
    for (size_t i = 0; i < count; i++) chain[i] = read_file(argv[i + 3], &sizes[i]);
    bool valid = dm_nextendo_x509_verify(argv[1], anchor, anchor_size, chain, sizes, count);
    for (size_t i = 0; i < count; i++) free((void *)chain[i]); free(anchor);
    return valid ? 0 : 1;
}
