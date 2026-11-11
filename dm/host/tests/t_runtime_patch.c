// Runtime choices are transactional: a bad later row never leaves an earlier write applied.
#include "../runtime_patch.h"
#include <assert.h>
static unsigned checks;
static void run(const char *body, const char *value, int good, unsigned count) {
    uint8_t image[64], original[64]; memset(image, 0x11, sizeof image); memcpy(original, image, sizeof image);
    FILE *f = tmpfile(); assert(f); assert(fputs(body, f) >= 0); assert(fseek(f, 0, SEEK_SET) == 0);
    if (value) assert(setenv("DM_INTERNAL_RESOLUTION", value, 1) == 0); else assert(unsetenv("DM_INTERNAL_RESOLUTION") == 0);
    char err[256] = {0}; unsigned applied = 99;
    int rc = runtime_patch_apply(f, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 32, image, sizeof image, err, sizeof err, &applied);
    fclose(f); checks++;
    assert((rc == 0) == good); assert(applied == count);
    if (!good || !count) assert(!memcmp(image, original, sizeof image));
    else { assert(image[4] == 0x22 && image[7] == 0x22); assert(image[3] == 0x11 && image[8] == 0x11); }
}
#define HEAD "DMRT1\taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\t20\tDM_INTERNAL_RESOLUTION\nchoice\tdefault\nchoice\t1440p\n"
#define WRITE "write\t1440p\t4\t11111111\t22222222\n"
int main(void) {
    run(HEAD WRITE, NULL, 1, 0); run(HEAD WRITE, "default", 1, 0); run(HEAD WRITE, "1440p", 1, 1);
    run(HEAD WRITE, "4K", 0, 0); run(HEAD WRITE, "", 0, 0);
    run("DMRT1\tbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\t20\tDM_INTERNAL_RESOLUTION\nchoice\tdefault\nchoice\t1440p\n" WRITE, "1440p", 0, 0);
    run("DMRT1\taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\t24\tDM_INTERNAL_RESOLUTION\nchoice\tdefault\nchoice\t1440p\n" WRITE, "1440p", 0, 0);
    run(HEAD "write\t1440p\t20\t11111111\t22222222\n", "1440p", 0, 0); // exact text end
    run(HEAD "write\t1440p\tffffffffffffffff\t11111111\t22222222\n", "1440p", 0, 0);
    run(HEAD "write\t1440p\t10000000000000000\t11111111\t22222222\n", "1440p", 0, 0);
    run(HEAD "write\t1440p\t-4\t11111111\t22222222\n", "1440p", 0, 0);
    run(HEAD "write\t1440p\t 4\t11111111\t22222222\n", "1440p", 0, 0);
    run(HEAD "write\t1440p\t5\t11111111\t22222222\n", "1440p", 0, 0);
    run(HEAD "write\t1440p\t4\t111111\t222222\n", "1440p", 0, 0);
    run(HEAD WRITE "write\t1440p\t8\t00000000\t22222222\n", "1440p", 0, 0); // no partial write
    run(HEAD WRITE "write\t1440p\t8\t00000000\t22222222\n", "default", 0, 0); // validate inactive rows too
    run(HEAD WRITE WRITE, "1440p", 0, 0);
    run(HEAD "write\t1440p\t4\t11111111\t2222222g\n", "1440p", 0, 0);
    run(HEAD "write\t1440p\t4\t11111111\t2222222222\n", "1440p", 0, 0);
    run(HEAD "write\tdefault\t4\t11111111\t22222222\n", "default", 0, 0);
    run(HEAD "write\tundeclared\t4\t11111111\t22222222\n", "default", 0, 0);
    run(HEAD "choice\t1440p\n" WRITE, "1440p", 0, 0);
    run(HEAD WRITE "choice\tnew\n", "1440p", 0, 0);
    run(HEAD "env\t1440p\tDM_FPS_TARGET=120\n", "1440p", 0, 0);
    run("", NULL, 0, 0); run(HEAD, NULL, 0, 0);
    run("DMRT2\taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\t20\tDM_INTERNAL_RESOLUTION\n" WRITE, NULL, 0, 0);
    run(HEAD WRITE "garbage\n", "1440p", 0, 0);
    run(HEAD "choice\tunused\n" WRITE, "1440p", 0, 0);
    assert(unsetenv("DM_INTERNAL_RESOLUTION") == 0); printf("runtime patch: %u cases passed\n", checks); return 0;
}
