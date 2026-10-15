# ctest fixture: builds the kit's own sdk/examples/widgets (native C) and widgets_lua (Lua pack)
# with their shipped CMakeLists and installs them the way the game reads plugins:
# <OUT>/install/data/plugins/<id>/.
#
#   cmake -DSDK_DIR=<kit root> -DOUT=<dir> -DGENERATOR=<gen> [-DPLATFORM=<arch>] [-DTOOLSET=<ts>]
#         [-DC_COMPILER=<cc>] -P build_widget_plugins.cmake
foreach(var SDK_DIR OUT GENERATOR)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "build_widget_plugins: ${var} is required")
  endif()
endforeach()

file(REMOVE_RECURSE "${OUT}")

set(configure_args -G "${GENERATOR}")
if(PLATFORM)
  list(APPEND configure_args -A "${PLATFORM}")
endif()
if(TOOLSET)
  list(APPEND configure_args -T "${TOOLSET}")
endif()
if(C_COMPILER)
  list(APPEND configure_args "-DCMAKE_C_COMPILER=${C_COMPILER}")
endif()

foreach(example widgets widgets_lua)
  set(src "${SDK_DIR}/sdk/examples/${example}")
  set(bin "${OUT}/build-${example}")
  foreach(step configure build install)
    if(step STREQUAL "configure")
      set(cmd "${CMAKE_COMMAND}" -S "${src}" -B "${bin}" ${configure_args} -DCMAKE_BUILD_TYPE=Release)
    elseif(step STREQUAL "build")
      set(cmd "${CMAKE_COMMAND}" --build "${bin}" --config Release)
    else()
      set(cmd "${CMAKE_COMMAND}" --install "${bin}" --config Release --prefix "${OUT}/install")
    endif()
    execute_process(COMMAND ${cmd} RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "build_widget_plugins: ${step} of ${example} failed (${rc})")
    endif()
  endforeach()
endforeach()

foreach(f widgets/plugin.ini widgets/widgets.dll widgets_lua/plugin.ini widgets_lua/main.lua)
  if(NOT EXISTS "${OUT}/install/data/plugins/${f}")
    message(FATAL_ERROR "build_widget_plugins: ${f} was not installed")
  endif()
endforeach()
message(STATUS "build_widget_plugins: plugins in ${OUT}/install/data/plugins")
