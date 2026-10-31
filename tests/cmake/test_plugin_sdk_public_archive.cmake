if(NOT DEFINED PROJECT_SOURCE_DIR OR NOT DEFINED TEST_BINARY_DIR OR NOT DEFINED GENERATOR)
  message(FATAL_ERROR "PROJECT_SOURCE_DIR, TEST_BINARY_DIR, and GENERATOR are required")
endif()

set(_archive_url "https://github.com/scuba-sc/sco-core/releases/download/sdk-v2.4.0-rc.2/sco-sdk-2.4.0.zip")
set(_archive_sha256 "51a7ce266d087bcbeac39c92f26a6f905b4473883dd7ed6f0f5ca823fa88c88e")
get_filename_component(_test_binary_dir "${TEST_BINARY_DIR}" ABSOLUTE)
file(MAKE_DIRECTORY "${_test_binary_dir}")
file(REAL_PATH "${_test_binary_dir}" _test_binary_dir)
set(_work_root "${_test_binary_dir}/work")
if(IS_SYMLINK "${_work_root}")
  message(FATAL_ERROR "CTest scratch directory must not be a symlink: ${_work_root}")
endif()

function(_require_archive_outside_work candidate description)
  file(RELATIVE_PATH _relative_to_work "${_work_root}" "${candidate}")
  if(_relative_to_work STREQUAL "" OR _relative_to_work STREQUAL "." OR
     (NOT IS_ABSOLUTE "${_relative_to_work}" AND
      NOT _relative_to_work MATCHES "^\\.\\.(/|$)"))
    message(FATAL_ERROR
      "SCUBA_PLUGIN_SDK_ARCHIVE_PATH ${description} must not be inside the CTest scratch directory ${_work_root}")
  endif()
endfunction()

if(DEFINED ARCHIVE_PATH AND NOT ARCHIVE_PATH STREQUAL "")
  if(NOT EXISTS "${ARCHIVE_PATH}")
    message(FATAL_ERROR "SCUBA_PLUGIN_SDK_ARCHIVE_PATH does not exist: ${ARCHIVE_PATH}")
  endif()
  get_filename_component(_archive_input "${ARCHIVE_PATH}" ABSOLUTE)
  _require_archive_outside_work("${_archive_input}" "input path")
  # Resolve directory aliases while retaining the final basename before resolving a final
  # symlink; otherwise an in-scratch link to an external target would evade the overlap check.
  get_filename_component(_archive_input_parent "${_archive_input}" DIRECTORY)
  get_filename_component(_archive_input_name "${_archive_input}" NAME)
  file(REAL_PATH "${_archive_input_parent}" _archive_input_parent_real)
  set(_archive_input_canonical "${_archive_input_parent_real}/${_archive_input_name}")
  _require_archive_outside_work("${_archive_input_canonical}" "input path after resolving parent aliases")
  file(REAL_PATH "${_archive_input}" _archive)
  _require_archive_outside_work("${_archive}" "resolved target")
else()
  set(_archive "${_work_root}/sco-sdk-2.4.0.zip")
endif()

file(REMOVE_RECURSE "${_work_root}")
file(MAKE_DIRECTORY "${_work_root}")
if(NOT DEFINED ARCHIVE_PATH OR ARCHIVE_PATH STREQUAL "")
  file(DOWNLOAD "${_archive_url}" "${_archive}"
    EXPECTED_HASH "SHA256=${_archive_sha256}"
    TLS_VERIFY ON TIMEOUT 120 INACTIVITY_TIMEOUT 30 STATUS _download_status)
  list(GET _download_status 0 _download_code)
  if(NOT _download_code EQUAL 0)
    file(REMOVE "${_archive}")
    message(FATAL_ERROR "Could not download the pinned plugin SDK archive: ${_download_status}")
  endif()
endif()

file(SHA256 "${_archive}" _observed_sha256)
if(NOT _observed_sha256 STREQUAL _archive_sha256)
  message(FATAL_ERROR
    "Plugin SDK archive SHA-256 mismatch: got ${_observed_sha256}, expected ${_archive_sha256}")
endif()
message(STATUS "Verified published plugin SDK archive SHA-256 ${_observed_sha256}")

set(_sdk_extract_dir "${_work_root}/sdk")
file(MAKE_DIRECTORY "${_sdk_extract_dir}")
file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_sdk_extract_dir}")
set(_sdk_root "${_sdk_extract_dir}/sco-sdk-2.4.0")
if(NOT EXISTS "${_sdk_root}/VERSION")
  message(FATAL_ERROR "Pinned plugin SDK archive did not contain sco-sdk-2.4.0/VERSION")
endif()

set(_plugin_build "${_work_root}/plugin-build")
set(_configure_args
  -S "${PROJECT_SOURCE_DIR}/plugins"
  -B "${_plugin_build}"
  -G "${GENERATOR}"
  "-DSCUBA_PLUGIN_SDK_ROOT=${_sdk_root}"
  -DSCUBA_BUILD_TESTS=OFF)
if(DEFINED PLATFORM)
  list(APPEND _configure_args -A "${PLATFORM}")
endif()
if(DEFINED TOOLSET)
  list(APPEND _configure_args -T "${TOOLSET}")
endif()
if(DEFINED C_COMPILER)
  list(APPEND _configure_args "-DCMAKE_C_COMPILER=${C_COMPILER}")
endif()
if(DEFINED CXX_COMPILER)
  list(APPEND _configure_args "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
endif()
if(DEFINED BUILD_CONFIG AND NOT BUILD_CONFIG STREQUAL "")
  list(APPEND _configure_args "-DCMAKE_BUILD_TYPE=${BUILD_CONFIG}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" ${_configure_args}
  RESULT_VARIABLE _configure_result
  OUTPUT_VARIABLE _configure_stdout
  ERROR_VARIABLE _configure_stderr)
if(NOT _configure_result EQUAL 0)
  message(FATAL_ERROR "Plugin-only SDK configure failed:\n${_configure_stdout}\n${_configure_stderr}")
endif()

set(_build_args --build "${_plugin_build}" --target scuba_plugins sco-plugin-check --parallel 4)
if(DEFINED BUILD_CONFIG AND NOT BUILD_CONFIG STREQUAL "")
  list(APPEND _build_args --config "${BUILD_CONFIG}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" ${_build_args}
  RESULT_VARIABLE _build_result
  OUTPUT_VARIABLE _build_stdout
  ERROR_VARIABLE _build_stderr)
if(NOT _build_result EQUAL 0)
  message(FATAL_ERROR "Plugin-only SDK build failed:\n${_build_stdout}\n${_build_stderr}")
endif()

set(_checker_candidates
  "${_plugin_build}/sco-plugin-check"
  "${_plugin_build}/sco-plugin-check.exe")
if(DEFINED BUILD_CONFIG AND NOT BUILD_CONFIG STREQUAL "")
  list(PREPEND _checker_candidates
    "${_plugin_build}/${BUILD_CONFIG}/sco-plugin-check"
    "${_plugin_build}/${BUILD_CONFIG}/sco-plugin-check.exe")
endif()
file(GLOB _multi_config_checker_candidates
  "${_plugin_build}/*/sco-plugin-check"
  "${_plugin_build}/*/sco-plugin-check.exe")
list(PREPEND _checker_candidates ${_multi_config_checker_candidates})
foreach(_candidate IN LISTS _checker_candidates)
  if(EXISTS "${_candidate}")
    set(_checker "${_candidate}")
    break()
  endif()
endforeach()
if(NOT _checker)
  message(FATAL_ERROR "Built sco-plugin-check executable was not found under ${_plugin_build}")
endif()

foreach(_plugin IN ITEMS mp_chat org_social diegetic_asop diegetic_shop_inv)
  set(_plugin_root "${_plugin_build}/runtime/${_plugin}")
  foreach(_variant IN ITEMS full short-tables)
    set(_checker_args "${_plugin_root}")
    if(_variant STREQUAL "short-tables")
      list(APPEND _checker_args --short-tables)
    endif()
    execute_process(
      COMMAND "${_checker}" ${_checker_args}
      RESULT_VARIABLE _check_result
      OUTPUT_VARIABLE _check_stdout
      ERROR_VARIABLE _check_stderr)
    string(FIND "${_check_stdout}" "OK: 0 failure(s)" _check_ok)
    if(NOT _check_result EQUAL 0 OR _check_ok EQUAL -1)
      message(FATAL_ERROR
        "${_plugin} ${_variant} checker failed (exit ${_check_result}):\n${_check_stdout}\n${_check_stderr}")
    endif()
    message(STATUS "${_plugin} ${_variant}: OK: 0 failure(s)")
  endforeach()
endforeach()
