set(SCUBA_RESERVED_CORE_COMMIT "" CACHE STRING "Exact candidate core source commit")
if(NOT SCUBA_RESERVED_CORE_COMMIT MATCHES "^[0-9a-f]+$" OR
   NOT SCUBA_SDK_ARCHIVE_DIR OR NOT SCUBA_SDK_HOST_SOURCE_SHA256)
  message(FATAL_ERROR "Curated host needs exact core commit, local host-source kit directory and SHA256")
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${PROJECT_SOURCE_DIR}/tools/curated_mock_host/verify_archive.py"
  "${SCUBA_SDK_ARCHIVE_DIR}/sco-sdk-host-source-sc-2.4.0.zip"
  "${SCUBA_SDK_HOST_SOURCE_SHA256}" "${SCUBA_RESERVED_CORE_COMMIT}"
  COMMAND_ERROR_IS_FATAL ANY)
include("${PROJECT_SOURCE_DIR}/cmake/ScubaSdk.cmake")

function(scuba_curated_target name entry)
  add_executable(${name} "${entry}"
    "${PROJECT_SOURCE_DIR}/src/hostd/reserved_mock_host.cpp"
    "${PROJECT_SOURCE_DIR}/src/hostd/json.cpp"
    "${PROJECT_SOURCE_DIR}/src/hostd/handlers.cpp")
  target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/src")
  # The authority contains the complete reserved path. Do not pull in legacy
  # runtime, scanner, app, plugin loader or native hook implementations.
  target_link_libraries(${name} PRIVATE sco_authority)
  target_compile_definitions(${name} PRIVATE SCUBA_RESERVED_CORE_COMMIT="${SCUBA_RESERVED_CORE_COMMIT}")
  if(MSVC)
    target_compile_options(${name} PRIVATE /W4 /WX /utf-8)
  else()
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()
scuba_curated_target(scuba-reserved-hostd "${PROJECT_SOURCE_DIR}/src/hostd/reserved_main.cpp")
if(SCUBA_BUILD_TESTS)
  enable_testing()
  scuba_curated_target(test_reserved_mock_host "${PROJECT_SOURCE_DIR}/tests/test_reserved_mock_host.cpp")
  foreach(mode normal abandon self_retire)
    add_test(NAME reserved_host_${mode} COMMAND test_reserved_mock_host ${mode})
    set_tests_properties(reserved_host_${mode} PROPERTIES TIMEOUT 15)
  endforeach()
  add_test(NAME reserved_host_stdio COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/test_reserved_mock_stdio.py" "$<TARGET_FILE:scuba-reserved-hostd>")
  set_tests_properties(reserved_host_stdio PROPERTIES TIMEOUT 15)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(SCUBA_NM nm REQUIRED)
    find_program(SCUBA_READELF readelf REQUIRED)
    add_test(NAME reserved_host_binary_composition COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tools/curated_mock_host/audit_binary.py"
      "${SCUBA_NM}" "${SCUBA_READELF}" "$<TARGET_FILE:scuba-reserved-hostd>")
  endif()
endif()
