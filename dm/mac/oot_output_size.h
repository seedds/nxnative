// Largest integer 16:9 picture that fits a physical panel, not a scaled desktop.
#pragma once
#include <stdint.h>

typedef struct { uint32_t width, height; } oot_output_size;

static oot_output_size oot_output_fit(uint32_t width, uint32_t height) {
    if (!width || !height || width > 16384 || height > 16384) return (oot_output_size){0, 0};
    if ((uint64_t)width * 9 <= (uint64_t)height * 16)
        height = (uint32_t)((uint64_t)width * 9 / 16);
    else width = (uint32_t)((uint64_t)height * 16 / 9);
    return width && height ? (oot_output_size){width, height} : (oot_output_size){0, 0};
}
