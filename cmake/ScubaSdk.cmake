# Fetches the published sco-core host-source kit, verifies its SHA-256 and adds it to the build.
#
#   SCUBA_SDK_VERSION       kit version (default 2.4.0)
#   SCUBA_SDK_RELEASE_TAG   sco-core release tag the kit is downloaded from (default
#                           sdk-v2.4.0-rc.2, SDK 2.4.0 release candidate 2)
#   SCUBA_SDK_ARCHIVE_DIR   directory holding a local copy of the release assets; when set, the
#                           archive is taken from there instead of downloaded. Either way the
#                           archive must match SCUBA_SDK_HOST_SOURCE_SHA256 or configure fails.
#
# The kit sources are never copied into this repository: they are extracted under the build tree.

set(SCUBA_SDK_VERSION "2.4.0" CACHE STRING "sco-core SDK version consumed by scuba-engine")
set(SCUBA_SDK_RELEASE_TAG "sdk-v2.4.0-rc.2" CACHE STRING "sco-core release tag holding the kit")
set(SCUBA_SDK_ARCHIVE_DIR "" CACHE PATH "Local directory with the ${SCUBA_SDK_RELEASE_TAG} release assets (optional)")
set(SCUBA_SDK_HOST_SOURCE_SHA256 "3d711463f7dff3dcd372ffb70b09951ecf21f1d0e2c5f0a899cccbb9977b8201"
    CACHE STRING "SHA-256 of sco-sdk-host-source-sc-<version>.zip (from the release SHA256SUMS)")

set(_scuba_sdk_name "sco-sdk-host-source-sc-${SCUBA_SDK_VERSION}")
set(_scuba_sdk_zip_name "${_scuba_sdk_name}.zip")
set(_scuba_sdk_url
    "https://github.com/scuba-sc/sco-core/releases/download/${SCUBA_SDK_RELEASE_TAG}/${_scuba_sdk_zip_name}")
set(_scuba_sdk_dl_dir "${CMAKE_BINARY_DIR}/_sdk")
set(_scuba_sdk_zip "${_scuba_sdk_dl_dir}/${_scuba_sdk_zip_name}")
set(_scuba_sdk_src_parent "${_scuba_sdk_dl_dir}/src-${SCUBA_SDK_HOST_SOURCE_SHA256}")
set(SCUBA_SDK_SOURCE_DIR "${_scuba_sdk_src_parent}/${_scuba_sdk_name}")

function(_scuba_sdk_hash_ok file out)
  set(${out} FALSE PARENT_SCOPE)
  if(EXISTS "${file}")
    file(SHA256 "${file}" _h)
    if(_h STREQUAL SCUBA_SDK_HOST_SOURCE_SHA256)
      set(${out} TRUE PARENT_SCOPE)
    endif()
  endif()
endfunction()

file(MAKE_DIRECTORY "${_scuba_sdk_dl_dir}")
_scuba_sdk_hash_ok("${_scuba_sdk_zip}" _have)
if(NOT _have)
  if(SCUBA_SDK_ARCHIVE_DIR)
    set(_local "${SCUBA_SDK_ARCHIVE_DIR}/${_scuba_sdk_zip_name}")
    if(NOT EXISTS "${_local}")
      message(FATAL_ERROR "SCUBA_SDK_ARCHIVE_DIR is set but ${_local} does not exist")
    endif()
    message(STATUS "sco-core SDK: using local archive ${_local}")
    file(COPY_FILE "${_local}" "${_scuba_sdk_zip}")
  else()
    message(STATUS "sco-core SDK: downloading ${_scuba_sdk_url}")
    file(DOWNLOAD "${_scuba_sdk_url}" "${_scuba_sdk_zip}"
         EXPECTED_HASH SHA256=${SCUBA_SDK_HOST_SOURCE_SHA256}
         TLS_VERIFY ON
         STATUS _dl_status)
    list(GET _dl_status 0 _dl_code)
    if(NOT _dl_code EQUAL 0)
      file(REMOVE "${_scuba_sdk_zip}")
      message(FATAL_ERROR "sco-core SDK download failed: ${_dl_status}")
    endif()
  endif()
  _scuba_sdk_hash_ok("${_scuba_sdk_zip}" _have)
  if(NOT _have)
    file(SHA256 "${_scuba_sdk_zip}" _got)
    file(REMOVE "${_scuba_sdk_zip}")
    message(FATAL_ERROR "sco-core SDK archive ${_scuba_sdk_zip_name} has SHA-256 ${_got}, "
                        "expected ${SCUBA_SDK_HOST_SOURCE_SHA256}; refusing to build it")
  endif()
endif()
message(STATUS "sco-core SDK: ${_scuba_sdk_zip_name} SHA-256 verified")

# The extraction directory is keyed by the verified hash, so a different archive never reuses it.
if(NOT EXISTS "${SCUBA_SDK_SOURCE_DIR}/CMakeLists.txt")
  file(REMOVE_RECURSE "${_scuba_sdk_src_parent}")
  file(ARCHIVE_EXTRACT INPUT "${_scuba_sdk_zip}" DESTINATION "${_scuba_sdk_src_parent}")
  if(NOT EXISTS "${SCUBA_SDK_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "sco-core SDK archive did not contain ${_scuba_sdk_name}/CMakeLists.txt")
  endif()
endif()

# Consumer settings for the kit: no kit unit tests, and its own -Werror/-WX is left to the kit's
# CI (a newer compiler warning in kit code must not break this project's build).
set(SCO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SCO_WERROR OFF CACHE BOOL "" FORCE)
set(SCO_GAME_SC ON CACHE BOOL "" FORCE)
add_subdirectory("${SCUBA_SDK_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_sdk/build" EXCLUDE_FROM_ALL)
