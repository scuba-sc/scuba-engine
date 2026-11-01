include_guard(GLOBAL)

if(NOT DEFINED SCUBA_WERROR)
  option(SCUBA_WERROR "Treat warnings in scuba-engine's own code as errors" ON)
endif()

function(scuba_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /utf-8 $<$<BOOL:${SCUBA_WERROR}>:/WX>)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion
                           $<$<BOOL:${SCUBA_WERROR}>:-Werror>)
  endif()
endfunction()
