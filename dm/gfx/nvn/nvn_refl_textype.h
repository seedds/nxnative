// The texture types the shader reflection gives a binding's dummy texture (nvn_rbind.dtt, nvn_refl.h): the kind of texture an MSL binding type names (texture2d_array, depthcube,
// texture_buffer, ...). The frontend's own enum, readable without any backend header (step M6). The values follow the order in which Metal numbers its texture types, so the Metal
// backend's mapping is exact (mtl_tex_type in nvn_mtl.m, which checks each value against Metal's own constant) and a reflection struct holds the bytes it held before the enum
// was neutral; another backend maps them to its own types (a Vulkan image view type)
#pragma once

enum nvn_dtt {
    NVN_DTT_1D = 0,
    NVN_DTT_1D_ARRAY = 1,
    NVN_DTT_2D = 2,
    NVN_DTT_2D_ARRAY = 3,
    NVN_DTT_2D_MULTISAMPLE = 4,
    NVN_DTT_CUBE = 5,
    NVN_DTT_CUBE_ARRAY = 6,
    NVN_DTT_3D = 7,
    NVN_DTT_2D_MULTISAMPLE_ARRAY = 8,
    NVN_DTT_BUFFER = 9,
};
