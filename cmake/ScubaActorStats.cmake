# Decides whether the game.actor_stats / game.actor_stats_write mock (src/hostd/mock_game/actor_stats.cpp)
# and the actor_stats_probe plugin build. Needs sc_actor_stats.h and sc_actor_stats_write.h.
#
# Those headers are NOT in the published sdk-v2.3.0 kit (they are on sco-core main, unreleased), and
# scuba-engine consumes published, hash-verified archives only. So this is off by default and turns
# itself on, with no code change, once the pinned kit ships them (sdk-v2.4.0 or later).
#
#   SCUBA_ACTOR_STATS_SDK_DIR  optional, for local verification only: a sco-core checkout whose
#                              include/ and sdk/examples/actor_stats_probe/ are used instead of
#                              the kit's. Pre-release, never a CI or release input; nothing from
#                              it is copied into this repository.
#
# Sets SCUBA_HAVE_ACTOR_STATS, SCUBA_ACTOR_STATS_INCLUDE_DIR (a build-tree dir holding just the two
# headers when SCUBA_ACTOR_STATS_SDK_DIR is used) and SCUBA_ACTOR_STATS_PROBE_DIR.
include_guard(GLOBAL)

set(SCUBA_ACTOR_STATS_SDK_DIR "" CACHE PATH
    "Local sco-core checkout providing the unreleased actor stats headers (verification only)")

set(SCUBA_HAVE_ACTOR_STATS FALSE)
set(SCUBA_ACTOR_STATS_INCLUDE_DIR "")
set(SCUBA_ACTOR_STATS_PROBE_DIR "")

if(SCUBA_ACTOR_STATS_SDK_DIR)
  set(_stats_root "${SCUBA_ACTOR_STATS_SDK_DIR}")
  set(_stats_pre_release TRUE)
else()
  set(_stats_root "${SCUBA_SDK_SOURCE_DIR}")
  set(_stats_pre_release FALSE)
endif()

if(EXISTS "${_stats_root}/include/sc_actor_stats.h" AND EXISTS "${_stats_root}/include/sc_actor_stats_write.h")
  set(SCUBA_HAVE_ACTOR_STATS TRUE)
  if(_stats_pre_release)
    # Only these two headers, from the checkout, behind the kit's own include directory.
    set(SCUBA_ACTOR_STATS_INCLUDE_DIR "${CMAKE_BINARY_DIR}/_actor_stats_include")
    file(MAKE_DIRECTORY "${SCUBA_ACTOR_STATS_INCLUDE_DIR}")
    file(COPY "${_stats_root}/include/sc_actor_stats.h" "${_stats_root}/include/sc_actor_stats_write.h"
         DESTINATION "${SCUBA_ACTOR_STATS_INCLUDE_DIR}")
    message(WARNING "game.actor_stats mock built against UNRELEASED headers from ${_stats_root} "
                    "(SCUBA_ACTOR_STATS_SDK_DIR). Local verification only: not run in game.")
  endif()
  if(EXISTS "${_stats_root}/sdk/examples/actor_stats_probe/actor_stats_probe.c")
    set(SCUBA_ACTOR_STATS_PROBE_DIR "${_stats_root}/sdk/examples/actor_stats_probe")
  endif()
  message(STATUS "game.actor_stats mock: ON (headers from ${_stats_root})")
else()
  message(STATUS "game.actor_stats mock: off (the pinned sco-core kit ${SCUBA_SDK_VERSION} has no sc_actor_stats.h)")
endif()
