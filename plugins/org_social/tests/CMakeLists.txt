# org_social tests: the pure-C core, the kit's sco-plugin-check (normal, --short-tables, --invoke),
# sco-host-sim, and an in-process host test that boots sco::app with storage and a synthetic
# game.spatial. Every fixture is synthetic.

function(org_social_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion -Werror)
  endif()
endfunction()

# The SDK's checker, built from the kit sources (the kit builds it only with SCO_BUILD_TESTS).
# Shared by every plugin lane under plugins/: the first one to get here defines it.
if(NOT TARGET scuba-plugin-check)
  add_executable(scuba-plugin-check "${SCUBA_SDK_SOURCE_DIR}/sdk/tools/sco-plugin-check.c")
  target_include_directories(scuba-plugin-check PRIVATE "${SCUBA_SDK_SOURCE_DIR}/include")
  set_target_properties(scuba-plugin-check PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON
                        MSVC_RUNTIME_LIBRARY "MultiThreaded")
  if(MSVC)
    target_compile_options(scuba-plugin-check PRIVATE /wd5105 /D_CRT_SECURE_NO_WARNINGS)
  else()
    target_link_libraries(scuba-plugin-check PRIVATE ${CMAKE_DL_LIBS})
  endif()
endif()

add_executable(org_social_core_test test_core.c ${ORG_SOCIAL_CORE_SOURCES})
target_include_directories(org_social_core_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
set_target_properties(org_social_core_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(UNIX AND NOT APPLE)
  target_link_libraries(org_social_core_test PRIVATE m)
endif()
org_social_warnings(org_social_core_test)
add_test(NAME org_social_core COMMAND org_social_core_test)

add_executable(org_social_host_test test_host.cpp ../src/os_wire.c ../src/os_model.c)
target_include_directories(org_social_host_test PRIVATE "${SCUBA_SDK_SOURCE_DIR}/include" "../src")
target_link_libraries(org_social_host_test PRIVATE sco_app)
org_social_warnings(org_social_host_test)
add_dependencies(org_social_host_test org_social scuba-plugin-check)
add_test(NAME org_social_host
         COMMAND org_social_host_test "${ORG_SOCIAL_STAGE_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/host_data")

set(_dir "${ORG_SOCIAL_STAGE_ROOT}/org_social")
add_test(NAME org_social_plugin_check COMMAND scuba-plugin-check "${_dir}")
add_test(NAME org_social_plugin_check_short COMMAND scuba-plugin-check "${_dir}" --short-tables)
add_test(NAME org_social_plugin_check_invoke
         COMMAND scuba-plugin-check "${_dir}" --invoke org_social.member_add Alpha officer)
set_tests_properties(org_social_plugin_check_invoke PROPERTIES PASS_REGULAR_EXPRESSION "Alpha added as officer")

if(TARGET sco-host-sim)
  add_dependencies(org_social_host_test sco-host-sim)
  add_test(NAME org_social_host_sim
           COMMAND sco-host-sim "${ORG_SOCIAL_STAGE_ROOT}" --ticks 3 --no-lua --invoke org_social.roster)
  set_tests_properties(org_social_host_sim PROPERTIES
    PASS_REGULAR_EXPRESSION "org_social\\.roster -> OK \"org 0, online 0, party 0, waypoints 0 \\(no session\\)\""
    FAIL_REGULAR_EXPRESSION "\\[sim\\] FAIL")
endif()

set_tests_properties(org_social_core org_social_host org_social_plugin_check org_social_plugin_check_short
                     org_social_plugin_check_invoke PROPERTIES TIMEOUT 60)
