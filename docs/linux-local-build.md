# Experimental local Linux build

This is an unofficial build route for an unsigned iOS IPA. It uses xtool's SwiftPM app builder on Linux and compiles the packaged Metal source on the iPhone at runtime. It does not use a remote build service.

The route is experimental and has not yet been verified by building and installing an IPA. Keep the archive, SDK, game files and `prod.keys` on your own machine.

**Apple SDK terms:** xtool's Linux setup extracts an iOS SDK from `Xcode.xip`, but Apple's [Xcode and Apple SDKs Agreement](https://www.apple.com/legal/sla/docs/xcode.pdf) says Apple software is authorized to run on Apple-branded computers and prohibits installing or using it on non-Apple hardware (sections 2.2 and 2.7). This is a technical experiment, not an Apple-authorized Linux workflow; check the agreement that applies to you before using it.

## Requirements

- Linux x86-64 (the first supported build target) with Python 3.12 or newer and Swift 6.4 or newer.
- A local `Xcode.xip` archive and [xtool](https://github.com/xtool-org/xtool). Install the Darwin SDK once with `xtool sdk install /path/to/Xcode.xip`; `xtool sdk status` should report it installed.
- .NET SDK 10, CMake, Ninja, Clang, Git, `glslangValidator`, `zstd`, `ldid` and one of the common Linux TTF fonts (DejaVu Sans, Liberation Sans or Noto Sans).
- Python modules `lz4`, `capstone`, `fontTools` and `backports.zstd` (before Python 3.14), plus `nsz` if the input folder contains compressed NSZ/XCZ files.
- A supported game's NSP folder and its `prod.keys` file.

The Linux pipeline builds the shader translator and pinned SPIRV-Cross under the game's work folder. It downloads the pinned SPIRV-Cross source on the first build; later runs reuse the local build.

## Build

If xtool is not on `PATH`, point `XTOOL` at its executable or AppImage launcher:

```sh
XTOOL=/path/to/xtool python3 scripts/build_ipa_from_nsp.py /path/to/game -k /path/to/prod.keys
```

The IPA is written beside the NSP files by default. Sideload it on the iPhone. The first launch compiles MSL on the device and can take significantly longer than a normal precompiled-shader build. Metal caches device-side shader work, but this route has not verified how much of the MSL compilation NxNative can reuse across launches.

## Current limits

- Static shader inputs can be translated for NVN, Vulkan and OpenGL. Games that compile shaders dynamically and need the Mac host to capture them stop with a specific error.
- Unreal/Oodle shader decoding, Mac-host heap/address measurements and offline Metal 4 pipeline archives are not available in this local route. A provisional game profile may therefore produce an IPA that does not boot.
- The Metal source compiler and runtime APIs on the iPhone are not equivalent to Apple's offline `metal`/`metallib` toolchain. Rendering, startup time and game compatibility need per-game validation.
- The IPA is unsigned apart from an ad-hoc executable signature carrying entitlements. A sideloading tool and any required device provisioning remain necessary.

Report a reproducible Linux build failure with the game's graphics API, the log under `<title ID>_build/logs/`, the Linux distribution and the xtool/SDK versions. Never include `prod.keys`, decrypted game files or account credentials in an issue.
