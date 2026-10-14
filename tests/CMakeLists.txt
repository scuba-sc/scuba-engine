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
