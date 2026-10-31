if(NOT DEFINED PROJECT_SOURCE_DIR OR NOT DEFINED BUILD_DIR OR NOT DEFINED GENERATOR)
  message(FATAL_ERROR "PROJECT_SOURCE_DIR, BUILD_DIR, and GENERATOR are required")
endif()

file(REMOVE_RECURSE "${BUILD_DIR}")
set(_empty_sdk_assets "${BUILD_DIR}/empty-sdk-assets")
file(MAKE_DIRECTORY "${_empty_sdk_assets}")
set(_configure_args
  -S "${PROJECT_SOURCE_DIR}"
  -B "${BUILD_DIR}"
  -G "${GENERATOR}"
  -DSCUBA_BUILD_ENGINE=OFF
  -DSCUBA_PLUGIN_SDK_ROOT=reject-test-sdk-root
  "-DSCUBA_SDK_ARCHIVE_DIR=${_empty_sdk_assets}")
if(DEFINED PLATFORM)
  list(APPEND _configure_args -A "${PLATFORM}")
endif()
if(DEFINED TOOLSET)
  list(APPEND _configure_args -T "${TOOLSET}")
endif()
if(DEFINED C_COMPILER)
  list(APPEND _configure_args "-DCMAKE_C_COMPILER=${C_COMPILER}")
endif()
if(DEFINED CXX_COMPILER)
  list(APPEND _configure_args "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" ${_configure_args}
  RESULT_VARIABLE _result
  OUTPUT_VARIABLE _stdout
  ERROR_VARIABLE _stderr)
set(_output "${_stdout}\n${_stderr}")
if(_result EQUAL 0)
  message(FATAL_ERROR "the full scuba-engine build unexpectedly accepted SCUBA_PLUGIN_SDK_ROOT")
endif()
string(FIND "${_output}" "SCUBA_PLUGIN_SDK_ROOT is only supported with -S plugins" _reason)
if(_reason EQUAL -1)
  message(FATAL_ERROR "configure failed before the expected plugin-only mode guard:\n${_output}")
endif()
if(EXISTS "${BUILD_DIR}/_sdk")
  message(FATAL_ERROR "root configure created _sdk before rejecting plugin-only mode")
endif()
