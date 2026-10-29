# Third-party sources for the native engine, fetched like the SDK kit: a pinned archive, SHA-256
# checked before extraction, extracted under the build tree and never copied into this repository.
#
#   SCUBA_DEPS_ARCHIVE_DIR   directory holding local copies of the archives below (named as the
#                            download file); when set, nothing is downloaded. The hash still applies.
#
# Licences and upstreams: THIRD_PARTY_NOTICES.md.

set(SCUBA_DEPS_ARCHIVE_DIR "" CACHE PATH "Local directory with the pinned third-party archives (optional)")

# scuba_fetch_archive(<name> <url> <sha256> <file name> <out var: extracted root>)
function(scuba_fetch_archive name url sha256 file out_var)
  set(_dir "${CMAKE_BINARY_DIR}/_deps")
  set(_zip "${_dir}/${file}")
  set(_src "${_dir}/${name}-${sha256}")
  file(MAKE_DIRECTORY "${_dir}")
  if(NOT EXISTS "${_src}/.scuba-extracted")
    if(SCUBA_DEPS_ARCHIVE_DIR)
      file(COPY_FILE "${SCUBA_DEPS_ARCHIVE_DIR}/${file}" "${_zip}")
    elseif(NOT EXISTS "${_zip}")
      message(STATUS "scuba deps: downloading ${url}")
      file(DOWNLOAD "${url}" "${_zip}" EXPECTED_HASH SHA256=${sha256} TLS_VERIFY ON STATUS _st)
      list(GET _st 0 _code)
      if(NOT _code EQUAL 0)
        file(REMOVE "${_zip}")
        message(FATAL_ERROR "scuba deps: download of ${name} failed: ${_st}")
      endif()
    endif()
    file(SHA256 "${_zip}" _got)
    if(NOT _got STREQUAL sha256)
      message(FATAL_ERROR "scuba deps: ${file} has SHA-256 ${_got}, expected ${sha256}; refusing to build it")
    endif()
    file(REMOVE_RECURSE "${_src}")
    file(ARCHIVE_EXTRACT INPUT "${_zip}" DESTINATION "${_src}")
    file(TOUCH "${_src}/.scuba-extracted")
    message(STATUS "scuba deps: ${file} SHA-256 verified")
  endif()
  set(${out_var} "${_src}" PARENT_SCOPE)
endfunction()

# Dear ImGui, docking branch: the editor shell (docking workspace, menus, console) and the backend
# that sco::widgets::ImGuiRenderer draws plugin panels into. 1.92.8 is the release the SDK's
# pinned "1.92.8 WIP" headers became. Plugins never see these headers: their panels are sco.widgets.
set(SCUBA_IMGUI_VERSION "1.92.8-docking")
set(SCUBA_IMGUI_SHA256 "cb613f0dd1de5f8d9b199142f3122c122e44226c7e4edba758e011e79fafba62")
scuba_fetch_archive(imgui
  "https://github.com/ocornut/imgui/archive/refs/tags/v${SCUBA_IMGUI_VERSION}.zip"
  "${SCUBA_IMGUI_SHA256}" "imgui-v${SCUBA_IMGUI_VERSION}.zip" _imgui_root)
set(SCUBA_IMGUI_DIR "${_imgui_root}/imgui-${SCUBA_IMGUI_VERSION}" CACHE INTERNAL "")

# miniz: raw DEFLATE for zip archives mounted in the VFS (method 8 entries).
set(SCUBA_MINIZ_VERSION "3.1.2")
set(SCUBA_MINIZ_SHA256 "f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a")
scuba_fetch_archive(miniz
  "https://github.com/richgel999/miniz/releases/download/${SCUBA_MINIZ_VERSION}/miniz-${SCUBA_MINIZ_VERSION}.zip"
  "${SCUBA_MINIZ_SHA256}" "miniz-${SCUBA_MINIZ_VERSION}.zip" _miniz_root)

add_library(scuba_miniz STATIC "${_miniz_root}/miniz.c")
target_include_directories(scuba_miniz SYSTEM PUBLIC "${_miniz_root}")
target_compile_definitions(scuba_miniz PUBLIC MINIZ_NO_ZLIB_COMPATIBLE_NAMES MINIZ_NO_TIME
                           MINIZ_NO_ARCHIVE_APIS MINIZ_NO_STDIO)

# ufbx: single-source FBX importer for the model loader (docs/engine/architecture.md, asset intake).
# The upstream archive carries the whole tree (samples, tests, its own fixtures); only ufbx.c is
# compiled and only ufbx.h is exposed. Third-party code keeps its own warnings: scuba_warnings is
# deliberately not applied to it.
set(SCUBA_UFBX_VERSION "0.23.1")
set(SCUBA_UFBX_SHA256 "3e5d8219f5b634fd374949808206eba1a1da49b2b2e3ee31ff0bd228c0abd469")
scuba_fetch_archive(ufbx
  "https://github.com/ufbx/ufbx/archive/refs/tags/v${SCUBA_UFBX_VERSION}.zip"
  "${SCUBA_UFBX_SHA256}" "ufbx-v${SCUBA_UFBX_VERSION}.zip" _ufbx_root)
set(SCUBA_UFBX_DIR "${_ufbx_root}/ufbx-${SCUBA_UFBX_VERSION}" CACHE INTERNAL "")

add_library(scuba_ufbx STATIC "${SCUBA_UFBX_DIR}/ufbx.c")
target_include_directories(scuba_ufbx SYSTEM PUBLIC "${SCUBA_UFBX_DIR}")

