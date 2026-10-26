// Conservative promotion of storage-buffer write declarations from compiled
// argument-buffer reflection. No game keys, shader rewriting, or GPU commands.
#pragma once
#include "nvn_refl.h"
#include <stddef.h>
#include <stdint.h>

enum { NVN_STORAGE_READ_ONLY = 0, NVN_STORAGE_READ_WRITE = 1, NVN_STORAGE_WRITE_ONLY = 2 };
typedef struct {
    uint64_t aid, offset;
    int is_pointer, has_pointer_type, nested_argument_buffer, access;
} nvn_storage_access_member;
enum {
    NVN_STORAGE_NOT_STORAGE, NVN_STORAGE_BAD_METADATA, NVN_STORAGE_NO_REFLECTION,
    NVN_STORAGE_MEMBER_MISSING, NVN_STORAGE_MEMBER_DUPLICATE, NVN_STORAGE_NOT_POINTER,
    NVN_STORAGE_BAD_OFFSET, NVN_STORAGE_UNKNOWN_ACCESS, NVN_STORAGE_COMPILED_READ_ONLY,
    NVN_STORAGE_COMPILED_WRITE
};
typedef struct {
    int status, access, promoted;
    uint64_t offset;
    unsigned before, after;
} nvn_storage_access_result;

static inline const char *nvn_storage_access_status(int status) {
    switch (status) {
    case NVN_STORAGE_NOT_STORAGE: return "not-storage";
    case NVN_STORAGE_BAD_METADATA: return "metadata-mismatch";
    case NVN_STORAGE_NO_REFLECTION: return "reflection-unavailable";
    case NVN_STORAGE_MEMBER_MISSING: return "member-missing";
    case NVN_STORAGE_MEMBER_DUPLICATE: return "member-duplicate";
    case NVN_STORAGE_NOT_POINTER: return "pointer-type-mismatch";
    case NVN_STORAGE_BAD_OFFSET: return "offset-mismatch";
    case NVN_STORAGE_UNKNOWN_ACCESS: return "unknown-access";
    case NVN_STORAGE_COMPILED_READ_ONLY: return "compiled-read-only";
    case NVN_STORAGE_COMPILED_WRITE: return "compiled-write";
    default: return "invalid-result";
    }
}
static inline nvn_storage_access_result nvn_storage_access_apply(
        nvn_refl *rf, int index, const nvn_storage_access_member *members,
        size_t count, int root_valid, size_t encoded_len) {
    nvn_storage_access_result r = {.status = NVN_STORAGE_BAD_METADATA, .access = -1, .offset = UINT64_MAX};
    if (!rf || rf->nb < 0 || rf->nb > NVN_REFL_MAXB || index < 0 || index >= rf->nb) return r;
    nvn_rbind *b = &rf->b[index]; r.before = r.after = b->rw;
    // Uniform/support/driver/image pointers have different contracts. Only a
    // resolved NVN storage slot may gain Write from this policy.
    if (b->kind != NVN_RB_SBUFP) { r.status = NVN_STORAGE_NOT_STORAGE; return r; }
    if (!rf->route_b || b->reg >= 16 || b->aid >= NVN_REFL_MAXARG || b->n > 1 ||
        !encoded_len || (encoded_len & 7) || b->aid >= encoded_len / 8) return r;
    for (int i = 0; i < rf->nb; i++) if (i != index) {
        const nvn_rbind *other = &rf->b[i]; unsigned n = other->n ? other->n : 1;
        if (b->aid >= other->aid && (unsigned)(b->aid - other->aid) < n) return r;
    }
    if (!root_valid || !members || count > NVN_REFL_MAXARG) { r.status = NVN_STORAGE_NO_REFLECTION; return r; }
    const nvn_storage_access_member *match = NULL;
    for (size_t i = 0; i < count; i++) if (members[i].aid == b->aid) {
        if (match) { r.status = NVN_STORAGE_MEMBER_DUPLICATE; return r; }
        match = &members[i];
    }
    if (!match) { r.status = NVN_STORAGE_MEMBER_MISSING; return r; }
    r.access = match->access; r.offset = match->offset;
    if (!match->is_pointer || !match->has_pointer_type || match->nested_argument_buffer) {
        r.status = NVN_STORAGE_NOT_POINTER; return r;
    }
    // This backend's already-validated route-B ABI writes raw 8-byte slots at
    // aid*8. Argument ID is the identifier; byte offset is a separate ABI guard.
    if (match->offset != (uint64_t)b->aid * 8 || match->offset > encoded_len - 8) {
        r.status = NVN_STORAGE_BAD_OFFSET; return r;
    }
    if (match->access == NVN_STORAGE_READ_WRITE || match->access == NVN_STORAGE_WRITE_ONLY) {
        r.status = NVN_STORAGE_COMPILED_WRITE;
        if (!b->rw) { b->rw = 1; r.promoted = 1; }
        r.after = b->rw;
    } else r.status = match->access == NVN_STORAGE_READ_ONLY ? NVN_STORAGE_COMPILED_READ_ONLY : NVN_STORAGE_UNKNOWN_ACCESS;
    // Missing, unknown, or read-only reflection never downgrades original rw.
    return r;
}
