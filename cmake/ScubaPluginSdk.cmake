# Selects the public, plugin-only SDK from an already extracted package root.
# Unlike ScubaSdk.cmake, this module does not add the sco-core host implementation.
include_guard(GLOBAL)

if(NOT SCUBA_PLUGIN_SDK_ROOT)
  message(FATAL_ERROR "Set SCUBA_PLUGIN_SDK_ROOT to the extracted sco-sdk-2.4.0 directory")
endif()

get_filename_component(_scuba_plugin_sdk_root "${SCUBA_PLUGIN_SDK_ROOT}" ABSOLUTE)
if(NOT IS_DIRECTORY "${_scuba_plugin_sdk_root}")
  message(FATAL_ERROR "SCUBA_PLUGIN_SDK_ROOT is not a directory: ${_scuba_plugin_sdk_root}")
endif()

if(NOT EXISTS "${_scuba_plugin_sdk_root}/VERSION")
  message(FATAL_ERROR "SCUBA_PLUGIN_SDK_ROOT is not an extracted sco SDK: missing VERSION")
endif()
file(READ "${_scuba_plugin_sdk_root}/VERSION" _scuba_plugin_sdk_version)
string(STRIP "${_scuba_plugin_sdk_version}" _scuba_plugin_sdk_version)
if(NOT _scuba_plugin_sdk_version STREQUAL "2.4.0")
  message(FATAL_ERROR
    "SCUBA_PLUGIN_SDK_ROOT must be sco-sdk-2.4.0; found '${_scuba_plugin_sdk_version}'")
endif()

set(SCUBA_PLUGIN_SDK_CMAKE_FILE "${_scuba_plugin_sdk_root}/cmake/sco-plugin.cmake")
set(SCUBA_PLUGIN_SDK_INCLUDE_DIR "${_scuba_plugin_sdk_root}/include")
set(SCUBA_PLUGIN_SDK_CHECKER_SOURCE "${_scuba_plugin_sdk_root}/tools/sco-plugin-check.c")
foreach(_required IN ITEMS
    "${SCUBA_PLUGIN_SDK_CMAKE_FILE}"
    "${SCUBA_PLUGIN_SDK_INCLUDE_DIR}/sco_api.h"
    "${SCUBA_PLUGIN_SDK_CHECKER_SOURCE}")
  if(NOT EXISTS "${_required}")
    message(FATAL_ERROR "Incomplete extracted plugin SDK: missing ${_required}")
  endif()
endforeach()

set(SCUBA_PLUGIN_SDK_ROOT "${_scuba_plugin_sdk_root}")
set(SCO_SDK_INCLUDE_DIR "${SCUBA_PLUGIN_SDK_INCLUDE_DIR}")
include("${SCUBA_PLUGIN_SDK_CMAKE_FILE}")
