// sysmix's shared PCM ring, independent of the capture device and IPC transport.
// The guest owns the consumer cursor. Publish samples before the producer cursor;
// never move the consumer to make space: overflow drops incoming frames.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>

typedef struct {
    uint32_t *consumer, *producer;
    uint64_t *times;
    uint8_t *samples;
    uint32_t frames, frame_bytes, time_slots;
} audio_input_ring;

static size_t air_work_size(uint32_t capacity, uint32_t frame_bytes) {
    if (!capacity || capacity >= INT_MAX || !frame_bytes || frame_bytes > 24) return 0;
    uint64_t n = (uint64_t)capacity + 1, slots = n < 16 ? n : 16;
    uint64_t size = (((n * frame_bytes + 63) & ~UINT64_C(63)) +
                     ((slots * 8 + 127) & ~UINT64_C(63)) + 0x1fff) & ~UINT64_C(0xfff);
    return size <= SIZE_MAX ? (size_t)size : 0;
}
static uint32_t air_load(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void air_store(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static int air_bind(audio_input_ring *r, void *work, size_t size, uint32_t capacity, uint32_t frame_bytes, int timestamps) {
    size_t need = air_work_size(capacity, frame_bytes);
    if (!work || ((uintptr_t)work & 4095) || (size & 4095) || !need || size < need || (timestamps && capacity < 15)) return 0;
    uint8_t *p = work;
    *r = (audio_input_ring){(uint32_t *)(p + 0x40), (uint32_t *)(p + 0x1000),
                            timestamps ? (uint64_t *)(p + 0x1040) : NULL,
                            p + 0x1040 + (timestamps ? 128 : 0), capacity + 1, frame_bytes, timestamps ? 16u : 0u};
    return air_load(r->consumer + 1) < r->frames && air_load(r->producer + 1) < r->frames;
}
static int air_filled(const audio_input_ring *r, uint32_t *filled) {
    uint32_t rd = air_load(r->consumer + 1), wr = air_load(r->producer + 1);
    if (rd >= r->frames || wr >= r->frames) return 0;
    *filled = wr >= rd ? wr - rd : r->frames - rd + wr;
    return 1;
}
// One serialized producer. The guest's cache maintenance includes the matching
// barriers on Apple CPUs. No allocation, logging or host locks on this path.
static uint32_t air_write(audio_input_ring *r, const void *pcm, uint32_t count, uint64_t time) {
    uint32_t filled;
    if (!air_filled(r, &filled)) return 0;
    uint32_t n = r->frames - 1 - filled;
    if (n > count) n = count;
    uint32_t wr = air_load(r->producer + 1), first = r->frames - wr;
    if (first > n) first = n;
    if (n) {
        memcpy(r->samples + (size_t)wr * r->frame_bytes, pcm, (size_t)first * r->frame_bytes);
        if (first < n) memcpy(r->samples, (const uint8_t *)pcm + (size_t)first * r->frame_bytes, (size_t)(n - first) * r->frame_bytes);
        if (r->times) for (uint32_t k = 0; k < n; k++) {
            uint32_t at = (wr + k) % r->frames;
            __atomic_store_n(r->times + (uint64_t)at * r->time_slots / r->frames, time, __ATOMIC_RELEASE);
        }
        air_store(r->producer + 1, (wr + n) % r->frames);
    }
    air_store(r->producer, air_load(r->producer) + count - n);
    return n;
}
