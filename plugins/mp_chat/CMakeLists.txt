# mp_chat: a native sco plugin built with the kit's sdk/cmake/sco-plugin.cmake.
# The plugin folder is laid out under <build>/root/mp_chat/ (plugin.ini + mp_chat.dll), the shape
# sco-plugin-check, sco-host-sim and the host loader read.
if(NOT SCUBA_SDK_SOURCE_DIR)
  message(FATAL_ERROR "mp_chat: configure through plugins/CMakeLists.txt or a parent that includes cmake/ScubaSdk.cmake")
endif()
include("${SCUBA_SDK_SOURCE_DIR}/sdk/cmake/sco-plugin.cmake")

set(MP_CHAT_ROOT "${CMAKE_CURRENT_BINARY_DIR}/root")
sco_add_plugin(mp_chat SOURCES src/mp_chat.c src/chat_model.c INI plugin.ini)
set_target_properties(mp_chat PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "$<1:${MP_CHAT_ROOT}/mp_chat>"
  RUNTIME_OUTPUT_DIRECTORY "$<1:${MP_CHAT_ROOT}/mp_chat>")
configure_file(plugin.ini "${MP_CHAT_ROOT}/mp_chat/plugin.ini" COPYONLY)
if(NOT MSVC)
  target_compile_options(mp_chat PRIVATE -Wshadow -Wconversion -Wsign-conversion)
endif()

if(NOT SCUBA_BUILD_TESTS)
  return()
endif()

add_executable(mp_chat_model_test tests/test_chat_model.c src/chat_model.c)
target_include_directories(mp_chat_model_test PRIVATE src)
set_target_properties(mp_chat_model_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(mp_chat_model_test PRIVATE /W4 /WX)
else()
  target_compile_options(mp_chat_model_test PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror)
endif()
add_test(NAME mp_chat_model COMMAND mp_chat_model_test)

# The kit's stand-in host, built the way sdk/CMakeLists.txt builds it (the kit defines this target
# only with its own tests on).
if(NOT TARGET sco-plugin-check)
  add_executable(sco-plugin-check EXCLUDE_FROM_ALL "${SCUBA_SDK_SOURCE_DIR}/sdk/tools/sco-plugin-check.c")
  target_include_directories(sco-plugin-check PRIVATE "${SCO_SDK_INCLUDE_DIR}")
  set_target_properties(sco-plugin-check PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
  if(MSVC)
    target_compile_options(sco-plugin-check PRIVATE /W4 /WX /wd5105 /D_CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(sco-plugin-check PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(sco-plugin-check PRIVATE ${CMAKE_DL_LIBS})
  endif()
endif()

# Boots the real host kit (sco::app) headless with the plugin, as scuba-hostd's kernel does.
add_executable(mp_chat_host_test tests/test_mp_chat_host.cpp)
target_link_libraries(mp_chat_host_test PRIVATE sco_app)
set_target_properties(mp_chat_host_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(mp_chat_host_test PRIVATE /W4 /WX /utf-8)
  target_compile_definitions(mp_chat_host_test PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(mp_chat_host_test PRIVATE -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion -Werror)
endif()
add_dependencies(mp_chat_host_test mp_chat sco-plugin-check sco-host-sim)

set(_mp_dir "${MP_CHAT_ROOT}/mp_chat")
set(_mp_work "${CMAKE_CURRENT_BINARY_DIR}/work")
add_test(NAME mp_chat_check COMMAND sco-plugin-check "${_mp_dir}")
add_test(NAME mp_chat_check_short_tables COMMAND sco-plugin-check "${_mp_dir}" --short-tables)
add_test(NAME mp_chat_check_invoke_join COMMAND sco-plugin-check "${_mp_dir}" --invoke mp_chat.join lobby)
add_test(NAME mp_chat_host_sim COMMAND sco-host-sim "${MP_CHAT_ROOT}" --no-lua --ticks 3 --invoke mp_chat.status)
add_test(NAME mp_chat_host COMMAND mp_chat_host_test single "${MP_CHAT_ROOT}" "${_mp_work}/single")
add_test(NAME mp_chat_loopback
         COMMAND mp_chat_host_test pair "${MP_CHAT_ROOT}" "${_mp_work}/pair" "$<TARGET_FILE:mp_chat_host_test>")
set_tests_properties(mp_chat_check mp_chat_check_short_tables mp_chat_check_invoke_join mp_chat_host_sim
                     mp_chat_host mp_chat_loopback PROPERTIES TIMEOUT 120)
set_tests_properties(mp_chat_check mp_chat_check_short_tables mp_chat_check_invoke_join
                     PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL ")
