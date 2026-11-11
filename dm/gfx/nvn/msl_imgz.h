// A compressed file inside a shader library image (dm/tools/shader_lib.py compress): the file's bytes are a 16-byte header {"DMZ1", u32 libcompression algorithm, u64 size
// of the original} and the algorithm's raw stream. A metallib starts with "MTLB" and a json with "{", so the header tells the two kinds apart in any image, and an image may mix them
// (a partial compress, or files a later pack took from a folder). The host decodes a file when it loads it: Metal keeps the bytes it was given for as long as the library lives, so
// a decoded library costs its size in the process footprint where a mapped one costs page cache. Nothing is cached here: shader_load loads a shader's library once, and the prebuild, which loads
// a pair's libraries for each pair, is counted in the decode totals the host logs (on a 36,779-shader library: 2,048+ decodes, 0.3-0.5 s in all, off the submit thread).
// libcompression is in every macOS and iOS, so no codec is vendored (nvn_capture.c links it the same way). Measured on a 147,000-file library (20.7 GB): LZBITMAP
// 1.8-1.9x on metallibs at ~5 GB/s decode (8 µs a typical library), LZFSE 7.7x on the .metal sources the host never reads.
#pragma once
#include <compression.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
__asm__(".linker_option \"-lcompression\"");

#define MSL_IMGZ_HEAD 16
#define MSL_IMGZ_MAX (1ull << 30)   // no shader file is near this; a larger size is a damaged header

static int msl_imgz_is(const void *p, size_t n) { return n >= MSL_IMGZ_HEAD && !memcmp(p, "DMZ1", 4); }

static int msl_imgz_alg_ok(uint32_t alg) {
    return alg == COMPRESSION_LZ4_RAW || alg == COMPRESSION_LZFSE || alg == COMPRESSION_LZBITMAP || alg == COMPRESSION_ZLIB;
}

// The original bytes of a DMZ1 file, malloc'd (the caller frees), and their size in *out_len. NULL with a reason in *why for a header out of range or a stream that does not
// decode to exactly the size the header names
static void *msl_imgz_decode(const void *p, size_t n, size_t *out_len, const char **why) {
    uint32_t alg; uint64_t raw;
    if (!msl_imgz_is(p, n)) { *why = "not a DMZ1 file"; return NULL; }
    memcpy(&alg, (const char *)p + 4, 4); memcpy(&raw, (const char *)p + 8, 8);
    if (!msl_imgz_alg_ok(alg)) { *why = "unknown compression algorithm"; return NULL; }
    if (!raw || raw > MSL_IMGZ_MAX) { *why = "original size out of range"; return NULL; }
    // One byte of room past the original: a stream that would decode to more than the header says fills it, and is refused
    uint8_t *o = malloc((size_t)raw + 1);
    if (!o) { *why = "out of memory"; return NULL; }
    size_t got = compression_decode_buffer(o, (size_t)raw + 1, (const uint8_t *)p + MSL_IMGZ_HEAD, n - MSL_IMGZ_HEAD, NULL, (compression_algorithm)alg);
    if (got != raw) { free(o); *why = "the stream does not decode to the size its header names"; return NULL; }
    *out_len = (size_t)raw;
    return o;
}
