#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>

// The parser needs only pso_skey. This is the part of NvShader used by the writer in the same header.
typedef struct { int stage; char key[24]; } NvShader;
#include "nvn_pso_skey.h"

static int library_name(const char key[24]) {
    return strspn(key, "0123456789abcdef") == 16 && !strcmp(key + 16, "_b");
}

// Read a type and key per line. Return the referenced libraries and the host's normalized key.
int main(void) {
    char *line = NULL;
    size_t capacity = 0;
    while (getline(&line, &capacity, stdin) >= 0) {
        line[strcspn(line, "\n")] = '\0';
        pso_skey key;
        if (line[1] != '\t' || (line[0] != 'R' && line[0] != 'M') ||
            (line[0] == 'R' ? pso_skey_parse(line + 2, &key) : pso_mesh_skey_parse(line + 2, &key)) ||
            !memchr(key.vs, 0, sizeof key.vs) || !memchr(key.fs, 0, sizeof key.fs) ||
            !library_name(key.vs) || (key.fs[0] && !library_name(key.fs)) ||
            key.k.vs || key.k.fs || key.k.fsfn) {
            puts("-");
            continue;
        }
        char hex[sizeof key * 2 + 1];
        pso_skey_hex(&key, hex);
        printf("%s\t%u\t%s\t%u\t%s\n", key.vs, key.vs_stage, key.fs, key.fs_stage, hex);
    }
    free(line);
    return ferror(stdin) || fflush(stdout) == EOF;
}
