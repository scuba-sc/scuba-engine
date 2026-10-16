# scuba-engine plugins: one add_subdirectory per plugin folder.
#
# Built from a parent project that has already included cmake/ScubaSdk.cmake, or on its own:
#   cmake -S plugins -B build-plugins [-DSCUBA_SDK_ARCHIVE_DIR=<dir with the sdk-v2.3.0 assets>]
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
  include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ScubaActorStats.cmake")
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

# The kit's actor_stats_probe example, built only when the actor stats headers are available
# (cmake/ScubaActorStats.cmake). Its sources stay in the kit; nothing is copied here.
set(SCUBA_EXTRA_PLUGINS "")
if(SCUBA_HAVE_ACTOR_STATS AND SCUBA_ACTOR_STATS_PROBE_DIR)
  sco_add_plugin(actor_stats_probe SOURCES "${SCUBA_ACTOR_STATS_PROBE_DIR}/actor_stats_probe.c"
                 INI "${SCUBA_ACTOR_STATS_PROBE_DIR}/plugin.ini")
  if(SCUBA_ACTOR_STATS_INCLUDE_DIR)
    target_include_directories(actor_stats_probe PRIVATE "${SCUBA_ACTOR_STATS_INCLUDE_DIR}")
  endif()
  set_target_properties(actor_stats_probe PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_BINARY_DIR}/actor_stats_probe_root/actor_stats_probe>")
  add_custom_command(TARGET actor_stats_probe POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SCUBA_ACTOR_STATS_PROBE_DIR}/plugin.ini"
            "${CMAKE_CURRENT_BINARY_DIR}/actor_stats_probe_root/actor_stats_probe/plugin.ini")
  set(SCUBA_ACTOR_STATS_PROBE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/actor_stats_probe_root"
      CACHE INTERNAL "plugin root holding the built actor_stats_probe")
  set(SCUBA_EXTRA_PLUGINS actor_stats_probe)
endif()

set(SCUBA_PLUGIN_ROOT "${CMAKE_CURRENT_BINARY_DIR}/runtime")
add_custom_target(scuba_plugins ALL)
foreach(plugin mp_chat org_social diegetic_asop diegetic_shop_inv)
  add_custom_command(TARGET scuba_plugins POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SCUBA_PLUGIN_ROOT}/${plugin}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:${plugin}>" "${SCUBA_PLUGIN_ROOT}/${plugin}/"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/${plugin}/plugin.ini"
      "${SCUBA_PLUGIN_ROOT}/${plugin}/plugin.ini")
  add_dependencies(scuba_plugins ${plugin})
endforeach()
foreach(plugin ${SCUBA_EXTRA_PLUGINS})
  add_custom_command(TARGET scuba_plugins POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SCUBA_PLUGIN_ROOT}/${plugin}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:${plugin}>" "${SCUBA_PLUGIN_ROOT}/${plugin}/"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${SCUBA_ACTOR_STATS_PROBE_DIR}/plugin.ini" "${SCUBA_PLUGIN_ROOT}/${plugin}/plugin.ini")
  add_dependencies(scuba_plugins ${plugin})
endforeach()
if(TARGET scuba-hostd)
  add_dependencies(scuba-hostd scuba_plugins)
  target_compile_definitions(scuba-hostd PRIVATE SCUBA_DEFAULT_PLUGIN_DIR="${SCUBA_PLUGIN_ROOT}")
endif()
