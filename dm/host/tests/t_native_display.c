// Standalone: cc -std=c11 -Wall -Wextra -Werror -pthread ../native_display.c t_native_display.c -lm -o /tmp/t_native_display
#include "../native_display.h"
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>

static void *publish_many(void *unused) {
    (void)unused;
    for (int i = 0; i < 20000; ++i)
        assert(dm_native_display_publish(i & 1 ? 951 : 678, i & 1 ? 669 : 466, 3));
    return NULL;
}

int main(int argc, char **argv) {
    dm_native_display_request request;
    dm_native_display_render render;
    assert(!dm_native_display_snapshot(&request));
    assert(!dm_native_display_freeze(0, &render));
    assert(!dm_native_display_publish(NAN, 669, 3));
    assert(!dm_native_display_publish(951, 0, 3));
    assert(!dm_native_display_publish(INFINITY, 669, 3));
    assert(!dm_native_display_publish(951, 669, -1));
    assert(!dm_native_display_publish(951, 669, 0.0001));
    assert(dm_native_display_publish(951, 669, 3));
    assert(dm_native_display_snapshot(&request));
    assert(request.pixel_width == 2853 && request.pixel_height == 2007);
    uint64_t generation = request.generation;
    assert(dm_native_display_publish(669, 951, 3));
    assert(dm_native_display_snapshot(&request) && request.generation == generation);
    if (argc > 2) {
        assert(dm_native_display_publish(678, 466, 3));
        assert(dm_native_display_snapshot(&request));
        generation = request.generation;
    }
    double startup_aspect = argc > 2 ? 678.0 / 466 : 951.0 / 669;
    unsigned height = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 0;
    assert(dm_native_display_freeze(height, &render));
    unsigned expected = !height ? 720 : height < 360 ? 360 : height > 2160 ? 2160 : height;
    assert(render.height == expected);
    assert(fabs(render.width - expected * startup_aspect) <= 1.0);
    assert(render.aspect == (double)render.width / render.height);
    assert(!(render.width & 1));
    if (!height) assert(render.width == (argc > 2 ? 1048 : 1024));
    assert(dm_native_display_publish(678, 466, 3));
    assert(dm_native_display_snapshot(&request));
    assert(request.generation == generation + (argc > 2 ? 0 : 1) && request.aspect == 678.0 / 466);
    assert(request.pixel_width == 2034 && request.pixel_height == 1398);
    assert(!dm_native_display_publish(0, 0, 3));
    assert(dm_native_display_snapshot(&request) && request.aspect == 678.0 / 466);
    assert(dm_native_display_freeze(1080, &render) && render.scene.generation == generation);
    pthread_t writer;
    assert(!pthread_create(&writer, NULL, publish_many, NULL));
    for (int i = 0; i < 20000; ++i) {
        assert(dm_native_display_snapshot(&request));
        assert(request.width_points == 951 || request.width_points == 678);
        assert(request.height_points == (request.width_points == 951 ? 669 : 466));
        assert(request.pixel_width == request.width_points * 3);
        assert(request.aspect == request.width_points / request.height_points);
        assert(dm_native_display_frozen(&render) && render.scene.generation == generation);
    }
    assert(!pthread_join(writer, NULL));
    assert(!dm_native_display_snapshot(NULL));
    assert(!dm_native_display_freeze(0, NULL));
    assert(!dm_native_display_frozen(NULL));
    return 0;
}
