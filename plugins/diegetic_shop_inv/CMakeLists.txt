set(SHOP_CORE src/catalogue.cpp src/services.cpp src/shop.cpp src/panel.cpp)
set(SHOP_ROOT "${CMAKE_CURRENT_BINARY_DIR}/root")
set(SHOP_DIR "${SHOP_ROOT}/diegetic_shop_inv")
sco_add_plugin(diegetic_shop_inv SOURCES src/plugin.cpp ${SHOP_CORE} INI plugin.ini)
set_target_properties(diegetic_shop_inv PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "$<1:${SHOP_DIR}>")
target_include_directories(diegetic_shop_inv PRIVATE src)
if(NOT MSVC)
  target_compile_options(diegetic_shop_inv PRIVATE -Wshadow -Wconversion -Wsign-conversion)
endif()
add_custom_command(TARGET diegetic_shop_inv POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/plugin.ini" "${SHOP_DIR}/plugin.ini")

if(SCUBA_BUILD_TESTS)
  if(NOT TARGET sco-plugin-check)
    add_executable(sco-plugin-check "${SCUBA_SDK_SOURCE_DIR}/sdk/tools/sco-plugin-check.c")
    target_include_directories(sco-plugin-check PRIVATE "${SCO_SDK_INCLUDE_DIR}")
    set_target_properties(sco-plugin-check PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON
      MSVC_RUNTIME_LIBRARY "MultiThreaded")
    if(MSVC)
      target_compile_options(sco-plugin-check PRIVATE /W4 /WX /wd5105 /D_CRT_SECURE_NO_WARNINGS)
    else()
      target_compile_options(sco-plugin-check PRIVATE -Wall -Wextra -Werror)
      target_link_libraries(sco-plugin-check PRIVATE ${CMAKE_DL_LIBS})
    endif()
  endif()
  add_executable(test_shop tests/test_shop.cpp ${SHOP_CORE})
  target_include_directories(test_shop PRIVATE src "${SCO_SDK_INCLUDE_DIR}")
  if(MSVC)
    target_compile_options(test_shop PRIVATE /W4 /WX)
  else()
    target_compile_options(test_shop PRIVATE -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion -Werror)
  endif()
  add_dependencies(test_shop diegetic_shop_inv sco-plugin-check sco-host-sim)
  add_test(NAME shop_unit COMMAND test_shop)

  function(shop_expect name rc program)
    cmake_parse_arguments(E "" "MATCH" "ARGS" ${ARGN})
    string(REPLACE ";" "@@" encoded "${E_ARGS}")
    add_test(NAME ${name} COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=${program}"
      "-DARGS=${encoded}" "-DEXPECT_RC=${rc}" "-DMATCH=${E_MATCH}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/expect.cmake")
    set_tests_properties(${name} PROPERTIES TIMEOUT 60)
  endfunction()
  shop_expect(shop_check 0 "$<TARGET_FILE:sco-plugin-check>"
    ARGS "${SHOP_DIR}" MATCH "widgets events delivered=[1-9]")
  shop_expect(shop_check_short 0 "$<TARGET_FILE:sco-plugin-check>"
    ARGS "${SHOP_DIR}" --short-tables MATCH "OK: 0 failure")
  shop_expect(shop_check_caps 0 "$<TARGET_FILE:sco-plugin-check>"
    ARGS "${SHOP_DIR}" --cap game.economy.read --cap game.economy.apply
    --cap game.loadout.apply --cap game.entities.prefabs MATCH "OK: 0 failure")
  shop_expect(shop_sim_list 0 "$<TARGET_FILE:sco-host-sim>"
    ARGS "${SHOP_ROOT}" --no-lua --ticks 1 --invoke diegetic_shop_inv.list 2
    MATCH "p2/8 all: armor_arms_tern")
  shop_expect(shop_sim_prefabs 0 "$<TARGET_FILE:sco-host-sim>"
    ARGS "${SHOP_ROOT}" --no-lua --ticks 1 --invoke diegetic_shop_inv.filter prefabs
    MATCH "p1/2 Prefabs: prefab_crate_stack")
  shop_expect(shop_sim_equip_gate 1 "$<TARGET_FILE:sco-host-sim>"
    ARGS "${SHOP_ROOT}" --no-lua --ticks 1 --cap game.loadout.apply
    --invoke diegetic_shop_inv.equip armor_helmet_tern MATCH "game.loadout is not published")
endif()
