#include "../codec_cadence.h"
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
static _Atomic uint64_t published;
static _Atomic int holder_ready, reader_done;
static pthread_mutex_t conversion = PTHREAD_MUTEX_INITIALIZER;
static void *held_conversion(void *unused) {
    (void)unused; pthread_mutex_lock(&conversion);
    atomic_store(&holder_ready, 1);
    while (!atomic_load(&reader_done)) { /* emulate conversion holding the decoder lock */ }
    pthread_mutex_unlock(&conversion); return NULL;
}
int main(void) {
    codec_cadence s = {0};
    for (int i = 0; i < 4; i++) codec_cadence_note(&s, (int64_t)i * 16666667, (uint64_t)i * 33333333);
    assert(codec_cadence_60(&s, 100000000));
    assert(!codec_cadence_60(&s, s.seen + 500000000));
    uint64_t seen = s.seen; codec_cadence_note(&s, s.pts, seen + 400000000);
    assert(s.seen == seen); // repeated properties do not keep a stopped movie alive
    codec_cadence_note(&s, s.pts + 33333333, seen + 450000000);
    assert(!codec_cadence_60(&s, seen + 450000000));
    codec_cadence_note(&s, 0, seen + 450000001);
    assert(!s.samples);
    for (int i = 1; i < 5; i++) codec_cadence_note(&s, i * 33333333LL, seen + 450000001 + i);
    assert(!codec_cadence_60(&s, seen + 450000100));
    codec_cadence active = {0};
    for (int i = 0; i < 4; i++) codec_cadence_note(&active, i * 16666667LL, 1000000000ull + i);
    codec_cadence_publish(&published, &active, 1);
    assert(codec_cadence_published_60(&published, active.seen));
    assert(!codec_cadence_published_60(&published, active.seen - 1));
    assert(!codec_cadence_published_60(&published, active.seen + 500000000));
    uint64_t published_time = active.seen;
    codec_cadence_note(&active, active.pts, active.seen + 499999999);
    codec_cadence_publish(&published, &active, 1);
    assert(atomic_load(&published) == published_time); // polling cannot renew expiry
    codec_cadence_publish(&published, &active, 0);
    assert(!codec_cadence_published_60(&published, active.seen)); // non-AV1 / closing
    codec_cadence_publish(&published, &active, 1);
    pthread_t held; assert(!pthread_create(&held, NULL, held_conversion, NULL));
    while (!atomic_load(&holder_ready)) {}
    for (int i = 0; i < 100000; i++) assert(codec_cadence_published_60(&published, active.seen));
    atomic_store(&reader_done, 1); assert(!pthread_join(held, NULL));
    active = (codec_cadence){0}; codec_cadence_publish(&published, &active, 1);
    assert(!codec_cadence_published_60(&published, published_time)); // Stop/Flush/slot reuse
    puts("cached publication: concurrent locked conversion, expiry, repeat and reset pass");
    puts("codec cadence: 60-fps recognition, expiry, repeat reads, seek and 30-fps rejection pass");
}
