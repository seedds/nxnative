// ASTC LDR decoder in plain C (docs/design/vulkan-backend.md 5.2, decision D1): the reference the Vulkan backend's GPU decode (dm/gfx/hostvk/shaders/astc_decode.comp) is checked
// against bit for bit, and its CPU fallback. It follows the decoding procedure of the Khronos Data Format Specification's ASTC chapter (block modes, partitions, the integer sequence
// encoding, the LDR colour endpoint modes, weight unquantization and infill, dual planes, void-extent blocks, sRGB), for the 2D block sizes NVN has (4x4 to 12x12), first-party code.
// Output: RGBA8, the specification's 8-bit decode (the top 8 bits of the 16-bit interpolation result, as VK_EXT_astc_decode_mode's R8G8B8A8_UNORM; sRGB endpoints expanded as
// (e << 8) | 0x80, the result stored as the sRGB-encoded byte). Errors give the error colour, opaque magenta: an illegal block in every texel, an HDR endpoint mode (LDR profile)
// in the texels of its partition only, as the specification's list of illegal encodings says.
// The decode is texel by texel from a decoded block header (astc_hdr), the shape the GPU shader has, so the two can share each step.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { ASTC_NORMAL = 0, ASTC_VOID_EXTENT = 1, ASTC_ERROR = 2 };

// A block's header: everything a texel's decode needs beyond the block's bits and the texel's position
typedef struct {
    int kind;                 // ASTC_NORMAL, ASTC_VOID_EXTENT, ASTC_ERROR
    const char *why;          // for an error block: which rule it breaks
    uint16_t ve[4];           // void-extent: the constant colour (UNORM16)
    int gw, gh, dual, wr;     // weight grid, dual plane, weight ISE range (0..11)
    int wbits;                // the weight sequence's bit count (it ends at bit 127, read bit-reversed)
    int parts, seed, ccs;     // partitions (1..4), partition pattern index, the dual plane's component
    int cem[4];               // colour endpoint mode of each partition
    int cstart, cr, cbits;    // colour sequence: first bit, ISE range (4..20), bit count
    int nvals;                // colour values of all partitions
} astc_hdr;

// The header of block b (16 bytes, little endian) for block size bw x bh; returns h->kind
int astc_header(const uint8_t b[16], int bw, int bh, astc_hdr *h);
// Texel (x, y) of the block into rgba[4]; returns 1 when the texel has the error colour
int astc_texel(const uint8_t b[16], const astc_hdr *h, int bw, int bh, int srgb, int x, int y, uint8_t rgba[4]);
// The whole block into dst (bw x bh texels, rows pitch bytes apart); returns its kind
int astc_decode_block(const uint8_t b[16], int bw, int bh, int srgb, uint8_t *dst, size_t pitch);
// An image of w x h texels from linear blocks (rows of blocks src_pitch bytes apart) into RGBA8 rows dst_pitch apart; texels past w and h of the last blocks are dropped
void astc_decode_image(const uint8_t *src, size_t src_pitch, uint32_t w, uint32_t h, int bw, int bh, int srgb, uint8_t *dst, size_t dst_pitch);

// The steps, for tests (dm/gfx/tex/tests/t_astc.c) and the synthetic block writer they share with the GPU test (tests/astc_synth.h)
// ISE ranges 0..20 have astc_levels(r) levels: 2 3 4 5 6 8 10 12 16 20 24 32 40 48 64 80 96 128 160 192 256; weights use 0..11, colours 4..20
int astc_levels(int range);
int astc_range_bits(int range, int *kind);   // the plain bits of each value; *kind: 0 bits only, 1 a trit, 2 a quint
int astc_ise_bits(int count, int range);     // bits of a sequence of count values
void astc_trits(int T, int t[5]);            // the five trits of the 8-bit packing T
void astc_quints(int Q, int q[3]);           // the three quints of the 7-bit packing Q
// Value i of a sequence at bits [start, end) of the 128-bit block (words w, read upward); bits at end or past it read 0
int astc_ise_value(const uint32_t w[4], int start, int end, int range, int i);
int astc_unq_color(int range, int v);        // 0..255
int astc_unq_weight(int range, int v);       // 0..64
// The 2D block mode's weight grid, dual plane and weight range; 0 for a reserved mode (and for void-extent, 0x1FC in the low 9 bits)
int astc_block_mode(int mode, int *gw, int *gh, int *dual, int *wr);
int astc_partition(int seed, int parts, int x, int y, int small_block);   // small_block: fewer than 31 texels
// The endpoints of colour endpoint mode cem from its unquantized values v (2 to 8); returns 0 for an HDR mode (the error colour in LDR)
int astc_endpoints(int cem, const int v[8], int e0[4], int e1[4]);
// The weight of texel (x, y) of plane p from the grid's unquantized weights (plane-interleaved when dual)
int astc_infill(const int *wq, int gw, int gh, int dual, int plane, int bw, int bh, int x, int y);
