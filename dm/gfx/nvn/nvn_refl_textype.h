// The Metal texture type numbers the reflection carries in nvn_rbind.dtt (nvn_refl.h), without a Metal header: the values of MTLTextureType, which the plain C reflection parser
// (nvn_refl.c) writes so its structs equal those of the Objective-C one (nvn_refl.m). tests/t_refl_cmp.m checks each against the real constant
#pragma once

enum nvn_mtl_texture_type {
    NVN_MTL_TEX_1D = 0,
    NVN_MTL_TEX_1D_ARRAY = 1,
    NVN_MTL_TEX_2D = 2,
    NVN_MTL_TEX_2D_ARRAY = 3,
    NVN_MTL_TEX_2D_MULTISAMPLE = 4,
    NVN_MTL_TEX_CUBE = 5,
    NVN_MTL_TEX_CUBE_ARRAY = 6,
    NVN_MTL_TEX_3D = 7,
    NVN_MTL_TEX_2D_MULTISAMPLE_ARRAY = 8,
    NVN_MTL_TEX_BUFFER = 9,
};
