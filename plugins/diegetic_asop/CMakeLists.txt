# SPDX-License-Identifier: GPL-3.0-only
# diegetic_asop: ASOP and hangar lift terminal plugin (sco_add_plugin from the kit's SDK).
# The built plugin folder is laid out as a plugin root the checker and sco-host-sim can load:
#   <build>/plugins/diegetic_asop/root/diegetic_asop/{plugin.ini,diegetic_asop.dll}

set(DIEGETIC_ASOP_CORE
  src/hangar_client.cpp
  src/asop_client.cpp
  src/terminal.cpp)
set(DIEGETIC_ASOP_ROOT "${CMAKE_CURRENT_BINARY_DIR}/root")
set(DIEGETIC_ASOP_DIR "${DIEGETIC_ASOP_ROOT}/diegetic_asop")

sco_add_plugin(diegetic_asop SOURCES src/plugin.cpp ${DIEGETIC_ASOP_CORE} INI plugin.ini)
target_include_directories(diegetic_asop PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# $<1:...> keeps multi-config generators from appending a per-config folder.
set_target_properties(diegetic_asop PROPERTIES LIBRARY_OUTPUT_DIRECTORY "$<1:${DIEGETIC_ASOP_DIR}>")
if(NOT MSVC)
  target_compile_options(diegetic_asop PRIVATE -Wshadow -Wconversion -Wsign-conversion)
endif()
add_custom_command(TARGET diegetic_asop POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/plugin.ini"
          "${DIEGETIC_ASOP_DIR}/plugin.ini")

if(SCUBA_BUILD_TESTS)
  add_executable(test_diegetic_asop tests/test_diegetic_asop.cpp ${DIEGETIC_ASOP_CORE})
  target_include_directories(test_diegetic_asop PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${SCO_SDK_INCLUDE_DIR}")
  scuba_warnings(test_diegetic_asop)
  # ctest does not build: make the default build produce everything the tests below run.
  add_dependencies(test_diegetic_asop diegetic_asop sco-plugin-check sco-host-sim)
  add_test(NAME diegetic_asop_unit COMMAND test_diegetic_asop)

  set(_expect "${CMAKE_CURRENT_SOURCE_DIR}/tests/expect.cmake")
  set(_chk "$<TARGET_FILE:sco-plugin-check>")
  set(_sim "$<TARGET_FILE:sco-host-sim>")
  function(diegetic_asop_expect name rc)
    cmake_parse_arguments(E "" "PROGRAM;MATCH;MATCH2;MATCH3" "ARGS" ${ARGN})
    string(REPLACE ";" "@@" _args "${E_ARGS}")
    add_test(NAME ${name} COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=${E_PROGRAM}" "-DARGS=${_args}" "-DRC=${rc}"
             "-DMATCH=${E_MATCH}" "-DMATCH2=${E_MATCH2}" "-DMATCH3=${E_MATCH3}" -P "${_expect}")
    set_tests_properties(${name} PROPERTIES TIMEOUT 60)
  endfunction()

  # sco-plugin-check: a full 1.1 host, then a 1.0 host (--short-tables), then the game.hangar
  # capabilities on with no game.hangar service published (every call must stay gated).
  diegetic_asop_expect(diegetic_asop_check 0 PROGRAM "${_chk}" ARGS "${DIEGETIC_ASOP_DIR}"
    MATCH "widgets panel diegetic_asop\\.terminal title=\"ASOP Terminal\""
    MATCH2 "widgets events delivered=[1-9]"
    MATCH3 "diegetic_asop\\.status")
  diegetic_asop_expect(diegetic_asop_check_short 0 PROGRAM "${_chk}" ARGS "${DIEGETIC_ASOP_DIR}" --short-tables
    MATCH "no services \\(sco_api 1\\.0\\)")
  diegetic_asop_expect(diegetic_asop_check_caps 0 PROGRAM "${_chk}"
    ARGS "${DIEGETIC_ASOP_DIR}" --cap game.hangar.lift --cap game.hangar.atc_cancel
    MATCH "game\\.hangar: not published")

  # sco-host-sim: the real sco-core host kit loads the plugin from a plugin root, ticks, invokes.
  diegetic_asop_expect(diegetic_asop_host_sim_status 0 PROGRAM "${_sim}"
    ARGS "${DIEGETIC_ASOP_ROOT}" --no-lua --ticks 3 --invoke diegetic_asop.status
    MATCH "invoke diegetic_asop\\.status -> [A-Za-z]+ \"game\\.hangar: not published; lift: off; atc_cancel: off; asop: inert\""
    MATCH2 "\\[sim\\] PASS")
  diegetic_asop_expect(diegetic_asop_host_sim_find_lift 1 PROGRAM "${_sim}"
    ARGS "${DIEGETIC_ASOP_ROOT}" --no-lua --cap game.hangar.lift --invoke diegetic_asop.find_lift 0x2a
    MATCH "invoke diegetic_asop\\.find_lift -> [A-Za-z_]+ \"game\\.hangar 1\\.0 is not published by this host")
  diegetic_asop_expect(diegetic_asop_host_sim_asop 1 PROGRAM "${_sim}"
    ARGS "${DIEGETIC_ASOP_ROOT}" --no-lua --cap game.asop.claim_timeout --invoke diegetic_asop.asop
    MATCH "invoke diegetic_asop\\.asop -> [A-Za-z_]+ \"game\\.asop 1\\.0 is not published by this host")
  diegetic_asop_expect(diegetic_asop_host_sim_bad_id 1 PROGRAM "${_sim}"
    ARGS "${DIEGETIC_ASOP_ROOT}" --no-lua --invoke diegetic_asop.lift_open nope
    MATCH "lift 'nope' is not a non-zero id")
endif()
