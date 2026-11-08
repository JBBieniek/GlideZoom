# Do not let an empty/truncated executable become an empty successful shell
# script on hosts whose exec launcher falls back to /bin/sh after ENOEXEC.
if(NOT DEFINED GLIDEZOOM_TEST_EXE OR NOT EXISTS "${GLIDEZOOM_TEST_EXE}")
  message(FATAL_ERROR "The compiled test executable is missing")
endif()
file(SIZE "${GLIDEZOOM_TEST_EXE}" test_size)
if(test_size LESS 512)
  message(FATAL_ERROR "The compiled test executable is empty or truncated")
endif()
execute_process(COMMAND "${GLIDEZOOM_TEST_EXE}"
  RESULT_VARIABLE test_exit OUTPUT_VARIABLE test_output ERROR_VARIABLE test_error
  TIMEOUT 120)
if(NOT "${test_exit}" STREQUAL "0")
  message(FATAL_ERROR "${test_output}${test_error}\nTest failed: ${test_exit}")
endif()
if("${test_output}" STREQUAL "")
  message(FATAL_ERROR "Test returned no required completion evidence: ${test_error}")
endif()
message("${test_output}${test_error}")
