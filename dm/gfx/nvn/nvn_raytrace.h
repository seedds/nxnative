// NVN2 ray-tracing CPU ABI and backend entry points. nvn_rt.h is the unrelated
// real-time thread policy. No opaque NVIDIA shader handles or BVH bytes live here.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "nvn.h"

typedef struct prog_t prog_t;
typedef struct { uint64_t address, size, stride; } nvn_rt_table;
typedef struct { int32_t indices[3]; } nvn_rt_group;
_Static_assert(sizeof(nvn_rt_table) == 24 && offsetof(nvn_rt_table, stride) == 16, "NVN2 SBT description ABI");
_Static_assert(sizeof(nvn_rt_group) == 12, "NVN2 shader group is three signed module indices by value");

// Successful initialization owns a real catalog/library and assigns a serial.
// The builder's arrays/control bytes need only survive this synchronous call.
// Both backend calls run with the host submit/program-ownership mutex held.
int mtl_rt_program_set(prog_t *program, int count, const NVNshaderData *modules,
                       uint32_t trace_depth, uint32_t callable_depth, uint32_t flags);
// The backend writes its opaque 16-byte token prefix; the following 16 bytes of
// the device's 32-byte record remain untouched. This API has no return value.
void mtl_rt_group_handle(const prog_t *program, const int32_t indices[3], void *out16);

// Recovered SDK-only signatures absent from the legacy generated nvn_protos.h.
typedef void (*NVNT_nvnProgramBuilderSetDefaults)(void *);
typedef void (*NVNT_nvnProgramBuilderSetDevice)(void *, NVNdevice *);
typedef NVNdevice *(*NVNT_nvnProgramBuilderGetDevice)(const void *);
typedef void (*NVNT_nvnProgramBuilderSetShaders)(void *, int, const NVNshaderData *);
typedef int (*NVNT_nvnProgramBuilderGetShaderCount)(const void *);
typedef const NVNshaderData *(*NVNT_nvnProgramBuilderGetShaders)(const void *);
typedef void (*NVNT_nvnProgramBuilderSetRayTracingModules)(void *, int, const NVNshaderData *);
typedef int (*NVNT_nvnProgramBuilderGetRayTracingModuleCount)(const void *);
typedef const NVNshaderData *(*NVNT_nvnProgramBuilderGetRayTracingModuleData)(const void *);
typedef void (*NVNT_nvnProgramBuilderSetMaxTraceRecursionDepth)(void *, uint32_t);
typedef uint32_t (*NVNT_nvnProgramBuilderGetMaxTraceRecursionDepth)(const void *);
typedef void (*NVNT_nvnProgramBuilderSetMaxCallableRecursionDepth)(void *, uint32_t);
typedef uint32_t (*NVNT_nvnProgramBuilderGetMaxCallableRecursionDepth)(const void *);
typedef void (*NVNT_nvnProgramBuilderSetProgramRayTracingFlags)(void *, uint32_t);
typedef uint32_t (*NVNT_nvnProgramBuilderGetProgramRayTracingFlags)(const void *);
typedef NVNboolean (*NVNT_nvnProgramInitializeFromBuilder)(NVNprogram *, const void *);
typedef void (*NVNT_nvnProgramGetShaderGroupHandle)(const NVNprogram *, nvn_rt_group, void *);
typedef void (*NVNT_nvnCommandBufferDispatchRays)(NVNcommandBuffer *, uint64_t,
    const nvn_rt_table *, const nvn_rt_table *, const nvn_rt_table *, uint32_t, uint32_t, uint32_t);
typedef void (*NVNT_nvnCommandBufferDispatchRaysIndirect)(NVNcommandBuffer *, uint64_t,
    const nvn_rt_table *, const nvn_rt_table *, const nvn_rt_table *, uint64_t);
