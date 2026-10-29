# volk

- What: volk, a meta-loader for Vulkan in one C file (`volk.c`, `volk.h`), by Arseny Kapoulkine. It loads the system's Vulkan loader at run time (`libvulkan.so.1` on Linux, `vulkan-1.dll` on Windows) and the Vulkan functions into global pointers; `volkLoadDevice` loads the device functions straight from the driver, which saves the loader's dispatch on every command.
- Version: 1.4.350, release tag `1.4.350` (published 2026-05-04; the commit is of 2026-05-01), commit `3ca312a4f38baa63d8006b6905abbeeb89c8087d` of https://github.com/zeux/volk. It was the newest release at least two months old when it was chosen (2026-10-29): the SDK-sync tags `vulkan-sdk-1.4.357.0` (2026-07-18) and `vulkan-sdk-1.4.363.0` (2026-09-18) are not releases, and the second is younger than two months. It is generated for Vulkan headers 1.4.350 and built against the vendored 1.4.363 (`dm/third_party/vulkan-headers`): volk guards every extension it loads with its header macro, so newer headers only leave the newest functions unloaded.
- License: MIT, in `LICENSE.md` beside this file (the upstream file, unchanged; the same text ends `volk.h`).
- Changes: none. `volk.c`, `volk.h` and `LICENSE.md` are the files of the release archive, byte for byte (SHA-256: `volk.c` 698571dde0c43bc296ab5997ffebf5d8176a9d9e586894590069dbae5ace3c68, `volk.h` 479bab725a424d54715fe9e54b1a69b344a9c36fb617c9aa2cc4289814d8acdf, `LICENSE.md` 04a0693a84f19e53d281ca98bbb0c86ca77251ab13769c6168e6684feb9a1436).
- Downloaded with:

  ```sh
  curl -sL -o volk.tar.gz https://github.com/zeux/volk/archive/refs/tags/1.4.350.tar.gz   # SHA-256 e47c6efe5294bb03729a976b385864bba5daf46ec60f0bdea11e1d1446345f9a
  tar xzf volk.tar.gz
  cp volk-1.4.350/volk.c volk-1.4.350/volk.h volk-1.4.350/LICENSE.md dm/third_party/volk/
  ```

- Use: the Vulkan backend of the Windows and Linux port, `dm/gfx/hostvk` (docs/design/vulkan-backend.md, decision D9: volk rather than a loader of our own). Compiled with `VK_NO_PROTOTYPES` and the include path `dm/third_party/vulkan-headers/include`; nothing links against the Vulkan loader, so a program starts on a machine without Vulkan and reports it. `dm/host/CMakeLists.txt` builds `volk.c` into the hostvk tests (`t_vk_device`, `t_vk_import`) for Linux and Windows; the Apple builds do not use it.
- Not vendored with it: AMD's Vulkan Memory Allocator (decision D2: VMA behind a C wrapper), which comes with the backend's memory layer, not with this device layer.
