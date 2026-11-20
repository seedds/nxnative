// NVN2 acceleration structures. Layouts recovered from the SDK's public entry points,
// not per-game objects. CPU descriptors are read when their command is submitted.
#pragma once
#include <stdint.h>
#include <stddef.h>

// Bezel selects bit0 for its update path and bit1 for the path that requests a
// compacted-size report then compacts. Bits2/3 vary with mesh size; their precise
// quality preference is not established, so Metal uses its default build policy.
enum { NVN_AS_ALLOW_UPDATE = 1, NVN_AS_ALLOW_COMPACTION = 2 };
typedef struct {
    uint32_t type, _pad;
    union {
        struct { uint64_t vertices; uint32_t vertex_count, vertex_format, vertex_stride, _pad;
                 uint64_t indices; uint32_t index_count, index_type, primitive_offset, flags; uint64_t transform; } triangles;
        struct { uint64_t boxes; uint32_t count, stride, flags; uint8_t _pad[36]; } aabbs;
        struct { uint64_t address; uint32_t count; uint8_t _pad[44]; } instances;
    };
} nvn_as_geometry;
typedef struct {
    uint32_t flags, operation;
    const nvn_as_geometry *geometries;
    uint32_t count, _pad;
    uint64_t source, destination, destination_size, scratch, scratch_size;
} nvn_as_build;
typedef struct { uint32_t type, _pad; uint64_t address; } nvn_as_post;
typedef struct { uint64_t count; const nvn_as_build *builds; uint64_t post_count; const nvn_as_post *post; } p_as_build;
typedef struct { uint64_t source, destination; uint32_t mode, _pad; } p_as_copy; // 0 copy, 1 compact
typedef struct { uint32_t stage, index; uint64_t address; } p_bind_as;
_Static_assert(sizeof(nvn_as_geometry) == 64 && sizeof(nvn_as_build) == 64 && sizeof(nvn_as_post) == 16, "NVN2 AS descriptor ABI");
_Static_assert(offsetof(nvn_as_build,geometries)==8 && offsetof(nvn_as_build,count)==0x10 &&
               offsetof(nvn_as_build,source)==0x18 && offsetof(nvn_as_build,destination)==0x20 &&
               offsetof(nvn_as_build,scratch)==0x30,"NVN2 AS build offsets");
_Static_assert(offsetof(nvn_as_geometry,triangles.vertices)==8 && offsetof(nvn_as_geometry,triangles.indices)==0x20 &&
               offsetof(nvn_as_geometry,triangles.flags)==0x34 && offsetof(nvn_as_geometry,triangles.transform)==0x38,
               "NVN2 triangle offsets");

void mtl_as_sizes(const nvn_as_build *build, uint64_t *storage, uint64_t *scratch, uint64_t *update);
uint64_t mtl_as_resource_id(uint64_t address);
uint64_t mtl_as_bound_address(unsigned stage, unsigned slot);

// Acquire at dispatch encoding during an open replay submission. The actual AS
// and immutable SBT-offset buffer stay resident through that submission's fence.
// Offsets are u32s indexed by Metal's source instance_id, not user_instance_id.
// Current-hit IDs and transforms come directly from the Metal query result.
enum { NVN_AS_BINDING_BLAS = 1, NVN_AS_BINDING_TLAS = 2 };
typedef struct {
    uint64_t resource_id, instance_sbt_offsets; // Metal resource ID and GPU address
    uint32_t instance_count, type;
} nvn_as_binding;
_Static_assert(sizeof(nvn_as_binding)==24,"AS dispatch binding ABI");
// Address0 returns0/empty binding; unknown nonzero addresses are explicit errors.
// Encode the dispatch at this acquisition point: a compatible later rebuild may
// mutate the same AS object, while earlier dispatches retain their own SBT data.
int mtl_as_acquire(uint64_t address, nvn_as_binding *binding);
