# Dear ImGui

- What: Dear ImGui, an immediate-mode GUI library in C++, by Omar Cornut and the Dear ImGui contributors: the core files and two of its backends, `backends/imgui_impl_sdl3` (SDL3 input, windows, controllers) and `backends/imgui_impl_sdlrenderer3` (drawing through SDL3's 2D renderer, `SDL_Renderer`).
- Version: 1.91.9b, release tag `v1.91.9b`, commit `f5befd2d29e66809cd1110a152e375a7f1981f06` of https://github.com/ocornut/imgui (the tag's commit, and the commit `git get-tar-commit-id` reads from the release archive).
- License: MIT, in `LICENSE.txt` beside this file (the upstream file, unchanged).
- Changes: none. Every file here is the release archive's, byte for byte. Left out: `imgui_demo.cpp`, `misc/`, `examples/`, `docs/` and the other backends. SHA-256:

  | File | SHA-256 |
  |---|---|
  | `LICENSE.txt` | c80c5789748d955c4a650562baa0d750494e2c7128c2ca66aaebe2e3c4e198bf |
  | `imconfig.h` | 0884061cc43e27e1d346fe18dcd1425792eb2532d3d16610a82eb5ac751f7594 |
  | `imgui.cpp` | af1cc31255fbf58d57ecc9d727cbd39a84e44fa8f11be7fd419fa0100c5255d8 |
  | `imgui.h` | 2d0180859ff85e7fe66ed9999b0f05f2fc45176773046a8f86b3b6bd190b0765 |
  | `imgui_draw.cpp` | 9d2d9ee6745d9b98d7f39a16da8ffcf783f90bb8c9e931038fc8e9a15cbe3d84 |
  | `imgui_internal.h` | 1363c617a743957f367afe1c6c88345870b7aa25ee339809fb915466e4fb9575 |
  | `imgui_tables.cpp` | 4ea380fa9d49cac6cbfbc8b969969f9b0705518ec9ca340681d8c70e3eefbc28 |
  | `imgui_widgets.cpp` | 54ac29dbcbe70091f5e5fc2cb4099e6d0b9d1c939e5158f2be983cb508714414 |
  | `imstb_rectpack.h` | 2efa3d5f7d003c19743b15155dad9f46d9f9fc783a18893d703b431d3c990972 |
  | `imstb_textedit.h` | 77010e494caf72c178b573569612aec409d24380409676325a31d02de0ec64b7 |
  | `imstb_truetype.h` | 87f08919ae8e7223451d0e9d06b62829075d3e920c9e6ad436db9d7e7427ffce |
  | `backends/imgui_impl_sdl3.cpp` | f6973eebab67a6e2c6cad4a495c20d453c714b43ff90d8e444dd88a98c73c7e5 |
  | `backends/imgui_impl_sdl3.h` | a6866cb9f9d9eb69ad64d49075fbcc247cadb9c872fcdfb0569f4b93ccb802a6 |
  | `backends/imgui_impl_sdlrenderer3.cpp` | 410b191de535723a81dde83212b1be88adc2b6d3c61f9a02691c6f4d07a6ec20 |
  | `backends/imgui_impl_sdlrenderer3.h` | 3e356dd91a17f4210e2aab5d4e28aa55adb0c95c26dc87c5dd5a806d46bad52f |

- Downloaded with:

  ```sh
  curl -sSL -o imgui-1.91.9b.tar.gz https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.tar.gz   # SHA-256 8e1bbc76c71d74fef2fb85db7e7ca8eba13d6a86623c54992b60162db554ffdb
  gzip -dc imgui-1.91.9b.tar.gz | git get-tar-commit-id                                                  # f5befd2d29e66809cd1110a152e375a7f1981f06
  tar xzf imgui-1.91.9b.tar.gz
  S=imgui-1.91.9b
  cp $S/LICENSE.txt $S/imconfig.h $S/imgui.cpp $S/imgui.h $S/imgui_draw.cpp $S/imgui_internal.h $S/imgui_tables.cpp $S/imgui_widgets.cpp \
     $S/imstb_rectpack.h $S/imstb_textedit.h $S/imstb_truetype.h dm/third_party/imgui/
  cp $S/backends/imgui_impl_sdl3.cpp $S/backends/imgui_impl_sdl3.h $S/backends/imgui_impl_sdlrenderer3.cpp $S/backends/imgui_impl_sdlrenderer3.h dm/third_party/imgui/backends/
  ```

- Use: the launcher of Windows and Linux, `nxlauncher` (`dm/launcher`, Phase 6 of the port: [docs/design/migration-status.md](../../../docs/design/migration-status.md), the decision "Launcher: Dear ImGui"), built by `dm/launcher/CMakeLists.txt` into the library `nxlauncher_imgui` over the SDL3 that `dm/host/CMakeLists.txt` builds (`SDL3::SDL3-static`). The renderer backend draws with SDL's 2D renderer (the software one in that SDL build), so the launcher opens on a PC without a working Vulkan driver.
