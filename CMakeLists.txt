cmake_minimum_required(VERSION 3.21)
project(scuba_engine VERSION 0.1.0 LANGUAGES C CXX)

#   cmake -S . -B build [-DSCUBA_SDK_ARCHIVE_DIR=<dir with the sdk-v2.4.0 assets>]
#   cmake --build build && ctest --test-dir build --output-on-failure
# Multi-config generators (Visual Studio): add --config <cfg> to the build and -C <cfg> to ctest.

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "scuba-engine is 64-bit only (sco-core is). On Windows configure with -A x64.")
endif()

if(SCUBA_PLUGIN_SDK_ROOT)
  message(FATAL_ERROR
    "SCUBA_PLUGIN_SDK_ROOT is only supported with -S plugins; the full scuba-engine build requires the host-source kit.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SCUBA_BUILD_TESTS "Build the scuba-engine tests and register them with CTest" ON)
option(SCUBA_WERROR "Treat warnings in scuba-engine's own code as errors" ON)
option(SCUBA_BUILD_ENGINE "Build the native engine: core, VFS and assets everywhere; the scuba-engine app on Windows" ON)
option(SCUBA_TEST_PLUGIN_SDK_CONSUMER
  "Register the published plugin-SDK-only configure, build, and checker CTest" OFF)
option(SCUBA_CORE_LIFECYCLE_CANDIDATE
  "Use checked sco-core startup and shutdown results for the pinned SDK" ON)
option(SCUBA_ALLOW_UNCHECKED_DEV_REHEARSAL
  "Allow the exact local bf56 development SDK to use its unchecked lifecycle API" OFF)
set(SCUBA_CORE_LIFECYCLE_SOURCE_COMMIT "6106fa3639cbcc28f78a1ded8de2707de019f4d3" CACHE STRING
  "Exact sco-core source commit expected by the checked lifecycle profile")
set(SCUBA_CORE_LIFECYCLE_ARCHIVE_SHA256 "a0aaaba07e3d4eb0d8c264912cd6f2ab23f0d237a99fa07816005790a76efc4c" CACHE STRING
  "Exact host-source archive SHA-256 expected by the checked lifecycle profile")
set(SCUBA_PLUGIN_SDK_ARCHIVE_PATH "" CACHE FILEPATH
  "Optional local sco-sdk-2.4.0.zip for the plugin-SDK-only CTest (otherwise it downloads the pinned release asset)")

include(cmake/ScubaDevInputs.cmake)

find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Development-only curated composition. Select before adding the legacy app,
# plugin loader, network server, native providers or shared log callbacks.
option(SCUBA_RESERVED_MOCK_HOST_ONLY "Build only the candidate reserved mock host" OFF)
if(SCUBA_RESERVED_MOCK_HOST_ONLY)
  include(tools/curated_mock_host/build.cmake)
  return()
endif()

if(SCUBA_BUILD_ENGINE)
  include(cmake/ScubaDeps.cmake)
  # Also exercise the real host adapter in portable headless UI tests.
  set(SCO_WIDGETS_IMGUI ON CACHE BOOL "" FORCE)
  set(SCO_WIDGETS_IMGUI_INCLUDE_DIR "${SCUBA_IMGUI_DIR}" CACHE PATH "" FORCE)
endif()
include(cmake/ScubaSdk.cmake)
if(NOT SCUBA_CORE_LIFECYCLE_CANDIDATE)
  if(SCUBA_ALLOW_UNCHECKED_DEV_REHEARSAL)
    if(NOT "${SCUBA_SDK_RELEASE_TAG}" STREQUAL
           "source-bf56d2879393b2b5a603256347fc1d3cf9a1a751-unpublished" OR
       NOT "${SCUBA_CORE_LIFECYCLE_SOURCE_COMMIT}" STREQUAL
           "bf56d2879393b2b5a603256347fc1d3cf9a1a751" OR
       NOT "${SCUBA_SDK_HOST_SOURCE_SHA256}" STREQUAL
           "d41724ce46e34fa05a3ebfebc89ec3c16e6a2996061a183281dd17bcaa7fe0a8" OR
       "${SCUBA_SDK_ARCHIVE_DIR}" STREQUAL "")
      message(FATAL_ERROR
        "Unchecked development rehearsal requires the exact local bf56 SDK archive, tag and explicit opt-in")
    endif()
  elseif(NOT "${SCUBA_SDK_RELEASE_TAG}" STREQUAL "sdk-v2.4.0-rc.2" OR
         NOT "${SCUBA_SDK_HOST_SOURCE_SHA256}" STREQUAL
             "3d711463f7dff3dcd372ffb70b09951ecf21f1d0e2c5f0a899cccbb9977b8201")
    message(FATAL_ERROR
      "Unchecked lifecycle profile is restricted to the exact sdk-v2.4.0-rc.2 archive; the stable SDK requires checked lifecycle")
  endif()
endif()
if(SCUBA_CORE_LIFECYCLE_CANDIDATE)
  string(LENGTH "${SCUBA_CORE_LIFECYCLE_SOURCE_COMMIT}" _scuba_core_commit_length)
  string(LENGTH "${SCUBA_CORE_LIFECYCLE_ARCHIVE_SHA256}" _scuba_core_hash_length)
  if(NOT _scuba_core_commit_length EQUAL 40 OR
     NOT SCUBA_CORE_LIFECYCLE_SOURCE_COMMIT MATCHES "^[0-9a-f]+$" OR
     NOT _scuba_core_hash_length EQUAL 64 OR
     NOT SCUBA_CORE_LIFECYCLE_ARCHIVE_SHA256 MATCHES "^[0-9a-f]+$" OR
     NOT SCUBA_SDK_HOST_SOURCE_SHA256 STREQUAL SCUBA_CORE_LIFECYCLE_ARCHIVE_SHA256)
    message(FATAL_ERROR
      "Checked-lifecycle profile needs the exact source commit and matching host-source SHA256")
  endif()
  execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tools/curated_mock_host/verify_archive.py"
    "${_scuba_sdk_zip}"
    "${SCUBA_CORE_LIFECYCLE_ARCHIVE_SHA256}" "${SCUBA_CORE_LIFECYCLE_SOURCE_COMMIT}"
    COMMAND_ERROR_IS_FATAL ANY)
  include(CheckCXXSourceCompiles)
  set(_scuba_saved_required_includes "${CMAKE_REQUIRED_INCLUDES}")
  set(CMAKE_REQUIRED_INCLUDES "${SCUBA_SDK_SOURCE_DIR}/include")
  unset(SCUBA_SDK_CHECKED_LIFECYCLE CACHE)
  unset(SCUBA_SDK_CHECKED_LIFECYCLE)
  check_cxx_source_compiles("#include <type_traits>\n#include <utility>\n#include <sco/app.h>\nstatic_assert(std::is_same_v<decltype(sco::app::StartDetailed(std::declval<const sco::app::Platform&>())), sco::app::StartResult>);\nstatic_assert(std::is_same_v<decltype(sco::app::Stop()), sco::Result>);\nint main() { return 0; }" SCUBA_SDK_CHECKED_LIFECYCLE)
  set(CMAKE_REQUIRED_INCLUDES "${_scuba_saved_required_includes}")
  if(NOT SCUBA_SDK_CHECKED_LIFECYCLE)
    message(FATAL_ERROR "Checked-lifecycle profile requires StartDetailed and result-returning app::Stop")
  endif()
  message(STATUS "Checked lifecycle enabled: core ${SCUBA_CORE_LIFECYCLE_SOURCE_COMMIT}")
endif()
include(cmake/ScubaActorStats.cmake)
include(cmake/ScubaWarnings.cmake)

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

add_library(scuba_mock_game STATIC
  plugins/diegetic_shop_inv/src/catalogue.cpp
  src/hostd/mock_game/common.cpp
  src/hostd/mock_game/mock_game.cpp
  src/hostd/mock_game/actors.cpp
  src/hostd/mock_game/build.cpp
  src/hostd/mock_game/inventory.cpp
  src/hostd/mock_game/gamerules.cpp
  src/hostd/mock_game/input.cpp
  src/hostd/mock_game/asop.cpp
  src/hostd/mock_game/asop_selection.cpp
  src/hostd/mock_game/audio.cpp
  src/hostd/mock_game/datacore_live.cpp
  src/hostd/mock_game/inventory_contents.cpp
  src/hostd/mock_game/contracts.cpp
  src/hostd/mock_game/creative.cpp
  src/hostd/mock_game/cvars.cpp
  src/hostd/mock_game/cvars_write.cpp
  src/hostd/mock_game/economy.cpp
  src/hostd/mock_game/entities.cpp
  src/hostd/mock_game/events.cpp
  src/hostd/mock_game/game_spatial.cpp
  src/hostd/mock_game/hangar.cpp
  src/hostd/mock_game/loadout.cpp
  src/hostd/mock_game/mining.cpp
  src/hostd/mock_game/missions.cpp
  src/hostd/mock_game/owned.cpp
  src/hostd/mock_game/prefabs.cpp
  src/hostd/mock_game/private_session.cpp
  src/hostd/mock_game/quantum.cpp
  src/hostd/mock_game/relocation.cpp
  src/hostd/mock_game/reputation.cpp
  src/hostd/mock_game/vehicles.cpp
  src/hostd/mock_game/world.cpp
  src/hostd/mock_game/world_control.cpp
  src/hostd/mock_game/spawn.cpp
  src/hostd/mock_game/teleport_spatial.cpp
  src/hostd/mock_game/handlers.cpp)
file(STRINGS "${SCUBA_SDK_SOURCE_DIR}/src/game/services/input.cpp" _scuba_input_actions
  REGEX "^    \\{\"[^\"]+\", [0-9]+, [0-9]+u\\},$")
list(LENGTH _scuba_input_actions _scuba_input_action_count)
if(NOT _scuba_input_action_count EQUAL 1297)
  message(FATAL_ERROR "Locked host-source input registry contains ${_scuba_input_action_count} actions; expected 1297")
endif()
string(JOIN "\n" _scuba_input_action_rows ${_scuba_input_actions})
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated_mock_input_actions.inc"
  "constexpr Action actions[] = {\n${_scuba_input_action_rows}\n};\n")
target_include_directories(scuba_mock_game PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(scuba_mock_game PUBLIC scuba_bridge sco_app)
target_include_directories(scuba_mock_game PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/plugins/diegetic_shop_inv/src")
if(SCUBA_HAVE_ACTOR_STATS)
  target_sources(scuba_mock_game PRIVATE src/hostd/mock_game/actor_stats.cpp)
  target_compile_definitions(scuba_mock_game PUBLIC SCUBA_HAVE_ACTOR_STATS=1)
  if(SCUBA_ACTOR_STATS_INCLUDE_DIR)
    target_include_directories(scuba_mock_game PUBLIC "${SCUBA_ACTOR_STATS_INCLUDE_DIR}")
  endif()
endif()
scuba_warnings(scuba_mock_game)
add_library(scuba_assets STATIC
  src/assets/io.cpp
  src/assets/datacore.cpp
  src/assets/cry.cpp
  src/assets/dds.cpp
  src/assets/obj.cpp
  src/assets/gltf.cpp
  src/hostd/assets_handlers.cpp)
target_link_libraries(scuba_assets PUBLIC scuba_bridge sco_datacore)
scuba_warnings(scuba_assets)

# ---- scuba-hostd: the bridge plus the sco-core host kernel booted in-process ---------------------
add_library(scuba_net_sim STATIC
  src/hostd/net_sim/process.cpp
  src/hostd/net_sim/broker.cpp)
target_link_libraries(scuba_net_sim PUBLIC scuba_bridge)
scuba_warnings(scuba_net_sim)

# Scenario files: the bounded synthetic controls a scenario drives. Shared by scuba-hostd (which
# also serves them over the bridge) and the editor's scenario dock.
add_library(scuba_scenario STATIC src/hostd/scenario_runner.cpp)
target_link_libraries(scuba_scenario PUBLIC scuba_mock_game sco_app)
scuba_warnings(scuba_scenario)

# The sco-core kernel booted in-process with the mock game: shared by scuba-hostd and the engine.
add_library(scuba_kernel STATIC src/hostd/kernel.cpp)
target_link_libraries(scuba_kernel PUBLIC scuba_mock_game sco_lua_game)
target_compile_definitions(scuba_kernel PRIVATE SCUBA_VERSION="${PROJECT_VERSION}")
if(SCUBA_CORE_LIFECYCLE_CANDIDATE)
  target_compile_definitions(scuba_kernel PUBLIC SCUBA_CORE_LIFECYCLE_CANDIDATE)
endif()
scuba_warnings(scuba_kernel)

add_executable(scuba-hostd
  src/hostd/main.cpp
  src/hostd/modules.cpp
  src/hostd/system_handlers.cpp
  src/hostd/plugins_handlers.cpp
  src/hostd/widgets_bridge.cpp
  src/hostd/widgets_handlers.cpp
  src/hostd/shutdown.cpp
  src/hostd/net_sim/peer.cpp
  src/hostd/net_sim/probe_plugin.cpp)
# sco_app is the host kit; it links the kernel libraries transitively (sco_host, sco_runtime,
# sco_plugins, sco_widgets, sco_ui, sco_storage, sco_settings, sco_content, sco_ipc,
# sco_net_service -> sco_net, sco_datacore_service -> sco_datacore -> sco_vfs). Lua is separate:
# sco_lua_game links sco_lua.
target_link_libraries(scuba-hostd PRIVATE scuba_kernel scuba_assets scuba_net_sim scuba_scenario)
target_compile_definitions(scuba-hostd PRIVATE
  SCUBA_VERSION="${PROJECT_VERSION}"
  SCUBA_SDK_VERSION="${SCUBA_SDK_VERSION}"
  SCUBA_DEFAULT_STUDIO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/studio/dist")
scuba_warnings(scuba-hostd)

if(SCUBA_BUILD_ENGINE)
  add_subdirectory(engine)
endif()

if(SCUBA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
  if(SCUBA_HAVE_ACTOR_STATS)
    target_compile_definitions(test_e2e PRIVATE SCUBA_HAVE_ACTOR_STATS=1)
  endif()
endif()
add_subdirectory(plugins)
