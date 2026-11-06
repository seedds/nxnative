// Consumer is a wire fixture, deliberately independent of the producer helpers.
#include "../audio_input_ring.h"
#include <stdio.h>
#include <stdlib.h>
static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main(void) {
    uint8_t *work = aligned_alloc(4096, 16384); memset(work, 0, 16384);
    audio_input_ring r;
    CHECK(air_work_size(480, 4) == 8192);
    CHECK(!air_work_size(UINT32_MAX, 4));
    CHECK(!air_bind(&r, work + 1, 8192, 4, 4, 0));
    CHECK(!air_bind(&r, work, 4096, 4, 4, 0));
    CHECK(!air_bind(&r, work, 8192, 4, 4, 1));
    CHECK(air_bind(&r, work, 8192, 4, 4, 0));
    uint32_t pcm[] = {10, 20, 30, 40, 50, 60}, out[5] = {0};
    CHECK(air_write(&r, pcm, 6, 0) == 4);
    CHECK(*(uint32_t *)(work + 0x1004) == 4 && *(uint32_t *)(work + 0x1000) == 2);
    memcpy(out, work + 0x1040, 16);
    CHECK(out[0] == 10 && out[3] == 40 && out[4] == 0);
    CHECK(*(uint32_t *)(work + 0x44) == 0); // overflow never touches reader
    __atomic_store_n((uint32_t *)(work + 0x44), 3, __ATOMIC_RELEASE);
    CHECK(air_write(&r, pcm + 4, 2, 0) == 2);
    memcpy(out, work + 0x1040, 20);
    CHECK(out[3] == 40 && out[4] == 50 && out[0] == 60);
    CHECK(*(uint32_t *)(work + 0x1004) == 1);
    *(uint32_t *)(work + 0x44) = 5;
    CHECK(air_write(&r, pcm, 1, 0) == 0); // corrupt guest index cannot address outside the ring
    memset(work, 0, 16384);
    CHECK(air_bind(&r, work, 8192, 15, 4, 1));
    CHECK(air_write(&r, pcm, 6, 123456) == 6);
    CHECK(*(uint64_t *)(work + 0x1040 + 5 * 8) == 123456);
    CHECK(*(uint32_t *)(work + 0x10c0) == 10);
    free(work);
    printf("t_audio_input_ring: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
