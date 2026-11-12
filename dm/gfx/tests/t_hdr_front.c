// Strong frontend capability for graphics/HLE fixtures; no real layer is created.
#include <stdlib.h>
#include <string.h>
int front_hdr_enabled(void) {
    const char *value = getenv("DM_TEST_HDR");
    return value && !strcmp(value, "1");
}
