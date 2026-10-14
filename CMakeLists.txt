cmake_minimum_required(VERSION 3.21)
project(scuba_engine VERSION 0.1.0 LANGUAGES C CXX)

#   cmake -S . -B build [-DSCUBA_SDK_ARCHIVE_DIR=<dir with the sdk-v2.2.0 assets>]
#   cmake --build build && ctest --test-dir build --output-on-failure
# Multi-config generators (Visual Studio): add --config <cfg> to the build and -C <cfg> to ctest.

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "scuba-engine is 64-bit only (sco-core is). On Windows configure with -A x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SCUBA_BUILD_TESTS "Build the scuba-engine tests and register them with CTest" ON)
option(SCUBA_WERROR "Treat warnings in scuba-engine's own code as errors" ON)

find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

include(cmake/ScubaSdk.cmake)

function(scuba_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /utf-8 $<$<BOOL:${SCUBA_WERROR}>:/WX>)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion
                           $<$<BOOL:${SCUBA_WERROR}>:-Werror>)
  endif()
endfunction()

# ---- hostd protocol core: HTTP, WebSocket, JSON, handler registry, socket server (no kernel) ----
add_library(scuba_bridge STATIC
  src/hostd/sha1.cpp
  src/hostd/base64.cpp
  src/hostd/json.cpp
  src/hostd/http.cpp
  src/hostd/ws.cpp
  src/hostd/handlers.cpp
  src/hostd/designer_export.cpp
  src/hostd/server.cpp)
target_compile_definitions(scuba_bridge PRIVATE
  SCUBA_DESIGNER_SDK_ROOT="${SCUBA_SDK_SOURCE_DIR}"
  SCUBA_DESIGNER_PYTHON="${Python3_EXECUTABLE}")
target_include_directories(scuba_bridge PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(scuba_bridge PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(scuba_bridge PUBLIC ws2_32)
endif()
scuba_warnings(scuba_bridge)

# ---- scuba-hostd: the bridge plus the sco-core host kernel booted in-process ---------------------
add_executable(scuba-hostd
  src/hostd/main.cpp
  src/hostd/kernel.cpp
  src/hostd/modules.cpp
  src/hostd/system_handlers.cpp)
# sco_app is the host kit; it links the kernel libraries transitively (sco_host, sco_runtime,
# sco_plugins, sco_widgets, sco_ui, sco_storage, sco_settings, sco_content, sco_ipc,
# sco_net_service -> sco_net, sco_datacore_service -> sco_datacore -> sco_vfs). Lua is separate:
# sco_lua_game links sco_lua.
target_link_libraries(scuba-hostd PRIVATE scuba_bridge sco_app sco_lua_game)
target_compile_definitions(scuba-hostd PRIVATE
  SCUBA_VERSION="${PROJECT_VERSION}"
  SCUBA_SDK_VERSION="${SCUBA_SDK_VERSION}"
  SCUBA_DEFAULT_STUDIO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/studio/dist")
scuba_warnings(scuba-hostd)

if(SCUBA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
