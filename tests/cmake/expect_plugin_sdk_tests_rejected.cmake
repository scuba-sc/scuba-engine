if(NOT DEFINED PLUGIN_SOURCE_DIR OR NOT DEFINED BUILD_DIR OR NOT DEFINED GENERATOR)
  message(FATAL_ERROR "PLUGIN_SOURCE_DIR, BUILD_DIR, and GENERATOR are required")
endif()

file(REMOVE_RECURSE "${BUILD_DIR}")
set(_configure_args
  -S "${PLUGIN_SOURCE_DIR}"
  -B "${BUILD_DIR}"
  -G "${GENERATOR}"
  -DSCUBA_PLUGIN_SDK_ROOT=reject-test-sdk-root
  -DSCUBA_BUILD_TESTS=ON)
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
  message(FATAL_ERROR "plugin-SDK mode unexpectedly configured with SCUBA_BUILD_TESTS=ON")
endif()
string(FIND "${_output}" "SCUBA_PLUGIN_SDK_ROOT is a plugin-only build mode" _reason)
if(_reason EQUAL -1)
  message(FATAL_ERROR "configure failed for an unexpected reason:\n${_output}")
endif()
