// Copyright (c) 2026 MacNativeGames contributors (github.com/ly05010419/MacNativeGames), MIT License; the license text is in dm/third_party/macnativegames/LICENSE.
// MetalFX frame interpolation modes (adapted from MacNativeGames' totk_mfx_mode.h), shared by nvn_mfx.m (Objective-C) and the pacing in hle_nvn.c (C).
#pragma once
enum { MFX_OFF, MFX_30, MFX_HALF, MFX_MAX, MFX_NMODES };
int mfx_enabled(void);   // the mode of this game frame (0 = off)
