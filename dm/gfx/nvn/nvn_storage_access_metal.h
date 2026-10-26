// Public iOS 11+ compiled-pointer reflection, collected once at shader load.
// The reflection-taking overload is deprecated, but remains available on iOS16.
#pragma once
#import <Metal/Metal.h>
#include "nvn_storage_access.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef NVN_STORAGE_ACCESS_LOG
#define NVN_STORAGE_ACCESS_LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define NVN_STORAGE_ACCESS_LOCAL_LOG 1
#endif

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
_Static_assert(MTLArgumentAccessReadOnly == NVN_STORAGE_READ_ONLY &&
    MTLArgumentAccessReadWrite == NVN_STORAGE_READ_WRITE &&
    MTLArgumentAccessWriteOnly == NVN_STORAGE_WRITE_ONLY, "Metal access enum changed");

static id<MTLArgumentEncoder> nvn_storage_access_encoder(
        id<MTLFunction> fn, NSUInteger abi, MTLArgument **reflection) {
    id<MTLArgumentEncoder> encoder = [fn newArgumentEncoderWithBufferIndex:abi reflection:reflection];
    if (!encoder) {
        if (reflection) *reflection = nil;
        encoder = [fn newArgumentEncoderWithBufferIndex:abi];
    }
    return encoder;
}
static pthread_once_t g_storage_access_diag_once = PTHREAD_ONCE_INIT;
static char g_storage_access_diag_prefix[24];
static _Atomic unsigned g_storage_access_notes, g_storage_access_diag_notes;
static void nvn_storage_access_diag_init(void) {
    const char *s = getenv("DM_NVN_STORAGE_ACCESS_DIAG");
    if (!s) return;
    size_t n = strlen(s); if (n >= sizeof g_storage_access_diag_prefix) n = sizeof g_storage_access_diag_prefix - 1;
    memcpy(g_storage_access_diag_prefix, s, n);
}
static void nvn_storage_access_note(const char *key, int stage, const nvn_rbind *b, nvn_storage_access_result r) {
    pthread_once(&g_storage_access_diag_once, nvn_storage_access_diag_init);
    const char *prefix = g_storage_access_diag_prefix;
    int selected = *prefix && (!strcmp(prefix, "1") || (key && !strncmp(key, prefix, strlen(prefix))));
    _Atomic unsigned *budget = selected ? &g_storage_access_diag_notes : &g_storage_access_notes;
    unsigned note = atomic_load_explicit(budget, memory_order_relaxed);
    do { if (note >= 128) return; }
    while (!atomic_compare_exchange_weak_explicit(budget, &note, note + 1, memory_order_relaxed, memory_order_relaxed));
    NVN_STORAGE_ACCESS_LOG("[storage-access] key=%s stage=%d kind=%u reg=%u aid=%u offset=%llu access=%d rw=%u->%u promoted=%d result=%s",
        key ? key : "-", stage, b->kind, b->reg, b->aid, (unsigned long long)r.offset,
        r.access, r.before, r.after, r.promoted, nvn_storage_access_status(r.status));
}
static unsigned nvn_storage_access_promote(nvn_refl *rf, MTLArgument *argument,
        NSUInteger encoded_len, const char *key, int stage) {
    if (!rf || rf->nb < 0 || rf->nb > NVN_REFL_MAXB) return 0;
    nvn_storage_access_member members[NVN_REFL_MAXARG]; size_t count = 0;
    MTLStructType *type = argument.bufferStructType;
    BOOL valid = argument && argument.type == MTLArgumentTypeBuffer && rf->abi >= 0 &&
        argument.index == (NSUInteger)rf->abi && argument.bufferDataType == MTLDataTypeStruct &&
        type && type.members.count <= NVN_REFL_MAXARG;
    if (valid) for (MTLStructMember *member in type.members) {
        MTLPointerType *p = member.dataType == MTLDataTypePointer ? member.pointerType : nil;
        members[count++] = (nvn_storage_access_member){.aid = member.argumentIndex, .offset = member.offset,
            .is_pointer = member.dataType == MTLDataTypePointer, .has_pointer_type = p != nil,
            .nested_argument_buffer = p ? p.elementIsArgumentBuffer : 0, .access = p ? (int)p.access : -1};
    }
    // Never inspect argument.access here. The outer AB is constant/read-only;
    // the pointee of a device pointer inside it may still be written.
    unsigned promoted = 0;
    for (int i = 0; i < rf->nb; i++) if (rf->b[i].kind == NVN_RB_SBUFP) {
        nvn_storage_access_result r = nvn_storage_access_apply(rf, i, members, count, valid, encoded_len);
        promoted += (unsigned)r.promoted;
        nvn_storage_access_note(key, stage, &rf->b[i], r);
    }
    return promoted;
}
#pragma clang diagnostic pop
#ifdef NVN_STORAGE_ACCESS_LOCAL_LOG
#undef NVN_STORAGE_ACCESS_LOCAL_LOG
#undef NVN_STORAGE_ACCESS_LOG
#endif
