cmake_minimum_required(VERSION 3.21)
# org_social builds inside the scuba-engine tree (plugins/CMakeLists.txt) or on its own:
#   cmake -S plugins/org_social -B build [-DSCUBA_SDK_ARCHIVE_DIR=<dir with the sdk-v2.2.0 assets>]
#   cmake --build build && ctest --test-dir build --output-on-failure
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(org_social VERSION 0.1.0 LANGUAGES C CXX)
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "org_social is 64-bit only (sco plugins are). On Windows configure with -A x64.")
  endif()
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  option(SCUBA_BUILD_TESTS "Build the org_social tests and register them with CTest" ON)
  find_package(Threads REQUIRED)
  include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/ScubaSdk.cmake")
  if(SCUBA_BUILD_TESTS)
    enable_testing()
  endif()
endif()

include("${SCUBA_SDK_SOURCE_DIR}/sdk/cmake/sco-plugin.cmake")

set(ORG_SOCIAL_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/os_model.c" "${CMAKE_CURRENT_SOURCE_DIR}/src/os_wire.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/os_json.c")
sco_add_plugin(org_social SOURCES src/org_social.c ${ORG_SOCIAL_CORE_SOURCES} INI plugin.ini)
if(NOT MSVC)
  target_compile_options(org_social PRIVATE -Wshadow -Wconversion -Wsign-conversion)
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(org_social PRIVATE m)
endif()

# Staged plugin root: <stage>/org_social/{org_social.dll,plugin.ini}, the layout data/plugins uses.
set(ORG_SOCIAL_STAGE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/stage" CACHE INTERNAL "org_social staged plugin root")
set(_os_stage "${ORG_SOCIAL_STAGE_ROOT}/org_social")
add_custom_command(TARGET org_social POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_os_stage}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:org_social>" "${_os_stage}/"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/plugin.ini" "${_os_stage}/plugin.ini")

if(SCUBA_BUILD_TESTS)
  add_subdirectory(tests)
endif()
