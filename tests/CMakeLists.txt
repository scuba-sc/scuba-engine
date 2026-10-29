foreach(test_name test_websocket test_http_json test_handlers)
  add_executable(${test_name} ${test_name}.cpp)
  target_link_libraries(${test_name} PRIVATE scuba_bridge)
  scuba_warnings(${test_name})
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES TIMEOUT 60)
endforeach()

add_executable(test_e2e test_e2e.cpp)
target_link_libraries(test_e2e PRIVATE scuba_bridge)
scuba_warnings(test_e2e)
add_dependencies(test_e2e scuba-hostd)
add_test(NAME e2e_hostd_ping COMMAND test_e2e $<TARGET_FILE:scuba-hostd>)
set_tests_properties(e2e_hostd_ping PROPERTIES TIMEOUT 60)

# sco.widgets bridge: the kit's own widget examples, built with their shipped CMakeLists, loaded
# into scuba-hostd headless and driven over the WebSocket.
set(widget_plugins_out "${CMAKE_CURRENT_BINARY_DIR}/widget_plugins")
set(widget_plugins_args -DSDK_DIR=${SCUBA_SDK_SOURCE_DIR} -DOUT=${widget_plugins_out} "-DGENERATOR=${CMAKE_GENERATOR}")
if(CMAKE_GENERATOR_PLATFORM)
  list(APPEND widget_plugins_args -DPLATFORM=${CMAKE_GENERATOR_PLATFORM})
endif()
if(CMAKE_GENERATOR_TOOLSET)
  list(APPEND widget_plugins_args -DTOOLSET=${CMAKE_GENERATOR_TOOLSET})
endif()
if(NOT CMAKE_GENERATOR MATCHES "Visual Studio|Xcode")
  list(APPEND widget_plugins_args -DC_COMPILER=${CMAKE_C_COMPILER})
endif()
add_test(NAME widget_plugins_build
         COMMAND ${CMAKE_COMMAND} ${widget_plugins_args} -P "${CMAKE_CURRENT_SOURCE_DIR}/build_widget_plugins.cmake")
set_tests_properties(widget_plugins_build PROPERTIES TIMEOUT 300 FIXTURES_SETUP widget_plugins)

add_executable(test_widgets_e2e test_widgets_e2e.cpp)
target_link_libraries(test_widgets_e2e PRIVATE scuba_bridge)
scuba_warnings(test_widgets_e2e)
add_dependencies(test_widgets_e2e scuba-hostd)
add_test(NAME e2e_hostd_widgets
         COMMAND test_widgets_e2e $<TARGET_FILE:scuba-hostd> "${widget_plugins_out}/install/data/plugins")
set_tests_properties(e2e_hostd_widgets PROPERTIES TIMEOUT 120 FIXTURES_REQUIRED widget_plugins)
set(mock_plugin_root "${CMAKE_CURRENT_BINARY_DIR}/mock_plugins")
file(MAKE_DIRECTORY "${mock_plugin_root}/mock_game_probe")
configure_file(plugins/mock_game_probe/plugin.ini "${mock_plugin_root}/mock_game_probe/plugin.ini" COPYONLY)
add_library(mock_game_probe MODULE plugins/mock_game_probe/probe.cpp)
target_include_directories(mock_game_probe PRIVATE "${SCUBA_SDK_SOURCE_DIR}/include")
target_link_libraries(mock_game_probe PRIVATE Threads::Threads)
set_target_properties(mock_game_probe PROPERTIES PREFIX "" SUFFIX ".dll"
  CXX_VISIBILITY_PRESET hidden
  LIBRARY_OUTPUT_DIRECTORY "$<1:${mock_plugin_root}/mock_game_probe>")
scuba_warnings(mock_game_probe)

add_executable(test_registry_conformance test_registry_conformance.cpp)
target_link_libraries(test_registry_conformance PRIVATE scuba_mock_game)
scuba_warnings(test_registry_conformance)
add_test(NAME registry_conformance COMMAND test_registry_conformance "${SCUBA_SDK_SOURCE_DIR}/sdk/status_registry.json")
set_tests_properties(registry_conformance PROPERTIES TIMEOUT 60)

# Standalone service contract tests provide their own host seams. Compile only the
# services under test so their seams do not collide with the full mock host.
foreach(service asop_selection audio datacore_live gamerules inventory_contents relocation world_control)
  set(test_name test_mock_${service})
  if(service STREQUAL "gamerules" OR service STREQUAL "relocation" OR service STREQUAL "world_control")
    set(test_name test_mock_${service}_v11)
  endif()
  add_executable(${test_name} ${test_name}.cpp "../src/hostd/mock_game/${service}.cpp")
  if(service STREQUAL "relocation")
    target_sources(${test_name} PRIVATE ../src/hostd/mock_game/game_spatial.cpp)
  endif()
  target_link_libraries(${test_name} PRIVATE scuba_bridge sco_app)
  # Assertions in these contract tests must execute in Release builds as well.
  if(MSVC)
    target_compile_options(${test_name} PRIVATE /UNDEBUG)
  else()
    target_compile_options(${test_name} PRIVATE -UNDEBUG)
  endif()
  scuba_warnings(${test_name})
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES TIMEOUT 60)
endforeach()

foreach(service entities loadout)
  set(test_name test_mock_${service}_rc2)
  add_executable(${test_name} ${test_name}.cpp)
  target_link_libraries(${test_name} PRIVATE scuba_mock_game)
  scuba_warnings(${test_name})
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES TIMEOUT 60)
endforeach()

add_executable(test_mock_game test_mock_game.cpp)
target_link_libraries(test_mock_game PRIVATE scuba_mock_game)
add_dependencies(test_mock_game mock_game_probe)
scuba_warnings(test_mock_game)
add_test(NAME mock_game_plugins COMMAND test_mock_game "${mock_plugin_root}")
set_tests_properties(mock_game_plugins PROPERTIES TIMEOUT 60)
if(SCUBA_HAVE_ACTOR_STATS AND SCUBA_ACTOR_STATS_PROBE_DIR)
  # game.actor_stats(+_write) mock and the kit's actor_stats_probe loaded against it. Built only
  # when the pinned kit (or the local pre-release override) has the headers: cmake/ScubaActorStats.cmake.
  add_executable(test_mock_actor_stats test_mock_actor_stats.cpp)
  target_link_libraries(test_mock_actor_stats PRIVATE scuba_mock_game)
  add_dependencies(test_mock_actor_stats actor_stats_probe)
  scuba_warnings(test_mock_actor_stats)
  add_test(NAME mock_actor_stats COMMAND test_mock_actor_stats "${CMAKE_BINARY_DIR}/plugins/actor_stats_probe_root")
  set_tests_properties(mock_actor_stats PROPERTIES TIMEOUT 60)
endif()

# Kernel lifecycle and profile CLI tests.
add_executable(test_product_profile test_product_profile.cpp)
target_link_libraries(test_product_profile PRIVATE scuba_kernel)
add_dependencies(test_product_profile mock_game_probe)
scuba_warnings(test_product_profile)
add_test(NAME product_profile_lifecycle COMMAND test_product_profile
  "${CMAKE_CURRENT_BINARY_DIR}/product-profile-fixture" $<TARGET_FILE:mock_game_probe>)
set_tests_properties(product_profile_lifecycle PROPERTIES TIMEOUT 60)

# Product distribution export: the tree --product accepts again, and every refusal that keeps a
# half-written distribution from existing. Portable, so it runs everywhere the core builds.
add_executable(test_product_export test_product_export.cpp)
target_link_libraries(test_product_export PRIVATE scuba_engine_core)
scuba_warnings(test_product_export)
add_test(NAME product_export COMMAND test_product_export "${CMAKE_CURRENT_BINARY_DIR}/product-export-fixture")
set_tests_properties(product_export PROPERTIES TIMEOUT 60)

# The editor's headless path: --frames with --hidden renders off-screen and exits without a
# desktop, so the smoke run is a real test instead of a manual check.
if(TARGET scuba-engine)
  add_test(NAME engine_offscreen_smoke COMMAND scuba-engine --frames 2 --warp --hidden --no-plugins)
  set_tests_properties(engine_offscreen_smoke PROPERTIES TIMEOUT 180)

  # --scenario runs the file on the game thread before the first frame; a file that cannot be
  # loaded has to be a nonzero exit.
  add_test(NAME engine_scenario_missing COMMAND scuba-engine --frames 1 --warp --hidden --no-plugins
    --scenario "${CMAKE_CURRENT_BINARY_DIR}/no-such-scenario.json")
  set_tests_properties(engine_scenario_missing PROPERTIES TIMEOUT 180 WILL_FAIL TRUE)

  # The same scenario file scuba-hostd's CLI tests drive, run through the editor.
  add_test(NAME engine_scenario_valid COMMAND scuba-engine --frames 1 --warp --hidden --no-plugins
    --scenario "${CMAKE_CURRENT_SOURCE_DIR}/scenarios/valid.json")
  set_tests_properties(engine_scenario_valid PROPERTIES TIMEOUT 180)
endif()

add_executable(test_selected_profile test_selected_profile.cpp ${CMAKE_SOURCE_DIR}/src/hostd/scenario_runner.cpp)
target_link_libraries(test_selected_profile PRIVATE scuba_kernel)
add_dependencies(test_selected_profile mock_game_probe)
scuba_warnings(test_selected_profile)
add_test(NAME selected_profile_lifecycle COMMAND test_selected_profile
  "${CMAKE_CURRENT_BINARY_DIR}/selected-profile-fixture" $<TARGET_FILE:mock_game_probe>
  "${CMAKE_CURRENT_SOURCE_DIR}/scenarios/valid.json")
set_tests_properties(selected_profile_lifecycle PROPERTIES TIMEOUT 60)

# Record actual query/load/exit/unload callouts across an ABI refusal.
foreach(profile_id a_good b_bad c_unselected)
  add_library(profile_${profile_id} MODULE plugins/profile_lifecycle/plugin.cpp)
  target_include_directories(profile_${profile_id} PRIVATE "${SCUBA_SDK_SOURCE_DIR}/include")
  if(profile_id STREQUAL "b_bad")
    set(profile_api_major 99)
  else()
    set(profile_api_major 1)
  endif()
  target_compile_definitions(profile_${profile_id} PRIVATE PROFILE_PLUGIN_ID="${profile_id}" PROFILE_API_MAJOR=${profile_api_major})
  set_target_properties(profile_${profile_id} PROPERTIES PREFIX "" SUFFIX ".dll")
  scuba_warnings(profile_${profile_id})
endforeach()
add_executable(test_profile_failure test_profile_failure.cpp)
target_link_libraries(test_profile_failure PRIVATE scuba_kernel)
add_dependencies(test_profile_failure profile_a_good profile_b_bad profile_c_unselected)
scuba_warnings(test_profile_failure)
add_test(NAME profile_failed_start_cleanup COMMAND test_profile_failure
  "${CMAKE_CURRENT_BINARY_DIR}/profile-failure-fixture" $<TARGET_FILE:profile_a_good>
  $<TARGET_FILE:profile_b_bad> $<TARGET_FILE:profile_c_unselected>)
set_tests_properties(profile_failed_start_cleanup PROPERTIES TIMEOUT 60)

foreach(profile_case valid_empty valid_service valid_audio valid_plugin default malformed unknown_service unknown_field oversize depth missing)
  add_test(NAME profile_${profile_case} COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_profile_cli.py" "$<TARGET_FILE:scuba-hostd>"
    "${CMAKE_CURRENT_SOURCE_DIR}/profiles/${profile_case}.json" "${profile_case}"
    "${CMAKE_CURRENT_BINARY_DIR}/profile-${profile_case}")
  set_tests_properties(profile_${profile_case} PROPERTIES TIMEOUT 30)
endforeach()

foreach(scenario_case valid profile default unknown_field oversize)
  add_test(NAME scenario_${scenario_case} COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_scenario_cli.py" "$<TARGET_FILE:scuba-hostd>"
    "${CMAKE_CURRENT_SOURCE_DIR}/scenarios/${scenario_case}.json" "${scenario_case}"
    "${CMAKE_CURRENT_BINARY_DIR}/scenario-${scenario_case}" "${CMAKE_CURRENT_SOURCE_DIR}/profiles/valid_plugin.json")
  set_tests_properties(scenario_${scenario_case} PROPERTIES TIMEOUT 30)
endforeach()

# Scenario runner control-facade unit test (game-thread synthetic controls).
add_executable(test_scenario_runner test_scenario_runner.cpp
  ${CMAKE_SOURCE_DIR}/src/hostd/scenario_runner.cpp)
target_link_libraries(test_scenario_runner PRIVATE scuba_mock_game)
scuba_warnings(test_scenario_runner)
add_test(NAME scenario_runner_unit COMMAND test_scenario_runner)
set_tests_properties(scenario_runner_unit PROPERTIES TIMEOUT 60)

add_executable(test_designer_export test_designer_export.cpp)
target_link_libraries(test_designer_export PRIVATE scuba_bridge)
target_compile_definitions(test_designer_export PRIVATE
  TEST_DESIGNER_SDK_ROOT="${SCUBA_SDK_SOURCE_DIR}"
  TEST_DESIGNER_PYTHON="${Python3_EXECUTABLE}")
scuba_warnings(test_designer_export)
add_test(NAME designer_export COMMAND test_designer_export)
set_tests_properties(designer_export PROPERTIES TIMEOUT 60)

find_program(SCUBA_BUN_EXECUTABLE bun)
find_program(SCUBA_LUA_EXECUTABLE NAMES lua5.4 lua54 lua-5.4 HINTS "/opt/homebrew/opt/lua@5.4/bin")
if(SCUBA_BUN_EXECUTABLE)
  add_test(NAME designer_generated_plugins
    COMMAND "${SCUBA_BUN_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/designer-plugins.mjs"
      "${SCUBA_SDK_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/designer-plugins"
      "${Python3_EXECUTABLE}" "${SCUBA_LUA_EXECUTABLE}")
  set_tests_properties(designer_generated_plugins PROPERTIES TIMEOUT 240)
  add_test(NAME designer_codegen_unit COMMAND "${SCUBA_BUN_EXECUTABLE}" test
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../tools/designer-codegen")
  set_tests_properties(designer_codegen_unit PROPERTIES TIMEOUT 60)
  add_test(NAME studio_mcp_unit COMMAND "${SCUBA_BUN_EXECUTABLE}" test
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../tools/scuba-engine-mcp")
  set_tests_properties(studio_mcp_unit PROPERTIES TIMEOUT 60)
endif()
add_executable(test_assets test_assets.cpp)
target_link_libraries(test_assets PRIVATE scuba_assets)
scuba_warnings(test_assets)
add_dependencies(test_assets sco-dcb)
add_test(NAME test_assets COMMAND test_assets "${CMAKE_CURRENT_BINARY_DIR}/assets-fixtures")
set_tests_properties(test_assets PROPERTIES TIMEOUT 60)
add_test(NAME assets_pack_lint COMMAND sco-dcb lint "${CMAKE_CURRENT_BINARY_DIR}/assets-fixtures/export.toml")
add_test(NAME assets_pack_check COMMAND sco-dcb check
  "${CMAKE_CURRENT_BINARY_DIR}/assets-fixtures/tiny.dcb"
  "${CMAKE_CURRENT_BINARY_DIR}/assets-fixtures/imported-pack")
set_tests_properties(assets_pack_lint assets_pack_check PROPERTIES DEPENDS test_assets TIMEOUT 60)
add_executable(test_net_sim test_net_sim.cpp)
target_link_libraries(test_net_sim PRIVATE scuba_net_sim)
scuba_warnings(test_net_sim)
add_dependencies(test_net_sim scuba-hostd)
add_test(NAME netsim_two_peer_delivery COMMAND test_net_sim $<TARGET_FILE:scuba-hostd> delivery)
add_test(NAME netsim_four_peer_join_leave COMMAND test_net_sim $<TARGET_FILE:scuba-hostd> membership)
add_test(NAME netsim_eight_peer_bounds COMMAND test_net_sim $<TARGET_FILE:scuba-hostd> bounds)
set_tests_properties(netsim_two_peer_delivery netsim_four_peer_join_leave netsim_eight_peer_bounds PROPERTIES TIMEOUT 120)

find_program(SCUBA_NODE_EXECUTABLE node REQUIRED)
add_test(NAME studio_mcp_integration COMMAND "${SCUBA_NODE_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../tools/scuba-engine-mcp/integration.mjs"
  "$<TARGET_FILE:scuba-hostd>" "${CMAKE_BINARY_DIR}/plugins/runtime")
set_tests_properties(studio_mcp_integration PROPERTIES TIMEOUT 60)
add_test(NAME profile_mcp_integration COMMAND "${SCUBA_NODE_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/profile_mcp.mjs" "$<TARGET_FILE:scuba-hostd>"
  "${CMAKE_BINARY_DIR}/plugins/runtime" "${CMAKE_CURRENT_SOURCE_DIR}/profiles/valid_plugin.json"
  "${CMAKE_CURRENT_SOURCE_DIR}/../tools/scuba-engine-mcp/server.mjs")
set_tests_properties(profile_mcp_integration PROPERTIES TIMEOUT 60)
add_test(NAME engine_integration COMMAND "${SCUBA_NODE_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/integration-smoke.mjs" "$<TARGET_FILE:scuba-hostd>"
  "${CMAKE_CURRENT_BINARY_DIR}/assets-fixtures" "${CMAKE_CURRENT_BINARY_DIR}/")
set_tests_properties(engine_integration PROPERTIES DEPENDS test_assets TIMEOUT 90)
add_executable(test_glb_fixtures test_glb_fixtures.cpp)
target_link_libraries(test_glb_fixtures PRIVATE scuba_bridge)
scuba_warnings(test_glb_fixtures)
add_test(NAME glb_fixtures COMMAND test_glb_fixtures "${CMAKE_CURRENT_BINARY_DIR}/assets-fixtures")
set_tests_properties(glb_fixtures PROPERTIES DEPENDS test_assets TIMEOUT 60)

if(TARGET scuba_engine_core)
  add_executable(test_engine_core test_engine_core.cpp)
  target_link_libraries(test_engine_core PRIVATE scuba_engine_core scuba_assets scuba_miniz)
  scuba_warnings(test_engine_core)
  add_test(NAME engine_core COMMAND test_engine_core)
  set_tests_properties(engine_core PROPERTIES TIMEOUT 60)
endif()

# The engine's product-profile path: ScoHost::Start with productRoot, fail-closed on a missing
# data/plugins or a refused plugin, and the roster it reports.
if(TARGET scuba_engine_sco)
  add_executable(test_sco_host_product test_sco_host_product.cpp)
  target_link_libraries(test_sco_host_product PRIVATE scuba_engine_sco)
  scuba_warnings(test_sco_host_product)
  add_test(NAME sco_host_product COMMAND test_sco_host_product "${CMAKE_CURRENT_BINARY_DIR}/product-app-fixture")
  set_tests_properties(sco_host_product PROPERTIES TIMEOUT 60)
endif()
