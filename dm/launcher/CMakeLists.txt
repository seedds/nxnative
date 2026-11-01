# nxlauncher (dm/launcher): the launcher of Windows and Linux, the counterpart of the Mac app's settings window (docs/settings.md, "On Windows and Linux"). Added by
# dm/host/CMakeLists.txt (option NXHOST_LAUNCHER), which has SDL3 (SDL3::SDL3-static, its software 2D renderer), the UTF-8 manifest of Windows programs and the tests
# (t_launcher_model, t_launcher_window). Dear ImGui is vendored unmodified in dm/third_party/imgui (VENDOR.md); cJSON reads the settings list.
set(IMGUI ${REPO}/dm/third_party/imgui)
add_library(nxlauncher_imgui STATIC ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
            ${IMGUI}/backends/imgui_impl_sdl3.cpp ${IMGUI}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(nxlauncher_imgui PUBLIC ${IMGUI} ${IMGUI}/backends)
target_link_libraries(nxlauncher_imgui PUBLIC SDL3::SDL3-static)
target_compile_options(nxlauncher_imgui PRIVATE -O2 -w)   # third-party code, as it is

# The model, no UI: the tests link it too
add_library(nxlauncher_model STATIC launcher_model.cpp ${REPO}/dm/third_party/cjson/cJSON.c)
target_include_directories(nxlauncher_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${REPO}/dm/third_party/cjson)
target_compile_options(nxlauncher_model PRIVATE -O2 $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>)

# The program, beside nxhost-run and the tests (t_launcher_window starts it from there). Windows: a GUI program (no console window), which attaches to its caller's console for
# --list and --command
add_executable(nxlauncher main.cpp ${WIN32_MANIFEST_RC})
target_link_libraries(nxlauncher PRIVATE nxlauncher_model nxlauncher_imgui)
target_compile_options(nxlauncher PRIVATE -O2 -Wall)
set_target_properties(nxlauncher PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
if(WIN32)
    set_target_properties(nxlauncher PROPERTIES WIN32_EXECUTABLE ON)
endif()
set_target_properties(nxlauncher nxlauncher_model nxlauncher_imgui PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
