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

include("${SCUBA_SDK_SOURCE_DIR}/sdk/cmake/sco-plugin.cmake")

# The kit builds sco-plugin-check only with its own tests on (SCO_BUILD_TESTS), which scuba-engine
# turns off, so build it here from the kit's source, the way the kit's sdk/CMakeLists.txt does.
if(SCUBA_BUILD_TESTS AND NOT TARGET sco-plugin-check)
  add_executable(sco-plugin-check "${SCUBA_SDK_SOURCE_DIR}/sdk/tools/sco-plugin-check.c")
  target_include_directories(sco-plugin-check PRIVATE "${SCO_SDK_INCLUDE_DIR}")
  set_target_properties(sco-plugin-check PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON MSVC_RUNTIME_LIBRARY "MultiThreaded")
  if(MSVC)
    target_compile_options(sco-plugin-check PRIVATE /W4 /WX /wd5105 /D_CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(sco-plugin-check PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(sco-plugin-check PRIVATE ${CMAKE_DL_LIBS})
  endif()
endif()

add_subdirectory(mp_chat)
add_subdirectory(org_social)
add_subdirectory(diegetic_asop)
add_subdirectory(diegetic_shop_inv)
