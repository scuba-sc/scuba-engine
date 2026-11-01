if(NOT DEFINED PROJECT_SOURCE_DIR OR NOT DEFINED TEST_BINARY_DIR OR
   NOT DEFINED GENERATOR OR NOT DEFINED CMAKE_EXECUTABLE OR NOT DEFINED SDK_ARCHIVE_DIR)
  message(FATAL_ERROR
    "PROJECT_SOURCE_DIR, TEST_BINARY_DIR, GENERATOR, CMAKE_EXECUTABLE and SDK_ARCHIVE_DIR are required")
endif()

file(REMOVE_RECURSE "${TEST_BINARY_DIR}")
set(_configure_args
  -S "${PROJECT_SOURCE_DIR}"
  -B "${TEST_BINARY_DIR}"
  -G "${GENERATOR}"
  -DSCUBA_BUILD_ENGINE=OFF
  -DSCUBA_BUILD_TESTS=OFF
  -DSCUBA_CORE_LIFECYCLE_CANDIDATE=OFF
  -DSCUBA_CORE_LIFECYCLE_SOURCE_COMMIT=6106fa3639cbcc28f78a1ded8de2707de019f4d3
  -DSCUBA_CORE_LIFECYCLE_ARCHIVE_SHA256=a0aaaba07e3d4eb0d8c264912cd6f2ab23f0d237a99fa07816005790a76efc4c
  -DSCUBA_SDK_RELEASE_TAG=sdk-v2.4.0
  -DSCUBA_SDK_HOST_SOURCE_SHA256=a0aaaba07e3d4eb0d8c264912cd6f2ab23f0d237a99fa07816005790a76efc4c
  "-DSCUBA_SDK_ARCHIVE_DIR=${SDK_ARCHIVE_DIR}")
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
  COMMAND "${CMAKE_EXECUTABLE}" ${_configure_args}
  RESULT_VARIABLE _result
  OUTPUT_VARIABLE _stdout
  ERROR_VARIABLE _stderr)
set(_output "${_stdout}\n${_stderr}")
if(_result EQUAL 0)
  message(FATAL_ERROR "stable sdk-v2.4.0 unexpectedly accepted unchecked lifecycle")
endif()
string(FIND "${_output}"
  "Unchecked lifecycle profile is restricted"
  _reason)
if(_reason EQUAL -1)
  message(FATAL_ERROR "configure failed before the expected stable lifecycle guard:\n${_output}")
endif()
