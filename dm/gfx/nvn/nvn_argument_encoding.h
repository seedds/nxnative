#pragma once
// Tier 1 argument buffers have a GPU-private layout. Populate their public
// encoder instead of assuming an eight-byte pointer/resource-ID representation.
#import <Metal/Metal.h>
#include "nvn_refl.h"
#include <stdio.h>
#include <string.h>

static int nvn_argument_encode(id<MTLArgumentEncoder> encoder, id<MTLBuffer> destination,
        NSUInteger offset, const nvn_refl *reflection, const uint64_t *slots,
        size_t count, char *error, size_t error_size) {
    if (!encoder || !destination || !reflection || !slots || reflection->nb < 0 || reflection->nb > NVN_REFL_MAXB ||
        offset > destination.length || encoder.encodedLength > destination.length - offset ||
        (encoder.alignment && offset % encoder.alignment)) {
        snprintf(error, error_size, "invalid encoder, destination, alignment or reflection"); return 0;
    }
    @try {
        [encoder setArgumentBuffer:destination offset:offset];
        for (int i = 0; i < reflection->nb; i++) {
            const nvn_rbind *binding = &reflection->b[i];
            unsigned n = binding->n ? binding->n : 1;
            if (binding->aid >= count || n > count - binding->aid) {
                snprintf(error, error_size, "argument index %u length %u exceeds slot capacity %zu", binding->aid, n, count); return 0;
            }
            for (unsigned element = 0; element < n; element++) {
                NSUInteger index = binding->aid + element;
                uint64_t token = slots[index];
                switch (binding->kind) {
                case NVN_RB_CBUF: case NVN_RB_SBUFP: case NVN_RB_SBUFU: case NVN_RB_UBOP:
                case NVN_RB_SUPPORT: case NVN_RB_C1: case NVN_RB_DRV0: case NVN_RB_DRV2:
                case NVN_RB_IMGA: case NVN_RB_GEOMETRY: {
                    NSUInteger buffer_offset = 0;
                    id<MTLBuffer> buffer = legacy_buffer_at(token, &buffer_offset);
                    if (token && !buffer) { snprintf(error, error_size, "unregistered buffer token at argument %lu", (unsigned long)index); return 0; }
                    [encoder setBuffer:buffer offset:buffer_offset atIndex:index];
                    break;
                }
                case NVN_RB_TEX: case NVN_RB_TEXU: case NVN_RB_IMG: case NVN_RB_TBUF:
                case NVN_RB_TEXD: case NVN_RB_TEXA:
                    [encoder setTexture:(__bridge id<MTLTexture>)(void *)(uintptr_t)token atIndex:index]; break;
                case NVN_RB_SAMP: case NVN_RB_SAMPT: case NVN_RB_SAMPU: case NVN_RB_SAMPI:
                case NVN_RB_SAMPD: case NVN_RB_SAMPA:
                    [encoder setSamplerState:(__bridge id<MTLSamplerState>)(void *)(uintptr_t)token atIndex:index]; break;
                default:
                    snprintf(error, error_size, "unsupported binding kind %u at argument %lu", binding->kind, (unsigned long)index); return 0;
                }
            }
        }
    } @catch (NSException *exception) {
        snprintf(error, error_size, "%s: %s", exception.name.UTF8String, exception.reason.UTF8String); return 0;
    }
    return 1;
}

// Use argumentIndex rather than fixed byte offsets when reading the compiled
// pointer access on a device whose argument layout is private.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static void nvn_argument_private_access(nvn_refl *reflection, MTLArgument *argument) {
    if (!argument || argument.type != MTLArgumentTypeBuffer || argument.index != (NSUInteger)reflection->abi ||
        argument.bufferDataType != MTLDataTypeStruct) return;
    for (MTLStructMember *member in argument.bufferStructType.members) {
        if (member.dataType != MTLDataTypePointer) continue;
        MTLPointerType *pointer = member.pointerType;
        if (!pointer || pointer.elementIsArgumentBuffer ||
            (pointer.access != MTLArgumentAccessReadWrite && pointer.access != MTLArgumentAccessWriteOnly)) continue;
        for (int i = 0; i < reflection->nb; i++) if (reflection->b[i].kind == NVN_RB_SBUFP &&
            reflection->b[i].aid == member.argumentIndex) reflection->b[i].rw = 1;
    }
}
#pragma clang diagnostic pop
