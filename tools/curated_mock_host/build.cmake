set(SCUBA_RESERVED_CORE_COMMIT "" CACHE STRING "Exact candidate core source commit")
# Verify the same named archive that ScubaSdk.cmake will consume. Preserve the
# stable default while allowing explicitly selected development SDK versions.
set(SCUBA_SDK_VERSION "2.4.0" CACHE STRING "sco-core SDK version consumed by scuba-engine")
if(NOT SCUBA_RESERVED_CORE_COMMIT MATCHES "^[0-9a-f]+$" OR
   NOT SCUBA_SDK_ARCHIVE_DIR OR NOT SCUBA_SDK_HOST_SOURCE_SHA256)
  message(FATAL_ERROR "Curated host needs exact core commit, local host-source kit directory and SHA256")
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${PROJECT_SOURCE_DIR}/tools/curated_mock_host/verify_archive.py"
  "${SCUBA_SDK_ARCHIVE_DIR}/sco-sdk-host-source-sc-${SCUBA_SDK_VERSION}.zip"
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
  if(ARGC GREATER 2)
    target_link_libraries(${name} PRIVATE ${ARGV2})
  else()
    target_link_libraries(${name} PRIVATE sco_authority)
  endif()
  target_compile_definitions(${name} PRIVATE SCUBA_RESERVED_CORE_COMMIT="${SCUBA_RESERVED_CORE_COMMIT}")
  if(MSVC)
    target_compile_options(${name} PRIVATE /W4 /WX /utf-8)
  else()
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()
scuba_curated_target(scuba-reserved-hostd "${PROJECT_SOURCE_DIR}/src/hostd/reserved_main.cpp")
if(MSVC)
  # PE executables do not carry the full symbol inventory used by the ELF
  # audit. Emit a linker map for the production executable only.
  target_link_options(scuba-reserved-hostd PRIVATE
    "/MAP:$<TARGET_FILE_DIR:scuba-reserved-hostd>/$<TARGET_FILE_BASE_NAME:scuba-reserved-hostd>.map")
endif()
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
  # Observation-only core instrumentation is confined to this separate test
  # executable. The normal host retains the uninstrumented authority library.
  add_library(scuba_copy_test_authority STATIC "${SCUBA_SDK_SOURCE_DIR}/src/reserved_session.cpp")
  target_include_directories(scuba_copy_test_authority PUBLIC "${SCUBA_SDK_SOURCE_DIR}/include")
  target_compile_definitions(scuba_copy_test_authority PRIVATE SCO_RESERVED_DRAIN_TESTING)
  target_link_libraries(scuba_copy_test_authority PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
  scuba_curated_target(test_reserved_output_copy "${PROJECT_SOURCE_DIR}/tests/test_reserved_output_copy.cpp"
    scuba_copy_test_authority)
  target_include_directories(test_reserved_output_copy PRIVATE "${PROJECT_SOURCE_DIR}/tests")
  target_compile_definitions(test_reserved_output_copy PRIVATE SCUBA_RESERVED_COPY_TESTING SCO_RESERVED_DRAIN_TESTING)
  foreach(mode return unwind early_return early_unwind)
    add_test(NAME reserved_copy_${mode} COMMAND test_reserved_output_copy ${mode})
    set_tests_properties(reserved_copy_${mode} PROPERTIES TIMEOUT 15)
  endforeach()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(SCUBA_NM nm REQUIRED)
    find_program(SCUBA_READELF readelf REQUIRED)
    add_test(NAME reserved_host_binary_composition COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tools/curated_mock_host/audit_binary.py"
      "${SCUBA_NM}" "${SCUBA_READELF}" "$<TARGET_FILE:scuba-reserved-hostd>")
  elseif(WIN32 AND MSVC)
    find_program(SCUBA_DUMPBIN NAMES dumpbin REQUIRED)
    add_test(NAME reserved_host_binary_composition COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tools/curated_mock_host/audit_windows.py"
      --dumpbin "${SCUBA_DUMPBIN}" --exe "$<TARGET_FILE:scuba-reserved-hostd>"
      --map "$<TARGET_FILE_DIR:scuba-reserved-hostd>/$<TARGET_FILE_BASE_NAME:scuba-reserved-hostd>.map")
  endif()
endif()
