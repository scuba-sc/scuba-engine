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
endif()
