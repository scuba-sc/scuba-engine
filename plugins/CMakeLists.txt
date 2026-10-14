# SPDX-License-Identifier: GPL-3.0-only
# Plugins built against the sco-core kit's SDK helpers (sdk/cmake/sco-plugin.cmake). Each plugin
# lives in plugins/<id>/ and is added here with one add_subdirectory line.

include("${SCUBA_SDK_SOURCE_DIR}/sdk/cmake/sco-plugin.cmake")

# The kit builds sco-plugin-check only with its own tests on (SCO_BUILD_TESTS), which scuba-engine
# turns off, so build it here from the kit's source, the way the kit's sdk/CMakeLists.txt does.
if(SCUBA_BUILD_TESTS AND NOT TARGET sco-plugin-check)
  add_executable(sco-plugin-check "${SCUBA_SDK_SOURCE_DIR}/sdk/tools/sco-plugin-check.c")
  target_include_directories(sco-plugin-check PRIVATE "${SCO_SDK_INCLUDE_DIR}")
  set_target_properties(sco-plugin-check PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON MSVC_RUNTIME_LIBRARY "MultiThreaded")
  if(MSVC)
    target_compile_options(sco-plugin-check PRIVATE /W4 /WX /wd5105 /D_CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(sco-plugin-check PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(sco-plugin-check PRIVATE ${CMAKE_DL_LIBS})
  endif()
endif()

add_subdirectory(diegetic_asop)
