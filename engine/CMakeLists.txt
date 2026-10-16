# scuba-engine native runtime. Layout and binding rules: docs/engine/architecture.md.
#
#   scuba_engine_core      portable: job system, log, VFS (dir/zip/scpak/memory + sco::vfs overlay),
#                          model decode and the async model loader. No window, no GPU.
#   scuba_engine_sco       the sco-core host: kernel boot, tick, plugin + widget panel access.
#   scuba_engine_runtime   Windows: Win32 window, D3D12 renderer, ImGui shell, input router.
#   scuba-engine           the editor app (Windows).
#   scuba-pack             builds and verifies .scpak containers.

set(SCUBA_ENGINE_INCLUDE "${PROJECT_SOURCE_DIR}/include")

add_library(scuba_engine_core STATIC
  src/core/log.cpp
  src/core/job_system.cpp
  src/vfs/os_file.cpp
  src/vfs/common.cpp
  src/vfs/directory_fs.cpp
  src/vfs/zip_fs.cpp
  src/vfs/pack_fs.cpp
  src/vfs/virtual_file_system.cpp
  src/asset/decode.cpp
  src/asset/model_loader.cpp
  src/render/camera.cpp
  src/render/picking.cpp)
target_include_directories(scuba_engine_core
  PUBLIC "${SCUBA_ENGINE_INCLUDE}"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# scuba_assets: the bounded OBJ/CGF readers and the JSON parser the glTF decoder uses.
target_link_libraries(scuba_engine_core PUBLIC sco_vfs Threads::Threads PRIVATE scuba_assets scuba_miniz)
scuba_warnings(scuba_engine_core)

add_executable(scuba-pack tools/scuba_pack.cpp)
target_link_libraries(scuba-pack PRIVATE scuba_engine_core)
scuba_warnings(scuba-pack)

add_library(scuba_engine_sco STATIC src/sco/sco_host.cpp)
target_include_directories(scuba_engine_sco PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(scuba_engine_sco PUBLIC scuba_engine_core PRIVATE scuba_kernel)
target_compile_definitions(scuba_engine_sco PRIVATE SCUBA_VERSION="${PROJECT_VERSION}"
  SCUBA_SDK_VERSION_STRING="${SCUBA_SDK_VERSION}")
scuba_warnings(scuba_engine_sco)

if(WIN32)
  add_library(scuba_imgui STATIC
    "${SCUBA_IMGUI_DIR}/imgui.cpp"
    "${SCUBA_IMGUI_DIR}/imgui_draw.cpp"
    "${SCUBA_IMGUI_DIR}/imgui_tables.cpp"
    "${SCUBA_IMGUI_DIR}/imgui_widgets.cpp"
    "${SCUBA_IMGUI_DIR}/imgui_demo.cpp"
    "${SCUBA_IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${SCUBA_IMGUI_DIR}/backends/imgui_impl_dx12.cpp")
  target_include_directories(scuba_imgui SYSTEM PUBLIC "${SCUBA_IMGUI_DIR}" "${SCUBA_IMGUI_DIR}/backends")
  target_link_libraries(scuba_imgui PUBLIC d3d12 dxgi)

  add_library(scuba_engine_runtime STATIC
    src/platform/win32_window.cpp
    src/render/dx12/renderer_dx12.cpp
    src/ui/input_router.cpp
    src/ui/ui_layer.cpp)
  target_include_directories(scuba_engine_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(scuba_engine_runtime
    PUBLIC scuba_engine_core scuba_engine_sco scuba_imgui sco_widgets_imgui
    PRIVATE d3d12 dxgi d3dcompiler dxguid)
  target_compile_definitions(scuba_engine_runtime PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
  scuba_warnings(scuba_engine_runtime)

  add_executable(scuba-engine WIN32 app/main.cpp)
  target_include_directories(scuba-engine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(scuba-engine PRIVATE scuba_engine_runtime shell32)
  target_compile_definitions(scuba-engine PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    SCUBA_VERSION="${PROJECT_VERSION}" SCUBA_SDK_VERSION="${SCUBA_SDK_VERSION}"
    SCUBA_DEFAULT_PLUGIN_ROOT="${PROJECT_BINARY_DIR}/plugins/runtime"
    SCUBA_DEFAULT_CONTENT_DIR="${PROJECT_SOURCE_DIR}/engine/content")
  scuba_warnings(scuba-engine)
  # plugins/CMakeLists.txt makes scuba-engine depend on the staged APP plugins, as scuba-hostd does.
endif()
