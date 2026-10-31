if(NOT DEFINED PROJECT_SOURCE_DIR OR NOT DEFINED TEST_BINARY_DIR OR NOT DEFINED GENERATOR)
  message(FATAL_ERROR "PROJECT_SOURCE_DIR, TEST_BINARY_DIR, and GENERATOR are required")
endif()

set(_regression_root "${TEST_BINARY_DIR}/case")
set(_archive "${_regression_root}/work/preserved-input.zip")
file(REMOVE_RECURSE "${_regression_root}")
file(MAKE_DIRECTORY "${_regression_root}/work")
file(WRITE "${_archive}" "caller-owned archive placeholder")

function(_assert_rejected_and_preserved path test_binary_dir expect_symlink)
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      "-DPROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
      "-DTEST_BINARY_DIR=${test_binary_dir}"
      "-DGENERATOR=${GENERATOR}"
      "-DARCHIVE_PATH=${path}"
      -P "${CMAKE_CURRENT_LIST_DIR}/test_plugin_sdk_public_archive.cmake"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
  if(_result EQUAL 0)
    message(FATAL_ERROR "plugin SDK integration unexpectedly accepted archive ${path}")
  endif()
  string(FIND "${_stdout}\n${_stderr}" "must not be inside" _reason)
  if(_reason EQUAL -1)
    message(FATAL_ERROR "unexpected rejection of caller archive:\n${_stdout}\n${_stderr}")
  endif()
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "plugin SDK integration removed caller-owned archive ${path}")
  endif()
  if(expect_symlink AND NOT IS_SYMLINK "${path}")
    message(FATAL_ERROR "plugin SDK integration removed caller-owned archive symlink ${path}")
  endif()
  file(READ "${path}" _contents)
  if(NOT _contents STREQUAL "caller-owned archive placeholder")
    message(FATAL_ERROR "plugin SDK integration changed caller-owned archive ${path}")
  endif()
endfunction()

_assert_rejected_and_preserved("${_archive}" "${_regression_root}" OFF)

set(_symlink_target "${_regression_root}/caller-owned-outside.zip")
set(_symlink_input "${_regression_root}/work/archive-link.zip")
file(WRITE "${_symlink_target}" "caller-owned archive placeholder")
file(CREATE_LINK "${_symlink_target}" "${_symlink_input}" SYMBOLIC RESULT _link_result)
if(_link_result STREQUAL "0")
  _assert_rejected_and_preserved("${_symlink_input}" "${_regression_root}" ON)

  set(_root_alias "${TEST_BINARY_DIR}/case-alias")
  file(CREATE_LINK "${_regression_root}" "${_root_alias}" SYMBOLIC RESULT _alias_result)
  if(_alias_result STREQUAL "0")
    _assert_rejected_and_preserved("${_root_alias}/work/archive-link.zip" "${_root_alias}" ON)
    if(NOT IS_SYMLINK "${_root_alias}")
      message(FATAL_ERROR "plugin SDK integration removed the caller's TEST_BINARY_DIR symlink alias")
    endif()
  else()
    message(STATUS "TEST_BINARY_DIR alias preservation subtest skipped: ${_alias_result}")
  endif()
else()
  message(STATUS "Symlink preservation subtest skipped: ${_link_result}")
endif()
