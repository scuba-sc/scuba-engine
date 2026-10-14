# scuba-engine plugins: one add_subdirectory per plugin folder.
#
# Built from a parent project that has already included cmake/ScubaSdk.cmake, or on its own:
#   cmake -S plugins -B build-plugins [-DSCUBA_SDK_ARCHIVE_DIR=<dir with the sdk-v2.2.0 assets>]
#   cmake --build build-plugins && ctest --test-dir build-plugins --output-on-failure
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.21)
  project(scuba_plugins LANGUAGES C CXX)
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "scuba-engine plugins are 64-bit only (sco-core is). On Windows configure with -A x64.")
  endif()
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  option(SCUBA_BUILD_TESTS "Build the plugin tests and register them with CTest" ON)
  find_package(Threads REQUIRED)
  include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ScubaSdk.cmake")
  if(SCUBA_BUILD_TESTS)
    enable_testing()
  endif()
endif()

add_subdirectory(mp_chat)
add_subdirectory(org_social)
