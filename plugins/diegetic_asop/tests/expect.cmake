# SPDX-License-Identifier: GPL-3.0-only
# Runs PROGRAM with ARGS ("@@"-separated) and requires exit code RC and every non-empty
# MATCH/MATCH2/MATCH3 regex in its combined output.
#   cmake -DPROGRAM=<exe> -DARGS=a@@b -DRC=0 -DMATCH=<regex> [-DMATCH2=..] [-DMATCH3=..] -P expect.cmake
string(REPLACE "@@" ";" _args "${ARGS}")
execute_process(COMMAND "${PROGRAM}" ${_args}
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
set(_all "${_out}${_err}")
message("${_all}")
if(NOT _rc STREQUAL "${RC}")
  message(FATAL_ERROR "expect: exit ${_rc}, wanted ${RC}")
endif()
foreach(_m MATCH MATCH2 MATCH3)
  if(NOT "${${_m}}" STREQUAL "" AND NOT _all MATCHES "${${_m}}")
    message(FATAL_ERROR "expect: output does not match /${${_m}}/")
  endif()
endforeach()
message("expect: exit ${_rc} and every pattern matched")
