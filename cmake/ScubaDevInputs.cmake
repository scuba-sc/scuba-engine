include_guard(GLOBAL)

# Development builds must use an explicit, locally supplied, verified artifact.
# Never turn an absent development input into a request for a release asset.
if(SCUBA_SDK_VERSION MATCHES "-dev\\.")
  string(LENGTH "${SCUBA_SDK_HOST_SOURCE_SHA256}" _scuba_dev_hash_length)
  string(LENGTH "${SCUBA_CORE_LIFECYCLE_SOURCE_COMMIT}" _scuba_dev_commit_length)
  if(NOT SCUBA_SDK_ARCHIVE_DIR OR NOT _scuba_dev_hash_length EQUAL 64 OR
     NOT SCUBA_SDK_HOST_SOURCE_SHA256 MATCHES "^[0-9a-f]+$" OR
     NOT _scuba_dev_commit_length EQUAL 40 OR
     NOT SCUBA_CORE_LIFECYCLE_SOURCE_COMMIT MATCHES "^[0-9a-f]+$" OR
     NOT SCUBA_CORE_LIFECYCLE_CANDIDATE OR
     NOT SCUBA_SDK_HOST_SOURCE_SHA256 STREQUAL SCUBA_CORE_LIFECYCLE_ARCHIVE_SHA256)
    message(FATAL_ERROR
      "Development SDK needs a local archive directory, exact source commit, matching SHA256 and checked lifecycle ON")
  endif()
  set(_scuba_dev_archive "${SCUBA_SDK_ARCHIVE_DIR}/sco-sdk-host-source-sc-${SCUBA_SDK_VERSION}.zip")
  if(NOT EXISTS "${_scuba_dev_archive}")
    message(FATAL_ERROR "Development SDK archive is missing: ${_scuba_dev_archive}")
  endif()
  file(SHA256 "${_scuba_dev_archive}" _scuba_dev_archive_hash)
  if(NOT _scuba_dev_archive_hash STREQUAL SCUBA_SDK_HOST_SOURCE_SHA256)
    message(FATAL_ERROR "Development SDK archive SHA256 does not match the supplied coordinate")
  endif()
endif()
