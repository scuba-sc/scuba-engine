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

add_executable(test_mock_game test_mock_game.cpp)
target_link_libraries(test_mock_game PRIVATE scuba_mock_game)
add_dependencies(test_mock_game mock_game_probe)
scuba_warnings(test_mock_game)
add_test(NAME mock_game_plugins COMMAND test_mock_game "${mock_plugin_root}")
set_tests_properties(mock_game_plugins PROPERTIES TIMEOUT 60)
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
add_test(NAME engine_integration COMMAND "${SCUBA_NODE_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/integration-smoke.mjs" "$<TARGET_FILE:scuba-hostd>"
  "${CMAKE_CURRENT_BINARY_DIR}/assets-fixtures" "${CMAKE_CURRENT_BINARY_DIR}/")
set_tests_properties(engine_integration PROPERTIES DEPENDS test_assets TIMEOUT 90)
add_executable(test_glb_fixtures test_glb_fixtures.cpp)
target_link_libraries(test_glb_fixtures PRIVATE scuba_bridge)
scuba_warnings(test_glb_fixtures)
add_test(NAME glb_fixtures COMMAND test_glb_fixtures "${CMAKE_CURRENT_BINARY_DIR}/assets-fixtures")
set_tests_properties(glb_fixtures PROPERTIES DEPENDS test_assets TIMEOUT 60)
